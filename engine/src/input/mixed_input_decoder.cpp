#include "input/mixed_input_decoder.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <list>
#include <mutex>
#include <cmath>
#include <iterator>
#include <memory>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "buffer/composition_buffer.hpp"
#include "input/ascii_tokenizer.hpp"
#include "input/mixed_lexicon.hpp"
#include "text/utf.hpp"

namespace llavon::ime {

namespace {

// Centralized ranking weights. All ranking decisions live in the decoder.
constexpr double kLatinBase = -6.0;
constexpr double kAlnumBase = -3.0;
constexpr double kNumberBase = -3.0;
constexpr double kSymbolBase = 0.0;
constexpr double kStructuredBoost = -1.0;
constexpr double kKnownWordBoost = 4.0;
constexpr double kSwitchPenalty = 3.0;
constexpr double kFragmentPenalty = 3.0;
// A physical edit is weaker evidence than the spelling the user supplied.
constexpr double kTypingEditPenalty = 3.5;
// Zipf >= 5 means a very common completion; weak dictionary prefixes alone
// must not override an already complete reading with a letter tone key.
constexpr double kStrongPrefixFrequency = 5.0 / 8.0;

double phonetic_symbol_evidence(std::u16string_view body) {
    // Only an explicitly completed syllable gets this evidence, never an
    // unfinished English prefix. Layouts such as IBM put ordinary phonetic
    // components on punctuation keys; treating each as a free literal symbol
    // otherwise beats a complete reading even inside an established phrase.
    // A letter plus a phonetic punctuation key is a whole reading, while its
    // competing literal often gains a cheap dictionary score for just the one
    // letter. Account for that fragmentation using the same cost as adjacent
    // Latin fragments, rather than requiring preceding Chinese to rescue it.
    // This is a scored preference, not a forced rewrite; raw stays selectable.
    const auto symbols = std::ranges::count_if(body, [](char16_t key) {
        return key > u' ' && key <= u'~' &&
               !(key >= u'a' && key <= u'z') && !(key >= u'A' && key <= u'Z') &&
               !(key >= u'0' && key <= u'9');
    });
    return 0.4 * static_cast<double>(symbols) +
        (body.size() >= 2 && symbols != 0 ? kFragmentPenalty : 0.0);
}

bool is_explicit_tone_key(char32_t key, BopomofoKeyboardLayout layout) {
    return is_bopomofo_tone_key(key, layout);
}

void append_codepoint(std::u16string& text, char32_t value) {
    if (value <= 0xFFFF) {
        text.push_back(static_cast<char16_t>(value));
        return;
    }
    const char32_t codepoint = value - 0x10000;
    text.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
    text.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
}

std::u16string render_segment(const MixedSegment& segment) {
    if (segment.consumed_boundary) return {};
    if (segment.kind == MixedSegmentKind::BopomofoIncomplete && segment.phonetic_display) return segment.reading;
    if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
        std::u16string text;
        append_codepoint(text, segment.candidates.front());
        return text;
    }
    return segment.raw;
}

}  // namespace

// Only deterministic lexical work is retained. DP still scores each restored
// word against the current surrounding text and its current predecessor.
struct MixedInputDecoder::WordCache {
    struct Word {
        size_t end;
        std::vector<MixedSegment> segments;
        double score;
        double lexical_bonus;
    };
    struct Inventory {
        std::vector<char32_t> candidates;
        std::uint64_t id;
        size_t bytes;
    };
    struct Signature {
        std::uint64_t id;
        size_t bytes;
    };
    using Order = std::list<const std::u16string*>;
    struct Entry {
        std::shared_ptr<const std::vector<Word>> words;
        size_t bytes;
        Order::iterator position;
    };
    static constexpr size_t kMaxEntries = 512;
    static constexpr size_t kMaxBytes = 16 * 1024 * 1024;
    std::mutex mutex;
    std::unordered_map<std::u16string, Inventory> inventories;
    std::unordered_map<std::u16string, Signature> signatures;
    std::unordered_map<std::u16string, Entry> entries;
    // Map keys retain their addresses through rehash. LRU splices do not
    // invalidate the iterators held by entries.
    Order order;
    size_t bytes = 0;
    std::uint64_t next_inventory = 0;
    std::uint64_t next_signature = 0;

