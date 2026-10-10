#include "numeric_dataset.hpp"
#include "sqlite.hpp"

#include <nlohmann/json.hpp>
#include <utf8/cpp20.h>
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ime::unix_service {
namespace {

using json = nlohmann::json;
using Tokens = std::unordered_map<std::string, int>;

json load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read training table: " + path.string());
    return json::parse(input);
}

struct Tables {
    Tokens chars, latin, special, bopomofo;
    std::unordered_map<std::string, std::vector<std::string>> candidates;
};

Tables load_tables(const std::filesystem::path& directory, int vocab_size,
                   const std::filesystem::path& vocabulary_path) {
    Tables tables{
        load(directory / "tokens/chars.json").get<Tokens>(),
        load(directory / "tokens/latin.json").get<Tokens>(),
        load(directory / "tokens/special_tokens.json").get<Tokens>(),
        load(directory / "tokens/bpmf.json").get<Tokens>(),
        load(directory / "bopomofo_char.json").get<decltype(Tables::candidates)>(),
    };
    // The deployed HF checkpoint uses the table at ime-core 00e3042.
    // Current inference tables (53e2776) add two out-of-vocabulary readings
    // and nine candidate entries. Project just that known additive delta;
    // never silently train against another table revision.
    if (vocab_size != 18546) throw std::runtime_error("unsupported training checkpoint vocabulary");
    const std::unordered_map<std::string, int> extra_tokens{{"<ㄋㄜ >", 18546}, {"<ㄌㄜ >", 18547}};
    const bool modern = tables.bopomofo.contains("<ㄋㄜ >");
    if (modern != tables.bopomofo.contains("<ㄌㄜ >")) throw std::runtime_error("incomplete training table revision");
    for (const auto& [key, id] : tables.bopomofo) {
        if (id >= vocab_size && (!extra_tokens.contains(key) || extra_tokens.at(key) != id))
            throw std::runtime_error("unrecognized out-of-vocabulary Bopomofo token");
    }
    for (const auto& [key, id] : extra_tokens) tables.bopomofo.erase(key);
    const std::vector<std::pair<std::string, std::string>> extra_candidates{
        {"ㄅㄠ ", "鮑"}, {"ㄓㄥ ", "幀"}, {"ㄓㄜ ", "著"},
        {"ㄍㄜˇ", "蛤"}, {"ㄕˊ", "匙"}, {"ㄌㄧˋ", "蜊"},
    };
    // The six additions must be confirmed against the current table before
    // applying this projection. A different revision requires new validation.
    for (const auto& [reading, character] : extra_candidates) {
        const auto found = tables.candidates.find(reading);
        if (modern) {
            if (found == tables.candidates.end() || found->second.empty() || found->second.back() != character)
                throw std::runtime_error("unexpected inference candidate table revision");
            found->second.pop_back();
        }
    }
    for (const auto& reading : {"ㄌㄜ ", "ㄉㄜ ", "ㄇㄜ "}) tables.candidates.erase(reading);
    if (tables.bopomofo.size() != 1396 || tables.candidates.size() != 1366 ||
        tables.chars.size() != 14141 || tables.latin.size() != 1744 || tables.special.size() != 9)
        throw std::runtime_error("IME tables differ from the training checkpoint revision");
    for (const auto* map : {&tables.chars, &tables.latin, &tables.special, &tables.bopomofo}) {
        for (const auto& [key, id] : *map) {
            (void)key;
            if (id < 0 || id >= vocab_size) throw std::runtime_error("training token exceeds model vocabulary");
        }
    }
    const auto vocabulary = load(vocabulary_path).at("tokens");
    if (!vocabulary.is_array() || vocabulary.size() != static_cast<std::size_t>(vocab_size))
        throw std::runtime_error("training vocabulary size differs from model configuration");
    for (const auto* map : {&tables.chars, &tables.special, &tables.bopomofo}) {
        for (const auto& [key, id] : *map) {
            if (vocabulary[static_cast<std::size_t>(id)] != key)
                throw std::runtime_error("IME token table differs from model vocabulary");
        }
    }
    return tables;
}

std::string utf8_char(char32_t ch) { return utf8::utf32to8(std::u32string(1, ch)); }

bool latin(char32_t ch) {
    return (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z') ||
           (ch >= U'0' && ch <= U'9') || ch == U'-' || ch == U'_' || ch == U'+';
}

std::vector<int> tokenize_context(const std::string& text, const Tables& tables) {
    const auto chars = utf8::utf8to32(text);
    std::vector<int> tokens;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const auto ch = chars[i];
        if (ch == U' ') { tokens.push_back(tables.special.at("<SP>")); continue; }
        if (const auto found = tables.chars.find(utf8_char(ch)); found != tables.chars.end()) {
            tokens.push_back(found->second); continue;
        }
        if (latin(ch)) {
            std::string word;
            while (i < chars.size() && latin(chars[i])) {
                const auto letter = chars[i++];
                word += static_cast<char>(letter >= U'A' && letter <= U'Z' ? letter + 32 : letter);
            }
            --i;
            const auto found = tables.latin.find(word);
            tokens.push_back(found == tables.latin.end() ? tables.special.at("<LATIN>") : found->second);
        } else {
            tokens.push_back(tables.special.at("<UNK>"));
        }
    }
    const auto first = std::find_if(tokens.begin(), tokens.end(), [&](int token) {
        return token != tables.special.at("<UNK>");
    });
    tokens.erase(tokens.begin(), first);
    return tokens;
}

struct Row {
    std::string id, context, answer;
    std::vector<std::pair<std::string, char32_t>> entries;
    bool manually_selected = false;
    int schema_version = 1;
};

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value));
}

