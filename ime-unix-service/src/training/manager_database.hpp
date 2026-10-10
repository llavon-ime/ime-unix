#pragma once

#include "commit_store.hpp"
#include "sqlite.hpp"
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace ime::unix_service::manager {
namespace fs = std::filesystem;
using json = nlohmann::json;
json query_database(const fs::path& path, const char* sql, int columns);

// One database connection per request, never a long-lived manager handle.
class DatabaseHandle {
public:
    explicit DatabaseHandle(const fs::path& path) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("找不到訓練資料庫");
        db_ = ime::unix_service::sqlite::open(path, SQLITE_OPEN_READWRITE);
        sqlite3_busy_timeout(db_.get(), 1000);
        ime::unix_service::initialize_commit_database(db_.get());
    }
    ~DatabaseHandle() = default;
    DatabaseHandle(const DatabaseHandle&) = delete;
    DatabaseHandle& operator=(const DatabaseHandle&) = delete;
    sqlite3* get() const { return db_.get(); }

private:
    ime::unix_service::sqlite::Database db_;
};
}  // namespace ime::unix_service::manager
