#include "pma/core.hpp"
#include <sqlite3.h>
#include <memory>
namespace pma {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error("Cannot prepare database query");
    return Statement(stmt, sqlite3_finalize);
}
void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Cannot initialize database");
}
void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) throw std::runtime_error("Cannot bind database value");
}
}
Store::Store(const fs::path& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        throw std::runtime_error("Cannot open state database");
    }
    sqlite3_busy_timeout(db_, 5000);
    try {
        execute(db_, "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS reports(id INTEGER PRIMARY KEY, provider TEXT NOT NULL, checked_at TEXT NOT NULL, report TEXT NOT NULL); CREATE INDEX IF NOT EXISTS reports_provider ON reports(provider,id);");
    } catch (...) { sqlite3_close(db_); throw; }
}
Store::~Store() { sqlite3_close(db_); }
void Store::save(const Json& report) {
    std::lock_guard lock(mutex_);
    auto stmt = prepare(db_, "INSERT INTO reports(provider,checked_at,report) VALUES(?,?,?)");
    bind_text(stmt.get(), 1, report.at("id")); bind_text(stmt.get(), 2, report.at("checked_at")); bind_text(stmt.get(), 3, report.dump());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error("Cannot save monitoring report");
    // Keep 100 reports per provider; Git caches remain independently reusable.
    auto prune = prepare(db_, "DELETE FROM reports WHERE provider=? AND id NOT IN (SELECT id FROM reports WHERE provider=? ORDER BY id DESC LIMIT 100)");
    bind_text(prune.get(), 1, report.at("id")); bind_text(prune.get(), 2, report.at("id"));
    if (sqlite3_step(prune.get()) != SQLITE_DONE) throw std::runtime_error("Cannot prune monitoring history");
}
Json Store::latest() {
    std::lock_guard lock(mutex_); Json result = Json::object();
    auto stmt = prepare(db_, "SELECT provider,report FROM reports WHERE id IN (SELECT MAX(id) FROM reports GROUP BY provider)");
    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        result[reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0))] = Json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1)));
    }
    if (status != SQLITE_DONE) throw std::runtime_error("Cannot read latest reports");
    return result;
}
Json Store::history(const std::string& provider) {
    std::lock_guard lock(mutex_); Json result = Json::array();
    auto stmt = prepare(db_, "SELECT id,checked_at,report FROM reports WHERE provider=? ORDER BY id DESC LIMIT 20"); bind_text(stmt.get(), 1, provider);
    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        auto report = Json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2)));
        result.push_back({{"run_id", sqlite3_column_int64(stmt.get(), 0)}, {"checked_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1))}, {"status", report.value("status", "unknown")}, {"head", report.value("head", "")}, {"summary", report.value("summary", "")}});
    }
    if (status != SQLITE_DONE) throw std::runtime_error("Cannot read report history");
    return result;
}
}
