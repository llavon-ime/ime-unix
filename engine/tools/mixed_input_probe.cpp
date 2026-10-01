#include "engine/fallback_engine.hpp"
#include "input/mixed_input_decoder.hpp"
#include "text/utf.hpp"

#include <cstdio>
#include <chrono>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr, "usage: mixed_input_probe standard|hsu|ibm|et|ginyieh|et26|dachen_cp26 RAW_KEYS [CONTEXT] [--json]\n");
        return 1;
    }
    using namespace llavon::ime;
    const auto layout = bopomofo_keyboard_layout(argv[1]);
    if (bopomofo_keyboard_layout_name(layout) != argv[1]) {
        std::fprintf(stderr, "unknown keyboard layout: %s\n", argv[1]);
        return 1;
    }
    FallbackEngine fallback(LLAVON_IME_TEST_TABLE_PATH);
    MixedInputDecoder decoder([&](auto reading) { return fallback.lookup(reading); },
                              [&](auto word) { return fallback.latin_frequency(word); });
    const bool json_output = argc >= 4 && std::string_view(argv[argc - 1]) == "--json";
    const auto begin = std::chrono::steady_clock::now();
    const auto result = decoder.decode(utf8_to_u16(argv[2]),
        layout,
        false, argc > 3 && std::string_view(argv[3]) != "--json" ? utf8_to_u16(argv[3]) : std::u16string());
    if (json_output) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        nlohmann::json output{{"best", u16_to_utf8(result.paths[result.best_path].rendered)}, {"decode_us", elapsed},
                              {"paths", nlohmann::json::array()}, {"first_page", nlohmann::json::array()}};
        for (const auto& path : result.paths) output["paths"].push_back(u16_to_utf8(path.rendered));
        for (const auto& row : decoder.expand_candidates(result, 9)) {
            output["first_page"].push_back(u16_to_utf8(row.text));
        }
        std::printf("%s\n", output.dump().c_str());
        return 0;
    }
    for (size_t i = 0; i < result.paths.size(); ++i) {
        std::printf("%c %.3f %s\n", i == result.best_path ? '*' : ' ', result.paths[i].score,
                    u16_to_utf8(result.paths[i].rendered).c_str());
    }
}
