#include "commit_store.hpp"

#include <sqlite3.h>
#include <utf8/cpp20.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "commit_crypto.hpp"

namespace ime::unix_service {
namespace {

void check(int result, sqlite3* db) {
    if (result != SQLITE_OK && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}

void execute(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) { check(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), db); }
    ~Statement() { sqlite3_finalize(stmt_); }
    void text(int index, const std::string& value) {
        check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), db_);
    }
    void integer(int index, std::int64_t value) { check(sqlite3_bind_int64(stmt_, index, value), db_); }
    void step() { check(sqlite3_step(stmt_), db_); }
    void reset() { check(sqlite3_reset(stmt_), db_); check(sqlite3_clear_bindings(stmt_), db_); }
    sqlite3_stmt* get() const { return stmt_; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string event_id(const protocol::SessionId& bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

void validate(const protocol::RecordCommitRequest& request) {
    if (protocol::is_zero(request.event_id) || protocol::is_zero(request.source_id) ||
        request.entries.empty() || request.entries.size() > 1024 ||
        request.answer.empty() || request.answer.size() > 1024 || request.context.size() > 4096 ||
        !protocol::valid_utf16(request.answer) || !protocol::valid_utf16(request.context)) {
        throw std::invalid_argument("invalid commit event");
    }
    std::u32string answer;
    bool trainable = false;
    for (const auto& entry : request.entries) {
        // Literal positions carry no reading and only provide context; every
        // other position must name a Bopomofo reading.
        if (entry.literal != entry.reading.empty())
            throw std::invalid_argument("invalid commit literal");
        if (!entry.literal) {
            if (entry.reading.empty() || entry.reading.size() > 64 || !protocol::valid_utf16(entry.reading))
                throw std::invalid_argument("invalid commit reading");
            trainable = true;
        }
        if (entry.character == 0 || !protocol::valid_scalar(entry.character))
            throw std::invalid_argument("invalid commit reading");
        answer.push_back(entry.character);
    }
    // A commit without a single composed position teaches nothing; Windows
    // discards those as well.
    if (!trainable) throw std::invalid_argument("commit has no Bopomofo reading");
    if (utf8::utf8to32(utf8::utf16tou8(request.answer)) != answer)
        throw std::invalid_argument("commit answer does not match readings");
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value));
}

CommitProtectionStatus protection_status_of(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    // A database written before encrypted recording has no table at all.
    if (sqlite3_prepare_v2(db, "SELECT enabled FROM commit_protection WHERE id=1", -1, &statement, nullptr) != SQLITE_OK)
        return {};
    const bool row = sqlite3_step(statement) == SQLITE_ROW;
    const auto result = row ? CommitProtectionStatus{true, sqlite3_column_int(statement, 0) != 0}
                            : CommitProtectionStatus{};
    sqlite3_finalize(statement);
    return result;
}

commit_crypto::PublicParameters parameters_of(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT version,salt,public_key FROM commit_protection WHERE id=1", -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_finalize(statement);
        throw std::runtime_error("commit password is not configured");
    }
    if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) != 1) {
        sqlite3_finalize(statement);
        throw std::runtime_error("commit password is not configured");
    }
    commit_crypto::PublicParameters parameters;
    commit_crypto::unhex(column_text(statement, 1),
                         {reinterpret_cast<unsigned char*>(parameters.salt.data()), parameters.salt.size()});
    commit_crypto::unhex(column_text(statement, 2),
                         {reinterpret_cast<unsigned char*>(parameters.key.data()), parameters.key.size()});
    sqlite3_finalize(statement);
    return parameters;
}

}  // namespace

CommitProtectionStatus read_commit_protection(sqlite3* db) { return protection_status_of(db); }

std::filesystem::path CommitStore::default_path() {
    if (const char* path = std::getenv("LLAVON_IME_TRAINING_DATABASE_PATH"); path && *path) return path;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return std::filesystem::path(state) / "llavon-ime" / "training" / "commits.sqlite3";
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    return std::filesystem::path(home) / "Library" / "Application Support" / "llavon-ime" / "training" / "commits.sqlite3";
#else
    return std::filesystem::path(home) / ".local" / "state" / "llavon-ime" / "training" / "commits.sqlite3";
#endif
}