    void evict() {
        const auto found = entries.find(*order.front());
        order.pop_front();
        bytes -= found->second.bytes;
        entries.erase(found);
    }
    void clear() {
        order.clear();
        decltype(entries){}.swap(entries);
        decltype(inventories){}.swap(inventories);
        decltype(signatures){}.swap(signatures);
        bytes = 0;
        // IDs remain monotonic. Borrowed immutable words stay alive, and no
        // new pool content can acquire an identity used by an older key.
    }
    bool make_room(size_t retained, size_t released = 0) {
        while (!order.empty() && bytes - released + retained > kMaxBytes) evict();
        if (bytes - released + retained <= kMaxBytes) return false;
        clear();
        return true;
    }
    std::optional<std::uint64_t> inventory(std::u16string_view reading,
                                          const std::vector<char32_t>& candidates) {
        std::lock_guard lock(mutex);
        auto found = inventories.find(std::u16string(reading));
        if (found != inventories.end() && found->second.candidates == candidates) return found->second.id;
        const auto retained = sizeof(Inventory) + sizeof(std::u16string) + 64 +
            reading.size() * sizeof(char16_t) + candidates.size() * sizeof(char32_t);
        if (retained > kMaxBytes || next_inventory == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        size_t old = found == inventories.end() ? 0 : found->second.bytes;
        if (make_room(retained, old)) { found = inventories.end(); old = 0; }
        Inventory replacement{candidates, ++next_inventory, retained};
        bytes = bytes - old + retained;
        if (found == inventories.end()) inventories.emplace(std::u16string(reading), std::move(replacement));
        else found->second = std::move(replacement);
        return next_inventory;
    }
    std::optional<std::uint64_t> signature(const std::u16string& value) {
        std::lock_guard lock(mutex);
        const auto found = signatures.find(value);
        if (found != signatures.end()) return found->second.id;
        const auto retained = sizeof(Signature) + sizeof(std::u16string) + 64 +
            value.size() * sizeof(char16_t);
        if (retained > kMaxBytes || next_signature == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        (void)make_room(retained);
        const auto id = ++next_signature;
        signatures.emplace(value, Signature{id, retained});
        bytes += retained;
        return id;
    }
    std::shared_ptr<const std::vector<Word>> find(const std::u16string& key) {
        std::lock_guard lock(mutex);
        const auto found = entries.find(key);
        if (found == entries.end()) return nullptr;
        order.splice(order.end(), order, found->second.position);
        return found->second.words;
    }
    void insert(std::u16string key, std::vector<Word> words) {
        size_t retained = sizeof(Entry) + sizeof(std::u16string) + 96 + key.capacity() * sizeof(char16_t) +
            sizeof(std::vector<Word>) + words.capacity() * sizeof(Word);
        for (const auto& word : words) {
            retained += word.segments.capacity() * sizeof(MixedSegment);
            for (const auto& segment : word.segments) {
                retained += (segment.raw.capacity() + segment.body_keys.capacity() + segment.reading.capacity()) * sizeof(char16_t);
                retained += segment.candidates.capacity() * sizeof(char32_t);
            }
        }
        std::lock_guard lock(mutex);
        if (entries.contains(key) || retained > kMaxBytes) return;
        while (!order.empty() && entries.size() >= kMaxEntries) evict();
        (void)make_room(retained);
        auto [stored, inserted] = entries.emplace(std::move(key), Entry{
            std::make_shared<const std::vector<Word>>(std::move(words)), retained, {}});
        if (inserted) {
            try { order.push_back(&stored->first); }
            catch (...) { entries.erase(stored); throw; }
            stored->second.position = std::prev(order.end());
            bytes += retained;
        }
    }
};

MixedInputDecoder::MixedInputDecoder(LookupFn lookup, FrequencyFn frequency,
                                     LatinScoreFn latin_score, ChineseScoreFn chinese_score)
    : lookup_(std::move(lookup)), frequency_(std::move(frequency)),
      latin_score_(std::move(latin_score)), default_latin_score_(!latin_score_),
      chinese_score_(std::move(chinese_score)), default_chinese_score_(!chinese_score_),
      recovery_(lookup_), word_cache_(std::make_shared<WordCache>()) {
    if (!latin_score_) latin_score_ = [](std::u16string_view word, bool prefix) {
        return MixedLexicon::instance().english_score(word, prefix);
    };
    if (!chinese_score_) chinese_score_ = [](std::u16string_view history, char32_t ch) {
        return MixedLexicon::instance().chinese_score(history, ch);
    };
}

MixedDecodeResult MixedInputDecoder::decode(std::u16string_view raw, BopomofoKeyboardLayout layout,
                                            bool space_tone, std::u16string_view context,
                                            MixedDecodeOptions options) const {
    const size_t n = raw.size();
    if (n == 0) return {{}, {MixedPath{}}, 0};

    const auto latin_score = [&](std::u16string_view spelling, bool prefix) {
        if (!default_latin_score_) return latin_score_(spelling, prefix);
        const auto& lexicon = MixedLexicon::instance();
        const auto score = lexicon.english_score(spelling, prefix);
        const auto without_compound = lexicon.english_score(spelling, prefix, false);
        if (score <= without_compound) return score;
        // Synthetic English compounds are weaker evidence than an ordinary
        // completed, explicitly toned reading. Judge the prefix using the
        // active keyboard rather than treating vowels as universal tone keys.
        bool compound = true;
        Syllable syllable;
        for (size_t offset = 0; offset < std::min(spelling.size(), kMaxSyllableKeys); ++offset) {
            const auto step = apply_bopomofo_key(syllable, layout, spelling[offset]);
            if (!step.natural_extension) break;
            if (step.status == BopomofoKeyStatus::Completed) {
                if (offset >= 1 && syllable.text().back() != u'˙' &&
                    !lookup_(syllable.text()).empty()) compound = false;
                break;
            }
            if (step.status != BopomofoKeyStatus::Composing) break;
        }
        return compound ? score : without_compound;
    };

    struct Edge {
        size_t end;
        MixedSegment segment;
        bool lexical_word;
        bool recovery_piece = false;
        bool phrase_onset = false;
        bool natural_reading = false;
        bool structured_boundary = false;
        bool literal_island = false;
        size_t contextual_prefix_begin = std::u16string_view::npos;
        bool contextual_evidence = false;
    };
    struct DomainBoundary { size_t begin; size_t suffix_begin; size_t end; };
    std::vector<DomainBoundary> domain_boundaries;
    struct FileSpan { size_t begin; size_t end; };
    std::vector<FileSpan> file_spans;
    std::vector<std::vector<Edge>> edges(n + 1);
    bool has_structured_boundaries = false;

    // Letter tones are both completion keys and ordinary English letters.
    // Give a valid completed reading modest evidence, but not at the expense
    // of a complete literal word, opaque syntax, or a common word still being
    // typed. Classify whole ASCII runs, not arbitrary one-letter subedges.
    const bool letter_tones = std::ranges::any_of(std::u32string_view(U"abcdefghijklmnopqrstuvwxyz"),
        [layout](char32_t key) { return is_explicit_tone_key(key, layout); });
    std::vector<bool> literal_spans(n, false);
    std::vector<bool> boundary_protected_spans(n, false);
    std::vector<bool> word_spans(n, false);
    std::vector<bool> opaque_recovery_spans(n, false);
    const bool lexical_prefix = letter_tones &&
        MixedLexicon::instance().english_prefix_frequency(raw) > 0;
    {
        const auto& lexicon = MixedLexicon::instance();
        const auto word_key = [](char16_t key) {
            return (key >= u'a' && key <= u'z') || (key >= u'A' && key <= u'Z') ||
                   (key >= u'0' && key <= u'9') || key == u'_';
        };
        const auto known_word = [&](std::u16string_view spelling) {
            if (frequency_ && frequency_(spelling) > 0) return true;
            // A missing inflected form must not expose its first few letters
            // as a phonetic syllable (dictionary coverage is never exhaustive).
            for (const auto suffix : {u"ly", u"ness", u"s", u"es", u"ed", u"ing", u"er", u"est"}) {
                const std::u16string_view ending(suffix);
                if (spelling.size() <= ending.size() + 2 || !spelling.ends_with(ending)) continue;
                const auto stem = spelling.substr(0, spelling.size() - ending.size());
                if (frequency_ && frequency_(stem) > 0) return true;
            }
            return false;
        };
        for (size_t begin = 0; begin < n; ++begin) {
            if (begin != 0 && word_key(raw[begin - 1])) continue;
            for (const auto& token : tokenize_ascii(raw, begin)) {
                const auto spelling = raw.substr(begin, token.end - begin);
                const bool word = token.kind == AsciiTokenKind::LatinWord && spelling.size() >= 2 &&
                    known_word(spelling);
                const bool prefix = token.kind == AsciiTokenKind::LatinWord && spelling.size() >= 2 &&
                    token.end == n && !space_tone && lexicon.english_prefix_frequency(spelling) >= kStrongPrefixFrequency;
                const bool structure = token.kind == AsciiTokenKind::Identifier ||
                    token.kind == AsciiTokenKind::Email || token.kind == AsciiTokenKind::Domain ||
                    token.kind == AsciiTokenKind::URL || token.kind == AsciiTokenKind::FilesystemPath ||
                    token.kind == AsciiTokenKind::Alphanumeric;
                if (token.kind == AsciiTokenKind::Identifier || token.kind == AsciiTokenKind::Email ||
                    token.kind == AsciiTokenKind::URL) {
                    std::fill(opaque_recovery_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                              opaque_recovery_spans.begin() + static_cast<std::ptrdiff_t>(token.end), true);
                }
                if (word) {
                    std::fill(word_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                              word_spans.begin() + static_cast<std::ptrdiff_t>(token.end), true);
                }
                if (word || (letter_tones && (prefix || structure))) {
                    std::fill(literal_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                              literal_spans.begin() + static_cast<std::ptrdiff_t>(token.end), true);
                }
                if (word || (letter_tones && (prefix ||
                    (structure && token.kind != AsciiTokenKind::Alphanumeric)))) {
                    // A digit may be the next completed Chinese reading on
                    // layouts such as IBM. Permit the alphabetic part to end
                    // at an anchored reading while preserving opaque syntax.
                    std::fill(boundary_protected_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                              boundary_protected_spans.begin() + static_cast<std::ptrdiff_t>(token.end), true);
                }
            }
        }
    }
    const auto cuts_english_word = [&](size_t begin, size_t end) {
        return (begin > 0 && word_spans[begin - 1] && word_spans[begin]) ||
            (end < n && word_spans[end - 1] && word_spans[end]) ||
            (word_spans[begin] && word_spans[end - 1]);
    };
    const auto protects_typing_word = [&](const MixedSegment& segment) {
        if (!cuts_english_word(segment.begin, segment.end)) return false;
        // A phonetic punctuation key can create an apparent short English
        // word across two syllables (e.g. GinYieh's ㄣ/ㄊ keys). A complete
        // lexical word may recover that exact spelling; edited spellings and
        // all-letter English words retain their usual protection.
        return !(options.smart_typing && segment.inferred && !segment.abbreviated &&
            segment.recovery_edits == 0 && std::ranges::any_of(segment.body_keys, [&](char16_t key) {
                return key > u' ' && key <= u'~' && !(key >= u'a' && key <= u'z') &&
                    !(key >= u'0' && key <= u'9') && segment.raw.find(key) != std::u16string::npos;
            }));
    };
    const auto completion_evidence = [&](size_t begin, size_t end, std::u16string_view spelling) {
        // A complete leading English word can be followed by a letter-tone
        // reading without switching modes. Do not reward arbitrary internal
        // cuts that borrow an English word's last letter as a phonetic onset.
        if (begin != 0 && (!frequency_ || frequency_(raw.substr(0, begin)) <= 0)) return 0.0;
        // A weak but real English completion can explain a new tail even if
        // it cannot override the shorter completed Chinese input by itself.
        if (end < n && lexical_prefix) return 0.0;
        if (!letter_tones ||
            (frequency_ && frequency_(spelling) > 0) ||
            std::ranges::any_of(literal_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                                literal_spans.begin() + static_cast<std::ptrdiff_t>(end),
                                [](bool value) { return value; })) return 0.0;
        return kSwitchPenalty / 2.0;
    };

    const auto add_edge = [&](size_t begin, size_t end, MixedSegment segment) {
        segment.begin = begin;
        segment.end = end;
        segment.raw.assign(raw.substr(begin, end - begin));
        // Edge evidence is independent of a beam history. Compute it once,
        // rather than doing a lexicon lookup for every path extension.
        const bool lexical_word = segment.kind == MixedSegmentKind::Latin &&
            frequency_ && frequency_(segment.raw) > 0;
        edges[begin].push_back({end, std::move(segment), lexical_word});
    };

    // ASCII edges (generic grammar, no hardcoded websites or words).
    for (size_t i = 0; i < n; ++i) {
        for (const auto& token : tokenize_ascii(raw, i)) {
            MixedSegment segment;
            switch (token.kind) {
                case AsciiTokenKind::LatinWord:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = latin_score(raw.substr(token.begin, token.end - token.begin),
                                                token.end == n && !space_tone);
                    if (!latin_score_ && frequency_) {
                        segment.score += kKnownWordBoost * frequency_(raw.substr(token.begin, token.end - token.begin));
                    }
                    if (letter_tones && token.end == n && !space_tone &&
                        token.end - i >= 2 && i == 0) {
                        const auto prefix = MixedLexicon::instance().english_prefix_frequency(raw.substr(i));
                        // Common completions get the normal lexical scale while
                        // typing, not the weak-prefix backoff. Rare completions
                        // must not erase completed Chinese (e.g. a compact tone).
                        if (prefix >= kStrongPrefixFrequency) {
                            segment.score = std::max(segment.score, std::min(-0.1, 6.0 * prefix - 5.0));
                        }
                    }
                    break;
                case AsciiTokenKind::Alphanumeric:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kAlnumBase - 0.6 * static_cast<double>(token.end - token.begin);
                    if (frequency_) {
                        const auto word = raw.substr(token.begin, token.end - token.begin);
                        if (frequency_(word) > 0 && latin_score_) segment.score = latin_score(word, false) + 1.0;
                        const auto digit = word.find_first_of(u"0123456789");
                        const auto before = i == 0 ? u' ' : raw[i - 1];
                        const bool word_start = !((before >= u'a' && before <= u'z') ||
                            (before >= u'A' && before <= u'Z') || (before >= u'0' && before <= u'9') || before == u'_');
                        if (word_start && digit >= 4 && digit < word.size() &&
                            frequency_(word.substr(0, digit)) <= 0 &&
                            std::ranges::all_of(word.substr(digit), [](char16_t key) {
                                return key >= u'0' && key <= u'9';
                            }) && MixedLexicon::instance().english_scoring_frequency(word.substr(0, digit)) > 0) {
                            // A rare source word plus a numeric suffix is a
                            // plausible code/name, without strong dictionary
                            // evidence for a mixed boundary. Keep its literal
                            // prior; primary words retain ordinary mixed cuts.
                            segment.score = std::max(segment.score, kStructuredBoost);
                        }
                    }
                    break;
                case AsciiTokenKind::Identifier:
                    // Opaque identifiers need not resemble English words.
                    // A spelling penalty can silently rewrite valid names;
                    // preserve the literal and offer Chinese as an alternative.
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kStructuredBoost;
                    break;
                case AsciiTokenKind::Email:
                case AsciiTokenKind::Domain:
                case AsciiTokenKind::URL:
                case AsciiTokenKind::FilesystemPath:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kStructuredBoost;
                    break;
                case AsciiTokenKind::Number:
                    segment.kind = MixedSegmentKind::Number;
                    segment.score = raw.substr(token.begin, token.end - token.begin).find(u'.') !=
                                    std::u16string_view::npos ? kStructuredBoost : kNumberBase;
                    break;
                case AsciiTokenKind::OperatorOrSymbol:
                    segment.kind = MixedSegmentKind::Symbol;
                    segment.score = kSymbolBase;
                    break;
            }
            add_edge(i, token.end, std::move(segment));
            if (token.kind == AsciiTokenKind::Domain) {
                const auto dot = raw.substr(i, token.end - i).find(u'.');
                if (dot != std::u16string_view::npos) {
                    domain_boundaries.push_back({i, i + dot + 1, token.end});
                }
            }
            if (token.kind == AsciiTokenKind::FilesystemPath) file_spans.push_back({i, token.end});
        }
        // Dictionary edges expose internal English/Chinese boundaries that a
        // longest-token lexer cannot see (e.g. hello + Hsu nef). Contractions
        // and compounds also remain a single spelling-model interpretation.
        if (latin_score_) {
            for (size_t end = i + 1; end <= std::min(n, i + 48); ++end) {
                const auto ch = raw[end - 1];
                if (!((ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
                      ch == u'\'' || ch == u'-')) break;
                const auto word = raw.substr(i, end - i);
                if (end != n && (!frequency_ || frequency_(word) <= 0)) continue;
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Latin;
                segment.score = latin_score(word, end == n && !space_tone);
                add_edge(i, end, std::move(segment));
            }
        }
    }

    // Strict Bopomofo edges: one trigger completes exactly one syllable.
    const auto replay = [&](std::u16string_view body, char32_t tone_key) {
        std::vector<std::pair<std::u16string, std::vector<char32_t>>> readings;
        for (const auto& syllable : replay_bopomofo_keys(body, tone_key, layout)) {
            const auto reading = syllable.text();
            auto candidates = lookup_(reading);
            if (!candidates.empty()) readings.emplace_back(reading, std::move(candidates));
        }
        return readings;
    };

    const auto hsu_ordered_reading = [&](std::u16string_view body, char32_t tone_key, const auto& readings) {
        std::u16string reading;
        if (layout != BopomofoKeyboardLayout::Hsu || readings.size() < 2) return reading;
        Syllable syllable;
        for (const auto key : body) {
            const auto step = apply_bopomofo_key(syllable, layout, key);
            if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension) return reading;
        }
        if (apply_bopomofo_key(syllable, layout, tone_key).status != BopomofoKeyStatus::Completed) return reading;
        const auto natural = syllable.text();
        if (std::ranges::any_of(readings, [&](const auto& entry) { return entry.first == natural; })) reading = natural;
        return reading;
    };

    const auto unresolved = [&](size_t begin, size_t end) {
        // A wrong phonetic component is not evidence that all earlier Chinese
        // was English. Keep a raw repair island, using the same cost as an
        // unfinished reading. DP admits it only after a real phonetic segment.
        // Literal syntax stays in the ordinary ASCII graph.
        if (std::ranges::any_of(raw.substr(begin, end - begin), [&](char16_t key) {
            return key != u' ' && !lookup_bopomofo_key(key, layout, false);
        })) return;
        const auto keys = raw.substr(begin, end - begin);
        if (frequency_ && frequency_(keys) > 0) return;
        const auto tone = keys.back();
        if (tone == u' ' || is_explicit_tone_key(tone, layout)) {
            // Complete valid readings are not error states, even when their
            // lexical prior is weak (e.g. a vowel-only interjection).
            if (!replay(keys.substr(0, keys.size() - 1), tone).empty()) return;
            // A real English word followed by a valid reading is a normal
            // mixed boundary, not one malformed syllable. Use the existing
            // lexicon, without special-casing any abbreviation or character.
            if (frequency_) {
                for (size_t split = 1; split + 1 < keys.size(); ++split) {
                    if (frequency_(keys.substr(0, split)) > 0 &&
                        !replay(keys.substr(split, keys.size() - split - 1), tone).empty()) return;
                }
            }
        }
        Syllable ordinary;
        for (size_t index = begin; index < end; ++index) {
            const auto key = raw[index];
            const auto step = apply_bopomofo_key(ordinary, layout, key, false);
            const bool boundary = key == u' ' || step.status == BopomofoKeyStatus::Completed ||
                (is_explicit_tone_key(key, layout) && !(key >= u'a' && key <= u'z'));
            // An error island ends at the first actual completion boundary.
            // It must not swallow valid syllables to obtain a cheaper score.
            if (boundary && index + 1 != end) return;
        }
        MixedSegment segment;
        segment.kind = MixedSegmentKind::BopomofoUnresolved;
        segment.score = -2.0;
        add_edge(begin, end, std::move(segment));
    };

    for (size_t i = 0; i < n; ++i) {
        if (is_explicit_tone_key(raw[i], layout)) unresolved(i, i + 1);
        for (size_t len = 2; len <= kMaxSyllableKeys && i + len <= n; ++len) {
            const size_t j = i + len;
            const char32_t tone_key = raw[j - 1];
            if (tone_key != U' ' && !is_explicit_tone_key(tone_key, layout)) continue;
            const auto body = raw.substr(i, len - 1);
            auto readings = replay(body, tone_key);
            const auto ordered = hsu_ordered_reading(body, tone_key, readings);
            if (readings.empty()) unresolved(i, j);
            for (auto&& [reading, candidates] : readings) {
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Bopomofo;
                segment.body_keys.assign(body);
                segment.tone_key = tone_key;
                segment.reading = std::move(reading);
                segment.candidates = std::move(candidates);
                // Use the same existing evidence classes on every layout: digit/
                // punctuation tones are distinct, letter tones are ambiguous.
                const bool letter_tone = tone_key >= U'a' && tone_key <= U'z';
                segment.score = (tone_key == U' ' ? (is_compact_bopomofo_layout(layout) ? 3.5 : 3.4) :
                                 letter_tone ? 0.0 : 3.4) +
                                 0.4 * static_cast<double>(body.size()) +
                                 (i == 0 ? phonetic_symbol_evidence(body) : 0.0) +
                                 completion_evidence(i, j, tone_key == U' ' ? body : raw.substr(i, len));
                // Compact keys acquire contextual meanings. Reordered keys
                // remain available, but must not beat a valid ordinary Hsu
                // reading solely because its other reading has a common word.
                if (!ordered.empty() && segment.reading != ordered) segment.score -= kSwitchPenalty / 2.0;
                add_edge(i, j, std::move(segment));
            }
        }
        // A malformed prefix may be longer than one bounded graph piece.
        // Expose only prefixes that already failed natural phonetic editing,
        // so legal unfinished readings cannot become cheap raw error edges.
        CompositionBuffer continuation;
        bool malformed = false;
        for (size_t len = 1; len <= kMaxSyllableKeys && i + len <= n; ++len) {
            const auto step = continuation.add_bopomofo_key(raw[i + len - 1], layout);
            if (step && step->completed) break;
            if (!step || !step->natural_extension || continuation.segments().size() != 1) malformed = true;
            if (continuation.segments().size() != 1) break;
            const auto& syllable = continuation.segments().front().syllable;
            const auto occupied = static_cast<unsigned>(syllable.has_initial()) +
                static_cast<unsigned>(syllable.has_medial()) + static_cast<unsigned>(syllable.has_final());
            // Only a contradiction confined to one phonetic slot can be
            // split without a completion boundary. Mixed-slot spans could
            // hide an English word followed by a legal Chinese syllable.
            if (occupied != 1 || syllable.has_tone()) break;
            // Only expose a full-sized nonterminal piece. Short error tails
            // already have terminal edges; extra internal cuts can hide a
            // dictionary-word / legal-reading boundary.
            if (malformed && len == kMaxSyllableKeys && i + len < n) {
                const auto keys = raw.substr(i, len);
                bool lexical_boundary = false;
                if (frequency_) {
                    for (size_t split = 1; split < len; ++split) {
                        if (frequency_(keys.substr(0, split)) <= 0) continue;
                        CompositionBuffer suffix;
                        bool natural = true;
                        for (const auto key : keys.substr(split)) {
                            const auto added = suffix.add_bopomofo_key(key, layout);
                            if (!added || added->completed || !added->natural_extension || suffix.segments().size() != 1) {
                                natural = false;
                                break;
                            }
                        }
                        if (natural) { lexical_boundary = true; break; }
                    }
                }
                if (lexical_boundary) break;
                // Do not cut through the onset of a valid completed syllable
                // just beyond this piece (e.g. wrong initials + su3). Keeping
                // that onset literal would make its vowel decode separately.
                bool reading_boundary = false;
                for (size_t begin = i + 1; begin < i + len && !reading_boundary; ++begin) {
                    for (size_t end = i + len + 1; end <= std::min(n, begin + kMaxSyllableKeys); ++end) {
                        const auto tone = raw[end - 1];
                        if (tone != u' ' && !is_explicit_tone_key(tone, layout)) continue;
                        if (!replay(raw.substr(begin, end - begin - 1), tone).empty()) {
                            reading_boundary = true;
                            break;
                        }
                    }
                }
                if (reading_boundary) break;
                const auto count = edges[i].size();
                unresolved(i, i + len);
                if (edges[i].size() != count) edges[i].back().recovery_piece = true;
            }
        }
        // Unfinished syllables are real search states, not a rule that pins
        // yesterday's preview. They preserve consecutive Chinese while the
        // next body is typed, and still compete with complete English paths.
        if (n - i <= kMaxSyllableKeys) {
            CompositionBuffer scratch;
            bool valid = true;
            for (const auto ch : raw.substr(i)) {
                const auto step = scratch.add_bopomofo_key(ch, layout);
                if (!step || step->completed || !step->natural_extension || scratch.segments().size() != 1) {
                    valid = false;
                    break;
                }
            }
            if (valid && !scratch.segments().empty() && !scratch.segments().front().syllable.empty()) {
                MixedSegment segment;
                segment.kind = MixedSegmentKind::BopomofoIncomplete;
                segment.reading = scratch.segments().front().syllable.text();
                segment.phonetic_display = !options.allow_english;
                segment.score = -2.0;
                add_edge(i, n, std::move(segment));
            } else {
                unresolved(i, n);
            }
        }
    }

    // Space may close a first-tone syllable. The first-tone interpretation
    // only applies at the end of the pending input, and a suffix syllable
    // needs at least two body keys (a single leftover key stays English).
    if (space_tone) {
        for (size_t i = 0; i < n; ++i) {
            const size_t body_len = n - i;
            if (body_len < (i == 0 ? 1 : 2)) continue;
            auto readings = replay(raw.substr(i), U' ');
            const auto ordered = hsu_ordered_reading(raw.substr(i), U' ', readings);
            for (auto&& [reading, candidates] : readings) {
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Bopomofo;
                segment.body_keys.assign(raw.substr(i));
                segment.tone_key = U' ';
                segment.reading = std::move(reading);
                segment.candidates = std::move(candidates);
                segment.score = (is_compact_bopomofo_layout(layout) ? 3.5 : 3.4) +
                                 0.4 * static_cast<double>(body_len) +
                                 (i == 0 ? phonetic_symbol_evidence(raw.substr(i)) : 0.0) +
                                 completion_evidence(i, n, raw.substr(i));
                if (!ordered.empty() && segment.reading != ordered) segment.score -= kSwitchPenalty / 2.0;
                add_edge(i, n, std::move(segment));
            }
        }
    }

    // An unknown English island can still be real text. A complete
    // dictionary phrase at the following reading boundary supplies the same
    // resumption evidence as an English dictionary entry, without guessing or
    // completing the literal name itself. This never applies to opaque syntax
    // or a one/two-letter fragment masquerading as a word.
    {
        const auto& lexicon = MixedLexicon::instance();
        for (auto& bucket : edges) {
            for (auto& first : bucket) {
                if (first.segment.kind != MixedSegmentKind::Bopomofo) continue;
                Syllable ordinary;
                bool natural = true;
                for (const auto key : first.segment.body_keys) {
                    const auto step = apply_bopomofo_key(ordinary, layout, key);
                    if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension) {
                        natural = false;
                        break;
                    }
                }
                first.natural_reading = natural &&
                    apply_bopomofo_key(ordinary, layout, first.segment.tone_key).status == BopomofoKeyStatus::Completed &&
                    ordinary.text() == first.segment.reading;
            }
        }
        for (auto& bucket : edges) {
            for (auto& first : bucket) {
                // Reordered readings remain selectable, but cannot establish
                // a new literal boundary by borrowing the island's last key.
                if (!first.natural_reading || first.end == n) continue;
                // Match the ordinary beam's bounded homophone shortlist.
                for (size_t candidate = 0; candidate < std::min<size_t>(4, first.segment.candidates.size()); ++candidate) {
                    const auto ch = first.segment.candidates[candidate];
                    if (ch > 0xFFFF) continue;
                    for (const auto next : lexicon.common_chinese_pair_extensions(ch)) {
                        if (std::ranges::any_of(edges[first.end], [&](const auto& second) {
                            return second.natural_reading &&
                                std::ranges::find(second.segment.candidates, next) != second.segment.candidates.end();
                        })) { first.phrase_onset = true; break; }
                    }
                    if (first.phrase_onset) break;
                }
            }
        }
        // The lexicon need not contain every adjacent pair of a normal phrase.
        // Permit at most two completed readings before the lexical anchor,
        // e.g. a verb followed by a known noun. Never search an unbounded tail.
        for (int hop = 0; hop < 2; ++hop) {
            std::vector<bool> suffix(n + 1, false);
            for (size_t begin = 0; begin < n; ++begin) {
                suffix[begin] = std::ranges::any_of(edges[begin], [](const auto& edge) { return edge.phrase_onset; });
            }
            for (auto& bucket : edges) {
                for (auto& first : bucket) {
                    if (first.natural_reading && suffix[first.end]) first.phrase_onset = true;
                }
            }
        }
        // Dictionary-only cuts cannot represent an unseen literal island as
        // one token. Expose bounded alphabetic cuts at these proven Chinese
        // onsets; DP still requires a Chinese predecessor to reward resumption.
        // The spelling model scores the exact bytes, never supplies a word.
        std::vector<bool> onsets(n + 1, false);
        std::vector<bool> chinese_ends(n + 1, false);
        std::vector<std::vector<char32_t>> preceding_characters(n + 1);
        for (size_t begin = 0; begin < n; ++begin) {
            onsets[begin] = std::ranges::any_of(edges[begin], [](const auto& edge) { return edge.phrase_onset; });
            for (const auto& edge : edges[begin]) {
                if (edge.segment.kind != MixedSegmentKind::Bopomofo) continue;
                chinese_ends[edge.end] = true;
                if (edge.natural_reading) {
                    auto& characters = preceding_characters[edge.end];
                    for (size_t i = 0; i < std::min<size_t>(4, edge.segment.candidates.size()); ++i) {
                        const auto ch = edge.segment.candidates[i];
                        if (std::ranges::find(characters, ch) == characters.end()) characters.push_back(ch);
                    }
                }
            }
        }
        // A scoring-only whole English word can also expose a real boundary
        // before an anchored Chinese phrase. Keep its soft word score without
        // turning rare spellings into hard dictionary or prefix protection.
        for (size_t begin = 0; begin < n; ++begin) {
            for (size_t end = begin + 1; end < std::min(n, begin + 49); ++end) {
                const auto key = raw[end - 1];
                if (!((key >= u'a' && key <= u'z') || (key >= u'A' && key <= u'Z'))) break;
                if (end - begin < 4 || !onsets[end]) continue;
                const auto word = raw.substr(begin, end - begin);
                if ((frequency_ && frequency_(word) > 0) ||
                    lexicon.english_scoring_frequency(word) < 2.5 / 8.0) continue;
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Latin;
                segment.score = latin_score(word, false);
                add_edge(begin, end, std::move(segment));
            }
        }
        // A punctuation tone followed by English can resemble a domain on
        // IBM. Offer ordinary word resumption only when that exact prefix is
        // a natural reading and the suffix ends at an anchored Chinese run.
        // The path must also contain Chinese before that reading (checked in
        // extend); isolated domains keep their ordinary literal interpretation.
        for (const auto& domain : domain_boundaries) {
            if (!onsets[domain.end] || domain.end - domain.suffix_begin < 3 ||
                !std::ranges::all_of(raw.substr(domain.suffix_begin, domain.end - domain.suffix_begin),
                    [](char16_t key) { return key >= u'a' && key <= u'z'; })) continue;
            if (!std::ranges::any_of(edges[domain.begin], [&](const Edge& reading) {
                return reading.natural_reading && reading.end == domain.suffix_begin;
            })) continue;
            for (auto& edge : edges[domain.suffix_begin]) {
                if (edge.segment.kind == MixedSegmentKind::Latin && edge.end == domain.end) {
                    edge.contextual_prefix_begin = domain.begin;
                }
            }
        }
        std::vector<size_t> file_prefix_begin(n + 1, std::u16string_view::npos);
        for (const auto& file : file_spans) {
            const size_t earliest = file.begin >= kMaxSyllableKeys ? file.begin - kMaxSyllableKeys + 1 : 0;
            for (size_t prefix = earliest; prefix < file.begin; ++prefix) {
                if (!chinese_ends[prefix]) continue;
                for (const auto& reading : edges[prefix]) {
                    if (!reading.natural_reading || reading.end <= file.begin || reading.end >= file.end) continue;
                    bool resumes = false;
                    for (size_t end = reading.end + 1; end <= std::min(file.end, reading.end + 32); ++end) {
                        const auto key = raw[end - 1];
                        if (key < u'a' || key > u'z') break;
                        if (end - reading.end >= 3 && onsets[end]) { resumes = true; break; }
                    }
                    if (!resumes) continue;
                    file_prefix_begin[reading.end] = prefix;
                    for (auto& edge : edges[file.begin]) {
                        if (edge.segment.kind == MixedSegmentKind::Latin && edge.end == file.end) {
                            edge.contextual_prefix_begin = prefix;
                        }
                    }
                }
            }
        }
        for (size_t begin = 0; begin < n; ++begin) {
            if (!chinese_ends[begin] || (boundary_protected_spans[begin] &&
                file_prefix_begin[begin] == std::u16string_view::npos)) continue;
            for (size_t end = begin + 1; end <= std::min(n, begin + 32); ++end) {
                if (raw[end - 1] < u'a' || raw[end - 1] > u'z') break;
                if (end - begin < 3 || !onsets[end]) continue;
                const auto word = raw.substr(begin, end - begin);
                if (frequency_ && frequency_(word) > 0) continue;
                bool spans_phonetic_keys = false;
                for (size_t offset = begin; offset < end && !spans_phonetic_keys; ++offset) {
                    spans_phonetic_keys = std::ranges::any_of(edges[offset], [&](const Edge& reading) {
                        if (!reading.natural_reading || reading.end > end ||
                            latin_score(reading.segment.raw, false) >= kLatinBase) return false;
                        if (offset == begin) {
                            const auto spelling = std::u16string_view(reading.segment.raw);
                            const bool short_consonants = spelling.size() <= 3 &&
                                std::ranges::none_of(spelling, [](char16_t key) {
                                    return std::u16string_view(u"aeiouy").find(key) != std::u16string_view::npos;
                                });
                            if (lexicon.english_scoring_frequency(spelling) == 0 || short_consonants) return true;
                        }
                        if (offset >= begin + 3) {
                            // Do not attach the first completed reading of an
                            // anchored phrase to the end of a literal name.
                            if (reading.end == end && reading.phrase_onset) return true;
                            const auto prefix = raw.substr(begin, offset - begin);
                            const auto minimum = prefix.size() >= 4 ? 1.0 : 2.5;
                            if (lexicon.english_scoring_frequency(prefix) >= minimum / 8.0) return true;
                        }
                        // A real technical word after a completed reading is
                        // useful evidence even below hard dictionary cutoff.
                        // Use it only to reject an invented combined island;
                        // it does not establish a new English boundary.
                        for (size_t word_end = reading.end + 3; word_end <= end; ++word_end) {
                            const auto spelling = raw.substr(reading.end, word_end - reading.end);
                            const auto minimum = spelling.size() >= 4 ? 1.0 : 2.5;
                            if (lexicon.english_scoring_frequency(spelling) >= minimum / 8.0) {
                                return true;
                            }
                        }
                        return false;
                    });
                }
                if (spans_phonetic_keys) continue;
                const bool contains_phrase = [&] {
                    for (size_t offset = begin; offset < end; ++offset) {
                        for (const auto& first : edges[offset]) {
                            if (!first.natural_reading || first.end >= end) continue;
                            for (size_t candidate = 0; candidate < std::min<size_t>(4, first.segment.candidates.size()); ++candidate) {
                                const auto next = lexicon.common_chinese_pair_extensions(first.segment.candidates[candidate]);
                                if (std::ranges::any_of(edges[first.end], [&](const Edge& second) {
                                    return second.natural_reading &&
                                        ((first.phrase_onset && second.phrase_onset && second.end > end) ||
                                        std::ranges::any_of(second.segment.candidates, [&](char32_t ch) {
                                            return std::ranges::find(next, ch) != next.end();
                                        }));
                                })) return true;
                            }
                        }
                    }
                    return false;
                }();
                if (contains_phrase) continue;
                const bool absorbs_reading = std::ranges::any_of(edges[begin], [&](const Edge& reading) {
                    if (!reading.natural_reading || reading.end > end) return false;
                    return std::ranges::any_of(preceding_characters[begin], [&](char32_t ch) {
                        const auto next = lexicon.common_chinese_pair_extensions(ch);
                        return std::ranges::any_of(reading.segment.candidates, [&](char32_t candidate) {
                            return std::ranges::find(next, candidate) != next.end();
                        });
                    });
                });
                if (absorbs_reading) continue;
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Latin;
                // Both boundaries already have natural Chinese/phrase
                // evidence. Keep an unseen literal word as one island rather
                // than making its per-letter spelling cost erase a reading or
                // favour a manufactured short dictionary suffix.
                const auto spelling_score = latin_score(word, false);
                segment.score = boundary_protected_spans[begin] ? spelling_score :
                    std::max(kLatinBase, spelling_score);
                add_edge(begin, end, std::move(segment));
                edges[begin].back().literal_island = !boundary_protected_spans[begin];
                edges[begin].back().contextual_prefix_begin = file_prefix_begin[begin];
                edges[begin].back().contextual_evidence = boundary_protected_spans[begin];
            }
        }
        // A malformed island must not absorb an unseen English word followed
        // by a natural reading at a confirmed phrase boundary. The normal
        // literal edge preserves the word; the phonetic edge preserves its
        // following syllable, even when the English dictionary has no entry.
        for (size_t begin = 0; begin < n; ++begin) {
            std::erase_if(edges[begin], [&](const Edge& edge) {
                if (edge.segment.kind != MixedSegmentKind::BopomofoUnresolved) return false;
                for (size_t split = begin + 3; split + 1 < edge.end; ++split) {
                    if (!std::ranges::all_of(raw.substr(begin, split - begin), [](char16_t key) {
                        return key >= u'a' && key <= u'z';
                    })) break;
                    if (std::ranges::any_of(edges[split], [&](const Edge& reading) {
                        return reading.natural_reading && reading.phrase_onset && reading.end == edge.end;
                    })) return true;
                }
                return false;
            });
        }
        // A longest identifier/domain token may hide both boundaries of a
        // literal island. Expose only dictionary-supported structured names
        // followed by a natural, lexically anchored Chinese run. Retain the
        // longest opaque token; these are reversible alternatives, not lexer
        // truncations or a list of special filenames/identifiers.
        const bool structured_syntax = raw.find_first_of(u"._") != std::u16string_view::npos;
        for (size_t begin = 0; structured_syntax && begin < n; ++begin) {
            if (!chinese_ends[begin]) continue;
            for (size_t end = begin + 4; end < std::min(n, begin + 64); ++end) {
                if (!onsets[end]) continue;
                const auto spelling = raw.substr(begin, end - begin);
                const auto delimiter = spelling.find_first_of(u"._");
                if (delimiter == std::u16string_view::npos || delimiter < 2 ||
                    spelling.find_first_of(u"._", delimiter + 1) != std::u16string_view::npos) continue;
                const auto stem = spelling.substr(0, delimiter);
                const auto suffix = spelling.substr(delimiter + 1);
                const auto letters = [](std::u16string_view value) {
                    return std::ranges::all_of(value, [](char16_t key) { return key >= u'a' && key <= u'z'; });
                };
                if (!letters(stem) || !letters(suffix) || suffix.size() < 2 || !frequency_) continue;
                const auto stem_frequency = frequency_(stem);
                if (stem_frequency <= 0) continue;
                if (spelling[delimiter] == u'.' ? suffix.size() > 4 : frequency_(suffix) <= 0) continue;
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Latin;
                segment.score = kStructuredBoost;
                add_edge(begin, end, std::move(segment));
                edges[begin].back().structured_boundary = true;
                has_structured_boundaries = true;
            }
        }
    }

    // Dynamic programming over the DAG, keeping the top-K paths per vertex.
    // Paths are immutable link chains so extending a path is O(1); the full
    // segment list is materialized only for the surviving complete paths.
    struct PathLink;
    using PathLinkPtr = std::shared_ptr<const PathLink>;
    struct PathLink {
        PathLinkPtr prev;
        MixedSegment segment;
        double score;
        std::u16string rendered;
        size_t latin_count = 0;
        bool lexical_island = false;
        bool recovery_piece = false;
        bool literal_island = false;
        unsigned typing_edits = 0;
        bool typing_alternative = false;
    };
    struct Entry {
        double score = 0;
        PathLinkPtr path;
    };
    struct PhraseEdge {
        size_t end;
        std::vector<MixedSegment> segments;
        double score;
        double lexical_bonus = 0;
    };
    std::vector<std::vector<PhraseEdge>> phrases(n + 1);
    std::vector<std::vector<PhraseEdge>> typing_phrases(n + 1);
    std::vector<bool> repaired_singletons(n, false);

    // Exact high-frequency words can use their complete homophone inventory;
    // recovery adds validated physical spellings to the same lexical search.
    // A word trie bounds candidates instead of taking a character Cartesian
    // product. Guesses never become standalone syllable edges in the graph.
    {
        const auto& lexicon = MixedLexicon::instance();
        std::vector<std::vector<MixedSegment>> proposals(n + 1);
        size_t native_prefix_end = 0;
        if (!options.allow_english) {
            Syllable ordinary;
            bool valid = true;
            for (size_t offset = 0; offset < n; ++offset) {
                const auto step = apply_bopomofo_key(ordinary, layout, raw[offset]);
                if (!step.natural_extension || step.status == BopomofoKeyStatus::Rejected) {
                    valid = false;
                    break;
                }
                if (step.status == BopomofoKeyStatus::Completed) {
                    native_prefix_end = offset + 1;
                    ordinary = {};
                }
            }
            if (!valid || ordinary.empty()) native_prefix_end = 0;
        }
        for (size_t begin = 0; begin < n; ++begin) {
            for (const auto& edge : edges[begin]) {
                if (edge.natural_reading) proposals[begin].push_back(edge.segment);
            }
            if (!options.smart_typing || opaque_recovery_spans[begin]) continue;
            auto recovered = recovery_.recover_at(raw, begin, layout);
            const auto separator = raw.find(u' ', begin);
            const auto run_end = separator == std::u16string_view::npos ? n : separator;
            const auto previous_separator = begin == 0 ? std::u16string_view::npos : raw.rfind(u' ', begin - 1);
            const auto run_begin = previous_separator == std::u16string_view::npos ? 0 : previous_separator + 1;
            // Initial-only abbreviations represent at most four characters.
            // Do not search them at every position of a full sentence.
            if (run_end - run_begin <= 4) {
                auto initials = recovery_.recover_initial_at(raw, begin, layout);
                recovered.insert(recovered.end(), std::make_move_iterator(initials.begin()),
                                 std::make_move_iterator(initials.end()));
            }
            for (auto& syllable : recovered) {
                if (syllable.end > n || syllable.end <= begin ||
                    std::ranges::any_of(opaque_recovery_spans.begin() + static_cast<std::ptrdiff_t>(begin),
                                       opaque_recovery_spans.begin() + static_cast<std::ptrdiff_t>(syllable.end),
                                       [](bool value) { return value; })) continue;
                if (syllable.edited && syllable.end <= native_prefix_end) continue;
                // Consume a typed tone with its syllable. Dropping the tone
                // interpretation here lets the same key become a spurious
                // extra character at the next word boundary.
                if (syllable.missing_tone && !syllable.abbreviated && syllable.end < n &&
                    is_explicit_tone_key(raw[syllable.end], layout) &&
                    std::ranges::any_of(edges[begin], [&](const Edge& edge) {
                        return edge.natural_reading && edge.end == syllable.end + 1 &&
                               edge.segment.body_keys == syllable.body_keys;
                    })) continue;
                // A valid completed syllable followed by a new phonetic key
                // is ordinary continued typing. Do not delete that final key
                // as a typo just to retain the preceding word preview.
                if (syllable.edited && syllable.end == n && n > begin + 1) {
                    Syllable onset;
                    const auto next = apply_bopomofo_key(onset, layout, raw.back());
                    if (next.status == BopomofoKeyStatus::Composing && next.natural_extension &&
                        std::ranges::any_of(edges[begin], [&](const Edge& edge) {
                            return edge.natural_reading && edge.end == n - 1 &&
                                   edge.segment.reading == syllable.reading;
                        })) continue;
                }
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Bopomofo;
                segment.begin = begin;
                segment.end = syllable.end;
                segment.raw.assign(raw.substr(begin, syllable.end - begin));
                segment.body_keys = std::move(syllable.body_keys);
                segment.tone_key = syllable.tone_key;
                segment.reading = std::move(syllable.reading);
                segment.candidates = std::move(syllable.candidates);
                segment.inferred = syllable.missing_tone || syllable.edited || syllable.abbreviated;
                segment.missing_tone = syllable.missing_tone;
                segment.abbreviated = syllable.abbreviated;
                segment.recovery_edits = syllable.edited ? 1U : 0U;
                if (options.allow_english && !segment.abbreviated && protects_typing_word(segment)) continue;
                // Omitted tones supply no explicit completion evidence.
                // Awarding it per inferred syllable favours splitting one
                // intended word into several unrelated short readings.
                const bool letter_tone = segment.tone_key >= U'a' && segment.tone_key <= U'z';
                const auto completion = syllable.missing_tone || letter_tone ? 0.0 : 3.4;
                const auto supplied_body_size = syllable.abbreviated ? segment.raw.size() : segment.body_keys.size();
                segment.score = completion + 0.4 * static_cast<double>(supplied_body_size) -
                    syllable.edit_cost - (syllable.missing_tone ? 0.35 : 0.0) -
                    (syllable.abbreviated ? kSwitchPenalty : 0.0) -
                    (syllable.edited ? kTypingEditPenalty : 0.0);
                if (!syllable.abbreviated && std::ranges::any_of(segment.body_keys, [&](char16_t key) {
                    return key > u' ' && key <= u'~' && !(key >= u'a' && key <= u'z') &&
                           !(key >= u'0' && key <= u'9') && segment.raw.find(key) != std::u16string::npos;
                })) {
                    segment.score += phonetic_symbol_evidence(segment.body_keys);
                }
                proposals[begin].push_back(std::move(segment));
            }
            std::stable_sort(proposals[begin].begin(), proposals[begin].end(), [](const auto& a, const auto& b) {
                if (a.abbreviated != b.abbreviated) return !a.abbreviated;
                if (a.recovery_edits != b.recovery_edits) return a.recovery_edits < b.recovery_edits;
                return a.end > b.end;
            });
        }
        struct Choice { const MixedSegment* proposal; size_t candidate; };
        // Index the immutable proposal inventory once. Later word characters
        // come from the lexical trie; scan only their matching homophones,
        // rather than every character of every proposal on each visit.
        std::vector<std::vector<Choice>> initial_choices(n + 1);
        std::vector<std::unordered_map<char32_t, std::vector<Choice>>> character_choices(n + 1);
        std::vector<std::unordered_map<std::u16string, std::vector<Choice>>> continuation_choices(n + 1);
        std::unordered_map<char32_t, bool> word_initials;
        std::vector<bool> prepared_vertices(n + 1, false);
        const auto prepare_vertex = [&](size_t vertex) {
            if (prepared_vertices[vertex]) return;
            prepared_vertices[vertex] = true;
            if (opaque_recovery_spans[vertex]) return;
            for (const auto& proposal : proposals[vertex]) {
                if (options.allow_english && !proposal.abbreviated && protects_typing_word(proposal)) continue;
                for (size_t candidate = 0; candidate < proposal.candidates.size(); ++candidate) {
                    const auto ch = proposal.candidates[candidate];
                    if (ch > 0xFFFF) continue;
                    const Choice choice{&proposal, candidate};
                    character_choices[vertex][ch].push_back(choice);
                    if (candidate >= 16) continue;
                    auto [initial, inserted] = word_initials.try_emplace(ch, false);
                    if (inserted) {
                        initial->second = !lexicon.chinese_phrase_extensions(
                            std::u16string(1, static_cast<char16_t>(ch))).empty();
                    }
                    if (initial->second) initial_choices[vertex].push_back(choice);
                }
            }
        };
        constexpr auto kShortLimit = std::numeric_limits<char16_t>::max();
        const bool cache_words = options.smart_typing && default_chinese_score_ &&
            static_cast<std::uint64_t>(layout) <= kShortLimit;
        const auto append_number = [](std::u16string& key, std::uint64_t value) {
            for (int shift = 0; shift < 64; shift += 16) {
                key.push_back(static_cast<char16_t>((value >> shift) & 0xffffULL));
            }
        };
        const auto append_string = [&](std::u16string& key, std::u16string_view value) {
            key.push_back(static_cast<char16_t>(value.size()));
            key.append(value);
        };
        std::vector<std::u16string> proposal_signatures(n + 1);
        std::vector<std::optional<std::uint64_t>> signature_ids(n + 1);
        std::vector<bool> cacheable(n + 1, cache_words);
        if (cache_words) {
            for (size_t vertex = 0; vertex < n; ++vertex) {
                if (opaque_recovery_spans[vertex]) continue;
                auto& signature = proposal_signatures[vertex];
                for (const auto& proposal : proposals[vertex]) {
                    if (options.allow_english && !proposal.abbreviated && protects_typing_word(proposal)) continue;
                    if (proposal.begin != vertex || proposal.end <= vertex ||
                        proposal.end - vertex > PhoneticRecovery::kMaxRawSpan ||
                        static_cast<std::uint64_t>(proposal.kind) > kShortLimit ||
                        proposal.tone_key > kShortLimit || proposal.recovery_edits > kShortLimit ||
                        proposal.raw.size() > kShortLimit || proposal.body_keys.size() > kShortLimit ||
                        proposal.reading.size() > kShortLimit) {
                        cacheable[vertex] = false;
                        break;
                    }
                    const auto inventory = word_cache_->inventory(proposal.reading, proposal.candidates);
                    if (!inventory) { cacheable[vertex] = false; break; }
                    // Bounded fields use one exact UTF-16 unit. Scores and
                    // inventory identities retain all 64 bits.
                    signature.push_back(static_cast<char16_t>(proposal.begin - vertex));
                    signature.push_back(static_cast<char16_t>(proposal.end - vertex));
                    signature.push_back(static_cast<char16_t>(proposal.kind));
                    append_string(signature, proposal.raw);
                    append_string(signature, proposal.body_keys);
                    append_string(signature, proposal.reading);
                    signature.push_back(static_cast<char16_t>(proposal.tone_key));
                    append_number(signature, std::bit_cast<std::uint64_t>(proposal.score));
                    const auto flags = static_cast<unsigned>(proposal.consumed_boundary) |
                        (static_cast<unsigned>(proposal.inferred) << 1U) |
                        (static_cast<unsigned>(proposal.missing_tone) << 2U) |
                        (static_cast<unsigned>(proposal.abbreviated) << 3U) |
                        (static_cast<unsigned>(proposal.phonetic_display) << 4U);
                    signature.push_back(static_cast<char16_t>(flags));
                    signature.push_back(static_cast<char16_t>(proposal.recovery_edits));
                    append_number(signature, *inventory);
                    if (signature.size() > kShortLimit) { cacheable[vertex] = false; break; }
                }
            }
        }
        for (size_t begin = 0; begin < n; ++begin) {
            // Search reads proposal vectors only at the first four syllable
            // starts. Follow every eligible edge for three steps; unrelated
            // future keys cannot invalidate these deterministic dependencies.
            constexpr size_t kLastStart = 3 * PhoneticRecovery::kMaxRawSpan;
            bool cached_word = cache_words;
            std::u16string cache_key;
            if (cached_word) {
                std::vector<bool> reached(kLastStart + 1, false);
                reached[0] = true;
                std::vector<size_t> vertices{begin}, frontier{begin};
                for (size_t depth = 0; depth < 3 && cached_word; ++depth) {
                    std::vector<size_t> next;
                    for (const auto vertex : frontier) {
                        if (!cacheable[vertex]) { cached_word = false; break; }
                        if (vertex == n || opaque_recovery_spans[vertex]) continue;
                        for (const auto& proposal : proposals[vertex]) {
                            if (options.allow_english && !proposal.abbreviated && protects_typing_word(proposal)) continue;
                            const auto relative = proposal.end - begin;
                            if (relative > kLastStart) { cached_word = false; break; }
                            if (!reached[relative]) {
                                reached[relative] = true;
                                vertices.push_back(proposal.end);
                                next.push_back(proposal.end);
                            }
                        }
                        if (!cached_word) break;
                    }
                    frontier = std::move(next);
                }
                if (cached_word) {
                    std::sort(vertices.begin(), vertices.end());
                    cache_key.reserve(2 + 5 * vertices.size());
                    cache_key.push_back(static_cast<char16_t>(layout));
                    const auto flags = static_cast<unsigned>(space_tone) |
                        (static_cast<unsigned>(options.smart_typing) << 1U) |
                        (static_cast<unsigned>(options.allow_english) << 2U);
                    cache_key.push_back(static_cast<char16_t>(flags));
                    for (const auto vertex : vertices) {
                        if (!cacheable[vertex]) { cached_word = false; break; }
                        if (!signature_ids[vertex]) signature_ids[vertex] = word_cache_->signature(proposal_signatures[vertex]);
                        if (!signature_ids[vertex]) { cached_word = false; break; }
                        cache_key.push_back(static_cast<char16_t>(vertex - begin));
                        append_number(cache_key, *signature_ids[vertex]);
                    }
                    if (cached_word) {
                        if (const auto found = word_cache_->find(cache_key)) {
                            auto& bucket = typing_phrases[begin];
                            bucket.reserve(found->size());
                            for (const auto& saved : *found) {
                                auto segments = saved.segments;
                                for (auto& segment : segments) { segment.begin += begin; segment.end += begin; }
                                bucket.push_back({saved.end + begin, std::move(segments), saved.score, saved.lexical_bonus});
                            }
                            continue;
                        }
                    }
                }
            }
            std::u16string word;
            std::vector<Choice> chain;
            std::unordered_map<std::u16string, double> visited;
            size_t visits = 0;
            const auto visit = [&](auto&& self, size_t vertex, double score, unsigned edits, bool abbreviated) -> void {
                constexpr size_t kMaxPhraseVisits = 4096;
                if (visits >= kMaxPhraseVisits || edits > 1) return;
                ++visits;
                auto state = word;
                state.push_back(static_cast<char16_t>(vertex));
                state.push_back(static_cast<char16_t>(edits));
                state.push_back(static_cast<char16_t>(abbreviated));
                const auto [seen, inserted] = visited.try_emplace(std::move(state), score);
                if (!inserted) {
                    if (score <= seen->second) return;
                    seen->second = score;
                }
                const auto frequency = lexicon.chinese_phrase_frequency(word);
                if (word.size() >= 2 && frequency >= (abbreviated ? 3.0 : 4.0) / 8.0) {
                    auto& bucket = typing_phrases[begin];
                    const auto rank = score + kKnownWordBoost * frequency + kSwitchPenalty / 2.0;
                    const auto existing = std::ranges::find_if(bucket, [&](const auto& item) {
                        if (item.end != vertex || item.segments.size() != chain.size()) return false;
                        for (size_t index = 0; index < chain.size(); ++index) {
                            const auto& choice = chain[index];
                            if (item.segments[index].candidates.front() != choice.proposal->candidates[choice.candidate] ||
                                item.segments[index].reading != choice.proposal->reading ||
                                item.segments[index].abbreviated != choice.proposal->abbreviated) return false;
                        }
                        return true;
                    });
                    const auto bonus = kKnownWordBoost * frequency + kSwitchPenalty / 2.0;
                    if (existing == bucket.end() || rank > existing->score) {
                        std::vector<MixedSegment> segments;
                        segments.reserve(chain.size());
                        for (const auto& choice : chain) {
                            auto segment = *choice.proposal;
                            std::rotate(segment.candidates.begin(),
                                        segment.candidates.begin() + static_cast<std::ptrdiff_t>(choice.candidate),
                                        segment.candidates.end());
                            segments.push_back(std::move(segment));
                        }
                        if (existing == bucket.end()) bucket.push_back({vertex, std::move(segments), rank, bonus});
                        else *existing = {vertex, std::move(segments), rank, bonus};
                    }
                    std::stable_sort(bucket.begin(), bucket.end(), [](const auto& a, const auto& b) {
                        return a.score > b.score;
                    });
                    if (bucket.size() > kTopK) bucket.resize(kTopK);
                }
                if (word.size() == 4 || vertex >= n) return;
                const auto extensions = lexicon.chinese_phrase_extensions(word);
                if (!word.empty() && extensions.empty()) return;
                prepare_vertex(vertex);
                const std::vector<Choice>* choices = &initial_choices[vertex];
                if (!word.empty()) {
                    auto [cached, new_choices] = continuation_choices[vertex].try_emplace(word);
                    if (new_choices) {
                        for (const auto ch : extensions) {
                            const auto found = character_choices[vertex].find(ch);
                            if (found != character_choices[vertex].end()) {
                                cached->second.insert(cached->second.end(), found->second.begin(), found->second.end());
                            }
                        }
                        // Preserve the original proposal/candidate traversal:
                        // tied scores and the bounded visit budget depend on it.
                        std::sort(cached->second.begin(), cached->second.end(), [](const Choice& a, const Choice& b) {
                            if (a.proposal != b.proposal) return a.proposal < b.proposal;
                            return a.candidate < b.candidate;
                        });
                    }
                    choices = &cached->second;
                }
                for (const auto& choice : *choices) {
                    if (visits >= kMaxPhraseVisits) return;
                    const auto& proposal = *choice.proposal;
                    if (edits + proposal.recovery_edits > 1) continue;
                    const auto ch = proposal.candidates[choice.candidate];
                    const auto value = proposal.score + (chinese_score_ ? chinese_score_(word, ch) : -5.0);
                    word.push_back(static_cast<char16_t>(ch));
                    chain.push_back(choice);
                    self(self, proposal.end, score + value, edits + proposal.recovery_edits,
                         abbreviated || proposal.abbreviated);
                    chain.pop_back();
                    word.pop_back();
                }
            };
            visit(visit, begin, 0, 0, false);
            if (cached_word) {
                std::vector<WordCache::Word> saved;
                saved.reserve(typing_phrases[begin].size());
                for (const auto& phrase : typing_phrases[begin]) {
                    auto segments = phrase.segments;
                    for (auto& segment : segments) { segment.begin -= begin; segment.end -= begin; }
                    saved.push_back({phrase.end - begin, std::move(segments), phrase.score, phrase.lexical_bonus});
                }
                word_cache_->insert(std::move(cache_key), std::move(saved));
            }
        }
    }

    const auto insert_entry = [&](std::vector<Entry>& bucket, Entry entry) {
        // Merge only equivalent rendered history and transition state. A
        // literal tail and an unfinished syllable may render identically but
        // carry different language-transition evidence.
        for (auto& existing : bucket) {
            if (existing.path->rendered == entry.path->rendered &&
                existing.path->segment.kind == entry.path->segment.kind &&
                existing.path->latin_count == entry.path->latin_count &&
                existing.path->lexical_island == entry.path->lexical_island &&
                existing.path->recovery_piece == entry.path->recovery_piece &&
                existing.path->literal_island == entry.path->literal_island &&
                existing.path->typing_edits == entry.path->typing_edits &&
                existing.path->typing_alternative == entry.path->typing_alternative) {
                if (entry.score > existing.score) existing = std::move(entry);
                return;
            }
        }
        bucket.push_back(std::move(entry));
        std::sort(bucket.begin(), bucket.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
        if (bucket.size() > kTopK) {
            // Reserve one real, scored literal interpretation even when many
            // homophones fill the beam. A synthetic fixed-score raw fallback
            // must never decide the language of a long composition.
            const auto literal = std::find_if(bucket.begin(), bucket.end(), [&](const Entry& candidate) {
                return candidate.path->rendered == raw.substr(0, candidate.path->segment.end);
            });
            if (literal != bucket.end() && literal >= bucket.begin() + static_cast<std::ptrdiff_t>(kTopK)) {
                auto retained = *literal;
                bucket.resize(kTopK - 1);
                bucket.push_back(std::move(retained));
            } else {
                bucket.resize(kTopK);
            }
        }
    };

    std::unordered_map<std::u16string, bool> inferred_body_support;
    const auto extend = [&](const Entry& entry, MixedSegment segment, size_t candidate,
                            bool lexical_word, bool ordinal_prior, bool recovery_piece = false,
                            bool phrase_onset = false, bool literal_island = false,
                            size_t contextual_prefix_begin = std::u16string_view::npos,
                            bool contextual_evidence = false, double lexical_bonus = 0) {
        auto previous = entry.path;
        const auto typing_edits = (previous ? previous->typing_edits : 0U) + segment.recovery_edits;
        const bool typing_alternative = (previous && previous->typing_alternative) || segment.abbreviated;
        if (contextual_prefix_begin != std::u16string_view::npos && previous && previous->prev &&
            previous->segment.kind == MixedSegmentKind::Bopomofo &&
            previous->segment.begin == contextual_prefix_begin && previous->segment.end == segment.begin &&
            previous->prev->segment.kind == MixedSegmentKind::Bopomofo) {
            literal_island = true;
            segment.score = std::max(kLatinBase, segment.score);
            if (contextual_evidence) segment.score += kSwitchPenalty / 2.0;
        }
        if (contextual_prefix_begin != std::u16string_view::npos && previous && previous->prev &&
            previous->segment.kind == MixedSegmentKind::Symbol &&
            previous->segment.begin == contextual_prefix_begin && previous->segment.end == segment.begin &&
            previous->prev->segment.kind == MixedSegmentKind::Bopomofo) {
            // An apparent path starts inside a naturally completed phonetic
            // prefix and covers the next Chinese phrase. In this particular
            // history it gets the ordinary spelling prior, not opaque syntax
            // protection. Real paths retain their usual score and raw choice.
            segment.score = latin_score(segment.raw, false);
        }
        if (segment.kind == MixedSegmentKind::Latin && previous &&
            previous->segment.consumed_boundary && previous->segment.raw == u" ") {
            // A separate Space after a completed reading confirms Chinese
            // while it is the tail. Once English follows, that same key is a
            // word separator. A first-tone Space belongs to its reading and
            // never reaches this separate boundary edge.
            auto boundary = previous->segment;
            boundary.consumed_boundary = false;
            auto rendered = previous->rendered + u" ";
            previous = std::make_shared<PathLink>(PathLink{
                previous->prev, std::move(boundary), previous->score, std::move(rendered),
                previous->latin_count, previous->lexical_island, previous->recovery_piece,
                previous->literal_island, previous->typing_edits, previous->typing_alternative});
        }
        double score = entry.score + segment.score + lexical_bonus;
        // Six-key pieces bound parsing work, not the number of independent
        // errors. A contiguous unresolved run pays its cost once; repeatedly
        // charging that cost makes a long local typo erase earlier Chinese.
        // Legal completed readings remain separate edges and end the run.
        if (segment.kind == MixedSegmentKind::BopomofoUnresolved && previous &&
            previous->recovery_piece) score -= segment.score;
        if (segment.raw == u" " && previous && previous->segment.kind == MixedSegmentKind::Bopomofo) {
            segment.consumed_boundary = true;
            score += 0.01;
        }
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            const bool splits_word =
                (segment.begin > 0 && word_spans[segment.begin - 1] && word_spans[segment.begin]) ||
                (segment.end < n && word_spans[segment.end - 1] && word_spans[segment.end]);
            if (splits_word && (!segment.inferred || protects_typing_word(segment))) score -= kFragmentPenalty;
            if (segment.begin != 0 && previous && previous->segment.kind == MixedSegmentKind::Bopomofo) {
                score += phonetic_symbol_evidence(segment.body_keys);
                if (segment.tone_key > U' ' && segment.tone_key <= U'~' &&
                    !(segment.tone_key >= U'a' && segment.tone_key <= U'z') &&
                    !(segment.tone_key >= U'0' && segment.tone_key <= U'9')) {
                    // A punctuation tone continues established Chinese; it
                    // need not turn the reading plus its English neighbour
                    // into a domain (for example on the IBM keyboard).
                    score += kSwitchPenalty / 2.0;
                }
            }
            std::u16string history(context.substr(context.size() > 3 ? context.size() - 3 : 0));
            if (previous) {
                const auto& text = previous->rendered;
                history += text.substr(text.size() > 3 ? text.size() - 3 : 0);
            }
            score += chinese_score_ ? chinese_score_(history, segment.candidates[candidate]) : -5.0;
            if (ordinal_prior) score -= 1.5 * static_cast<double>(candidate);
            std::rotate(segment.candidates.begin(), segment.candidates.begin() +
                        static_cast<std::ptrdiff_t>(candidate), segment.candidates.end());
        }
        const size_t prev_latin = previous ? previous->latin_count : 0;
        // A known English word inside Chinese is one literal island: entry
        // already paid the switch penalty. Opaque tokens retain their costs.
        const bool resume_after_unknown = phrase_onset && previous && previous->prev &&
            previous->literal_island &&
            previous->segment.kind == MixedSegmentKind::Latin &&
            previous->prev->segment.kind == MixedSegmentKind::Bopomofo &&
            previous->segment.raw.size() >= 3 &&
            std::ranges::all_of(previous->segment.raw, [](char16_t key) { return key >= u'a' && key <= u'z'; });
        const bool resume_after_word = previous && (previous->lexical_island || resume_after_unknown) &&
            (segment.kind == MixedSegmentKind::Bopomofo || segment.kind == MixedSegmentKind::BopomofoIncomplete);
        if (resume_after_unknown) score += kSwitchPenalty / 2.0;
        if (previous && previous->segment.kind != segment.kind &&
            previous->segment.kind != MixedSegmentKind::Symbol && segment.kind != MixedSegmentKind::Symbol &&
            !(previous->segment.kind == MixedSegmentKind::Bopomofo &&
              (segment.kind == MixedSegmentKind::BopomofoIncomplete || segment.kind == MixedSegmentKind::BopomofoUnresolved)) &&
            !(previous->segment.kind == MixedSegmentKind::BopomofoUnresolved &&
              (segment.kind == MixedSegmentKind::Bopomofo || segment.kind == MixedSegmentKind::BopomofoIncomplete))) {
            if (!resume_after_word) score -= kSwitchPenalty;
            if ((previous->segment.kind == MixedSegmentKind::Latin && previous->segment.raw.size() == 1) ||
                (segment.kind == MixedSegmentKind::Latin && segment.raw.size() == 1)) score -= 1.0;
        }
        // Each additional adjacent Latin segment pays fragmentation once.
        if (previous && previous->segment.kind == MixedSegmentKind::Latin &&
            segment.kind == MixedSegmentKind::Latin) score -= kFragmentPenalty;
        if (previous && previous->segment.missing_tone && segment.missing_tone &&
            !previous->segment.abbreviated && !segment.abbreviated) {
            // A compact layout may naturally extend/cycle these same keys
            // within one syllable. Splitting them into two inferred readings
            // needs stronger evidence than a genuine phonetic boundary.
            Syllable combined;
            bool natural = true;
            const auto replay_body = [&](std::u16string_view keys) {
                for (const auto key : keys) {
                    const auto step = apply_bopomofo_key(combined, layout, key);
                    if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension) {
                        natural = false;
                        break;
                    }
                }
            };
            replay_body(previous->segment.body_keys);
            if (natural) replay_body(segment.body_keys);
            if (natural) {
                const auto body = combined.text();
                const auto [supported, inserted] = inferred_body_support.try_emplace(body, false);
                if (inserted) {
                    for (const char16_t tone : std::u16string_view(u" ˊˇˋ˙")) {
                        if (!lookup_(body + tone).empty()) { supported->second = true; break; }
                    }
                }
                if (supported->second) score -= kFragmentPenalty;
            }
        }
        if (previous && previous->segment.inferred && segment.kind == MixedSegmentKind::BopomofoUnresolved) {
            // Recovery words must compete with the actual spelling cost of
            // a raw tail, not a cheap malformed-phonetic placeholder.
            score += std::min(-kSwitchPenalty, latin_score(segment.raw, false) - segment.score);
        }
        std::u16string rendered = previous ? previous->rendered : std::u16string();
        rendered += render_segment(segment);
        const size_t latin_count = prev_latin + (segment.kind == MixedSegmentKind::Latin ? 1 : 0);
        const bool lexical_island = lexical_word && previous && previous->segment.kind == MixedSegmentKind::Bopomofo;
        auto link = std::make_shared<PathLink>(
            PathLink{std::move(previous), std::move(segment), score, std::move(rendered), latin_count,
                     lexical_island, recovery_piece, literal_island, typing_edits, typing_alternative});
        return Entry{score, std::move(link)};
    };

