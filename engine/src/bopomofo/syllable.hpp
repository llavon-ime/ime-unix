#pragma once

#include <string>

namespace llavon::ime {

class Syllable {
public:
    bool accept(char32_t symbol);
    bool overwrite(char32_t symbol);
    bool pop_back();
    bool empty() const noexcept;
    bool complete() const noexcept;
    std::u16string text() const;

    char32_t initial() const noexcept;
    char32_t medial() const noexcept;
    char32_t final() const noexcept;
    char32_t tone() const noexcept;

    bool has_initial() const noexcept;
    bool has_medial() const noexcept;
    bool has_final() const noexcept;
    bool has_tone() const noexcept;

    bool remove_initial();
    bool remove_medial();
    bool remove_final();
    bool remove_tone();

private:
    char32_t initial_ = 0;
    char32_t medial_ = 0;
    char32_t final_ = 0;
    char32_t tone_ = 0;
};

bool is_bopomofo_initial(char32_t symbol);
bool is_bopomofo_medial(char32_t symbol);
bool is_bopomofo_final(char32_t symbol);
bool is_bopomofo_tone(char32_t symbol);

}  // namespace llavon::ime
