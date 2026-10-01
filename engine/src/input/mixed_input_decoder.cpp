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
    };
    std::vector<std::vector<Edge>> edges(n + 1);

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
                                0.4 * static_cast<double>(body.size());
                add_edge(i, j, std::move(segment));
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
            for (auto&& [reading, candidates] : replay(raw.substr(i), U' ')) {
                MixedSegment segment;
                segment.kind = MixedSegmentKind::Bopomofo;
                segment.body_keys.assign(raw.substr(i));
                segment.tone_key = U' ';
                segment.reading = std::move(reading);
                segment.candidates = std::move(candidates);
                segment.score = (is_compact_bopomofo_layout(layout) ? 3.5 : 3.4) +
                                0.4 * static_cast<double>(body_len);
                add_edge(i, n, std::move(segment));
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
                existing.path->lexical_island == entry.path->lexical_island) {
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
                            bool lexical_word, bool ordinal_prior) {
        double score = entry.score + segment.score;
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
        const bool resume_after_word = entry.path && entry.path->lexical_island &&
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
            PathLink{entry.path, std::move(segment), score, std::move(rendered), latin_count, lexical_island});
        return Entry{score, std::move(link)};
    };

    const auto search = [&](bool lexical_spans) {
        std::vector<std::vector<Entry>> buckets(n + 1);
        buckets[0].push_back({0, nullptr});
        for (size_t v = 0; v < n; ++v) {
            for (const auto& edge : edges[v]) {
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
                        insert_entry(buckets[edge.end], extend(entry, edge.segment, candidate, edge.lexical_word, true));
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
    std::vector<bool> protected_raw(n, false);
    if (!dp[n].empty() && (!literal || dp[n].front().score > literal->score)) {
        for (auto link = dp[n].front().path; link; link = link->prev) {
            const auto& segment = link->segment;
            if (segment.kind == MixedSegmentKind::Bopomofo) chinese_anchor = true;
            // Repair a complete singleton first-tone reading currently split
            // into one literal letter and a Space. Do not reinterpret words,
            // opaque tokens, malformed raw islands or unfinished readings.
            const bool singleton = (segment.kind == MixedSegmentKind::Latin || segment.kind == MixedSegmentKind::Symbol) &&
                segment.raw.size() == 1 && segment.end < n && raw[segment.end] == u' ' &&
                !replay(segment.raw, U' ').empty();
            if (singleton) singleton_literal[segment.begin] = true;
            const bool protect = segment.kind != MixedSegmentKind::Bopomofo &&
                !singleton &&
                !(segment.kind == MixedSegmentKind::Symbol && segment.raw == u" ");
            if (protect) std::fill(protected_raw.begin() + static_cast<std::ptrdiff_t>(segment.begin),
                                  protected_raw.begin() + static_cast<std::ptrdiff_t>(segment.end), true);
        }
    }
    if (chinese_anchor && std::ranges::any_of(singleton_literal, [](bool value) { return value; })) {
        const auto& lexicon = MixedLexicon::instance();
        for (size_t begin = 0; begin < n; ++begin) {
            std::u16string word;
            std::vector<MixedSegment> chain;
            const auto visit = [&](auto&& self, size_t vertex, double score) -> void {
                if (word.size() >= 2 && lexicon.is_chinese_phrase(word) &&
                    std::ranges::any_of(singleton_literal.begin() + static_cast<std::ptrdiff_t>(begin),
                                        singleton_literal.begin() + static_cast<std::ptrdiff_t>(vertex),
                                        [](bool value) { return value; })) {
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
    for (size_t i = 1; i < result.paths.size(); ++i) {
        if (result.paths[i].score > result.paths[result.best_path].score) result.best_path = i;
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