    const auto exact_word_surroundings = [&](size_t begin, size_t end) {
        const auto english_words = [&](std::u16string_view spelling) {
            while (!spelling.empty() && spelling.front() == u' ') spelling.remove_prefix(1);
            while (!spelling.empty() && spelling.back() == u' ') spelling.remove_suffix(1);
            while (!spelling.empty()) {
                const auto separator = spelling.find(u' ');
                const auto word = spelling.substr(0, separator);
                const bool known = (frequency_ && frequency_(word) > 0) ||
                    (default_latin_score_ && MixedLexicon::instance().english_scoring_frequency(word) >= 2.0 / 8.0);
                if (!known ||
                    !std::ranges::all_of(word, [](char16_t key) {
                        return (key >= u'a' && key <= u'z') || (key >= u'A' && key <= u'Z');
                    })) return false;
                if (separator == std::u16string_view::npos) break;
                spelling.remove_prefix(separator + 1);
                while (!spelling.empty() && spelling.front() == u' ') spelling.remove_prefix(1);
            }
            return true;
        };
        // Complete-word inventory supplements short Chinese compositions and
        // confirmed English neighbours. It must not rerank interior words of
        // a mature Chinese sentence or silently reinterpret opaque syntax.
        return english_words(raw.substr(0, begin)) && english_words(raw.substr(end));
    };

