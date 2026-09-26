#include "anchor.hpp"
#include "scan.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using llavon::memscan::Hint;
using llavon::memscan::Match;
using llavon::memscan::Anchor;
using llavon::memscan::AnchorError;
using llavon::memscan::ScanError;
using llavon::memscan::ScanLimits;

struct Options {
    // Text to search for: what the input method recently committed at the
    // caret. Nothing is ever written to the client.
    std::string anchor;
    std::vector<pid_t> pids;
    std::vector<Hint> hints;
    std::size_t before = 4096;
    std::size_t after = 512;
    std::size_t max_bytes = 512ull * 1024 * 1024;
    long timeout_ms = 3000;
};

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const unsigned char value : input) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                    output += buffer;
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    return output;
}

std::optional<std::uintptr_t> parse_hex(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    std::uintptr_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

void usage() {
    std::fprintf(stderr,
                 "usage: llavon-ime-memscan --anchor <text> --pid <pid> [--pid <pid>...]\n"
                 "       [--before <bytes>] [--after <bytes>] [--max-bytes <bytes>]\n"
                 "       [--timeout-ms <ms>] [--hint <pid>:<hexstart>-<hexend>]\n");
}

void fail(const std::string& code, const std::string& detail, int status) {
    std::printf("{\"found\":false,\"error\":\"%s\",\"detail\":\"%s\"}\n", code.c_str(),
                json_escape(detail).c_str());
    std::exit(status);
}

void print_match(const Match& match) {
    char address[32];
    std::snprintf(address, sizeof(address), "0x%lx",
                  static_cast<unsigned long>(match.address));
    std::printf(
        "{\"found\":true,\"pid\":%d,\"encoding\":\"%s\",\"address\":\"%s\","
        "\"before\":\"%s\",\"after\":\"%s\",\"before_bytes\":%zu,\"after_bytes\":%zu,"
        "\"scanned_bytes\":%zu,\"truncated\":%s}\n",
        static_cast<int>(match.pid), llavon::memscan::encoding_name(match.encoding), address,
        json_escape(match.before).c_str(), json_escape(match.after).c_str(), match.before_bytes,
        match.after_bytes, match.scanned_bytes, match.truncated ? "true" : "false");
}

// A private, newline-delimited protocol for the IME-owned child. The normal
// one-shot CLI above stays compatible with existing callers.
void serve() {
    std::string line;
    while (std::getline(std::cin, line)) {
        nlohmann::json response;
        try {
            if (line.size() > 65536) throw std::runtime_error("request too long");
            const auto request = nlohmann::json::parse(line);
            const auto text = request.at("anchor").get<std::string>();
            AnchorError anchor_error;
            const auto anchor = llavon::memscan::parse_anchor(text, anchor_error);
            if (!anchor) throw std::runtime_error(anchor_error.detail);
            const auto pids = request.at("pids").get<std::vector<int>>();
            if (pids.empty() || pids.size() > 16) throw std::runtime_error("invalid pids");
            const int hint_pid = request.value("hint_pid", 0);
            const auto hint_address = request.value("hint_address", std::uintptr_t{0});
            const auto hint_before = request.value("hint_before", std::string{});
            std::vector<Hint> hints;
            if (hint_pid > 1 && hint_address > 0 &&
                hint_address < std::numeric_limits<std::uintptr_t>::max() - 32768) {
                const auto start = hint_address > 32768 ? hint_address - 32768 : 0;
                hints.push_back({hint_pid, start, hint_address + 32768,
                                 hint_address, hint_before});
            }
            ScanLimits limits;
            limits.max_bytes_per_pid = 128ull * 1024 * 1024;
            nlohmann::json matches = nlohmann::json::array();
            std::string error_code = "not-found";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            for (const int pid : pids) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0 || matches.size() >= 16) break;
                limits.timeout = std::min(remaining, std::chrono::milliseconds(120));
                ScanError error;
                std::vector<Match> candidates;
                (void)llavon::memscan::scan_pid(pid, *anchor, 256, 0, limits, hints,
                                                error, &candidates);
                // One process may hold many display/protocol copies. Keep a
                // bounded share from each named PID so another client process
                // with the real document buffer can still be considered.
                std::ranges::stable_sort(candidates, [&](const Match& left, const Match& right) {
                    const bool left_hint = left.pid == hint_pid && left.address == hint_address &&
                                           left.before == hint_before && !hint_before.empty();
                    const bool right_hint = right.pid == hint_pid && right.address == hint_address &&
                                            right.before == hint_before && !hint_before.empty();
                    if (left_hint != right_hint) return left_hint;
                    return left.before.size() > right.before.size();
                });
                std::size_t selected = 0;
                for (const auto& match : candidates) {
                    if (selected == 4 || matches.size() >= 16) break;
                    ++selected;
                    matches.push_back({{"pid", match.pid}, {"address", match.address},
                                       {"encoding", llavon::memscan::encoding_name(match.encoding)},
                                       {"before", match.before}});
                }
                if (!error.code.empty()) error_code = error.code;
            }
            if (!matches.empty()) error_code.clear();
            response = {{"matches", std::move(matches)}, {"error", error_code}};
        } catch (const std::exception& exception) {
            response = {{"matches", nlohmann::json::array()}, {"error", "invalid-request"}};
        }
        std::cout << response.dump() << '\n' << std::flush;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--serve") {
        serve();
        return 0;
    }
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto next = [&](const char* name) -> std::string_view {
            if (index + 1 >= argc) fail("usage", std::string("missing value for ") + name, 2);
            return argv[++index];
        };
        if (argument == "--anchor") {
            options.anchor = std::string(next("--anchor"));
        } else if (argument == "--pid") {
            const auto value = parse_number<long>(next("--pid"));
            if (!value || *value <= 1 || *value > 4194304) fail("usage", "invalid --pid", 2);
            options.pids.push_back(static_cast<pid_t>(*value));
        } else if (argument == "--before") {
            const auto value = parse_number<std::size_t>(next("--before"));
            if (!value || *value > 65536) fail("usage", "invalid --before", 2);
            options.before = *value;
        } else if (argument == "--after") {
            const auto value = parse_number<std::size_t>(next("--after"));
            if (!value || *value > 65536) fail("usage", "invalid --after", 2);
            options.after = *value;
        } else if (argument == "--max-bytes") {
            const auto value = parse_number<std::size_t>(next("--max-bytes"));
            if (!value || *value > (4ull * 1024 * 1024 * 1024)) fail("usage", "invalid --max-bytes", 2);
            options.max_bytes = *value;
        } else if (argument == "--timeout-ms") {
            const auto value = parse_number<long>(next("--timeout-ms"));
            if (!value || *value < 0 || *value > 60000) fail("usage", "invalid --timeout-ms", 2);
            options.timeout_ms = *value;
        } else if (argument == "--hint") {
            const std::string_view text = next("--hint");
            const auto colon = text.find(':');
            const auto dash = text.find('-', colon == std::string_view::npos ? 0 : colon + 1);
            if (colon == std::string_view::npos || dash == std::string_view::npos)
                fail("usage", "invalid --hint", 2);
            const auto pid = parse_number<long>(text.substr(0, colon));
            const auto start = parse_hex(text.substr(colon + 1, dash - colon - 1));
            const auto end = parse_hex(text.substr(dash + 1));
            if (!pid || !start || !end || *end <= *start) fail("usage", "invalid --hint", 2);
            options.hints.push_back(Hint{static_cast<pid_t>(*pid), *start, *end});
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else {
            fail("usage", std::string("unknown argument: ") + std::string(argument), 2);
        }
    }

    if (options.pids.empty()) fail("usage", "--pid is required", 2);

    AnchorError anchor_error;
    const auto anchor = llavon::memscan::parse_anchor(options.anchor, anchor_error);
    if (!anchor) fail(anchor_error.code, anchor_error.detail, 2);

    ScanLimits limits;
    limits.max_bytes_per_pid = options.max_bytes;
    limits.timeout = std::chrono::milliseconds(options.timeout_ms);


    ScanError last_error{"not-found", "anchor not found"};
    for (const pid_t pid : options.pids) {
        if (!llavon::memscan::same_uid(pid)) {
            last_error = {"foreign-pid", "process " + std::to_string(pid) +
                                              " is not owned by this user"};
            continue;
        }
        ScanError error;
        const auto match = llavon::memscan::scan_pid(pid, *anchor, options.before, options.after,
                                                     limits, options.hints, error);
        if (match) {
            print_match(*match);
            return 0;
        }
        last_error = error;
    }

    int status = 1;
    if (last_error.code == "foreign-pid" || last_error.code == "denied") status = 3;
    else if (last_error.code == "timeout" || last_error.code == "budget") status = 4;
    fail(last_error.code, last_error.detail, status);
    return status;
}
