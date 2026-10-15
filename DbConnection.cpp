#include "DbConnection.h"

#include <sqlite3.h>
#include <mysql.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>

// ------------------------------------------------------------------------------------------
// SQLite
// ------------------------------------------------------------------------------------------
namespace {

class SqliteStatement final : public DbStatement {
public:
    SqliteStatement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error("SQL error: " + std::string(sqlite3_errmsg(db)) + " in: " + sql);
    }
    ~SqliteStatement() override { sqlite3_finalize(statement_); }
    void bindInt(int index, int64_t value) override { sqlite3_bind_int64(statement_, index, value); }
    void bindReal(int index, double value) override { sqlite3_bind_double(statement_, index, value); }
    void bindText(int index, const std::string& value) override {
        sqlite3_bind_text(statement_, index, value.c_str(), int(value.size()), SQLITE_TRANSIENT);
    }
    void bindNull(int index) override { sqlite3_bind_null(statement_, index); }
    bool step() override { return sqlite3_step(statement_) == SQLITE_ROW; }
    void run() override {
        const int rc = sqlite3_step(statement_);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) throw std::runtime_error(std::string(sqlite3_errmsg(db_)));
    }
    DbType columnType(int column) const override {
        switch (sqlite3_column_type(statement_, column)) {
            case SQLITE_INTEGER: return DbType::Int;
            case SQLITE_FLOAT: return DbType::Real;
            case SQLITE_NULL: return DbType::Null;
            default: return DbType::Text;
        }
    }
    int64_t integer(int column) const override { return sqlite3_column_int64(statement_, column); }
    double realDouble(int column) const override { return sqlite3_column_double(statement_, column); }
    std::string text(int column) const override {
        const unsigned char* t = sqlite3_column_text(statement_, column);
        return t ? reinterpret_cast<const char*>(t) : "";
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class SqliteConnection final : public DbConnection {
public:
    SqliteConnection(const std::string& path, std::vector<std::string>& warnings) : path_(path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            db_ = nullptr;
            readOnly_ = true;
            if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
                const std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
                sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error("cannot open database: " + message);
            }
            warnings.push_back("the file is not writable; opened read-only");
        }
        sqlite3_busy_timeout(db_, 2000);              // the server may be using the file
        // Enforce the schema's foreign keys on the editor's saves (SQLite leaves them off per connection by default): a save that
        // would orphan rows fails and is rolled back instead of writing, and ON DELETE CASCADE rows go with their parent.
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
    }
    ~SqliteConnection() override {
        if (db_) sqlite3_close(db_);
    }
    std::unique_ptr<DbStatement> prepare(const std::string& sql) override { return std::make_unique<SqliteStatement>(db_, sql); }
    void execute(const std::string& sql) override {
        char* message = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            const std::string text = message ? message : "unknown error";
            sqlite3_free(message);
            throw std::runtime_error(text);
        }
    }
    void begin() override { execute("BEGIN IMMEDIATE"); }
    void commit() override { execute("COMMIT"); }
    void rollback() override { execute("ROLLBACK"); }
    std::vector<std::string> tables() override {
        std::vector<std::string> out;
        SqliteStatement s(db_, "SELECT name FROM sqlite_master WHERE type = 'table'");
        while (s.step()) out.push_back(s.text(0));
        return out;
    }
    std::vector<DbColumnInfo> columns(const std::string& table) override {
        std::vector<DbColumnInfo> out;
        SqliteStatement s(db_, "PRAGMA table_info(\"" + table + "\")");
        while (s.step()) out.push_back(DbColumnInfo{s.text(1), s.text(2), s.integer(3) == 0, int(s.integer(5))});
        return out;
    }
    bool readOnly() const override { return readOnly_; }
    bool hasRowId() const override { return true; }
    std::string description() const override { return path_; }

private:
    sqlite3* db_ = nullptr;
    std::string path_;
    bool readOnly_ = false;
};

// ------------------------------------------------------------------------------------------
// MySQL / MariaDB (MariaDB Connector/C, text protocol)
// ------------------------------------------------------------------------------------------
class MySqlConnection;

