#include "pipe/protocol.hpp"
#include "session/session_manager.hpp"
#include "training/commit_store.hpp"
#include "training/lora_history_lca.hpp"
#include "training/numeric_dataset.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>
#include <unistd.h>

#include <iostream>

namespace {

bool protocol_test() {
    using namespace ime::unix_service::protocol;
    OpenSessionResponse opened;
    for (std::size_t i = 0; i < opened.session_id.size(); ++i) {
        opened.session_id[i] = static_cast<std::uint8_t>(i + 1);
        opened.service_epoch[i] = static_cast<std::uint8_t>(0xf0U - i);
    }
    const auto bytes = encode(Message{opened});
    const ByteVector expected{34, 0, 0, 0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                              0xf0, 0xef, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4,
                              0xe3, 0xe2, 0xe1};
    if (bytes != expected) return false;
    auto decoded = decode(bytes);
    if (std::get<OpenSessionResponse>(decoded).service_epoch != opened.service_epoch) return false;

    RecordCommitRequest mixed;
    mixed.event_id[0] = 9;
    mixed.source_id[0] = 3;
    mixed.context = u"早安";
    mixed.answer = u"你h";
    mixed.entries = {{u"ㄋㄧˇ", U'你', false, false}, {u"", U'h', false, true}};
    const auto mixed_bytes = decode(encode(Message{mixed}));
    const auto& decoded_mixed = std::get<RecordCommitRequest>(mixed_bytes);
    if (decoded_mixed.source_id != mixed.source_id || decoded_mixed.entries.size() != 2 || decoded_mixed.entries[0].literal ||
        !decoded_mixed.entries[1].literal || !decoded_mixed.entries[1].reading.empty()) return false;
    RecordCommitRequest literal_only;
    literal_only.event_id[0] = 9;
    literal_only.source_id[0] = 3;
    literal_only.answer = u"h";
    literal_only.entries = {{u"", U'h', false, true}};
    bool literal_rejected = false;
    try { (void)encode(Message{literal_only}); } catch (const ProtocolError&) { literal_rejected = true; }
    if (literal_rejected) return false;

    DiscardCommitRequest discard;
    discard.event_id[0] = 7;
    if (decode_message_type(encode(Message{discard})) != MessageType::DiscardCommit) return false;
    const auto rejected = std::get<DiscardCommitRequest>(decode(encode(Message{discard})));
    if (rejected.event_id != discard.event_id) return false;
    const DiscardCommitResponse accepted{discard.event_id, true};
    const auto answered = std::get<DiscardCommitResponse>(decode(encode(Message{accepted})));
    return answered.event_id == discard.event_id && answered.discarded;
}

bool core_adapter_test() {
    ime::unix_service::protocol::PredictRequest request;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    request.padding.push_back({true, {}, U'好'});

    const auto core_padding = ime::unix_service::detail::to_core_padding(request);
    if (core_padding.size() != 2 || core_padding[0].chosen || core_padding[0].bopomofo != u"ㄋㄧˇ" ||
        !core_padding[1].chosen || core_padding[1].chosen_char != U'好') {
        return false;
    }

    std::vector<llavon::ime::core::Prediction> predictions(2);
    predictions[0].candidates = {{U'你', 0.75F}, {U'擬', 0.25F}};
    const auto candidates = ime::unix_service::detail::to_protocol_candidates(request, predictions);
    return candidates == std::vector<std::vector<char32_t>>{{U'你', U'擬'}, {U'好'}};
}

bool core_runtime_test() {
    ime::unix_service::RuntimeConfig config;
    config.model_path = std::filesystem::path(IME_UNIX_SERVICE_TEST_TABLE_DIR) / "bopomofo_char.json";
    config.tables_dir = IME_UNIX_SERVICE_TEST_TABLE_DIR;
    ime::unix_service::CoreRuntime runtime(std::move(config));
    runtime.validate_configuration();
    return !runtime.loaded();
}

class MockEngine final : public ime::unix_service::ISessionEngine {
public:
    std::vector<std::vector<char32_t>> predict(const ime::unix_service::protocol::PredictRequest& request) override {
        std::vector<std::vector<char32_t>> result;
        for (const auto& entry : request.padding) result.push_back(entry.chosen ? std::vector<char32_t>{entry.chosen_char} : std::vector<char32_t>{U'你'});
        return result;
    }
    bool loaded() const noexcept override { return true; }
};

bool session_test() {
    ime::unix_service::SessionLimits limits;
    limits.max_sessions = 2;
    limits.max_idle_sessions = 2;
    limits.idle_timeout = std::chrono::seconds(60);
    limits.max_concurrent_predictions = 2;
    ime::unix_service::SessionManager manager(nullptr, limits, []() { return std::make_unique<MockEngine>(); });

    const auto first = manager.open_session(1000);
    const auto second = manager.open_session(1000);
    if (!std::holds_alternative<ime::unix_service::protocol::OpenSessionResponse>(first) ||
        !std::holds_alternative<ime::unix_service::protocol::OpenSessionResponse>(second)) return false;
    const auto first_id = std::get<ime::unix_service::protocol::OpenSessionResponse>(first).session_id;

    ime::unix_service::protocol::PredictRequest request;
    request.session_id = first_id;
    request.request_id = 1;
    request.buffer_revision = 9;
    request.padding.push_back({false, u"ㄋㄧˇ", 0});
    const auto prediction = manager.predict(1000, request);
    if (!std::holds_alternative<ime::unix_service::protocol::Prediction>(prediction)) return false;
    const auto& value = std::get<ime::unix_service::protocol::Prediction>(prediction);
    if (value.candidates.size() != 1 || value.candidates.front().empty() || value.candidates.front().front() != U'你') return false;

    const auto duplicate = manager.predict(1000, request);
    if (!std::holds_alternative<ime::unix_service::protocol::Error>(duplicate) ||
        std::get<ime::unix_service::protocol::Error>(duplicate).code !=
            ime::unix_service::protocol::ErrorCode::OutOfOrder) return false;
    const auto unauthorized = manager.status(2000, first_id);
    if (!std::holds_alternative<ime::unix_service::protocol::Error>(unauthorized) ||
        std::get<ime::unix_service::protocol::Error>(unauthorized).code !=
            ime::unix_service::protocol::ErrorCode::Unauthorized) return false;

    const auto closed = manager.close_session(1000, first_id);
    if (!std::holds_alternative<ime::unix_service::protocol::CloseSessionResponse>(closed)) return false;
    return manager.session_count() == 1;
}

bool commit_test() {
    using namespace ime::unix_service;
    protocol::RecordCommitRequest request;
    request.event_id[0] = 0x45;
    request.source_id[0] = 0x31;
    request.context = u"早安";
    request.answer = u"你好";
    request.entries = {{u"ㄋㄧˇ", U'你', false}, {u"ㄏㄠˇ", U'好', true}};
    const auto decoded = std::get<protocol::RecordCommitRequest>(protocol::decode(protocol::encode(request)));
    if (decoded.event_id != request.event_id || decoded.source_id != request.source_id || decoded.context != request.context ||
        decoded.answer != request.answer || decoded.entries.size() != 2 ||
        !decoded.entries[1].manually_selected) return false;

    const auto directory = std::filesystem::temp_directory_path() /
                           ("llavon-commit-store-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "commits.sqlite3";
    const std::string password = "correct horse battery";
    const std::string legacy_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    bool good = false;
    sqlite3* db = nullptr;
    try {
        // A database written before encrypted recording existed. Its readable
        // rows must convert in place when the password is set.
        {
            sqlite3* legacy = nullptr;
            if (sqlite3_open_v2(path.c_str(), &legacy, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
                throw std::runtime_error("could not create legacy database");
            const char* schema =
                "CREATE TABLE commits (id TEXT PRIMARY KEY, context TEXT NOT NULL, answer TEXT NOT NULL, "
                "state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','excluded','trained')), "
                "committed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
                "CREATE TABLE readings (commit_id TEXT NOT NULL REFERENCES commits(id) ON DELETE CASCADE, "
                "position INTEGER NOT NULL, reading TEXT NOT NULL, character INTEGER NOT NULL, "
                "manually_selected INTEGER NOT NULL CHECK(manually_selected IN (0,1)), PRIMARY KEY(commit_id,position));"
                "INSERT INTO commits (id,context,answer,state) VALUES ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','早安','你好','trained');"
                "INSERT INTO readings VALUES ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',0,'ㄋㄧˇ',20320,1),"
                "('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',1,'ㄏㄠˇ',22909,0);";
            char* message = nullptr;
            if (sqlite3_exec(legacy, schema, nullptr, nullptr, &message) != SQLITE_OK) {
                const std::string error = message ? message : "legacy schema failed";
                sqlite3_free(message);
                sqlite3_close(legacy);
                throw std::runtime_error(error);
            }
            sqlite3_close(legacy);
        }
        {
            CommitStore store(path);
            if (store.protection_status().configured || store.recording_enabled())
                throw std::runtime_error("fresh store claims protection");
            // Without a password nothing is stored, so typing never reaches
            // the database in readable form.
            if (store.record(request)) throw std::runtime_error("stored a commit without a password");
            store.configure_password(password);
            const auto status = store.protection_status();
            if (!status.configured || !status.enabled) throw std::runtime_error("password was not stored");
            // A commit is staged first, so an immediate Backspace can withdraw
            // it before anything reaches the database.
            auto withdrawn = request;
            withdrawn.event_id[0] += 5;
            if (!store.record(withdrawn)) throw std::runtime_error("could not stage a commit");
            if (!store.discard_staged(withdrawn.event_id)) throw std::runtime_error("staged commit was not withdrawable");
            if (store.discard_staged(withdrawn.event_id)) throw std::runtime_error("withdrew a commit twice");
            // A commit from another input context must not settle the first
            // context's correction window. Only a new commit from that same
            // source settles it, matching the Windows writer.
            {
                CommitStore contexts(directory / "contexts.sqlite3");
                contexts.configure_password(password);
                auto first = request;
                auto other = request;
                auto next = request;
                first.event_id[0] = 0x70;
                other.event_id[0] = 0x71;
                other.source_id[0] = 0x32;
                next.event_id[0] = 0x72;
                if (!contexts.record(first) || !contexts.record(other) || !contexts.record(next))
                    throw std::runtime_error("could not stage commits across contexts");
                if (!contexts.discard_staged(other.event_id))
                    throw std::runtime_error("other context was settled early");
                if (contexts.discard_staged(first.event_id))
                    throw std::runtime_error("same context was not settled");
                if (contexts.flush_staged(true) != 1)
                    throw std::runtime_error("latest same-context commit was not staged");
            }
            // Once the correction window elapses, an untouched commit is written.
            {
                CommitStore elapsed_store(directory / "elapsed.sqlite3");
                elapsed_store.configure_password(password);
                auto elapsed = request;
                elapsed.event_id[0] += 6;
                if (!elapsed_store.record(elapsed)) throw std::runtime_error("could not stage a commit");
                if (elapsed_store.flush_staged(false, std::chrono::steady_clock::now() + kCommitCorrectionWindow) != 1)
                    throw std::runtime_error("an elapsed commit was not written");
            }
            // A following commit settles the previous one early, and a repeated
            // event id is accepted but never written twice.
            if (!store.record(request)) throw std::runtime_error("could not record commit");
            if (store.flush_staged(false) != 0) throw std::runtime_error("wrote a commit before its correction window");
            if (store.flush_staged(true) != 1) throw std::runtime_error("staged commit was not written");
            if (!store.record(request)) throw std::runtime_error("could not stage duplicate commit");
            if (store.flush_staged(true) != 0) throw std::runtime_error("duplicate commit was written twice");
            auto bad = request; bad.event_id[0]++; bad.answer = u"不符";
            try { (void)store.record(bad); throw std::runtime_error("invalid commit accepted"); }
            catch (const std::invalid_argument&) {}
            // A commit with no composed position teaches nothing, even though
            // the protocol itself can describe literal positions.
            protocol::RecordCommitRequest literal_only;
            literal_only.event_id[0] = 0x4b;
            literal_only.source_id = request.source_id;
            literal_only.answer = u"hi";
            literal_only.entries = {{u"", U'h', false, true}, {u"", U'i', false, true}};
            try { (void)store.record(literal_only); throw std::runtime_error("literal-only commit accepted"); }
            catch (const std::invalid_argument&) {}
            // Literal positions stay as context around the composed ones.
            protocol::RecordCommitRequest mixed;
            mixed.event_id[0] = 0x49;
            mixed.source_id = request.source_id;
            mixed.context = u"早安";
            mixed.answer = u"你hi";
            mixed.entries = {{u"ㄋㄧˇ", U'你', false, false}, {u"", U'h', false, true},
                             {u"", U'i', false, true}};
            if (!store.record(mixed)) throw std::runtime_error("could not record mixed commit");
            auto new_reading = request;
            new_reading.event_id[0] += 2;
            new_reading.answer = u"了";
            new_reading.entries = {{u"ㄌㄜ ", U'了', false}};
            if (!store.record(new_reading)) throw std::runtime_error("could not record new reading");
            auto long_context = request;
            long_context.event_id[0] += 3;
            long_context.context.assign(500, u'你');
            if (!store.record(long_context)) throw std::runtime_error("could not record long context");
            // Only the newest commit in this source remains staged.
            if (store.flush_staged(true) != 1) throw std::runtime_error("staged commits were not written");
            // Disabling stops storage without touching what is already there.
            store.set_recording_enabled(false);
            auto disabled = request;
            disabled.event_id[0] += 4;
            if (store.record(disabled)) throw std::runtime_error("stored while collection was disabled");
            store.set_recording_enabled(true);
        }
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            throw std::runtime_error("could not inspect commit database");
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db,
                    "SELECT (SELECT COUNT(*) FROM commits), (SELECT COUNT(*) FROM readings), "
                    "(SELECT COUNT(*) FROM commits WHERE schema_version=2), "
                    "(SELECT COUNT(*) FROM commits WHERE answer='你好' OR context='早安'), "
                    "(SELECT COUNT(*) FROM readings WHERE reading='ㄋㄧˇ' OR character != 0)",
                    -1, &stmt, nullptr) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW)
                throw std::runtime_error("could not inspect commit database");
            good = sqlite3_column_int(stmt, 0) == 5 && sqlite3_column_int(stmt, 1) == 10 &&
                   sqlite3_column_int(stmt, 2) == 5 && sqlite3_column_int(stmt, 3) == 0 && sqlite3_column_int(stmt, 4) == 0;
            sqlite3_finalize(stmt);
        }
        {
            const auto find = [](const std::vector<CommitRecord>& records, const std::string& id) -> const CommitRecord* {
                for (const auto& record : records) if (record.id == id) return &record;
                return nullptr;
            };
            CommitCipher locked;
            bool refused = false;
            try { (void)read_commits(db, "pending", locked, 0, 10); }
            catch (const std::runtime_error&) { refused = true; }
            if (!refused) throw std::runtime_error("read sealed records without a password");
            bool wrong = false;
            try { CommitCipher cipher; cipher.unlock(db, "wrong password"); }
            catch (const std::runtime_error&) { wrong = true; }
            if (!wrong) throw std::runtime_error("wrong password was accepted");
            CommitCipher cipher;
            cipher.unlock(db, password);
            std::vector<CommitRecord> pending, migrated;
            try { pending = read_commits(db, "pending", cipher, 0, 10); }
            catch (const std::exception& error) {
                throw std::runtime_error(std::string("pending read failed: ") + error.what());
            }
            std::string recorded_id = "45";
            recorded_id.append(30, '0');
            const auto* recorded = find(pending, recorded_id);
            if (pending.size() != 4 ||
                recorded == nullptr || recorded->context != "早安" || recorded->answer != "你好" ||
                recorded->readings.size() != 2 || recorded->readings.front() != "ㄋㄧˇ" ||
                recorded->manual.front() || !recorded->manual.back())
                throw std::runtime_error("decrypted records differ");
            const auto* mixed_record = find(pending, std::string("49") + std::string(30, '0'));
            if (mixed_record == nullptr || mixed_record->answer != "你hi" ||
                mixed_record->readings.size() != 3 || mixed_record->readings.front() != "ㄋㄧˇ" ||
                !mixed_record->readings[1].empty() || !mixed_record->readings[2].empty())
                throw std::runtime_error("mixed commit did not survive the round trip");
            const auto manually_selected = read_commits(db, "pending", cipher, 0, 10, true);
            if (manually_selected.size() != 2 || find(manually_selected, recorded_id) == nullptr ||
                find(manually_selected, std::string("48") + std::string(30, '0')) == nullptr ||
                find(manually_selected, std::string("49") + std::string(30, '0')) != nullptr)
                throw std::runtime_error("manual-only listing included an automatic commit");
            try { migrated = read_commits(db, "trained", cipher, 0, 10); }
            catch (const std::exception& error) {
                throw std::runtime_error(std::string("migrated read failed: ") + error.what());
            }
            if (migrated.size() != 1 || migrated.front().id != legacy_id ||
                migrated.front().context != "早安" || migrated.front().answer != "你好" ||
                migrated.front().readings.size() != 2)
                throw std::runtime_error("legacy rows were not converted");
        }
        const auto config = directory / "config.json";
        std::ofstream(config) << R"({"vocab_size":18546,"max_position_embeddings":384})";
        nlohmann::json vocab = nlohmann::json::array();
        for (int i = 0; i < 18546; ++i) vocab.push_back("");
        for (const auto name : {"chars", "special_tokens", "bpmf"}) {
            std::ifstream table(std::filesystem::path(IME_UNIX_SERVICE_TEST_TABLE_DIR) / "tokens" /
                                (std::string(name) + ".json"));
            const auto entries = nlohmann::json::parse(table);
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                const auto id = it.value().get<int>();
                if (id < 18546) vocab[id] = it.key();
            }
        }
        std::ofstream(directory / "ime_vocab.json") << nlohmann::json{{"tokens", vocab}}.dump();
        const auto output = directory / "training.jsonl";
        CommitCipher cipher;
        cipher.unlock(db, password);
        const auto decryption = cipher.decryption();
        NumericDataset dataset;
        try { dataset = write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
        catch (const std::exception& error) { throw std::runtime_error(std::string("dataset failed: ") + error.what()); }
        if (dataset.included_ids.size() != 2 || dataset.skipped != 2 || dataset.pad_token_id != 0) good = false;
        std::ifstream input(output);
        nlohmann::json row;
        input >> row;
        if (row.at("tokens").size() != row.at("candidate_masks").size() ||
            row.at("loss_weights").back() != 1 || row.at("candidate_masks").back().is_null()) good = false;
        for (int copy = 0; copy < 2; ++copy) {
            nlohmann::json repeated;
            input >> repeated;
            if (repeated != row) good = false;
        }
        // The overlong context is skipped, not truncated into a training row.
        // The mixed row keeps literal positions as context: no mask and no loss
        // for them, a real mask and loss for the composed position.
        nlohmann::json mixed_row;
        input >> mixed_row;
        const auto& mixed_weights = mixed_row.at("loss_weights");
        const auto& mixed_masks = mixed_row.at("candidate_masks");
        if (mixed_row.at("tokens").size() != mixed_weights.size() || mixed_weights.size() != mixed_masks.size() ||
            mixed_row.at("tokens").size() >= 384)
            good = false;
        bool saw_trained_position = false;
        for (std::size_t i = 0; i < mixed_weights.size(); ++i) {
            const auto weight = mixed_weights[i].get<int>();
            if (weight != 0 && weight != 1) good = false;
            if (weight == 0 && !mixed_masks[i].is_null()) good = false;
            if (weight == 1 && !mixed_masks[i].is_null()) saw_trained_position = true;
        }
        if (!saw_trained_position) good = false;
        // Manual-only training excludes entire automatic commits, including a
        // mixed literal/composed sentence, while retaining all positions of
        // the selected commit and its three training copies.
        const auto manual_dataset = write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR,
            config, output, 384, nullptr, &decryption, true);
        if (manual_dataset.included_ids.size() != 1 || manual_dataset.included_ids.front() !=
                std::string("45") + std::string(30, '0')) good = false;
        {
            std::ifstream manual_rows(output);
            std::string line;
            int copies = 0;
            while (std::getline(manual_rows, line)) ++copies;
            if (copies != 3) good = false;
        }
        vocab[1427] = "wrong token";  // "你" is token 1427 in this checkpoint.
        std::ofstream(directory / "ime_vocab.json") << nlohmann::json{{"tokens", vocab}}.dump();
        bool mismatched = false;
        try { (void)write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
        catch (const std::runtime_error&) { mismatched = true; }
        good = good && mismatched;
        // A locked reader cannot turn sealed rows into a dataset.
        bool sealed_refused = false;
        {
            const CommitCipher locked;
            const auto decryption = locked.decryption();
            try { (void)write_numeric_dataset(db, IME_UNIX_SERVICE_TEST_TABLE_DIR, config, output, 384, nullptr, &decryption); }
            catch (const std::runtime_error&) { sealed_refused = true; }
        }
        good = good && sealed_refused;
        // Forgetting the password removes the conversation records and the
        // keys; the database stays usable.
        {
            CommitStore store(path);
            store.reset_conversation_data();
            const CommitCipher locked;
            if (store.protection_status().configured ||
                !read_commits(db, "pending", locked, 0, 10).empty()) good = false;
            {
                sqlite3_stmt* count = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT (SELECT COUNT(*) FROM commits), (SELECT COUNT(*) FROM readings)",
                                       -1, &count, nullptr) == SQLITE_OK && sqlite3_step(count) == SQLITE_ROW)
                    good = good && sqlite3_column_int(count, 0) == 0 && sqlite3_column_int(count, 1) == 0;
                sqlite3_finalize(count);
            }
        }
        sqlite3_close(db);
    } catch (const std::exception& error) { std::cerr << "commit test: " << error.what() << '\n'; good = false; }
    catch (...) { std::cerr << "commit test: unknown error\n"; good = false; }
    std::filesystem::remove_all(directory);
    return good;
}

}  // namespace

