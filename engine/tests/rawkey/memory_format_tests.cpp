// Raw-key validation of every client memory format the probe supports.
//
// Each case spawns a synthetic client that keeps its composition in one
// format (plain UTF-8, UTF-16, kitty's 12-byte cells, Konsole's 16-byte
// cells, VTE's 20-byte cells), drives the engine with raw keys, and requires
// the adopted context to be the document prefix typed before the composition.
//
// The suite needs the real helper binary and ptrace permission for it, so it
// runs when LLAVON_IME_TEST_MEMSCAN points at a built (and, on hosts with
// yama ptrace_scope=1, cap_sys_ptrace-enabled) llavon-ime-memscan.

#include "raw_key_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace llavon::ime::rawkey {
namespace {

void allow_memory_probe() {
#if defined(__linux__)
    // The resident helper is a sibling of this synthetic client under Yama.
    // Permit it to read the child's memory without requiring a privileged
    // helper binary for the raw-key suite.
    (void)::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
#endif
}

struct FormatClient {
    pid_t pid = -1;
    int commands = -1;
    int responses = -1;
};

std::vector<char32_t> codepoints(std::string_view input) {
    std::vector<char32_t> output;
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first < 0x80) {
            output.push_back(static_cast<char32_t>(first));
            ++index;
        } else if ((first & 0xE0) == 0xC0) {
            output.push_back(static_cast<char32_t>(
                ((first & 0x1F) << 6) | (static_cast<unsigned char>(input[index + 1]) & 0x3F)));
            index += 2;
        } else {
            output.push_back(static_cast<char32_t>(
                ((first & 0x0F) << 12) |
                ((static_cast<unsigned char>(input[index + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(input[index + 2]) & 0x3F)));
            index += 3;
        }
    }
    return output;
}

std::string encode_utf16(std::string_view input) {
    std::string output;
    for (const char32_t codepoint : codepoints(input)) {
        output.push_back(static_cast<char>(codepoint & 0xff));
        output.push_back(static_cast<char>((codepoint >> 8) & 0xff));
    }
    return output;
}

std::string encode_utf32(std::string_view input) {
    std::string output;
    for (const char32_t codepoint : codepoints(input)) {
        for (int shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<char>((codepoint >> shift) & 0xff));
        }
    }
    return output;
}

// How a format stores a wide character's continuation cell.
enum class Continuation { KittyFlag, Spacer, Blank, VteFragment };

std::size_t format_cell(std::string_view format) {
    if (format == "cell8") return 8;
    if (format == "cell12" || format == "foot") return 12;
    if (format == "cell16") return 16;
    if (format == "cell20") return 20;
    return 24;
}

Continuation format_continuation(std::string_view format) {
    if (format == "cell12") return Continuation::KittyFlag;
    if (format == "cell16" || format == "cell24") return Continuation::Blank;
    if (format == "cell20") return Continuation::VteFragment;
    return Continuation::Spacer;
}

std::string cell_bytes(std::string_view format, char32_t codepoint, bool continuation) {
    const std::size_t cell = format_cell(format);
    const Continuation style = format_continuation(format);
    const bool fragment = continuation && style == Continuation::VteFragment;
    std::string output;
    if (continuation && style == Continuation::Spacer) {
        // foot and the 8-byte grids use a code point above Unicode as spacer.
        const char32_t spacer = 0x00200000u;
        for (int shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<char>((spacer >> shift) & 0xff));
        }
    } else if (continuation && style == Continuation::Blank) {
        output.push_back(0x20);
        output.append(3, '\0');
    } else {
        for (int shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<char>((codepoint >> shift) & 0xff));
        }
    }
    const unsigned char attributes[20] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01,
                                          0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00};
    for (std::size_t index = 4; index < cell; ++index) {
        unsigned char value = attributes[index - 4];
        // Installed Foot's attrs.clean bit must not be interpreted as Kitty's
        // continuation flag. This remains a synthetic persistent-grid fixture,
        // not Foot's temporary renderer overlay or native field authority.
        if (format == "foot" && index == 8) value = 0x01;
        if (continuation && style == Continuation::KittyFlag && index == 8) value = 0x01;
        // VTE repeats the code point in the fragment cell and flags it in the
        // low attribute byte.
        if (fragment && index == 4) value = 0x11;
        output.push_back(static_cast<char>(value));
    }
    return output;
}