std::optional<json> build_row(const Row& row, const Tables& tables, int max_length) {
    const auto answer = utf8::utf8to32(row.answer);
    if (answer.empty() || answer.size() != row.entries.size()) return std::nullopt;
    const auto required = 2 + 2 * row.entries.size();  // BOS, readings, SEP, answers
    if (required > static_cast<std::size_t>(max_length)) return std::nullopt;
    std::vector<int> tokens{tables.special.at("<BOS>")};
    auto context = tokenize_context(row.context, tables);
    // Windows skips the whole record rather than silently shortening its
    // training context when it cannot fit the checkpoint's sequence length.
    if (context.size() > static_cast<std::size_t>(max_length) - required) return std::nullopt;
    tokens.insert(tokens.end(), context.begin(), context.end());
    const auto literal_token = [&](char32_t character) {
        const auto token = tables.chars.find(utf8_char(character));
        return token == tables.chars.end() ? tables.special.at("<UNK>") : token->second;
    };
    for (std::size_t i = 0; i < row.entries.size(); ++i) {
        const auto& reading = row.entries[i].first;
        if (reading.empty()) {
            // A literal position keeps its inference token as context.
            tokens.push_back(literal_token(answer[i]));
            continue;
        }
        const auto found = tables.bopomofo.find("<" + reading + ">");
        if (found == tables.bopomofo.end()) return std::nullopt;
        tokens.push_back(found->second);
    }
    tokens.push_back(tables.special.at("<SEP>"));
    json masks = json::array();
    for (std::size_t i = 0; i < tokens.size(); ++i) masks.push_back(nullptr);
    std::vector<int> weights(tokens.size(), 0);
    for (std::size_t i = 0; i < row.entries.size(); ++i) {
        const auto& reading = row.entries[i].first;
        if (reading.empty()) {
            // Literal positions are context only: no mask and no loss.
            tokens.push_back(literal_token(answer[i]));
            masks.push_back(nullptr);
            weights.push_back(0);
            continue;
        }
        const auto found = tables.candidates.find(reading);
        if (found == tables.candidates.end()) return std::nullopt;
        std::vector<int> allowed;
        std::unordered_set<int> seen;
        for (const auto& candidate : found->second) {
            const auto characters = utf8::utf8to32(candidate);
            if (characters.empty()) continue;
            const auto token = tables.chars.find(utf8_char(characters.front()));
            if (token != tables.chars.end() && seen.insert(token->second).second)
                allowed.push_back(token->second);
        }
        const auto answer_token = tables.chars.find(utf8_char(answer[i]));
        if (answer_token == tables.chars.end() ||
            std::find(allowed.begin(), allowed.end(), answer_token->second) == allowed.end()) return std::nullopt;
        tokens.push_back(answer_token->second);
        masks.push_back(allowed);
        weights.push_back(1);
    }
    // A row whose positions are all literal teaches nothing.
    if (std::find(weights.begin(), weights.end(), 1) == weights.end()) return std::nullopt;
    if (tokens.size() > static_cast<std::size_t>(max_length)) return std::nullopt;
    std::vector<int> attention(tokens.size(), 1);
    return std::optional<json>{std::in_place,
        json{{"tokens", tokens}, {"labels", tokens}, {"loss_weights", weights},
             {"attention_mask", attention}, {"candidate_masks", masks}}};
}

}  // namespace

