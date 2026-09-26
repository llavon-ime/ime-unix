// Raw-key validation of every client memory format the probe supports.
//
// Each case spawns a synthetic client that keeps its composition in one
// format (plain UTF-8, UTF-16, kitty's 12-byte cells, Konsole's 16-byte
// cells), drives the engine with raw keys, and requires the adopted context
// to be the document prefix typed before the composition.
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
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace llavon::ime::rawkey {
namespace {

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

// How a format stores a wide character's continuation cell.
enum class Continuation { KittyFlag, Spacer, Blank };

std::size_t format_cell(std::string_view format) {
    if (format == "cell8") return 8;
    if (format == "cell12" || format == "foot") return 12;
    if (format == "cell16") return 16;
    return 24;
}

Continuation format_continuation(std::string_view format) {
    if (format == "cell12") return Continuation::KittyFlag;
    if (format == "cell16" || format == "cell24") return Continuation::Blank;
    return Continuation::Spacer;
}

std::string cell_bytes(std::string_view format, char32_t codepoint, bool continuation) {
    const std::size_t cell = format_cell(format);
    std::string output;
    if (continuation && format_continuation(format) == Continuation::Spacer) {
        // foot and the 8-byte grids use a code point above Unicode as spacer.
        const char32_t spacer = 0x00200000u;
        for (int shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<char>((spacer >> shift) & 0xff));
        }
    } else if (continuation && format_continuation(format) == Continuation::Blank) {
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
        output.push_back(static_cast<char>(
            continuation && format_continuation(format) == Continuation::KittyFlag &&
                    index == 8
                ? 0x01
                : attributes[index - 4]));
    }
    return output;
}

std::string encode_composition(std::string_view format, std::string_view text) {
    if (format == "utf8") return std::string(text);
    if (format == "utf16") return encode_utf16(text);
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
    std::string output;
    for (const char32_t codepoint : codepoints(text)) {
        output += cell_bytes(format, codepoint, false);
        if (codepoint > 0x2000) output += cell_bytes(format, codepoint, true);
    }
    return output;
}

std::string format_prefix(std::string_view format) {
    // A leading empty cell / NUL separates the document text from whatever
    // precedes the buffer in the client's heap, exactly like an empty screen
    // cell does in a real terminal row.
    if (format == "utf8") return std::string("\0", 1) + "document prefix ";
    if (format == "utf16") return encode_utf16(std::string("\0", 1)) + encode_utf16("document prefix ");
    std::string output(format_cell(format), '\0');
    for (const char value : std::string_view("document prefix ")) {
        output += cell_bytes(format, static_cast<unsigned char>(value), false);
    }
    return output;
}

FormatClient spawn_client(std::string_view format, std::string_view suffix = {}) {
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
        const std::string suffix_bytes = encode_suffix(format, suffix);
        std::string buffer = format_prefix(format);
        const std::size_t composition_start = buffer.size();
        constexpr std::size_t kCompositionBytes = 1024;
        buffer.append(kCompositionBytes + suffix_bytes.size(), '\0');
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
            std::string composition;
            if (step == '1') composition = encode_composition(format, "ㄋ");
            if (step == '2') composition = encode_composition(format, "ㄋㄧ");
            if (step == '3') composition = encode_composition(format, "你");
            if (step == '4') composition = encode_composition(format, "ㄋㄧㄨ");
            if (step == '5') composition = encode_composition(format, "ㄋㄧㄣ");
            std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(composition_start),
                        static_cast<std::ptrdiff_t>(kCompositionBytes + suffix_bytes.size()), '\0');
            std::copy(composition.begin(), composition.end(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(composition_start));
            std::copy(suffix_bytes.begin(), suffix_bytes.end(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(composition_start + composition.size()));
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

// A client whose buffer never changes: a static string that starts with the
// composition, like a path or message elsewhere in the process.
FormatClient spawn_static_client(std::string_view format, std::string_view text) {
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
        std::string buffer = format_prefix(format) + encode_suffix(format, text);
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

// A buffer that the scanner can read with the wrong width and get
// plausible-looking text from.
enum class Decoy { Repeated, Consecutive };

// A client that keeps its composition in one format while another buffer
// holds a misread-prone byte pattern followed by the composition in UTF-16.
// Reading that buffer as UTF-16 decodes repeated ASCII pairs into one
// repeated CJK code point (or sequential bytes into sequential code points),
// exactly what a terminal's byte stream can look like.
FormatClient spawn_decoy_client(std::string_view format, Decoy decoy) {
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
        std::string buffer = format_prefix(format);
        const std::size_t composition_start = buffer.size();
        constexpr std::size_t kCompositionBytes = 1024;
        buffer.append(kCompositionBytes, '\0');
        std::string decoy_buffer;
        for (int index = 0; index < 60; ++index) {
            const char32_t codepoint =
                decoy == Decoy::Repeated ? 0x557Cu : static_cast<char32_t>(0x4E00 + index);
            decoy_buffer.push_back(static_cast<char>(codepoint & 0xff));
            decoy_buffer.push_back(static_cast<char>((codepoint >> 8) & 0xff));
        }
        const std::size_t decoy_composition = decoy_buffer.size();
        decoy_buffer.append(64, '\0');
        [[maybe_unused]] auto* held = new std::string(std::move(decoy_buffer));
        const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
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
            std::fill(held->begin() + static_cast<std::ptrdiff_t>(decoy_composition), held->end(),
                      '\0');
            std::copy(utf16.begin(), utf16.end(),
                      held->begin() + static_cast<std::ptrdiff_t>(decoy_composition));
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

// A client with a terminal grid row and a plain UTF-32 copy of the same row:
// reading the plain copy with a cell stride drops every other character, and
// that misread must not win over the real grid row.
FormatClient spawn_plain_copy_client() {
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
        (void)::write(responses[1], &address, sizeof(address));
        char step = 0;
        while (::read(commands[0], &step, 1) == 1) {
            std::string composition_text;
            if (step == '1') composition_text = "ㄋ";
            if (step == '2') composition_text = "ㄋㄧ";
            if (step == '3') composition_text = "你";
            std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(composition_start),
                        static_cast<std::ptrdiff_t>(kCompositionBytes), '\0');
            const std::string composition = encode_composition("cell12", composition_text);
            std::copy(composition.begin(), composition.end(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(composition_start));
            std::fill(held->begin() + static_cast<std::ptrdiff_t>(plain_composition), held->end(),
                      '\0');
            std::size_t offset = plain_composition;
            for (const char32_t codepoint : codepoints(composition_text)) {
                for (int shift = 0; shift < 32; shift += 8) {
                    (*held)[offset++] = static_cast<char>((codepoint >> shift) & 0xff);
                }
            }
            (void)::write(responses[1], &step, 1);
        }
        ::_exit(0);
    }
    ::close(commands[0]);
    ::close(responses[1]);
    std::uintptr_t address = 0;
    if (::read(responses[0], &address, sizeof(address)) != sizeof(address)) address = 0;
    return {pid, commands[1], responses[0]};
}

bool advance(FormatClient& client, char step) {
    char ack = 0;
    return ::write(client.commands, &step, 1) == 1 &&
           ::read(client.responses, &ack, 1) == 1 && ack == step;
}

}  // namespace

RAWKEY_SUITE("memory formats validate through raw keys", memory_formats) {
    const char* helper = std::getenv("LLAVON_IME_TEST_MEMSCAN");
    if (helper == nullptr || helper[0] == '\0') {
        std::printf("[skip] memory formats: set LLAVON_IME_TEST_MEMSCAN to the built helper\n");
        return;
    }
    for (const std::string_view format :
         {"utf8", "utf16", "cell8", "cell12", "foot", "cell16", "cell24"}) {
        FormatClient client = spawn_client(format);
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
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

        // Heap bytes in front of the synthetic buffer can stay in the context;
        // what matters is that the document prefix is adopted intact.
        const bool adopted = harness.pump_until([&] {
            const std::string text = harness.context_text();
            return text.size() >= 16 && text.ends_with("document prefix ");
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
    for (const std::string_view format : {"utf8", "cell8", "cell12", "foot", "cell16", "cell24"}) {
        FormatClient client = spawn_static_client(format, "ㄋㄧㄣ囉");
        RAWKEY_ASSERT(client.pid > 0);
        HarnessOptions options;
        options.memory_helper_path = helper;
        Harness harness(options);
        harness.host().set_probe_pids({static_cast<int>(client.pid)});
        harness.activate();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        harness.key("s");
        RAWKEY_ASSERT(harness.preedit() == "ㄋ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("u");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        harness.key("p");
        RAWKEY_ASSERT(harness.preedit() == "ㄋㄧㄣ");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const bool adopted = harness.pump_until([&] {
            return harness.context_text().ends_with("document prefix ");
        }, std::chrono::milliseconds(2500));
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
        for (const Decoy decoy : {Decoy::Repeated, Decoy::Consecutive}) {
            const std::string_view decoy_name =
                decoy == Decoy::Repeated ? "repeated" : "consecutive";
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
                return text.ends_with("document prefix ") &&
                       text.find("啼") == std::string::npos &&
                       text.find("一") == std::string::npos;
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
        return text.find("watch") != std::string::npos &&
               text.find("程式") != std::string::npos;
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