    const auto search = [&](bool lexical_spans) {
        std::vector<std::vector<Entry>> buckets(n + 1);
        buckets[0].push_back({0, nullptr});
        for (size_t v = 0; v < n; ++v) {
            for (const auto& edge : edges[v]) {
                // An opaque spelling and Chinese may share the same bytes.
                // Exposing a repair must not silently reclassify identifiers.
                if (edge.structured_boundary) continue;
                // Once a complete dictionary span accounts for a stranded
                // singleton in a Chinese composition, compare its reading
                // interpretations, not layout-dependent English-letter priors.
                if (lexical_spans && edge.segment.kind != MixedSegmentKind::Bopomofo &&
                    std::ranges::any_of(repaired_singletons.begin() + static_cast<std::ptrdiff_t>(v),
                                        repaired_singletons.begin() + static_cast<std::ptrdiff_t>(edge.end),
                                        [](bool value) { return value; })) continue;
                for (const auto& entry : buckets[v]) {
                    if (edge.segment.kind == MixedSegmentKind::BopomofoUnresolved &&
                        (!entry.path || (entry.path->segment.kind != MixedSegmentKind::Bopomofo &&
                                         entry.path->segment.kind != MixedSegmentKind::BopomofoUnresolved))) continue;
                    const size_t alternatives = edge.segment.kind == MixedSegmentKind::Bopomofo ?
                                                std::min<size_t>(4, edge.segment.candidates.size()) : 1;
                    for (size_t candidate = 0; candidate < alternatives; ++candidate) {
                        insert_entry(buckets[edge.end], extend(entry, edge.segment, candidate, edge.lexical_word, true,
                                                               edge.recovery_piece, edge.phrase_onset, edge.literal_island,
                                                               edge.contextual_prefix_begin, edge.contextual_evidence));
                    }
                }
            }
            for (const auto& phrase : typing_phrases[v]) {
                const bool inferred_word = std::ranges::any_of(phrase.segments,
                    [](const auto& segment) { return segment.inferred; });
                if (!inferred_word && !exact_word_surroundings(v, phrase.end)) continue;
                for (const auto& entry : buckets[v]) {
                    auto next = entry;
                    for (size_t index = 0; index < phrase.segments.size(); ++index) {
                        next = extend(next, phrase.segments[index], 0, false, false, false, false, false,
                                      std::u16string_view::npos, false,
                                      index + 1 == phrase.segments.size() ? phrase.lexical_bonus : 0);
                        if (next.path->typing_edits > 1) break;
                    }
                    if (next.path->typing_edits <= 1) insert_entry(buckets[phrase.end], std::move(next));
                }
            }
            if (lexical_spans) {
                for (const auto& phrase : phrases[v]) {
                    for (const auto& entry : buckets[v]) {
                        auto next = entry;
                        for (const auto& segment : phrase.segments) next = extend(next, segment, 0, false, false);
                        insert_entry(buckets[phrase.end], std::move(next));
                    }
                }
            }
        }
        return buckets;
    };

