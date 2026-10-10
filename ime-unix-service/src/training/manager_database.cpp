#include "manager_database.hpp"

namespace ime::unix_service::manager {
json query_database(const fs::path& path, const char* sql, int columns) {
    if (!fs::is_regular_file(path)) return json::array();
    const auto owned_db = ime::unix_service::sqlite::open(path, SQLITE_OPEN_READONLY);
    auto* db = owned_db.get();
    sqlite3_busy_timeout(db, 1000);
    const auto owned_stmt = ime::unix_service::sqlite::prepare(db, sql);
    auto* stmt = owned_stmt.get();
    json result = json::array();
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        json row = json::array();
        for (int i = 0; i < columns; ++i) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.push_back(text ? text : "");
        }
        result.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return result;
}
}  // namespace ime::unix_service::manager
