#include "test_suites.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "phrase_override/phrase_override_store.hpp"

int run_phrase_override_store_tests() {
    using llavon::ime::PhraseOverrideStore;

    bool ok = true;
    const auto root = std::filesystem::temp_directory_path() / "llavon-ime-user-phrase-test";
    const auto path = root / "phrase_overrides.txt";
    std::filesystem::remove_all(root);

    PhraseOverrideStore store(path);
    ok = ok && store.load();
    const std::vector<std::u16string> readings{u"ㄡ ", u"ㄧㄤˊ", u"ㄓˇ", u"ㄏㄥˊ"};
    ok = ok && store.add(u"歐陽芷珩", readings);
    ok = ok && store.lookup(readings) == std::optional<std::u16string>(u"歐陽芷珩");

    const std::vector<std::u16string> normalized_readings{u"ㄡ", u"ㄧㄤˊ", u"ㄓˇ", u"ㄏㄥˊ"};
    ok = ok && store.lookup(normalized_readings) == std::optional<std::u16string>(u"歐陽芷珩");

    PhraseOverrideStore reloaded(path);
    ok = ok && reloaded.load();
    ok = ok && reloaded.lookup(normalized_readings) == std::optional<std::u16string>(u"歐陽芷珩");
    ok = ok && reloaded.add(u"歐陽芷衡", normalized_readings);
    ok = ok && reloaded.lookup(readings) == std::optional<std::u16string>(u"歐陽芷衡");

    {
        std::ofstream output(path, std::ios::app);
        output << "broken\n"
               << "太短 ㄊㄞˋ\n"
               << "宇文澄曜 ㄩˇ-ㄨㄣˊ-ㄔㄥˊ-ㄧㄠˋ\n";
    }
    ok = ok && reloaded.load();
    const std::vector<std::u16string> test_readings{u"ㄩˇ", u"ㄨㄣˊ", u"ㄔㄥˊ", u"ㄧㄠˋ"};
    ok = ok && reloaded.lookup(test_readings) == std::optional<std::u16string>(u"宇文澄曜");

    const std::vector<llavon::ime::PhraseOverrideRecord> replacement{
        {u"歐陽芷珩", readings}, {u"宇文澄曜", test_readings}};
    ok = ok && reloaded.replace(replacement);
    ok = ok && reloaded.entries().size() == 2;
    ok = ok && reloaded.lookup(normalized_readings) == std::optional<std::u16string>(u"歐陽芷珩");
    const std::vector<llavon::ime::PhraseOverrideRecord> invalid_replacement{{u"宇文澄", test_readings}};
    ok = ok && !reloaded.replace(invalid_replacement);
    ok = ok && reloaded.entries().size() == 2;

    // The editor and the file share one line format, so entries can be copied
    // between them verbatim.
    const auto parsed = PhraseOverrideStore::parse_line("歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ");
    ok = ok && parsed && parsed->phrase == std::u16string(u"歐陽芷珩") && parsed->readings.size() == 4;
    ok = ok && !PhraseOverrideStore::parse_line("只有詞彙").has_value();
    // Whitespace separates readings too, so a first tone is just a space and
    // needs no tone symbol; stray separators are ignored.
    const auto spaced = PhraseOverrideStore::parse_line("宇文澄曜 ㄩˇ ㄨㄣˊ ㄔㄥˊ ㄧㄠˋ");
    ok = ok && spaced && spaced->readings.size() == 4;
    const auto first_tone = PhraseOverrideStore::parse_line("媽媽 ㄇㄚ ㄇㄚ");
    ok = ok && first_tone && first_tone->readings.size() == 2;
    ok = ok && PhraseOverrideStore::parse_line("詞彙   ㄡ-ㄧㄤˊ").has_value();
    ok = ok && PhraseOverrideStore::parse_line("詞彙 ㄡ--ㄧㄤˊ").has_value();
    ok = ok && PhraseOverrideStore::format_line(u"歐陽芷珩", "ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ") == "歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ";
    ok = ok && PhraseOverrideStore::format_readings(normalized_readings) == "ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ";

    const std::vector<std::u16string> one_reading{u"ㄗˋ"};
    ok = ok && !reloaded.add(u"字", one_reading);
    ok = ok && !reloaded.add(u"宇文澄", test_readings);

    std::filesystem::remove_all(root);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