    auto dp = search(false);
    const auto raw_entry = std::ranges::find_if(dp[n], [&](const auto& entry) {
        return entry.path->rendered == raw;
    });
    const auto literal = raw_entry == dp[n].end() ? std::optional<Entry>{} : *raw_entry;
    // An exact literal winner is evidence of ambiguous or English intent.
    // Lexical spans repair reading coverage only after the ordinary graph has
    // already selected complete Chinese, without reclassifying raw winners.
    bool chinese_anchor = false;
    std::vector<bool> singleton_literal(n, false);
    std::vector<bool> hsu_fragment(n, false);
    std::vector<MixedSegment> anchored_readings;
    std::vector<bool> protected_raw(n, false);
    if (!dp[n].empty() && (!literal || dp[n].front().score > literal->score)) {
        for (auto link = dp[n].front().path; link; link = link->prev) {
            const auto& segment = link->segment;
            if (segment.kind == MixedSegmentKind::Bopomofo) {
                chinese_anchor = true;
                anchored_readings.push_back(segment);
            }
            // Repair a complete singleton first-tone reading currently split
            // into one literal letter and a Space. Do not reinterpret words,
            // opaque tokens, malformed raw islands or unfinished readings.
            const bool singleton = (segment.kind == MixedSegmentKind::Latin || segment.kind == MixedSegmentKind::Symbol) &&
                segment.raw.size() == 1 && segment.end < n && raw[segment.end] == u' ' &&
                !replay(segment.raw, U' ').empty();
            if (singleton) singleton_literal[segment.begin] = true;
            // Letter tones can make a short dictionary abbreviation look
            // cheaper than a reading inside established Chinese. Admit its
            // ordinary reading only as part of a dictionary phrase overlapping
            // the already selected Chinese; complete words/opaque syntax keep
            // their protection and the literal path is never discarded.
            bool fragment = layout == BopomofoKeyboardLayout::Hsu &&
                segment.kind == MixedSegmentKind::Latin && segment.raw.size() >= 2 &&
                segment.raw.size() <= 3 && frequency_ &&
                frequency_(segment.raw) < kStrongPrefixFrequency;
            if (fragment) {
                fragment = std::ranges::any_of(edges[segment.begin], [&](const auto& edge) {
                    return edge.segment.kind == MixedSegmentKind::Bopomofo && edge.end >= segment.end;
                });
            }
            if (fragment) std::fill(hsu_fragment.begin() + static_cast<std::ptrdiff_t>(segment.begin),
                                    hsu_fragment.begin() + static_cast<std::ptrdiff_t>(segment.end), true);
            const bool protect = segment.kind != MixedSegmentKind::Bopomofo &&
                !singleton && !fragment &&
                !(segment.kind == MixedSegmentKind::Symbol && segment.raw == u" ");
            if (protect) std::fill(protected_raw.begin() + static_cast<std::ptrdiff_t>(segment.begin),
                                  protected_raw.begin() + static_cast<std::ptrdiff_t>(segment.end), true);
        }
    }
    if (chinese_anchor && (std::ranges::any_of(singleton_literal, [](bool value) { return value; }) ||
                          std::ranges::any_of(hsu_fragment, [](bool value) { return value; }))) {
        const auto& lexicon = MixedLexicon::instance();
        for (size_t begin = 0; begin < n; ++begin) {
            std::u16string word;
            std::vector<MixedSegment> chain;
            const auto visit = [&](auto&& self, size_t vertex, double score) -> void {
                const bool singleton = std::ranges::any_of(singleton_literal.begin() + static_cast<std::ptrdiff_t>(begin),
                                         singleton_literal.begin() + static_cast<std::ptrdiff_t>(vertex),
                                         [](bool value) { return value; });
                const bool fragment = std::ranges::any_of(hsu_fragment.begin() + static_cast<std::ptrdiff_t>(begin),
                                         hsu_fragment.begin() + static_cast<std::ptrdiff_t>(vertex),
                                         [](bool value) { return value; }) &&
                    lexicon.chinese_phrase_frequency(word) >= 4.0 / 8.0 &&
                    std::ranges::any_of(chain, [&](const auto& segment) {
                        // A nearby Chinese byte range is not enough: preserve
                        // one complete selected reading AND its character.
                        // Otherwise an acronym next to 正常 could manufacture
                        // a new word by borrowing 正's onset or changing its
                        // homophone. Weak names/phrases are not repair evidence.
                        return std::ranges::any_of(anchored_readings, [&](const auto& anchor) {
                            return segment.begin == anchor.begin && segment.end == anchor.end &&
                                segment.reading == anchor.reading &&
                                segment.candidates.front() == anchor.candidates.front();
                        });
                    });
                if (word.size() >= 2 && lexicon.is_chinese_phrase(word) && (singleton || fragment)) {
                    auto& bucket = phrases[begin];
                    bucket.push_back({vertex, chain, score});
                    std::stable_sort(bucket.begin(), bucket.end(), [](const auto& a, const auto& b) {
                        return a.score > b.score;
                    });
                    if (bucket.size() > kTopK) bucket.resize(kTopK);
                }
                if (word.size() == 4) return;
                const auto extensions = lexicon.chinese_phrase_extensions(word);
                if (!word.empty() && extensions.empty()) return;
                for (const auto& edge : edges[vertex]) {
                    if (edge.segment.kind != MixedSegmentKind::Bopomofo) continue;
                    if (std::ranges::any_of(protected_raw.begin() + static_cast<std::ptrdiff_t>(vertex),
                                            protected_raw.begin() + static_cast<std::ptrdiff_t>(edge.end),
                                            [](bool value) { return value; })) continue;
                    for (size_t candidate = 0; candidate < edge.segment.candidates.size(); ++candidate) {
                        const auto ch = edge.segment.candidates[candidate];
                        if (ch > 0xFFFF || (!word.empty() && std::ranges::find(extensions, ch) == extensions.end())) continue;
                        auto segment = edge.segment;
                        std::rotate(segment.candidates.begin(), segment.candidates.begin() +
                                    static_cast<std::ptrdiff_t>(candidate), segment.candidates.end());
                        // A dictionary word is a lexical unit, ranked by the
                        // existing lexical scores rather than character-table
                        // ordinals. Keep every underlying reading for replay,
                        // editing and the same single-path model coordinator.
                        const double value = segment.score + (chinese_score_ ? chinese_score_(word, ch) : -5.0);
                        word.push_back(static_cast<char16_t>(ch));
                        chain.push_back(std::move(segment));
                        self(self, edge.end, score + value);
                        chain.pop_back();
                        word.pop_back();
                    }
                }
            };
            visit(visit, begin, 0);
        }
        if (std::ranges::any_of(phrases, [](const auto& bucket) { return !bucket.empty(); })) {
            for (size_t begin = 0; begin < n; ++begin) {
                for (const auto& phrase : phrases[begin]) {
                    for (size_t index = begin; index < phrase.end; ++index) {
                        if (singleton_literal[index]) repaired_singletons[index] = true;
                    }
                }
            }
            // Retain the ordinary graph's scored exact raw alternative even
            // when the bounded repair search filters its literal subedges.
            dp = search(true);
            if (literal) insert_entry(dp[n], *literal);
        }
    }

