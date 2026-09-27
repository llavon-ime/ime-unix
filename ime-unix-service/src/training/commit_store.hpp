#pragma once

#include "pipe/protocol.hpp"
#include "training/commit_crypto.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace ime::unix_service {

// A commit is staged for this long before it is written, so an immediate
// Backspace can withdraw it. The engine uses the same window.
inline constexpr std::chrono::seconds kCommitCorrectionWindow{10};

// Whether encrypted recording has been set up and whether it is currently on.
struct CommitProtectionStatus {
    bool configured = false;
    bool enabled = false;
};

// Decrypted view of one commit, used by the review and training paths.
struct CommitRecord {
    std::string id;
    std::string committed_at;
    std::string state;
    std::string context;
    std::string answer;
    std::vector<std::string> readings;
    std::vector<bool> manual;
    bool text_available = false;
};

// Read side shared by the service, the CLI, and the manager.
// Creates the tables and adds the encrypted-recording marker when missing.
// Idempotent; needs a writable connection.
void initialize_commit_database(sqlite3* db);
CommitProtectionStatus read_commit_protection(sqlite3* db);

// Holds the derived key material of one verified password. Locks again when
// destroyed, wiping the key. Not copyable or movable: the stored decryption
// view points into the instance.
class CommitCipher final {
public:
    // Verifies the password against the stored parameters. Throws when no
    // password is configured or the password is wrong.
    void unlock(sqlite3* db, const std::string& password);
    bool unlocked() const { return unlocked_; }
    commit_crypto::Decryption decryption() const;
    std::string open(const std::string& ciphertext, const std::string& identity) const;

private:
    bool unlocked_ = false;
    commit_crypto::PublicParameters parameters_{};
    commit_crypto::PrivateKey private_key_{};
};

// When protection is configured the cipher must be unlocked and every row must
// be encrypted. Without protection the legacy plaintext rows stay readable.
// manual_only keeps only records that explicitly selected a candidate.
std::vector<CommitRecord> read_commits(sqlite3* db, const std::string& state,
                                       const CommitCipher& cipher, int offset, int limit,
                                       bool manual_only = false);
std::vector<CommitRecord> read_commits_by_id(sqlite3* db, const std::vector<std::string>& ids,
                                             const CommitCipher& cipher);

// Per-user store. Recording is best effort for the caller, but a record and
// its readings are always written in one transaction. Commits are only stored
// while encrypted recording is enabled; without a configured password nothing
// is written, so plaintext typing never reaches the database.
class CommitStore final {
public:
    explicit CommitStore(std::filesystem::path path = default_path());
    ~CommitStore();
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    bool record(const protocol::RecordCommitRequest& request);
    // Drops a staged commit that the user corrected with an immediate
    // Backspace. Returns whether it was still staged.
    bool discard_staged(const protocol::SessionId& event_id);
    // Writes staged commits whose correction window elapsed, or all of them
    // when `all` is set. Returns how many were written. `now` exists so tests
    // can simulate an elapsed window.
    std::size_t flush_staged(bool all, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    static std::filesystem::path default_path();

    CommitProtectionStatus protection_status() const;
    bool recording_enabled() const;
    // Sets up the password, converts existing plaintext rows in one
    // transaction, then vacuums and truncates the WAL.
    void configure_password(const std::string& password);
    void set_recording_enabled(bool enabled);
    // Removes every conversation record and the derived parameters, and
    // vacuums the database. Models and training history stay.
    void reset_conversation_data();

private:
    struct StagedCommit {
        protocol::RecordCommitRequest request;
        std::chrono::steady_clock::time_point queued_at{};
    };
    // Writes one validated commit; the caller holds the mutex.
    bool write_locked(const protocol::RecordCommitRequest& request);
    std::size_t flush_locked(bool all, std::chrono::steady_clock::time_point now);

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<StagedCommit> staged_;
};

}  // namespace ime::unix_service