class MySqlStatement final : public DbStatement {
public:
    MySqlStatement(MySqlConnection& connection, const std::string& sql) : connection_(connection), sql_(sql) {}
    ~MySqlStatement() override {
        if (result_) mysql_free_result(result_);
    }
    void bindInt(int index, int64_t value) override { params_[index] = std::to_string(value); }
    void bindReal(int index, double value) override {
        if (!std::isfinite(value)) {
            params_[index] = "NULL";
            return;
        }
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.17g", value);
        params_[index] = buffer;
    }
    void bindText(int index, const std::string& value) override;
    void bindNull(int index) override { params_[index] = "NULL"; }
    bool step() override;
    void run() override;
    DbType columnType(int column) const override {
        if (!row_ || column < 0 || unsigned(column) >= fieldCount_ || !row_[column]) return DbType::Null;
        switch (fields_[column].type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
            case MYSQL_TYPE_BIT: return DbType::Int;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL: return DbType::Real;
            default: return DbType::Text;
        }
    }
    int64_t integer(int column) const override {
        const char* v = value(column);
        if (!v) return 0;
        if (fields_[column].type == MYSQL_TYPE_BIT) {        // BIT(n) arrives as raw big-endian bytes
            int64_t bits = 0;
            for (unsigned long k = 0; k < lengths_[column]; ++k) bits = (bits << 8) | static_cast<unsigned char>(v[k]);
            return bits;
        }
        if (fields_[column].flags & UNSIGNED_FLAG) return int64_t(std::strtoull(v, nullptr, 10));
        if (columnType(column) == DbType::Real) return int64_t(std::strtod(v, nullptr));
        return std::strtoll(v, nullptr, 10);
    }
    double realDouble(int column) const override {
        const char* v = value(column);
        return v ? std::strtod(v, nullptr) : 0.0;
    }
    std::string text(int column) const override {
        const char* v = value(column);
        return v ? std::string(v, lengths_[column]) : std::string();
    }

private:
    const char* value(int column) const { return row_ && column >= 0 && unsigned(column) < fieldCount_ ? row_[column] : nullptr; }
    std::string finalSql() const;
    void query();

    MySqlConnection& connection_;
    std::string sql_;
    std::map<int, std::string> params_;               // number -> SQL literal
    MYSQL_RES* result_ = nullptr;
    MYSQL_FIELD* fields_ = nullptr;
    unsigned fieldCount_ = 0;
    MYSQL_ROW row_ = nullptr;
    unsigned long* lengths_ = nullptr;
    bool queried_ = false;
};