    // Keep one literal structured island between natural Chinese runs in a
    // separate bounded search. Ordinary top-K pruning otherwise loses this
    // interpretation to many cheap, longest opaque spellings. This search
    // changes candidate availability, not the automatic winner or its score.
    const auto boundary_repairs = [&] {
        if (!has_structured_boundaries) return std::vector<Entry>{};
        std::vector<std::vector<Entry>> boundary_prefix(n + 1), boundary_suffix(n + 1);
        boundary_prefix[0].push_back({0, nullptr});
        for (size_t vertex = 0; vertex < n; ++vertex) {
            if (boundary_prefix[vertex].empty() && boundary_suffix[vertex].empty()) continue;
            for (const auto& edge : edges[vertex]) {
                if (edge.structured_boundary) {
                    for (const auto& entry : boundary_prefix[vertex]) {
                        if (!entry.path) continue;
                        insert_entry(boundary_suffix[edge.end], extend(entry, edge.segment, 0, true, true));
                    }
                } else if (edge.natural_reading) {
                    const size_t alternatives = std::min<size_t>(4, edge.segment.candidates.size());
                    for (size_t candidate = 0; candidate < alternatives; ++candidate) {
                        for (const auto& entry : boundary_prefix[vertex]) {
                            insert_entry(boundary_prefix[edge.end], extend(entry, edge.segment, candidate, false, true));
                        }
                        for (const auto& entry : boundary_suffix[vertex]) {
                            insert_entry(boundary_suffix[edge.end], extend(entry, edge.segment, candidate, false, true));
                        }
                    }
                }
            }
            // After the literal island, rank real dictionary pairs as units
            // rather than filling the panel with table-order homophones. The
            // lexicon index bounds extensions; no full homophone product and
            // no model inference is needed just to offer the repaired path.
            if (boundary_suffix[vertex].empty()) continue;
            std::vector<PhraseEdge> pairs;
            const auto& lexicon = MixedLexicon::instance();
            for (const auto& first : edges[vertex]) {
                if (!first.natural_reading || first.end == n) continue;
                for (size_t candidate = 0; candidate < std::min<size_t>(16, first.segment.candidates.size()); ++candidate) {
                    const auto ch = first.segment.candidates[candidate];
                    if (ch > 0xFFFF) continue;
                    std::u16string word(1, static_cast<char16_t>(ch));
                    for (const auto next : lexicon.chinese_phrase_extensions(word)) {
                        if (next > 0xFFFF) continue;
                        word.push_back(static_cast<char16_t>(next));
                        const bool phrase = lexicon.chinese_phrase_frequency(word) >= 3.0 / 8.0;
                        word.pop_back();
                        if (!phrase) continue;
                        for (const auto& second : edges[first.end]) {
                            if (!second.natural_reading) continue;
                            const auto found = std::ranges::find(second.segment.candidates, next);
                            if (found == second.segment.candidates.end()) continue;
                            auto a = first.segment;
                            auto b = second.segment;
                            std::rotate(a.candidates.begin(), a.candidates.begin() + static_cast<std::ptrdiff_t>(candidate), a.candidates.end());
                            std::rotate(b.candidates.begin(), b.candidates.begin() + (found - second.segment.candidates.begin()), b.candidates.end());
                            pairs.push_back({second.end, {std::move(a), std::move(b)}, 0});
                        }
                    }
                }
            }
            for (const auto& pair : pairs) {
                for (const auto& entry : boundary_suffix[vertex]) {
                    auto next = entry;
                    for (const auto& segment : pair.segments) next = extend(next, segment, 0, false, false);
                    insert_entry(boundary_suffix[pair.end], std::move(next));
                }
            }
        }
        return std::move(boundary_suffix[n]);
    }();

