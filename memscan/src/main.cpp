#include "anchor.hpp"
#include "scan.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
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
    for (const char character : input) {
        const auto value = static_cast<unsigned char>(character);
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

// The screen layout a known client uses, if the name matches one. The plain
// byte encodings are always kept: toolkit buffers and document text are not
// grids.
std::vector<llavon::memscan::Encoding> preferred_encodings(std::string_view program) {
    std::string lowered;
    lowered.reserve(program.size());
    for (const char value : program) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }
    const auto has = [&](std::string_view needle) {
        return lowered.find(needle) != std::string::npos;
    };
    using llavon::memscan::Encoding;
    const std::vector<Encoding> plain{Encoding::Utf8, Encoding::Utf16Le, Encoding::Utf32Le};
    const auto with = [&](Encoding cell) {
        std::vector<Encoding> list = plain;
        list.push_back(cell);
        return list;
    };
    if (has("foot")) return with(Encoding::FootCell12Le);
    if (has("kitty")) return with(Encoding::Utf32Cell12Le);
    if (has("konsole")) return with(Encoding::Utf32Cell16Le);
    if (has("alacritty")) return with(Encoding::Utf32Cell24Le);
    if (has("gnome-terminal") || has("vte") || has("xfce4-terminal") ||
        has("mate-terminal") || has("tilix") || has("terminator")) {
        return with(Encoding::Utf32Cell20Le);
    }
    return {};
}