NumericDataset write_numeric_dataset(sqlite3* db, const std::filesystem::path& tables_dir,
                                      const std::filesystem::path& model_config,
                                      const std::filesystem::path& output, int max_sequence_length,
                                      const std::unordered_set<std::string>* selected_ids,
                                      const commit_crypto::Decryption* decryption,
                                      bool manual_only) {
    const auto config = load(model_config);
    NumericDataset result;
    result.vocab_size = config.at("vocab_size").get<int>();
    result.max_sequence_length = config.at("max_position_embeddings").get<int>();
    if (max_sequence_length < 2 || max_sequence_length > result.max_sequence_length)
        throw std::invalid_argument("training sequence length exceeds checkpoint configuration");
    const auto tables = load_tables(tables_dir, result.vocab_size, model_config.parent_path() / "ime_vocab.json");
    result.pad_token_id = tables.special.at("<PAD>");
    auto owned_query = sqlite::prepare(db, "SELECT c.id,c.context,c.answer,r.reading,r.position,r.character,r.manually_selected,"
                                "c.schema_version FROM commits c JOIN readings r ON r.commit_id=c.id "
                                "WHERE c.state='pending' ORDER BY c.id,r.position");
    auto* query = owned_query.get();
    // Only rows sealed after the password was configured are trainable; a
    // password-verified caller must never see a plaintext row slip through.
    const bool decrypt = decryption != nullptr && decryption->configured;
    auto field = [&](const std::string& id, std::string_view name, int column) {
        const auto value = column_text(query, column);
        if (!decrypt) return value;
        return commit_crypto::open(value, commit_crypto::field_identity(id, name),
                                   *decryption->parameters, *decryption->private_key);
    };
    auto partial = output; partial += ".partial";
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::filesystem::remove(partial);
    try {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("cannot create numeric dataset");
        Row row;
        auto flush = [&]() {
            if (row.id.empty()) return;
            // The manual filter mirrors the Windows manager: a record takes
            // part only when a candidate was explicitly selected somewhere in
            // it. Records that only typed literal positions are skipped whole.
            if (manual_only && !row.manually_selected) return;
            if (selected_ids && !selected_ids->contains(row.id)) return;
            if (auto numeric = build_row(row, tables, max_sequence_length)) {
                const int copies = row.manually_selected ? 3 : 1;
                for (int copy = 0; copy < copies; ++copy)
                    file << numeric->dump() << '\n';
                result.samples += static_cast<std::size_t>(copies);
                result.included_ids.push_back(row.id);
            } else {
                ++result.skipped;
                result.skipped_ids.push_back(row.id);
            }
        };
        int status;
        while ((status = sqlite3_step(query)) == SQLITE_ROW) {
            const std::string id = column_text(query, 0);
            if (row.id != id) {
                flush();
                row = Row{};
                row.id = id;
                row.schema_version = sqlite3_column_int(query, 7);
                if (decrypt && row.schema_version != 2)
                    throw std::runtime_error("unencrypted training record blocked");
                if (!decrypt && row.schema_version == 2)
                    throw std::runtime_error("training password required");
                row.context = field(id, "context", 1);
                row.answer = field(id, "answer", 2);
            }
            const auto position = sqlite3_column_int(query, 4);
            const auto reading = decrypt
                ? commit_crypto::open(column_text(query, 3), commit_crypto::reading_identity(id, position),
                                      *decryption->parameters, *decryption->private_key)
                : column_text(query, 3);
            row.entries.emplace_back(reading, static_cast<char32_t>(sqlite3_column_int64(query, 5)));
            row.manually_selected |= sqlite3_column_int(query, 6) != 0;
        }
        if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        flush();
        file.flush();
        // An empty dataset is not an error here: the caller decides whether it
        // wants to report the skipped records or refuse the run.
        if (!file) throw std::runtime_error("cannot write numeric dataset");
        file.close();
        owned_query.reset();
        std::filesystem::rename(partial, output);
    } catch (...) {
        owned_query.reset();
        std::filesystem::remove(partial);
        throw;
    }
    return result;
}

}  // namespace ime::unix_service
