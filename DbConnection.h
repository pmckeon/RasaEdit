#pragma once

// The World database connection behind WorldDatabase: SQLite (a rasaworld.db file) or MySQL / MariaDB (Rasa.NET's other provider,
// Pomelo; MariaDB Connector/C here, which talks to both). WorldDatabase writes portable SQL - double-quoted identifiers, numbered
// parameters ?1 ?2 ... - and each backend adapts it: SQLite takes it as is; MySQL runs with sql_mode ANSI_QUOTES and gets the
// parameters filled in client side as escaped literals (its prepared statements take only plain ?), over the text protocol.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class DbType : uint8_t { Null, Int, Real, Text };

class DbStatement {
public:
    virtual ~DbStatement() = default;
    // Parameters by number, 1-based (?1, ?2, ...); a parameter left unbound is NULL.
    virtual void bindInt(int index, int64_t value) = 0;
    virtual void bindReal(int index, double value) = 0;
    virtual void bindText(int index, const std::string& value) = 0;
    virtual void bindNull(int index) = 0;
    virtual bool step() = 0;                         // the next row; false at the end (a query runs on the first call)
    virtual void run() = 0;                          // INSERT / UPDATE / DELETE; throws on failure
    virtual DbType columnType(int column) const = 0; // of the current row's value (Null for NULL)
    virtual int64_t integer(int column) const = 0;
    virtual double realDouble(int column) const = 0;
    virtual std::string text(int column) const = 0;
};

struct DbColumnInfo {
    std::string name;
    std::string declaredType;                        // "INTEGER", "varchar(64)", "int unsigned", "tinyint(1)", ...
    bool nullable = true;
    int keyPosition = 0;                             // 1-based position in the primary key, 0 = not in it
};

class DbConnection {
public:
    virtual ~DbConnection() = default;
    virtual std::unique_ptr<DbStatement> prepare(const std::string& sql) = 0;   // throws on a syntax / connection error
    virtual void execute(const std::string& sql) = 0;
    virtual void begin() = 0;                         // a write transaction (SQLite: BEGIN IMMEDIATE; MySQL: START TRANSACTION)
    virtual void commit() = 0;
    virtual void rollback() = 0;
    virtual std::vector<std::string> tables() = 0;
    virtual std::vector<DbColumnInfo> columns(const std::string& table) = 0;   // in table order; empty if no such table
    virtual bool readOnly() const = 0;
    virtual bool hasRowId() const = 0;                // SQLite's rowid (insertion order of keyless tables)
    virtual std::string description() const = 0;     // the file, or mysql://user@host:port/database (no password)
};

// A MySQL connection spec: mysql://user[:password]@host[:port]/database. The password may instead be given separately.
struct MySqlSpec {
    std::string user, password, host = "127.0.0.1", database;
    int port = 3306;
};
bool isMySqlSpec(const std::string& spec);
bool parseMySqlSpec(const std::string& spec, MySqlSpec& out);   // false when malformed
std::string mySqlSpecString(const MySqlSpec& spec);              // without the password

// Opens a SQLite file (read-write, else read-only with a warning) or a MySQL database. pluginDir: where MariaDB Connector/C finds
// its authentication plugins (MySQL 8's default caching_sha2_password is one); "" = the connector's built-in default.
std::unique_ptr<DbConnection> openSqlite(const std::string& path, std::vector<std::string>& warnings);
std::unique_ptr<DbConnection> openMySql(const MySqlSpec& spec, const std::string& pluginDir);
// The plugin folder the build copies next to the executable ("<exe dir>/plugins/libmariadb").
std::string defaultMySqlPluginDir(const std::string& executableDir);
