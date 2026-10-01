#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace ime::unix_service::sqlite {

struct DatabaseCloser {
    void operator()(sqlite3* db) const noexcept { (void)sqlite3_close(db); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const noexcept { (void)sqlite3_finalize(statement); }
};

using Database = std::unique_ptr<sqlite3, DatabaseCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

[[nodiscard]] inline Database open(const std::filesystem::path& path, int flags) {
    sqlite3* raw = nullptr;
    const int result = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    Database db(raw);
    if (result != SQLITE_OK) {
        throw std::runtime_error(db ? sqlite3_errmsg(db.get()) : "cannot open training database");
    }
    return db;
}

[[nodiscard]] inline Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement statement(raw);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    return statement;
}

}  // namespace ime::unix_service::sqlite