class MySqlConnection final : public DbConnection {
public:
    MySqlConnection(const MySqlSpec& spec, const std::string& pluginDir) : spec_(spec) {
        mysql_ = mysql_init(nullptr);
        if (!mysql_) throw std::runtime_error("cannot create a MySQL connection (out of memory)");
        mysql_optionsv(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!pluginDir.empty()) mysql_optionsv(mysql_, MYSQL_PLUGIN_DIR, pluginDir.c_str());
        unsigned int timeout = 10;
        mysql_optionsv(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!mysql_real_connect(mysql_, spec.host.c_str(), spec.user.c_str(), spec.password.c_str(), spec.database.c_str(), unsigned(spec.port),
                                nullptr, 0)) {
            const std::string message = mysql_error(mysql_);
            mysql_close(mysql_);
            mysql_ = nullptr;
            throw std::runtime_error("cannot connect to " + mySqlSpecString(spec) + ": " + message);
        }
        // Identifiers in double quotes, as WorldDatabase writes them (string literals stay single-quoted).
        execute("SET SESSION sql_mode = CONCAT(@@SESSION.sql_mode, ',ANSI_QUOTES')");
    }
    ~MySqlConnection() override {
        if (mysql_) mysql_close(mysql_);
    }
    MYSQL* handle() { return mysql_; }
    std::unique_ptr<DbStatement> prepare(const std::string& sql) override { return std::make_unique<MySqlStatement>(*this, sql); }
    void execute(const std::string& sql) override {
        if (mysql_real_query(mysql_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
            throw std::runtime_error(std::string(mysql_error(mysql_)) + " in: " + sql);
        if (MYSQL_RES* r = mysql_store_result(mysql_)) mysql_free_result(r);
    }
    void begin() override { execute("START TRANSACTION"); }
    void commit() override { execute("COMMIT"); }
    void rollback() override { execute("ROLLBACK"); }
    std::vector<std::string> tables() override {
        std::vector<std::string> out;
        MySqlStatement s(*this, "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_TYPE = 'BASE TABLE'");
        while (s.step()) out.push_back(s.text(0));
        return out;
    }
    std::vector<DbColumnInfo> columns(const std::string& table) override {
        std::vector<DbColumnInfo> out;
        {
            MySqlStatement s(*this, "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                    "AND TABLE_NAME = ?1 ORDER BY ORDINAL_POSITION");
            s.bindText(1, table);
            while (s.step()) out.push_back(DbColumnInfo{s.text(0), s.text(1), s.text(2) == "YES", 0});
        }
        MySqlStatement k(*this, "SELECT COLUMN_NAME, ORDINAL_POSITION FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA = DATABASE() "
                                "AND TABLE_NAME = ?1 AND CONSTRAINT_NAME = 'PRIMARY'");
        k.bindText(1, table);
        while (k.step())
            for (DbColumnInfo& c : out)
                if (c.name == k.text(0)) c.keyPosition = int(k.integer(1));
        return out;
    }
    bool readOnly() const override { return false; }
    bool hasRowId() const override { return false; }
    std::string description() const override { return mySqlSpecString(spec_); }

private:
    MYSQL* mysql_ = nullptr;
    MySqlSpec spec_;
};

void MySqlStatement::bindText(int index, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    const unsigned long n = mysql_real_escape_string(connection_.handle(), escaped.data(), value.c_str(), static_cast<unsigned long>(value.size()));
    escaped.resize(n);
    params_[index] = "'" + escaped + "'";
}

// The SQL with every ?N (outside quoted text) replaced by parameter N's literal; an unbound one is NULL.
std::string MySqlStatement::finalSql() const {
    std::string out;
    out.reserve(sql_.size() + 32);
    char quote = 0;
    for (size_t i = 0; i < sql_.size(); ++i) {
        const char c = sql_[i];
        if (quote) {
            out += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            out += c;
            continue;
        }
        if (c == '?' && i + 1 < sql_.size() && std::isdigit(static_cast<unsigned char>(sql_[i + 1]))) {
            size_t j = i + 1;
            int n = 0;
            while (j < sql_.size() && std::isdigit(static_cast<unsigned char>(sql_[j]))) n = n * 10 + (sql_[j++] - '0');
            const auto it = params_.find(n);
            out += it == params_.end() ? std::string("NULL") : it->second;
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}

void MySqlStatement::query() {
    queried_ = true;
    const std::string sql = finalSql();
    MYSQL* m = connection_.handle();
    if (mysql_real_query(m, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
        throw std::runtime_error("SQL error: " + std::string(mysql_error(m)) + " in: " + sql_);
    result_ = mysql_store_result(m);
    if (!result_ && mysql_field_count(m) != 0) throw std::runtime_error("SQL error: " + std::string(mysql_error(m)) + " in: " + sql_);
    if (result_) {
        fieldCount_ = mysql_num_fields(result_);
        fields_ = mysql_fetch_fields(result_);
    }
}

bool MySqlStatement::step() {
    if (!queried_) query();
    if (!result_) return false;
    row_ = mysql_fetch_row(result_);
    lengths_ = row_ ? mysql_fetch_lengths(result_) : nullptr;
    return row_ != nullptr;
}

void MySqlStatement::run() {
    if (!queried_) query();
}

}  // namespace

// ------------------------------------------------------------------------------------------
bool isMySqlSpec(const std::string& spec) { return spec.rfind("mysql://", 0) == 0; }

bool parseMySqlSpec(const std::string& spec, MySqlSpec& out) {
    if (!isMySqlSpec(spec)) return false;
    std::string rest = spec.substr(8);
    const size_t at = rest.rfind('@');
    if (at == std::string::npos) return false;
    const std::string credentials = rest.substr(0, at);
    rest = rest.substr(at + 1);
    const size_t colon = credentials.find(':');
    out.user = credentials.substr(0, colon);
    out.password = colon == std::string::npos ? std::string() : credentials.substr(colon + 1);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    std::string host = rest.substr(0, slash);
    out.database = rest.substr(slash + 1);
    const size_t portColon = host.rfind(':');
    if (portColon != std::string::npos) {
        out.port = std::atoi(host.c_str() + portColon + 1);
        host = host.substr(0, portColon);
    }
    out.host = host.empty() ? "127.0.0.1" : host;
    return !out.user.empty() && !out.database.empty() && out.port > 0;
}

std::string mySqlSpecString(const MySqlSpec& spec) {
    return "mysql://" + spec.user + "@" + spec.host + ":" + std::to_string(spec.port) + "/" + spec.database;
}

std::unique_ptr<DbConnection> openSqlite(const std::string& path, std::vector<std::string>& warnings) {
    return std::make_unique<SqliteConnection>(path, warnings);
}

std::unique_ptr<DbConnection> openMySql(const MySqlSpec& spec, const std::string& pluginDir) {
    return std::make_unique<MySqlConnection>(spec, pluginDir);
}

std::string defaultMySqlPluginDir(const std::string& executableDir) {
    std::string dir = executableDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir + "plugins/libmariadb";
}