// The history tree's shortcut follows the Windows manager: the common ancestor
// of two runs must be a real lineage node, and malformed history is refused.
bool lora_lca_test() {
    using ime::unix_service::LoraHistoryParent;
    using ime::unix_service::tarjan_lca;
    bool good = true;
    const std::vector<LoraHistoryParent> chain{{1, 0}, {2, 1}, {3, 2}};
    good = good && tarjan_lca(chain, 3, 2) == 2;
    good = good && tarjan_lca(chain, 2, 3) == 2;
    good = good && tarjan_lca(chain, 3, 1) == 1;
    good = good && tarjan_lca(chain, 1, 1) == 1;
    good = good && tarjan_lca(chain, 3, 0) == 0;
    const std::vector<LoraHistoryParent> branch{{1, 0}, {2, 1}, {3, 1}, {4, 3}};
    good = good && tarjan_lca(branch, 2, 3) == 1;
    good = good && tarjan_lca(branch, 4, 2) == 1;
    good = good && tarjan_lca(branch, 4, 3) == 3;
    const std::vector<LoraHistoryParent> siblings{{1, 0}, {2, 0}};
    good = good && tarjan_lca(siblings, 1, 2) == 0;
    if (tarjan_lca(chain, 3, 9)) good = false;                    // unknown run
    if (tarjan_lca(chain, 0, 0) != 0) good = false;               // base only
    const std::vector<LoraHistoryParent> duplicate{{1, 0}, {1, 0}};
    if (tarjan_lca(duplicate, 1, 1)) good = false;                // duplicate IDs
    const std::vector<LoraHistoryParent> orphan{{1, 0}, {2, 99}};
    if (tarjan_lca(orphan, 1, 2)) good = false;                   // missing parent
    return good;
}

int main() {
    struct Case { const char* name; bool (*run)(); };
    const Case cases[] = {{"protocol", protocol_test}, {"core-adapter", core_adapter_test},
                          {"core-runtime", core_runtime_test}, {"session", session_test}, {"commit", commit_test},
                          {"lora-lca", lora_lca_test}};
    bool good = true;
    for (const auto& item : cases) {
        if (item.run()) continue;
        std::cerr << "failed: " << item.name << '\n';
        good = false;
    }
    return good ? EXIT_SUCCESS : EXIT_FAILURE;
}
