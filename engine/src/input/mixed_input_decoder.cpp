#include "input/mixed_input_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <optional>
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
// Zipf >= 5 means a very common completion; weak dictionary prefixes alone
// must not override an already complete compact reading.
constexpr double kHsuStrongPrefixFrequency = 5.0 / 8.0;

double first_tone_symbol_evidence(std::u16string_view body) {
    // Only an explicitly completed leading syllable gets this evidence, never
    // an unfinished English prefix or a command option after a literal prefix.
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
    if (segment.kind == MixedSegmentKind::Bopomofo && !segment.candidates.empty()) {
        std::u16string text;
        append_codepoint(text, segment.candidates.front());
        return text;
    }
    return segment.raw;
}

}  // namespace

MixedInputDecoder::MixedInputDecoder(LookupFn lookup, FrequencyFn frequency,
                                     LatinScoreFn latin_score, ChineseScoreFn chinese_score)
    : lookup_(std::move(lookup)), frequency_(std::move(frequency)),
      latin_score_(std::move(latin_score)), chinese_score_(std::move(chinese_score)) {
    if (!latin_score_) latin_score_ = [](std::u16string_view word, bool prefix) {
        return MixedLexicon::instance().english_score(word, prefix);
    };
    if (!chinese_score_) chinese_score_ = [](std::u16string_view history, char32_t ch) {
        return MixedLexicon::instance().chinese_score(history, ch);
    };
}