void initialize_commit_database(sqlite3* db) {
    execute(db, "PRAGMA journal_mode=WAL");
    execute(db, "CREATE TABLE IF NOT EXISTS commits (id TEXT PRIMARY KEY, context TEXT NOT NULL, answer TEXT NOT NULL, "
                "state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','excluded','trained')), "
                "schema_version INTEGER NOT NULL DEFAULT 1, "
                "committed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))) ");
    execute(db, "CREATE TABLE IF NOT EXISTS readings (commit_id TEXT NOT NULL REFERENCES commits(id) ON DELETE CASCADE, "
                "position INTEGER NOT NULL, reading TEXT NOT NULL, character INTEGER NOT NULL, "
                "manually_selected INTEGER NOT NULL CHECK(manually_selected IN (0,1)), "
                "PRIMARY KEY(commit_id,position))");
    execute(db, "CREATE INDEX IF NOT EXISTS commits_state_time ON commits(state,committed_at,id)");
    execute(db, "CREATE TABLE IF NOT EXISTS lora_runs (id INTEGER PRIMARY KEY, base_revision TEXT NOT NULL, "
                "adapter_path TEXT NOT NULL, model_path TEXT NOT NULL, record_count INTEGER NOT NULL, "
                "completed_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')))");
    execute(db, "CREATE TABLE IF NOT EXISTS commit_protection (id INTEGER PRIMARY KEY CHECK(id=1), "
                "version INTEGER NOT NULL CHECK(version=1), salt TEXT NOT NULL, public_key TEXT NOT NULL, "
                "enabled INTEGER NOT NULL CHECK(enabled IN (0,1)))");
    // Databases created before encrypted recording lack the marker; the
    // registration is idempotent.
    {
        Statement info(db, "PRAGMA table_info(commits)");
        bool marker = false;
        while (sqlite3_step(info.get()) == SQLITE_ROW) {
            if (column_text(info.get(), 1) == "schema_version") { marker = true; break; }
        }
        if (!marker) execute(db, "ALTER TABLE commits ADD COLUMN schema_version INTEGER NOT NULL DEFAULT 1");
    }
}

CommitStore::CommitStore(std::filesystem::path path) {
    std::filesystem::create_directories(path.parent_path());
    struct stat status {};
    if (::lstat(path.parent_path().c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::getuid())
        throw std::runtime_error("training data directory must belong to the current user");
    if (::chmod(path.parent_path().c_str(), 0700) != 0) throw std::runtime_error("cannot protect training directory");
    if (::lstat(path.c_str(), &status) == 0 && (!S_ISREG(status.st_mode) || status.st_uid != ::getuid() || S_ISLNK(status.st_mode)))
        throw std::runtime_error("unsafe training database path");
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot open training database";
        sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(message);
    }
    try {
        if (::chmod(path.c_str(), 0600) != 0) throw std::runtime_error("cannot protect training database");
        sqlite3_busy_timeout(db_, 3000);
        execute(db_, "PRAGMA foreign_keys=ON");
        initialize_commit_database(db_);
        commit_crypto::initialize();
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}

CommitStore::~CommitStore() {
    // A graceful shutdown settles everything that is still staged.
    if (db_) {
        try {
            std::lock_guard lock(mutex_);
            flush_locked(true, std::chrono::steady_clock::now());
        } catch (...) {
            // Shutdown must not throw; the staged commits are simply lost.
        }
        sqlite3_close(db_);
    }
}

CommitProtectionStatus CommitStore::protection_status() const {
    std::lock_guard lock(mutex_);
    return protection_status_of(db_);
}

bool CommitStore::recording_enabled() const {
    std::lock_guard lock(mutex_);
    const auto status = protection_status_of(db_);
    return status.configured && status.enabled;
}

bool CommitStore::record(const protocol::RecordCommitRequest& request) {
    validate(request);
    std::lock_guard lock(mutex_);
    const auto status = protection_status_of(db_);
    if (!status.configured || !status.enabled) return false;
    // Only the previous commit in this input context is settled early. A
    // different context must keep its own correction window (as on Windows).
    for (auto it = staged_.begin(); it != staged_.end();) {
        if (it->request.source_id == request.source_id) {
            write_locked(it->request);
            it = staged_.erase(it);
        } else ++it;
    }
    if (staged_.size() >= 256) return false;  // never let a stalled store grow without bound
    staged_.push_back(StagedCommit{request, std::chrono::steady_clock::now()});
    return true;
}

bool CommitStore::discard_staged(const protocol::SessionId& event_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(staged_.begin(), staged_.end(),
        [&](const StagedCommit& staged) { return staged.request.event_id == event_id; });
    if (found == staged_.end()) return false;
    staged_.erase(found);
    return true;
}

std::size_t CommitStore::flush_staged(bool all, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex_);
    return flush_locked(all, now);
}

std::size_t CommitStore::flush_locked(bool all, std::chrono::steady_clock::time_point now) {
    std::size_t written = 0;
    std::vector<StagedCommit> remaining;
    for (auto& staged : staged_) {
        if (!all && now - staged.queued_at < kCommitCorrectionWindow) {
            remaining.push_back(std::move(staged));
            continue;
        }
        if (write_locked(staged.request)) ++written;
    }
    staged_ = std::move(remaining);
    return written;
}