std::string encode_composition(std::string_view format, std::string_view text) {
    if (format == "utf8") return std::string(text);
    if (format == "utf16") return encode_utf16(text);
    if (format == "utf32") return encode_utf32(text);
    std::string output;
    for (const char32_t codepoint : codepoints(text)) {
        output += cell_bytes(format, codepoint, false);
        output += cell_bytes(format, codepoint, true);
    }
    return output;
}

// Document text after the caret: ASCII (single cells) in the cell formats,
// plain code units otherwise. It moves with the composition, exactly like an
// editor that inserts the preedit into its text buffer.
std::string encode_suffix(std::string_view format, std::string_view text) {
    if (format == "utf8") return std::string(text);
    if (format == "utf16") return encode_utf16(text);
    if (format == "utf32") return encode_utf32(text);
    std::string output;
    for (const char32_t codepoint : codepoints(text)) {
        output += cell_bytes(format, codepoint, false);
        if (codepoint > 0x2000) output += cell_bytes(format, codepoint, true);
    }
    return output;
}

std::string format_prefix(std::string_view format, std::string_view text = "document prefix ") {
    // A leading empty cell / NUL separates the document text from whatever
    // precedes the buffer in the client's heap, exactly like an empty screen
    // cell does in a real terminal row.
    if (format == "utf8") return std::string("\0", 1) + std::string(text);
    if (format == "utf16") return encode_utf16(std::string("\0", 1)) + encode_utf16(text);
    if (format == "utf32") return encode_utf32(std::string("\0", 1)) + encode_utf32(text);
    std::string output(format_cell(format), '\0');
    for (const char value : text) {
        output += cell_bytes(format, static_cast<unsigned char>(value), false);
    }
    return output;
}