// A private, newline-delimited protocol for the IME-owned child. The normal
// one-shot CLI above stays compatible with existing callers.
void serve() {
    std::unordered_map<int, std::uintptr_t> next_address;
    // A PID whose soft-dirty bits were reset once can be scanned by only
    // reading the pages written since, instead of its whole address space.
    std::unordered_map<int, bool> changed_only;
    std::uint64_t epoch = 0;
    std::size_t next_pid_index = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        nlohmann::json response;
        try {
            if (line.size() > 65536) throw std::runtime_error("request too long");
            const auto request = nlohmann::json::parse(line);
            const auto pids = request.at("pids").get<std::vector<int>>();
            if (pids.empty() || pids.size() > 16) throw std::runtime_error("invalid pids");
            // Prime can run after the client already wrote its first preedit.
            // Resetting dirty bits is not an input-order receipt: the first
            // anchored request must still inspect existing writable memory.
            if (request.value("prime", false)) {
                nlohmann::json primed = nlohmann::json::array();
                for (const int pid : pids) {
                    const bool ready = llavon::memscan::reset_soft_dirty(pid);
                    changed_only[pid] = false;
                    next_address[pid] = 0;
                    if (ready) primed.push_back(pid);
                }
#ifdef LLAVON_IME_DEBUG
                std::fprintf(stderr, "[prime] pids=%zu ready=%zu\n", pids.size(), primed.size());
#endif
                response = {{"matches", nlohmann::json::array()}, {"error", ""},
                            {"primed", std::move(primed)}};
                // The outer write is skipped by the continue.
                std::cout << response.dump() << '\n' << std::flush;
                continue;
            }
            // One request may carry several anchors: the composition on screen
            // and the text committed just before it. They are scanned over the
            // same dirty set, so a commit and the composition that follows can
            // both be found by the same request.
            std::vector<std::string> anchor_texts;
            if (request.contains("anchors") && request["anchors"].is_array()) {
                for (const auto& entry : request["anchors"]) {
                    if (entry.is_string()) anchor_texts.push_back(entry.get<std::string>());
                    if (anchor_texts.size() >= 4) break;
                }
            }
            if (anchor_texts.empty()) anchor_texts.push_back(request.at("anchor").get<std::string>());
            // A known client stores its screen with one cell width; trying
            // only that width avoids reading the same bytes with a wrong
            // stride, which produces plausible but wrong text.
            const auto program = request.value("program", std::string{});
            const auto preferred = preferred_encodings(program);
            std::vector<Anchor> anchors;
            std::vector<llavon::memscan::AnchorRequest> requests;
            // Pointers into `anchors` are kept, so reserve up front.
            anchors.reserve(anchor_texts.size() * 2);
            for (const auto& text : anchor_texts) {
                AnchorError anchor_error;
                auto parsed = llavon::memscan::parse_anchor(text, anchor_error);
                if (!parsed) continue;
                llavon::memscan::AnchorRequest entry;
                entry.full = &anchors.emplace_back(std::move(*parsed));
                if (!preferred.empty()) {
                    Anchor narrowed = *entry.full;
                    narrowed.patterns.clear();
                    narrowed.encodings.clear();
                    for (std::size_t index = 0; index < entry.full->encodings.size(); ++index) {
                        if (std::ranges::find(preferred, entry.full->encodings[index]) !=
                            preferred.end()) {
                            narrowed.encodings.push_back(entry.full->encodings[index]);
                            narrowed.patterns.push_back(entry.full->patterns[index]);
                        }
                    }
                    entry.narrowed = &anchors.emplace_back(std::move(narrowed));
                }
                requests.push_back(entry);
            }
            if (requests.empty()) throw std::runtime_error("anchor-invalid");
            // A commit may have written the text before the dirty baseline
            // was reset, so the first scan after it reads everything.
            const bool force_full = request.value("full", false);
            const auto request_epoch = request.value("epoch", std::uint64_t{0});
            if (request_epoch != epoch) {
                next_address.clear();
                next_pid_index = 0;
                epoch = request_epoch;
            }
            for (auto it = next_address.begin(); it != next_address.end();) {
                if (std::ranges::find(pids, it->first) == pids.end()) it = next_address.erase(it);
                else ++it;
            }
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
            // Effectively a full scan of the writable address space; the
            // per-request deadline still bounds the work.
            limits.max_bytes_per_pid = 8ull * 1024 * 1024 * 1024;
            limits.stop_at_strong_grid_candidate = !preferred.empty();
            nlohmann::json matches = nlohmann::json::array();
            nlohmann::json progress = nlohmann::json::array();
            std::string error_code = "not-found";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            const auto start_pid = next_pid_index % pids.size();
            next_pid_index = (start_pid + 1) % pids.size();
            for (std::size_t index = 0; index < pids.size(); ++index) {
                const int pid = pids[(start_pid + index) % pids.size()];
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0 || matches.size() >= 32) break;
                limits.timeout = std::min(remaining, std::chrono::milliseconds(1500));
                ScanError error;
                llavon::memscan::ScanStats stats;
                std::vector<Match> candidates;
                // Enough context for the model: 1024 bytes is 85 terminal
                // grid cells, or 1024/512/256 units in the byte encodings.
                if (force_full) next_address[pid] = 0;
                llavon::memscan::scan_pid_anchors(pid, requests, 1024, 16, limits, hints, error,
                                                  &candidates, &next_address[pid], &stats,
                                                  changed_only[pid] && !force_full);
                // The first request for a PID is a full scan; reset afterwards
                // so later requests only read what the client writes. A
                // changed-only scan resets internally, right after reading the
                // dirty set. Without permission to reset, fall back to
                // scanning everything each time.
                if (!changed_only[pid]) {
                    changed_only[pid] = llavon::memscan::reset_soft_dirty(pid);
                }
                progress.push_back({{"pid", pid}, {"next_address", next_address[pid]},
                                    {"hits", stats.hits}, {"qualified", stats.qualified}});
                std::ranges::stable_sort(candidates, [&](const Match& left, const Match& right) {
                    const bool left_hint = left.pid == hint_pid && left.address == hint_address &&
                                           left.before == hint_before && !hint_before.empty();
                    const bool right_hint = right.pid == hint_pid && right.address == hint_address &&
                                            right.before == hint_before && !hint_before.empty();
                    if (left_hint != right_hint) return left_hint;
                    if (left.confidence != right.confidence)
                        return left.confidence > right.confidence;
                    return left.before.size() > right.before.size();
                });
                std::size_t selected = 0;
                for (const auto& match : candidates) {
                    if (selected == 8 || matches.size() >= 32) break;
                    ++selected;
                    matches.push_back({{"pid", match.pid}, {"address", match.address},
                                       {"encoding", llavon::memscan::encoding_name(match.encoding)},
                                       {"confidence", match.confidence},
                                       {"before", match.before},
                                       {"after", match.after},
                                       {"anchor_index", match.anchor_index},
                                       {"continuation", match.continuation}});
                }
                if (!error.code.empty()) error_code = error.code;
            }
            if (!matches.empty()) error_code.clear();
            response = {{"matches", std::move(matches)}, {"error", error_code},
                        {"progress", std::move(progress)}};
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
