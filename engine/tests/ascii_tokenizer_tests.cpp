#include "test_suites.h"
#include "input/ascii_tokenizer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace llavon::ime {

namespace {

using Tokens = std::vector<AsciiToken>;

std::u16string utf16(const char* text) {
    std::u16string result;
    for (const char* p = text; *p != '\0'; ++p) result.push_back(static_cast<char16_t>(*p));
    return result;
}

std::vector<std::pair<std::string, size_t>> describe(const Tokens& tokens, const std::u16string& raw) {
    std::vector<std::pair<std::string, size_t>> result;
    for (const auto& token : tokens) {
        std::string span;
        for (size_t i = token.begin; i < token.end; ++i) span.push_back(static_cast<char>(raw[i]));
        const char* kind = "";
        switch (token.kind) {
            case AsciiTokenKind::LatinWord:
                kind = "latin";
                break;
            case AsciiTokenKind::Alphanumeric:
                kind = "alnum";
                break;
            case AsciiTokenKind::Email:
                kind = "email";
                break;
            case AsciiTokenKind::Domain:
                kind = "domain";
                break;
            case AsciiTokenKind::URL:
                kind = "url";
                break;
            case AsciiTokenKind::FilesystemPath:
                kind = "path";
                break;
            case AsciiTokenKind::Identifier:
                kind = "ident";
                break;
            case AsciiTokenKind::Number:
                kind = "number";
                break;
            case AsciiTokenKind::OperatorOrSymbol:
                kind = "symbol";
                break;
        }
        result.push_back({std::string(kind) + ":" + span, token.end});
    }
    return result;
}

bool check(const char* name, const char* input, const std::vector<std::pair<std::string, size_t>>& expected) {
    const auto raw = utf16(input);
    const auto tokens = tokenize_ascii(raw, 0);
    const auto actual = describe(tokens, raw);
    if (actual == expected) return true;

    std::printf("[FAIL] %s: tokenize_ascii(\"%s\")\n", name, input);
    for (const auto& [span, end] : actual) {
        std::printf("  got  %s @%zu\n", span.c_str(), end);
    }
    for (const auto& [span, end] : expected) {
        std::printf("  want %s @%zu\n", span.c_str(), end);
    }
    return false;
}

bool check_has(const char* name, const char* input, const std::string& wanted) {
    const auto raw = utf16(input);
    const auto tokens = tokenize_ascii(raw, 0);
    for (const auto& token : tokens) {
        std::string span;
        for (size_t i = token.begin; i < token.end; ++i) span.push_back(static_cast<char>(raw[i]));
        if (span == wanted) return true;
    }
    std::printf("[FAIL] %s: tokenize_ascii(\"%s\") has no token \"%s\"\n", name, input, wanted.c_str());
    for (const auto& token : tokens) {
        std::string span;
        for (size_t i = token.begin; i < token.end; ++i) span.push_back(static_cast<char>(raw[i]));
        std::printf("  got %s @%zu\n", span.c_str(), token.end);
    }
    return false;
}

bool run_ascii_tokenizer_tests() {
    bool ok = true;

    ok &= check("latin word", "hello", {{"latin:h", 1}, {"latin:hello", 5}});
    ok &= check("latin mixed case", "Hello", {{"latin:H", 1}, {"latin:Hello", 5}});
    ok &= check("single letter", "a", {{"latin:a", 1}});
    ok &= check("unknown long word", "qwertyuiop", {{"latin:q", 1}, {"latin:qwertyuiop", 10}});

    ok &= check("alphanumeric", "mp3", {{"latin:m", 1}, {"latin:mp", 2}, {"alnum:mp3", 3}});
    ok &= check("alphanumeric suffix", "hello4", {{"latin:h", 1}, {"latin:hello", 5}, {"alnum:hello4", 6}});

    ok &= check("email", "user@example.com", {{"latin:u", 1}, {"latin:user", 4}, {"email:user@example.com", 16}});
    ok &= check("email local dots", "a.b+c@d.co.uk",
                {{"latin:a", 1}, {"email:a.b+c@d.co.uk", 13}});
    ok &= check("email truncates at suffix",
                "user@example.com283",
                {{"latin:u", 1}, {"latin:user", 4}, {"email:user@example.com", 16}});

    ok &= check("domain", "gmail.com", {{"latin:g", 1}, {"latin:gmail", 5}, {"domain:gmail.com", 9}});
    ok &= check("multi-level domain", "mail.google.com.tw", {{"latin:m", 1}, {"latin:mail", 4}, {"domain:mail.google.com.tw", 18}});
    ok &= check("domain truncates at suffix",
                "gmail.com283",
                {{"latin:g", 1}, {"latin:gmail", 5}, {"domain:gmail.com", 9}});
    ok &= check_has("domain before slash", "gmail.com5j/", "gmail.com");
    ok &= check("single label is not a domain", "localhost", {{"latin:l", 1}, {"latin:localhost", 9}});
    ok &= check("dot-led is not a domain", ".com", {{"symbol:.", 1}});

    ok &= check("url", "https://example.com/v1.2.3",
                {{"latin:h", 1}, {"latin:https", 5}, {"url:https://example.com/v1.2.3", 26}, {"url:https://example.com", 19}});
    ok &= check("url prefix at slash",
                "https://example.com5j/",
                {{"latin:h", 1}, {"latin:https", 5}, {"url:https://example.com5j/", 22}, {"url:https://example.com", 19}, {"url:https://example.com5j", 21}});

    ok &= check("filesystem path", "/tmp/283",
                {{"symbol:/", 1}, {"path:/tmp/283", 8}, {"path:/tmp/", 5}});
    ok &= check("relative path", "/a/b/c", {{"symbol:/", 1}, {"path:/a/b/c", 6}, {"path:/a/", 3}, {"path:/a/b/", 5}});
    ok &= check("bare slash is a symbol", "/", {{"symbol:/", 1}});

    ok &= check("identifier", "hello_world", {{"latin:h", 1}, {"latin:hello", 5}, {"ident:hello_world", 11}});
    ok &= check("identifier with digits", "foo_bar283",
                {{"latin:f", 1}, {"latin:foo", 3}, {"ident:foo_bar283", 10}});

    ok &= check("number", "283", {{"number:2", 1}, {"number:283", 3}});
    ok &= check("decimal", "3.14", {{"number:3", 1}, {"number:3.14", 4}});
    ok &= check("ipv4", "192.168.1.1", {{"number:1", 1}, {"number:192.168.1.1", 11}});
    ok &= check("version suffix", "v1.2.3", {{"latin:v", 1}, {"alnum:v1", 2}});

    ok &= check("symbol", "@", {{"symbol:@", 1}});
    ok &= check("symbol plus letter", "a@b", {{"latin:a", 1}});
    ok &= check("underscore alone", "_", {{"symbol:_", 1}});

    if (ok) std::printf("ascii tokenizer tests passed\n");
    return ok;
}

}  // namespace

}  // namespace llavon::ime

int run_ascii_tokenizer_tests() {
    return llavon::ime::run_ascii_tokenizer_tests() ? EXIT_SUCCESS : EXIT_FAILURE;
}