// A frontend is a separate executable, not a forked copy of the IME's heap.
// Exec each fixture so earlier suites' document/preedit strings cannot occupy
// the bounded candidate slots and hide the actual client buffer.
FormatClient spawn_memory_client(const nlohmann::json& specification) {
    int commands[2], responses[2];
    if (::pipe(commands) != 0) return {};
    if (::pipe(responses) != 0) {
        ::close(commands[0]);
        ::close(commands[1]);
        return {};
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(commands[0]);
        ::close(commands[1]);
        ::close(responses[0]);
        ::close(responses[1]);
        return {};
    }
    if (pid == 0) {
        ::close(commands[1]);
        ::close(responses[0]);
        const std::string serialized = specification.dump();
#if defined(__linux__)
        const std::string command_fd = std::to_string(commands[0]);
        const std::string response_fd = std::to_string(responses[1]);
        ::execl("/proc/self/exe", "llavon_ime_rawkey_tests", "--memory-client",
                serialized.c_str(), command_fd.c_str(), response_fd.c_str(), nullptr);
        ::_exit(127);
#else
        ::_exit(memory_client_main(serialized, commands[0], responses[1]));
#endif
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

int run_format_client(std::string_view format, std::string_view suffix, bool preedit_prefix,
                      std::string_view document,
                      int commands, int responses) {
    allow_memory_probe();
    const std::string suffix_bytes = encode_suffix(format, suffix);
    std::string buffer = format_prefix(format, document);
    const std::size_t composition_start = buffer.size();
    constexpr std::size_t kCompositionBytes = 1024;
    buffer.append(kCompositionBytes + suffix_bytes.size(), '\0');
    const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (::write(responses, &address, sizeof(address)) != sizeof(address)) return 1;
    char step = 0;
    while (::read(commands, &step, 1) == 1) {
        std::string composition;
        if (step == '1') composition = encode_composition(format, "ㄋ");
        if (step == '2') composition = encode_composition(format, "ㄋㄧ");
        if (step == '3') composition = encode_composition(format, "你");
        if (step == '4') composition = encode_composition(format, "ㄋㄧㄨ");
        if (step == '5') composition = encode_composition(format, "ㄋㄧㄣ");
        if (step == '7') composition = encode_composition(format, "ㄋㄧㄥ");
        if (preedit_prefix && (step == '4' || step == '5' || step == '6')) {
            const std::string last = step == '4' ? "ㄋ" : step == '5' ? "ㄋㄧ" : "你";
            composition = encode_composition(format, "你") + encode_composition(format, last);
        }
        std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(composition_start),
                    static_cast<std::ptrdiff_t>(kCompositionBytes + suffix_bytes.size()), '\0');
        std::copy(composition.begin(), composition.end(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(composition_start));
        std::copy(suffix_bytes.begin(), suffix_bytes.end(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(composition_start + composition.size()));
        if (::write(responses, &step, 1) != 1) return 1;
    }
    return 0;
}

FormatClient spawn_client(std::string_view format, std::string_view suffix = {},
                            bool preedit_prefix = false,
                            std::string_view document = "document prefix ") {
    return spawn_memory_client({{"kind", "format"}, {"format", format}, {"suffix", suffix},
                                {"preedit_prefix", preedit_prefix}, {"document", document}});
}

// A client that never draws the composition into its document (Konsole, VTE):
// the document holds the prefix and, once the engine commits it, the committed
// text. The composition itself only lives in the client's preedit member,
// which is outside the scanned document, so only the committed text can
// locate the caret.
int run_committed_client(std::string_view format, int commands, int responses) {
    allow_memory_probe();
    std::string prefix = "anchor document prefix ";
    while (prefix.size() < 460) prefix += "anchor document prefix ";
    std::string buffer;
    if (format == "utf8") {
        buffer = std::string("\0", 1) + prefix;
    } else if (format == "utf16") {
        buffer = encode_utf16(std::string("\0", 1)) + encode_utf16(prefix);
    } else {
        buffer.assign(format_cell(format), '\0');
        for (const char value : prefix) {
            buffer += cell_bytes(format, static_cast<unsigned char>(value), false);
        }
    }
    const std::size_t committed_at = buffer.size();
    buffer.append(256, '\0');
    [[maybe_unused]] auto* held = new std::string(std::move(buffer));
    const auto address = reinterpret_cast<std::uintptr_t>(held->data());
    if (::write(responses, &address, sizeof(address)) != sizeof(address)) return 1;
    char step = 0;
    while (::read(commands, &step, 1) == 1) {
        if (step == '4') {
            const std::string committed = encode_composition(format, "你");
            std::fill(held->begin() + static_cast<std::ptrdiff_t>(committed_at), held->end(), '\0');
            std::copy(committed.begin(), committed.end(),
                      held->begin() + static_cast<std::ptrdiff_t>(committed_at));
        }
        if (::write(responses, &step, 1) != 1) return 1;
    }
    return 0;
}

FormatClient spawn_committed_client(std::string_view format) {
    int commands[2], responses[2];
    if (::pipe(commands) != 0) return {};
    if (::pipe(responses) != 0) {
        ::close(commands[0]);
        ::close(commands[1]);
        return {};
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(commands[1]);
        ::close(responses[0]);
#if defined(__linux__)
        // Forking keeps prior suites' context and debug logs in the client's
        // heap. An exec leaves only the document this client actually owns.
        const std::string name(format);
        const std::string command_fd = std::to_string(commands[0]);
        const std::string response_fd = std::to_string(responses[1]);
        ::execl("/proc/self/exe", "llavon_ime_rawkey_tests", "--committed-client",
                name.c_str(), command_fd.c_str(), response_fd.c_str(), nullptr);
        ::_exit(127);
#else
        ::_exit(run_committed_client(format, commands[0], responses[1]));
#endif
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

// A client whose buffer never changes: a static string that starts with the
// composition, like a path or message elsewhere in the process.
int run_static_client(std::string_view format, std::string_view text, int commands, int responses) {
    allow_memory_probe();
    const std::string buffer = format_prefix(format) + encode_suffix(format, text);
    const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (::write(responses, &address, sizeof(address)) != sizeof(address)) return 1;
    char step = 0;
    while (::read(commands, &step, 1) == 1) {
        if (::write(responses, &step, 1) != 1) return 1;
    }
    return 0;
}

FormatClient spawn_static_client(std::string_view format, std::string_view text) {
    return spawn_memory_client({{"kind", "static"}, {"format", format}, {"text", text}});
}

// A buffer that the scanner can read with the wrong width and get
// plausible-looking text from.
enum class Decoy { Repeated, Consecutive, GlyphStride };

// A client that keeps its composition in one format while another buffer
// holds a misread-prone byte pattern followed by the composition in UTF-16.
// Reading that buffer as UTF-16 decodes repeated ASCII pairs into one
// repeated CJK code point (or sequential bytes into sequential code points),
// exactly what a terminal's byte stream can look like.
int run_decoy_client(std::string_view format, Decoy decoy, int commands, int responses) {
    allow_memory_probe();
    std::string buffer = format_prefix(format);
    const std::size_t composition_start = buffer.size();
    constexpr std::size_t kCompositionBytes = 1024;
    buffer.append(kCompositionBytes, '\0');
    std::string decoy_buffer;
    for (int index = 0; index < 60; ++index) {
        const char32_t codepoint = decoy == Decoy::Repeated ? 0x557Cu :
            static_cast<char32_t>(0x4E00 + index * (decoy == Decoy::GlyphStride ? 8 : 1));
        decoy_buffer.push_back(static_cast<char>(codepoint & 0xff));
        decoy_buffer.push_back(static_cast<char>((codepoint >> 8) & 0xff));
    }
    const std::size_t decoy_composition = decoy_buffer.size();
    decoy_buffer.append(64, '\0');
    [[maybe_unused]] auto* held = new std::string(std::move(decoy_buffer));
    const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (::write(responses, &address, sizeof(address)) != sizeof(address)) return 1;
    char step = 0;
    while (::read(commands, &step, 1) == 1) {
        std::string composition_text;
        if (step == '1') composition_text = "ㄋ";
        if (step == '2') composition_text = "ㄋㄧ";
        if (step == '3') composition_text = "你";
        std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(composition_start),
                    static_cast<std::ptrdiff_t>(kCompositionBytes), '\0');
        const std::string composition = encode_composition(format, composition_text);
        std::copy(composition.begin(), composition.end(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(composition_start));
        const std::string utf16 = encode_utf16(composition_text);
        std::fill(held->begin() + static_cast<std::ptrdiff_t>(decoy_composition), held->end(), '\0');
        std::copy(utf16.begin(), utf16.end(),
                  held->begin() + static_cast<std::ptrdiff_t>(decoy_composition));
        if (::write(responses, &step, 1) != 1) return 1;
    }
    return 0;
}

FormatClient spawn_decoy_client(std::string_view format, Decoy decoy) {
    return spawn_memory_client({{"kind", "decoy"}, {"format", format},
                                {"decoy", static_cast<int>(decoy)}});
}

// A client with a terminal grid row and a plain UTF-32 copy of the same row:
// reading the plain copy with a cell stride drops every other character, and
// that misread must not win over the real grid row.
int run_plain_copy_client(int commands, int responses) {
    allow_memory_probe();
    const std::string row = "watch 程式 ";
    std::string buffer(12, '\0');
    for (const char32_t codepoint : codepoints(row)) {
        buffer += cell_bytes("cell12", codepoint, false);
        if (codepoint > 0x2000) buffer += cell_bytes("cell12", codepoint, true);
    }
    const std::size_t composition_start = buffer.size();
    constexpr std::size_t kCompositionBytes = 1024;
    buffer.append(kCompositionBytes, '\0');
    // The plain copy holds the same row text: read with a cell stride it
    // drops every other character, and that misread must not be used.
    std::string plain;
    for (const char32_t codepoint : codepoints(row)) {
        for (int shift = 0; shift < 32; shift += 8) {
            plain.push_back(static_cast<char>((codepoint >> shift) & 0xff));
        }
    }
    const std::size_t plain_composition = plain.size();
    plain.append(1024, '\0');
    [[maybe_unused]] auto* held = new std::string(std::move(plain));
    const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (::write(responses, &address, sizeof(address)) != sizeof(address)) return 1;
    char step = 0;
    while (::read(commands, &step, 1) == 1) {
        std::string composition_text;
        if (step == '1') composition_text = "ㄋ";
        if (step == '2') composition_text = "ㄋㄧ";
        if (step == '3') composition_text = "你";
        std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(composition_start),
                    static_cast<std::ptrdiff_t>(kCompositionBytes), '\0');
        const std::string composition = encode_composition("cell12", composition_text);
        std::copy(composition.begin(), composition.end(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(composition_start));
        std::fill(held->begin() + static_cast<std::ptrdiff_t>(plain_composition), held->end(), '\0');
        std::size_t offset = plain_composition;
        for (const char32_t codepoint : codepoints(composition_text)) {
            for (int shift = 0; shift < 32; shift += 8) {
                (*held)[offset++] = static_cast<char>((codepoint >> shift) & 0xff);
            }
        }
        if (::write(responses, &step, 1) != 1) return 1;
    }
    return 0;
}

FormatClient spawn_plain_copy_client() {
    return spawn_memory_client({{"kind", "plain-copy"}, {"format", "cell12"}});
}

bool advance(FormatClient& client, char step) {
    char ack = 0;
    return ::write(client.commands, &step, 1) == 1 &&
           ::read(client.responses, &ack, 1) == 1 && ack == step;
}

bool wait_for_probe(Harness& harness, std::size_t completed,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5),
                    bool require_context = false) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Unverified probes do not necessarily post work to the frontend. A single
    // long pump_until would sleep to its timeout even after the probe finished,
    // falsely reporting the timeout as scan latency. Poll at 1ms while still
    // running all normal host callbacks.
    do {
        if (harness.pump_until([&] {
                return harness.memory_probe_count() > completed &&
                       (!require_context || !harness.context_text().empty());
            },
                               std::chrono::milliseconds(1))) return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

}  // namespace

int committed_client_main(std::string_view format, int commands, int responses) {
    return run_committed_client(format, commands, responses);
}

int memory_client_main(std::string_view specification, int commands, int responses) {
    const auto spec = nlohmann::json::parse(specification);
    const auto format = spec.at("format").get<std::string>();
    if (spec.at("kind") == "plain-copy") return run_plain_copy_client(commands, responses);
    if (spec.at("kind") == "static") {
        return run_static_client(format, spec.at("text").get<std::string>(), commands, responses);
    }
    if (spec.at("kind") == "decoy") {
        return run_decoy_client(format, static_cast<Decoy>(spec.at("decoy").get<int>()),
                                commands, responses);
    }
    return run_format_client(format, spec.at("suffix").get<std::string>(),
                             spec.at("preedit_prefix").get<bool>(), spec.at("document").get<std::string>(),
                             commands, responses);
}

// A client that never draws the composition into its document (Konsole, VTE)
// is located through the text the engine committed: that text is in the
// document, and the caret sits right behind it while the next composition is
// typed. The composition itself is not in the document at all.
RAWKEY_SUITE("the committed text locates the caret without client preedit", memory_commit_anchor) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory commit anchor: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    for (const std::string_view format :
         {"utf8", "cell8", "cell12", "foot", "cell16", "cell20", "cell24"}) {
        FormatClient client = spawn_committed_client(format);
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        if (format == "cell12") harness.host().set_program("kitty");
        if (format == "foot") harness.host().set_program("foot");
        if (format == "cell16") harness.host().set_program("konsole");
        if (format == "cell20") harness.host().set_program("vte");
        if (format == "cell24") harness.host().set_program("alacritty");
        harness.activate();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        // First composition: the engine commits 你 and the client writes it
        // into its document.
        harness.type("su3");
        harness.expect_commit("你");
        RAWKEY_ASSERT(advance(client, '4'));

        // Second composition: nothing but the committed text is in the
        // document, so only the committed-text anchor can locate the caret.
        harness.key("c");
        RAWKEY_ASSERT(harness.preedit() == "ㄏ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("l");
        RAWKEY_ASSERT(harness.preedit() == "ㄏㄠ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("3");
        RAWKEY_ASSERT(harness.preedit() == "好");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const bool adopted = harness.pump_until([&] {
            return harness.context_text().ends_with("anchor document prefix 你");
        }, std::chrono::milliseconds(2500));
        std::printf("[%s] commit-anchor format=%.*s adopted=%d context=%s\n",
                    adopted ? "ok  " : "FAIL", static_cast<int>(format.size()), format.data(),
                    adopted ? 1 : 0, harness.context_text().c_str());
        RAWKEY_ASSERT(adopted);

        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
}

// The scanned document ends in a real commit that happens to be identical to
// the next rendered preedit. It is still document context: the memory provider
// already excludes the preedit at the match boundary.
RAWKEY_SUITE("memory context retains a commit matching the next preedit", memory_same_commit) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') return;
    for (const std::string_view format : {"utf8", "cell16", "cell20"}) {
        FormatClient client = spawn_committed_client(format);
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        if (format == "cell16") harness.host().set_program("konsole");
        if (format == "cell20") harness.host().set_program("vte");
        harness.activate();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        harness.type("su3");
        harness.expect_commit("你");
        RAWKEY_ASSERT(advance(client, '4'));

        harness.key("s");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("u");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        const bool adopted = harness.pump_until([&] {
            return harness.context_text().ends_with("anchor document prefix 你");
        }, std::chrono::milliseconds(2500));
        std::printf("[%s] repeated-commit format=%.*s context=%s\n", adopted ? "ok  " : "FAIL",
                    static_cast<int>(format.size()), format.data(), harness.context_text().c_str());
        RAWKEY_ASSERT(adopted);

        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
}

RAWKEY_SUITE("memory formats validate through raw keys", memory_formats) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory formats: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    for (const std::string_view format :
         {"utf8", "utf16", "utf32", "cell8", "cell12", "foot", "cell16", "cell20", "cell24"}) {
        FormatClient client = spawn_client(format);
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        if (format == "cell12") harness.host().set_program("kitty");
        if (format == "foot") harness.host().set_program("foot");
        if (format == "cell16") harness.host().set_program("konsole");
        if (format == "cell20") harness.host().set_program("vte");
        if (format == "cell24") harness.host().set_program("alacritty");
        harness.activate();
        // Let the activation prime (soft-dirty reset) reach the helper before
        // the client starts writing; the prime is asynchronous.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        // The client renders each state before the key that shows it, so the
        // probe always sees the composition it is looking for.
        RAWKEY_ASSERT(advance(client, '1'));
        harness.key("s");
        RAWKEY_ASSERT(harness.preedit() == "ㄋ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        RAWKEY_ASSERT(advance(client, '2'));
        harness.key("u");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        RAWKEY_ASSERT(advance(client, '3'));
        harness.key("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // The leading cleared cell/NUL is an explicit document boundary.
        // No unrelated heap text or live composition may cross it.
        const bool adopted = harness.pump_until([&] {
            const std::string text = harness.context_text();
            return text == "document prefix ";
        });
        std::printf("[%s] format=%.*s adopted=%d context=%s\n", adopted ? "ok  " : "FAIL",
                    static_cast<int>(format.size()), format.data(), adopted ? 1 : 0,
                    harness.context_text().c_str());
        RAWKEY_ASSERT(adopted);

        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
}

RAWKEY_SUITE("memory context stays in the focused kitty process", memory_focused_process) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') return;
    FormatClient focused = spawn_client("cell12", {}, false, "focused kitty document ");
    FormatClient other = spawn_client("cell12", {}, false, "another kitty window's unrelated text ");
    RAWKEY_ASSERT(focused.pid > 0 && other.pid > 0);
    HarnessOptions options;
    options.memory_helper_path = helper;
    Harness harness(options);
    // Both same-program windows render the same natural preedit. X11 focus is
    // stronger evidence, but both processes must remain available to scan.
    harness.host().set_probe_pids({static_cast<int>(other.pid), static_cast<int>(focused.pid)});
    harness.host().set_focused_probe_pid(static_cast<int>(focused.pid));
    harness.host().set_program("kitty");
    harness.activate();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (const auto [step, key] : std::vector<std::pair<char, char>>{
             {'1', 's'}, {'2', 'u'}, {'3', '3'}}) {
        RAWKEY_ASSERT(advance(focused, step));
        RAWKEY_ASSERT(advance(other, step));
        harness.key(std::string(1, key));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const bool adopted = harness.pump_until([&] {
        return harness.context_text().ends_with("focused kitty document ");
    });
    RAWKEY_ASSERT(adopted);
    RAWKEY_ASSERT(harness.context_text().find("another kitty window") == std::string::npos);
    for (const auto& client : {focused, other}) {
        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
}

RAWKEY_SUITE("memory probe abandons a vanished caret and searches the next process", memory_relocate) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') return;
    FormatClient old = spawn_client("cell12", {}, true, "old kitty document ");
    FormatClient next = spawn_client("cell12", {}, true, "new kitty document ");
    RAWKEY_ASSERT(old.pid > 0 && next.pid > 0);
    HarnessOptions options;
    options.memory_helper_path = helper;
    Harness harness(options);
    harness.host().set_program("kitty");
    harness.host().set_probe_pids({static_cast<int>(old.pid)});
    harness.activate();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (const auto [step, key] : std::vector<std::pair<char, char>>{
             {'1', 's'}, {'2', 'u'}, {'3', '3'}}) {
        RAWKEY_ASSERT(advance(old, step));
        RAWKEY_ASSERT(advance(next, step));
        harness.key(std::string(1, key));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    RAWKEY_ASSERT(harness.pump_until([&] {
        return harness.context_text().ends_with("old kitty document ");
    }));
    ::kill(old.pid, SIGKILL);
    int status = 0;
    ::waitpid(old.pid, &status, 0);
    ::close(old.commands);
    ::close(old.responses);
    harness.host().set_probe_pids({static_cast<int>(old.pid), static_cast<int>(next.pid)});
    for (const auto [step, key] : std::vector<std::pair<char, char>>{
             {'4', 's'}, {'5', 'u'}, {'6', '3'}}) {
        RAWKEY_ASSERT(advance(next, step));
        harness.key(std::string(1, key));
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    RAWKEY_ASSERT(harness.pump_until([&] {
        return harness.context_text().ends_with("new kitty document ");
    }, std::chrono::milliseconds(5000)));
    ::close(next.commands);
    ::close(next.responses);
    ::kill(next.pid, SIGKILL);
    ::waitpid(next.pid, &status, 0);
}

RAWKEY_SUITE("memory probe does not adopt its own diagnostic output", memory_diagnostic_copy) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') return;
    FormatClient copy = spawn_client("cell12", {}, false,
                                     "[CTX] source=memory-probe units=11 text=\"kitty\" ");
    RAWKEY_ASSERT(copy.pid > 0);
    HarnessOptions options;
    options.memory_helper_path = helper;
    Harness harness(options);
    harness.host().set_probe_pids({static_cast<int>(copy.pid)});
    harness.host().set_program("kitty");
    harness.activate();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (const auto [step, key] : std::vector<std::pair<char, char>>{
             {'1', 's'}, {'2', 'u'}, {'3', '3'}}) {
        RAWKEY_ASSERT(advance(copy, step));
        const auto completed = harness.memory_probe_count();
        harness.key(std::string(1, key));
        RAWKEY_ASSERT(harness.pump_until([&] { return harness.memory_probe_count() > completed; }));
    }
    RAWKEY_ASSERT(harness.context_text().empty());
    ::close(copy.commands);
    ::close(copy.responses);
    ::kill(copy.pid, SIGKILL);
    int status = 0;
    ::waitpid(copy.pid, &status, 0);
}


// A client can put the *whole* uncommitted composition in its document buffer.
// The memory anchor is the last segment, so the first segment appears in the
// scanner's "before" window but belongs in model padding, not context.
RAWKEY_SUITE("earlier preedit segments stay out of memory context", memory_preedit_prefix) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory preedit prefix: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    for (const std::string_view format : {"utf8", "cell12", "cell20"}) {
        FormatClient client = spawn_client(format, {}, true);
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        if (format == "cell12") harness.host().set_program("kitty");
        if (format == "cell20") harness.host().set_program("vte");
        harness.activate();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        for (const auto key : {"s", "u"}) {
            const auto completed = harness.memory_probe_count();
            harness.key(key);
            RAWKEY_ASSERT(wait_for_probe(harness, completed));
        }
        harness.key("3");
        RAWKEY_ASSERT(harness.preedit() == "你");
        auto completed = harness.memory_probe_count();
        RAWKEY_ASSERT(advance(client, '4'));
        harness.key("s");
        RAWKEY_ASSERT(harness.preedit() == "你ㄋ");
        RAWKEY_ASSERT(wait_for_probe(harness, completed));
        completed = harness.memory_probe_count();
        RAWKEY_ASSERT(advance(client, '5'));
        harness.key("u");
        RAWKEY_ASSERT(harness.preedit() == "你ㄋㄧ");
        RAWKEY_ASSERT(wait_for_probe(harness, completed));
        RAWKEY_ASSERT(advance(client, '6'));
        harness.key("3");
        RAWKEY_ASSERT(harness.preedit() == "你你");

        const bool adopted = harness.pump_until([&] {
            return harness.context_text() == "document prefix ";
        });
        std::printf("[%s] preedit-prefix format=%.*s context=%s\n",
                    adopted ? "ok  " : "FAIL", static_cast<int>(format.size()), format.data(),
                    harness.context_text().c_str());
        RAWKEY_ASSERT(adopted);
        RAWKEY_ASSERT(harness.context_text().find("你") == std::string::npos);

        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
}

// The same growing composition also matches a static string that begins with
// it. Its tail shrinks as the composition grows, so the provider must not
// adopt it; a real caret's following text stays the same.
RAWKEY_SUITE("a static string prefix is not adopted as context", memory_static_prefix) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory static prefix: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    bool all = true;
    for (const std::string_view format : {"utf8", "utf16", "utf32", "cell8", "cell12", "foot", "cell16", "cell20", "cell24"}) {
        FormatClient client = spawn_static_client(format, "ㄋㄧㄣ囉");
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        harness.activate();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        for (const auto key : {"s", "u", "p"}) {
            const auto completed = harness.memory_probe_count();
            harness.key(key);
            RAWKEY_ASSERT(wait_for_probe(harness, completed));
            // Reject *any* published text, including garbage or a different
            // static copy, rather than only the expected document suffix.
            all = all && harness.context_text().empty();
        }
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧㄣ");
        const bool adopted = !harness.context_text().empty();
        std::printf("[%s] static format=%.*s adopted=%d context=%s\n",
                    adopted ? "FAIL" : "ok  ", static_cast<int>(format.size()), format.data(),
                    adopted ? 1 : 0, harness.context_text().c_str());
        all = all && !adopted;

        ::close(client.commands);
        ::close(client.responses);
        ::kill(client.pid, SIGKILL);
        int status = 0;
        ::waitpid(client.pid, &status, 0);
    }
    RAWKEY_ASSERT(all);
}

// The user-visible failure this guards against: a terminal row is displayed in
// the grid, but another buffer in the same process decodes to a long run of
// repeated (or sequential) CJK code points that scores higher. Only the real
// row may be adopted.
RAWKEY_SUITE("misread byte streams do not shadow the terminal row", memory_decoy) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory decoy: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    bool all = true;
    for (const std::string_view format : {"utf8", "cell12"}) {
        for (const Decoy decoy : {Decoy::Repeated, Decoy::Consecutive, Decoy::GlyphStride}) {
            const std::string_view decoy_name =
                decoy == Decoy::Repeated ? "repeated" :
                decoy == Decoy::GlyphStride ? "glyph-stride" : "consecutive";
            FormatClient client = spawn_decoy_client(format, decoy);
            RAWKEY_ASSERT(client.pid > 0);
            HarnessOptions options;
            options.memory_helper_path = helper;
            Harness harness(options);
            harness.host().set_probe_pids({static_cast<int>(client.pid)});
            harness.activate();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));

            RAWKEY_ASSERT(advance(client, '1'));
            harness.key("s");
            RAWKEY_ASSERT(harness.preedit() == "ㄋ");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            RAWKEY_ASSERT(advance(client, '2'));
            harness.key("u");
            RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            RAWKEY_ASSERT(advance(client, '3'));
            harness.key("3");
            RAWKEY_ASSERT(harness.preedit() == "你");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            const bool adopted = harness.pump_until([&] {
                const std::string text = harness.context_text();
                return text == "document prefix ";
            });
            std::printf("[%s] decoy format=%.*s kind=%.*s adopted=%d context=%s\n",
                        adopted ? "ok  " : "FAIL", static_cast<int>(format.size()), format.data(),
                        static_cast<int>(decoy_name.size()), decoy_name.data(), adopted ? 1 : 0,
                        harness.context_text().c_str());
            all = all && adopted;

            ::close(client.commands);
            ::close(client.responses);
            ::kill(client.pid, SIGKILL);
            int status = 0;
            ::waitpid(client.pid, &status, 0);
        }
    }
    RAWKEY_ASSERT(all);
}

// The user-visible failure this guards against: the same row text also exists
// as a plain buffer in the process. Reading that buffer with a cell stride
// drops every other character, and that misread must not shadow the grid.
RAWKEY_SUITE("a misread plain copy does not shadow the terminal row", memory_plain_copy) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory plain copy: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    FormatClient client = spawn_plain_copy_client();
    RAWKEY_ASSERT(client.pid > 0);
    HarnessOptions options;
    options.memory_helper_path = helper;
    Harness harness(options);
    harness.host().set_probe_pids({static_cast<int>(client.pid)});
    harness.activate();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    RAWKEY_ASSERT(advance(client, '1'));
    harness.key("s");
    RAWKEY_ASSERT(harness.preedit() == "ㄋ");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    RAWKEY_ASSERT(advance(client, '2'));
    harness.key("u");
    RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    RAWKEY_ASSERT(advance(client, '3'));
    harness.key("3");
    RAWKEY_ASSERT(harness.preedit() == "你");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const bool adopted = harness.pump_until([&] {
        const std::string text = harness.context_text();
        return text == "watch 程式 ";
    });
    std::printf("[%s] plain copy adopted=%d context=%s\n", adopted ? "ok  " : "FAIL",
                adopted ? 1 : 0, harness.context_text().c_str());
    RAWKEY_ASSERT(adopted);

    ::close(client.commands);
    ::close(client.responses);
    ::kill(client.pid, SIGKILL);
    int status = 0;
    ::waitpid(client.pid, &status, 0);
}


}  // namespace llavon::ime::rawkey