MixedDecodeResult MixedInputDecoder::decode(std::u16string_view raw, BopomofoKeyboardLayout layout,
                                            bool space_tone, std::u16string_view context) const {
    const size_t n = raw.size();
    if (n == 0) return {{}, {MixedPath{}}, 0};

    struct Edge {
        size_t end;
        MixedSegment segment;
        bool lexical_word;
        bool recovery_piece = false;
        bool phrase_onset = false;
        bool natural_reading = false;
        bool structured_boundary = false;
    };
    std::vector<std::vector<Edge>> edges(n + 1);
    bool has_structured_boundaries = false;

    // Hsu's letter tones are both completion keys and ordinary English letters.
    // Give a valid completed reading modest evidence, but not at the expense
    // of a complete literal word, opaque syntax, or a common word still being
    // typed. Classify whole ASCII runs, not arbitrary one-letter subedges.
    std::vector<bool> hsu_literal(n, false);
    const bool hsu_lexical_prefix = layout == BopomofoKeyboardLayout::Hsu &&
        MixedLexicon::instance().english_prefix_frequency(raw) > 0;
    if (layout == BopomofoKeyboardLayout::Hsu) {
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
                    token.end == n && !space_tone && lexicon.english_prefix_frequency(spelling) >= kHsuStrongPrefixFrequency;
                const bool structure = token.kind == AsciiTokenKind::Identifier ||
                    token.kind == AsciiTokenKind::Email || token.kind == AsciiTokenKind::Domain ||
                    token.kind == AsciiTokenKind::URL || token.kind == AsciiTokenKind::FilesystemPath ||
                    token.kind == AsciiTokenKind::Alphanumeric;
                if (word || prefix || structure) {
                    std::fill(hsu_literal.begin() + static_cast<std::ptrdiff_t>(begin),
                              hsu_literal.begin() + static_cast<std::ptrdiff_t>(token.end), true);
                }
            }
        }
    }
    const auto hsu_completion_evidence = [&](size_t begin, size_t end, std::u16string_view spelling) {
        // Only the beginning of pending input needs this repair; established
        // Chinese already has transition/phrase evidence. Rescoring arbitrary
        // internal cuts can consume a literal island's last letter as an onset.
        if (begin != 0) return 0.0;
        // A weak but real English completion can explain a new tail even if
        // it cannot override the shorter completed Chinese input by itself.
        if (end < n && hsu_lexical_prefix) return 0.0;
        if (layout != BopomofoKeyboardLayout::Hsu ||
            (frequency_ && frequency_(spelling) > 0) ||
            std::ranges::any_of(hsu_literal.begin() + static_cast<std::ptrdiff_t>(begin),
                                hsu_literal.begin() + static_cast<std::ptrdiff_t>(end),
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
                    segment.score = latin_score_ ? latin_score_(raw.substr(token.begin, token.end - token.begin),
                                                               token.end == n && !space_tone) : kLatinBase;
                    if (!latin_score_ && frequency_) {
                        segment.score += kKnownWordBoost * frequency_(raw.substr(token.begin, token.end - token.begin));
                    }
                    if (layout == BopomofoKeyboardLayout::Hsu && token.end == n && !space_tone &&
                        token.end - i >= 2 && i == 0) {
                        const auto prefix = MixedLexicon::instance().english_prefix_frequency(raw.substr(i));
                        // Common completions get the normal lexical scale while
                        // typing, not the weak-prefix backoff. Rare completions
                        // must not erase completed Chinese (e.g. a compact tone).
                        if (prefix >= kHsuStrongPrefixFrequency) {
                            segment.score = std::max(segment.score, std::min(-0.1, 6.0 * prefix - 5.0));
                        }
                    }
                    break;
                case AsciiTokenKind::Alphanumeric:
                    segment.kind = MixedSegmentKind::Latin;
                    segment.score = kAlnumBase - 0.6 * static_cast<double>(token.end - token.begin);
                    if (frequency_) {
                        const auto word = raw.substr(token.begin, token.end - token.begin);
                        if (frequency_(word) > 0 && latin_score_) segment.score = latin_score_(word, false) + 1.0;
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
                segment.score = latin_score_(word, end == n && !space_tone);
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
                                 (tone_key == U' ' && i == 0 ? first_tone_symbol_evidence(body) : 0.0) +
                                 hsu_completion_evidence(i, j, tone_key == U' ' ? body : raw.substr(i, len));
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
                                 (i == 0 ? first_tone_symbol_evidence(raw.substr(i)) : 0.0) +
                                 hsu_completion_evidence(i, n, raw.substr(i));
                if (!ordered.empty() && segment.reading != ordered) segment.score -= kSwitchPenalty / 2.0;
                add_edge(i, n, std::move(segment));
            }
        }
    }

    // An unknown English island can still be real text. On Hsu, a complete
    // dictionary phrase at the following reading boundary supplies the same
    // resumption evidence as an English dictionary entry, without guessing or
    // completing the literal name itself. This never applies to opaque syntax
    // or a one/two-letter fragment masquerading as a word.
    if (layout == BopomofoKeyboardLayout::Hsu) {
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
        for (size_t begin = 0; begin < n; ++begin) {
            onsets[begin] = std::ranges::any_of(edges[begin], [](const auto& edge) { return edge.phrase_onset; });
            for (const auto& edge : edges[begin]) {
                if (edge.segment.kind == MixedSegmentKind::Bopomofo) chinese_ends[edge.end] = true;
            }
        }
        for (size_t begin = 0; begin < n; ++begin) {
            if (!chinese_ends[begin] || hsu_literal[begin]) continue;
            for (size_t end = begin + 1; end <= std::min(n, begin + 32); ++end) {
                if (raw[end - 1] < u'a' || raw[end - 1] > u'z') break;
                if (end - begin < 3 || !onsets[end]) continue;
                const auto word = raw.substr(begin, end - begin);
                if (frequency_ && frequency_(word) > 0) continue;
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Latin;
                segment.score = latin_score_ ? latin_score_(word, false) : kLatinBase;
                add_edge(begin, end, std::move(segment));
            }
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
    };
    struct Entry {
        double score = 0;
        PathLinkPtr path;
    };
    struct PhraseEdge {
        size_t end;
        std::vector<MixedSegment> segments;
        double score;
    };
    std::vector<std::vector<PhraseEdge>> phrases(n + 1);
    std::vector<bool> repaired_singletons(n, false);

    const auto insert_entry = [&](std::vector<Entry>& bucket, Entry entry) {
        // Merge only equivalent rendered history and transition state. A
        // literal tail and an unfinished syllable may render identically but
        // carry different language-transition evidence.
        for (auto& existing : bucket) {
            if (existing.path->rendered == entry.path->rendered &&
                existing.path->segment.kind == entry.path->segment.kind &&
                existing.path->latin_count == entry.path->latin_count &&
                existing.path->lexical_island == entry.path->lexical_island &&
                existing.path->recovery_piece == entry.path->recovery_piece) {
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

    const auto extend = [&](const Entry& entry, MixedSegment segment, size_t candidate,
                            bool lexical_word, bool ordinal_prior, bool recovery_piece = false,
                            bool phrase_onset = false) {
        double score = entry.score + segment.score;
        // Six-key pieces bound parsing work, not the number of independent
        // errors. A contiguous unresolved run pays its cost once; repeatedly
        // charging that cost makes a long local typo erase earlier Chinese.
        // Legal completed readings remain separate edges and end the run.
        if (segment.kind == MixedSegmentKind::BopomofoUnresolved && entry.path &&
            entry.path->recovery_piece) score -= segment.score;
        if (segment.raw == u" " && entry.path && entry.path->segment.kind == MixedSegmentKind::Bopomofo) {
            segment.consumed_boundary = true;
            score += 0.01;
        }
        if (segment.kind == MixedSegmentKind::Bopomofo) {
            std::u16string history(context.substr(context.size() > 3 ? context.size() - 3 : 0));
            if (entry.path) {
                const auto& text = entry.path->rendered;
                history += text.substr(text.size() > 3 ? text.size() - 3 : 0);
            }
            score += chinese_score_ ? chinese_score_(history, segment.candidates[candidate]) : -5.0;
            if (ordinal_prior) score -= 1.5 * static_cast<double>(candidate);
            std::rotate(segment.candidates.begin(), segment.candidates.begin() +
                        static_cast<std::ptrdiff_t>(candidate), segment.candidates.end());
        }
        const size_t prev_latin = entry.path ? entry.path->latin_count : 0;
        // A known English word inside Chinese is one literal island: entry
        // already paid the switch penalty. Opaque tokens retain their costs.
        const bool resume_after_unknown = phrase_onset && entry.path && entry.path->prev &&
            entry.path->segment.kind == MixedSegmentKind::Latin &&
            entry.path->prev->segment.kind == MixedSegmentKind::Bopomofo &&
            entry.path->segment.raw.size() >= 3 &&
            std::ranges::all_of(entry.path->segment.raw, [](char16_t key) { return key >= u'a' && key <= u'z'; });
        const bool resume_after_word = entry.path && (entry.path->lexical_island || resume_after_unknown) &&
            (segment.kind == MixedSegmentKind::Bopomofo || segment.kind == MixedSegmentKind::BopomofoIncomplete);
        if (entry.path && entry.path->segment.kind != segment.kind &&
            entry.path->segment.kind != MixedSegmentKind::Symbol && segment.kind != MixedSegmentKind::Symbol &&
            !(entry.path->segment.kind == MixedSegmentKind::Bopomofo &&
              (segment.kind == MixedSegmentKind::BopomofoIncomplete || segment.kind == MixedSegmentKind::BopomofoUnresolved)) &&
            !(entry.path->segment.kind == MixedSegmentKind::BopomofoUnresolved &&
              (segment.kind == MixedSegmentKind::Bopomofo || segment.kind == MixedSegmentKind::BopomofoIncomplete))) {
            if (!resume_after_word) score -= kSwitchPenalty;
            if ((entry.path->segment.kind == MixedSegmentKind::Latin && entry.path->segment.raw.size() == 1) ||
                (segment.kind == MixedSegmentKind::Latin && segment.raw.size() == 1)) score -= 1.0;
        }
        // Each additional adjacent Latin segment pays fragmentation once.
        if (entry.path && entry.path->segment.kind == MixedSegmentKind::Latin &&
            segment.kind == MixedSegmentKind::Latin) score -= kFragmentPenalty;
        std::u16string rendered = entry.path ? entry.path->rendered : std::u16string();
        rendered += render_segment(segment);
        const size_t latin_count = prev_latin + (segment.kind == MixedSegmentKind::Latin ? 1 : 0);
        const bool lexical_island = lexical_word && entry.path && entry.path->segment.kind == MixedSegmentKind::Bopomofo;
        auto link = std::make_shared<PathLink>(
            PathLink{entry.path, std::move(segment), score, std::move(rendered), latin_count, lexical_island, recovery_piece});
        return Entry{score, std::move(link)};
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
                                                               edge.recovery_piece, edge.phrase_onset));
                    }
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
                frequency_(segment.raw) < kHsuStrongPrefixFrequency;
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
        if (!result.paths[i].boundary_alternative && result.paths[i].score > result.paths[result.best_path].score) {
            result.best_path = i;
        }
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
    // Keep the existing repair's selection key stable while a model result
    // arrives. A new suggestion is additional, never a row replacement.
    for (size_t i = 1; i < result.paths.size(); ++i) {
        if (result.paths[i].boundary_model_refined) add_path(i);
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