    const auto materialize = [](PathLinkPtr link) {
        std::vector<MixedSegment> segments;
        std::vector<const PathLink*> chain;
        for (auto current = link.get(); current; current = current->prev.get()) chain.push_back(current);
        segments.reserve(chain.size());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) segments.push_back((*it)->segment);
        return segments;
    };

    MixedDecodeResult result;
    result.raw.assign(raw);

    // The exact raw ASCII path is always present and always listed first.
    MixedPath raw_path;
    // Score raw content using the same graph as all other interpretations.
    // It remains available even if pruned out of the beam.
    raw_path.score = -std::numeric_limits<double>::infinity();
    MixedSegment raw_segment;
    raw_segment.kind = MixedSegmentKind::Symbol;
    raw_segment.begin = 0;
    raw_segment.end = n;
    raw_segment.raw.assign(raw);
    raw_path.segments.push_back(std::move(raw_segment));
    raw_path.rendered.assign(raw);
    result.paths.push_back(std::move(raw_path));

    std::sort(dp[n].begin(), dp[n].end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });
    std::unordered_set<std::u16string> seen;
    seen.insert(result.paths.front().rendered);
    auto english_raw = raw;
    while (!english_raw.empty() && english_raw.back() == u' ') english_raw.remove_suffix(1);
    const bool alphabetic_raw = std::ranges::all_of(english_raw, [](char16_t key) {
        return key >= u'a' && key <= u'z';
    });
    const bool consonant_acronym = alphabetic_raw && english_raw.size() >= 4 &&
        english_raw.find_first_of(u"aeiou") == std::u16string_view::npos &&
        std::ranges::none_of(english_raw, [layout](char16_t key) {
            return is_explicit_tone_key(key, layout);
        });
    const auto ambiguous_english_recovery = [&](const std::vector<MixedSegment>& segments) {
        if (!options.allow_english || !alphabetic_raw || segments.empty()) return false;
        const auto unnatural_reading = [&](const MixedSegment& segment) {
            if (segment.kind != MixedSegmentKind::Bopomofo) return false;
            Syllable syllable;
            for (const auto key : segment.body_keys) {
                const auto step = apply_bopomofo_key(syllable, layout, key);
                if (step.status != BopomofoKeyStatus::Composing || !step.natural_extension) return true;
            }
            const auto completion = apply_bopomofo_key(syllable, layout, segment.tone_key);
            return completion.status != BopomofoKeyStatus::Completed ||
                !completion.natural_extension || syllable.text() != segment.reading;
        };
        if (consonant_acronym && std::ranges::any_of(segments, [&](const auto& segment) {
            return segment.recovery_edits != 0 || unnatural_reading(segment);
        })) return true;
        const auto& prefix = segments.front();
        if (prefix.kind != MixedSegmentKind::Latin || prefix.end < 3 ||
            prefix.end > english_raw.size() || english_raw.size() - prefix.end < 3 ||
            !frequency_ || frequency_(prefix.raw) <= 0) return false;
        // An ordinary English prefix plus a supported English suffix can be
        // an unseen compound. Its Chinese interpretation remains selectable,
        // including when Space can complete a reading inside that suffix.
        return MixedLexicon::instance().english_scoring_frequency(english_raw.substr(prefix.end)) >= 2.0 / 8.0 &&
            std::ranges::any_of(segments, [](const auto& segment) {
                return segment.kind == MixedSegmentKind::Bopomofo;
            });
    };
    for (auto& entry : dp[n]) {
        if (entry.path->rendered == raw) {
            if (entry.score > result.paths[0].score) {
                result.paths[0].score = entry.score;
                result.paths[0].segments = materialize(entry.path);
            }
            continue;
        }
        if (!seen.insert(entry.path->rendered).second) continue;
        MixedPath path;
        path.segments = materialize(entry.path);
        path.rendered = entry.path->rendered;
        path.score = entry.score;
        path.inferred_alternative = entry.path->typing_alternative || ambiguous_english_recovery(path.segments);
        result.paths.push_back(std::move(path));
    }
    if (!boundary_repairs.empty() && seen.insert(boundary_repairs.front().path->rendered).second) {
        const auto& entry = boundary_repairs.front();
        MixedPath path;
        path.segments = materialize(entry.path);
        path.rendered = entry.path->rendered;
        path.score = entry.score;
        path.boundary_alternative = true;
        result.paths.push_back(std::move(path));
    }
    std::stable_sort(result.paths.begin() + 1, result.paths.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });
    for (size_t i = 1; i < result.paths.size(); ++i) {
        if (!result.paths[i].boundary_alternative && !result.paths[i].inferred_alternative &&
            result.paths[i].score > result.paths[result.best_path].score) {
            result.best_path = i;
        }
    }
    if (!options.allow_english) {
        // Preserve ordinary Chinese intent while keeping the same reversible
        // raw channel. A valid natural editor state is a visible fallback;
        // complete lexical recovery paths can still improve its word choices.
        Syllable syllable;
        MixedPath ordinary;
        size_t start = 0;
        bool valid = true;
        for (size_t offset = 0; offset < n; ++offset) {
            if (raw[offset] == u' ' && syllable.empty()) {
                MixedSegment boundary;
                boundary.begin = offset;
                boundary.end = offset + 1;
                boundary.raw = u" ";
                boundary.consumed_boundary = true;
                ordinary.segments.push_back(std::move(boundary));
                start = offset + 1;
                continue;
            }
            const auto step = apply_bopomofo_key(syllable, layout, raw[offset]);
            if (step.status == BopomofoKeyStatus::Rejected || !step.natural_extension) { valid = false; break; }
            if (step.status != BopomofoKeyStatus::Completed) continue;
            MixedSegment segment;
            segment.kind = MixedSegmentKind::Bopomofo;
            segment.begin = start;
            segment.end = offset + 1;
            segment.raw.assign(raw.substr(start, segment.end - start));
            segment.body_keys.assign(raw.substr(start, offset - start));
            segment.tone_key = raw[offset];
            segment.reading = syllable.text();
            segment.candidates = lookup_(segment.reading);
            if (segment.candidates.empty()) { valid = false; break; }
            ordinary.segments.push_back(std::move(segment));
            syllable = {};
            start = offset + 1;
        }
        if (valid && !syllable.empty()) {
            MixedSegment tail;
            tail.kind = MixedSegmentKind::BopomofoIncomplete;
            tail.begin = start;
            tail.end = n;
            tail.raw.assign(raw.substr(start));
            tail.reading = syllable.text();
            tail.phonetic_display = true;
            ordinary.segments.push_back(std::move(tail));
        }
        if (valid && !ordinary.segments.empty()) {
            for (const auto& segment : ordinary.segments) ordinary.rendered += render_segment(segment);
            const auto same = std::ranges::find_if(result.paths, [&](const auto& path) { return path.rendered == ordinary.rendered; });
            if (same == result.paths.end()) {
                ordinary.score = result.paths[result.best_path].score;
                result.paths.push_back(std::move(ordinary));
            }
        }
        std::optional<size_t> chinese;
        for (size_t index = 1; index < result.paths.size(); ++index) {
            const auto& path = result.paths[index];
            if (path.inferred_alternative || path.boundary_alternative || !std::ranges::all_of(path.segments, [](const auto& segment) {
                return segment.kind == MixedSegmentKind::Bopomofo ||
                    segment.kind == MixedSegmentKind::BopomofoIncomplete || segment.consumed_boundary;
            })) continue;
            if (!chinese || path.score > result.paths[*chinese].score) chinese = index;
        }
        if (chinese) result.best_path = *chinese;
    }
    return result;
}