bool CommitStore::write_locked(const protocol::RecordCommitRequest& request) {
    const auto parameters = parameters_of(db_);
    const auto id = event_id(request.event_id);
    const auto context = commit_crypto::seal(utf8::utf16to8(request.context), commit_crypto::field_identity(id, "context"), parameters);
    const auto answer = commit_crypto::seal(utf8::utf16to8(request.answer), commit_crypto::field_identity(id, "answer"), parameters);
    execute(db_, "BEGIN IMMEDIATE");
    try {
        Statement insert(db_, "INSERT OR IGNORE INTO commits(id,context,answer,schema_version) VALUES (?,?,?,2)");
        insert.text(1, id); insert.text(2, context); insert.text(3, answer); insert.step();
        const bool inserted = sqlite3_changes(db_) != 0;
        if (inserted) {
            Statement reading(db_, "INSERT INTO readings(commit_id,position,reading,character,manually_selected) VALUES (?,?,?,?,?)");
            for (std::size_t i = 0; i < request.entries.size(); ++i) {
                // The character code would leak the answer, so encrypted rows
                // keep it zero and derive it from the decrypted answer.
                const auto ciphertext = commit_crypto::seal(utf8::utf16to8(request.entries[i].reading),
                                                            commit_crypto::reading_identity(id, static_cast<int>(i)), parameters);
                reading.text(1, id); reading.integer(2, static_cast<std::int64_t>(i));
                reading.text(3, ciphertext); reading.integer(4, 0);
                reading.integer(5, request.entries[i].manually_selected); reading.step(); reading.reset();
            }
        }
        execute(db_, "COMMIT");
        return inserted;
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

void CommitStore::configure_password(const std::string& password) {
    std::lock_guard lock(mutex_);
    if (protection_status_of(db_).configured) throw std::runtime_error("password already configured");
    staged_.clear();  // recording was off, so anything staged is stale
    commit_crypto::PublicParameters parameters;
    randombytes_buf(parameters.salt.data(), parameters.salt.size());
    commit_crypto::PrivateKey private_key;
    commit_crypto::derive(password, parameters, private_key, false);

    execute(db_, "PRAGMA secure_delete=ON");
    execute(db_, "BEGIN IMMEDIATE");
    // Updating a table while a statement scans it can visit a row twice, which
    // would seal it again; the plaintext is read first and wiped afterwards.
    struct PlainRow {
        std::string id, context, answer, reading;
        int position = 0;
    };
    std::vector<PlainRow> commits, readings;
    const auto wipe = [](std::vector<PlainRow>& rows) {
        for (auto& row : rows) {
            sodium_memzero(row.context.data(), row.context.size());
            sodium_memzero(row.answer.data(), row.answer.size());
            sodium_memzero(row.reading.data(), row.reading.size());
        }
        rows.clear();
    };
    try {
        Statement insert(db_, "INSERT INTO commit_protection(id,version,salt,public_key,enabled) VALUES (1,1,?,?,1)");
        insert.text(1, commit_crypto::hex(parameters.salt));
        insert.text(2, commit_crypto::hex(parameters.key));
        insert.step();
        // Convert every existing plaintext row in place; the records and their
        // state are preserved.
        {
            Statement select(db_, "SELECT id,context,answer FROM commits WHERE schema_version=1");
            while (sqlite3_step(select.get()) == SQLITE_ROW) {
                PlainRow row;
                row.id = column_text(select.get(), 0);
                row.context = column_text(select.get(), 1);
                row.answer = column_text(select.get(), 2);
                commits.push_back(std::move(row));
            }
        }
        Statement update(db_, "UPDATE commits SET schema_version=2,context=?,answer=? WHERE id=?");
        for (const auto& row : commits) {
            update.reset();
            update.text(1, commit_crypto::seal(row.context, commit_crypto::field_identity(row.id, "context"), parameters));
            update.text(2, commit_crypto::seal(row.answer, commit_crypto::field_identity(row.id, "answer"), parameters));
            update.text(3, row.id);
            update.step();
        }
        {
            Statement select(db_, "SELECT commit_id,position,reading FROM readings");
            while (sqlite3_step(select.get()) == SQLITE_ROW) {
                PlainRow row;
                row.id = column_text(select.get(), 0);
                row.reading = column_text(select.get(), 2);
                row.position = sqlite3_column_int(select.get(), 1);
                readings.push_back(std::move(row));
            }
        }
        Statement update_reading(db_, "UPDATE readings SET reading=?,character=0 WHERE commit_id=? AND position=?");
        for (const auto& row : readings) {
            update_reading.reset();
            update_reading.text(1, commit_crypto::seal(row.reading,
                commit_crypto::reading_identity(row.id, row.position), parameters));
            update_reading.text(2, row.id);
            update_reading.integer(3, row.position);
            update_reading.step();
        }
        wipe(commits); wipe(readings);
        execute(db_, "COMMIT");
    } catch (...) {
        wipe(commits); wipe(readings);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    execute(db_, "VACUUM");
    if (sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
}

void CommitStore::set_recording_enabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled) (void)parameters_of(db_);
    else staged_.clear();  // disabling collection drops what is not written yet
    Statement update(db_, "UPDATE commit_protection SET enabled=? WHERE id=1");
    update.integer(1, enabled ? 1 : 0);
    update.step();
}

void CommitStore::reset_conversation_data() {
    std::lock_guard lock(mutex_);
    staged_.clear();
    execute(db_, "PRAGMA secure_delete=ON");
    execute(db_, "BEGIN IMMEDIATE");
    try {
        execute(db_, "DELETE FROM commits");
        execute(db_, "DELETE FROM commit_protection");
        execute(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    execute(db_, "VACUUM");
    if (sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
}

void CommitCipher::unlock(sqlite3* db, const std::string& password) {
    parameters_ = parameters_of(db);
    commit_crypto::derive(password, parameters_, private_key_, true);
    unlocked_ = true;
}

commit_crypto::Decryption CommitCipher::decryption() const {
    return {.configured = unlocked_, .parameters = &parameters_, .private_key = &private_key_};
}

std::string CommitCipher::open(const std::string& ciphertext, const std::string& identity) const {
    if (!unlocked_) throw std::runtime_error("password required");
    return commit_crypto::open(ciphertext, identity, parameters_, private_key_);
}

namespace {

// Columns: id, committed_at, state, context, answer, schema_version.
CommitRecord load_record(sqlite3* db, sqlite3_stmt* query, const CommitCipher& cipher) {
    CommitRecord record;
    record.id = column_text(query, 0);
    record.committed_at = column_text(query, 1);
    record.state = column_text(query, 2);
    const auto schema_version = sqlite3_column_int(query, 5);
    if (cipher.unlocked()) {
        if (schema_version != 2) throw std::runtime_error("unencrypted training record blocked");
        record.context = cipher.open(column_text(query, 3), commit_crypto::field_identity(record.id, "context"));
        record.answer = cipher.open(column_text(query, 4), commit_crypto::field_identity(record.id, "answer"));
    } else {
        if (schema_version != 1) throw std::runtime_error("training password required");
        record.context = column_text(query, 3);
        record.answer = column_text(query, 4);
    }
    Statement readings(db, "SELECT position,reading,manually_selected FROM readings WHERE commit_id=? ORDER BY position");
    readings.text(1, record.id);
    while (sqlite3_step(readings.get()) == SQLITE_ROW) {
        record.readings.push_back(cipher.unlocked()
            ? cipher.open(column_text(readings.get(), 1),
                          commit_crypto::reading_identity(record.id, sqlite3_column_int(readings.get(), 0)))
            : column_text(readings.get(), 1));
        record.manual.push_back(sqlite3_column_int(readings.get(), 2) != 0);
    }
    record.text_available = true;
    return record;
}

}  // namespace

std::vector<CommitRecord> read_commits(sqlite3* db, const std::string& state,
                                       const CommitCipher& cipher, int offset, int limit, bool manual_only) {
    // Records that never explicitly selected a candidate take no part when the
    // view or the training run asks for manually selected data only.
    const std::string manual_filter = manual_only
        ? " AND EXISTS(SELECT 1 FROM readings r WHERE r.commit_id=commits.id AND r.manually_selected=1)"
        : "";
    const std::string sql =
        "SELECT id,committed_at,state,context,answer,schema_version FROM commits WHERE state=?" +
        manual_filter + " ORDER BY committed_at DESC,id DESC LIMIT ? OFFSET ?";
    Statement query(db, sql.c_str());
    query.text(1, state); query.integer(2, std::max(1, limit)); query.integer(3, std::max(0, offset));
    std::vector<CommitRecord> result;
    while (sqlite3_step(query.get()) == SQLITE_ROW) result.push_back(load_record(db, query.get(), cipher));
    return result;
}

std::vector<CommitRecord> read_commits_by_id(sqlite3* db, const std::vector<std::string>& ids,
                                             const CommitCipher& cipher) {
    std::vector<CommitRecord> result;
    for (const auto& id : ids) {
        Statement query(db, "SELECT id,committed_at,state,context,answer,schema_version FROM commits WHERE id=?");
        query.text(1, id);
        if (sqlite3_step(query.get()) == SQLITE_ROW) result.push_back(load_record(db, query.get(), cipher));
    }
    return result;
}

}  // namespace ime::unix_service