std::vector<MixedCandidateEntry> MixedInputDecoder::expand_candidates(
    const MixedDecodeResult& result, size_t page_size, std::optional<size_t> preferred_path) const {
    std::vector<MixedCandidateEntry> entries;
    if (page_size == 0 || result.paths.empty()) return entries;

    std::unordered_set<std::u16string> seen;
    const auto add_path = [&](size_t index) {
        if (index >= result.paths.size() || entries.size() >= page_size) return;
        const auto& path = result.paths[index];
        if (seen.insert(path.rendered).second) entries.push_back({index, 0, path.rendered});
    };
    add_path(preferred_path.value_or(result.best_path));
    add_path(0);  // Always keep the reversible raw choice within easy reach.
    for (size_t i = 1; i < result.paths.size(); ++i) {
        if (result.paths[i].boundary_alternative && !result.paths[i].boundary_model_refined) add_path(i);
    }
    size_t inferred_count = 0;
    for (size_t index = 1; index < result.paths.size() && inferred_count < 2; ++index) {
        if (result.paths[index].inferred_alternative && !result.paths[index].inferred_model_refined) {
            add_path(index);
            ++inferred_count;
        }
    }
    // Keep the existing repair's selection key stable while a model result
    // arrives. A new suggestion is additional, never a row replacement.
    for (size_t i = 1; i < result.paths.size(); ++i) {
        if (result.paths[i].boundary_model_refined || result.paths[i].inferred_model_refined) add_path(i);
    }
    for (size_t i = 1; i < result.paths.size(); ++i) add_path(i);

    // Fill remaining rows with homophones of the leading paths. Alternative
    // segmentations and earlier-character choices are already lattice paths.
    for (size_t index = 1; index < result.paths.size() && entries.size() < page_size; ++index) {
        const auto& path = result.paths[index];
        if (path.segments.empty() || path.segments.back().kind != MixedSegmentKind::Bopomofo) continue;
        std::u16string prefix;
        for (size_t i = 0; i + 1 < path.segments.size(); ++i) prefix += render_segment(path.segments[i]);
        const auto& tail = path.segments.back();
        for (size_t candidate = 1; candidate < tail.candidates.size() && entries.size() < page_size; ++candidate) {
            auto text = prefix;
            append_codepoint(text, tail.candidates[candidate]);
            if (seen.insert(text).second) entries.push_back({index, candidate, std::move(text)});
        }
    }
    return entries;
}

}  // namespace llavon::ime
