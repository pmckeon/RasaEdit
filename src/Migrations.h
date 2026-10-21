#pragma once

// Migrations of the server data (the user's rule, 2026-10-21: every edit of server data is exported as a migration that can be rolled
// back). Each save of WorldDatabase (save, saveCreatures, saveTables) runs in one transaction; a ChangeCapture snapshots every table
// the save writes, just before its first write to it, and diffs it against the table at the end of the save: rows by primary key
// (deleted / inserted / changed columns), keyless tables (vendor_item, creature_appearance) as multisets of whole rows. From that
// diff it writes plain SQL:
//   Up   - the save's changes: deletes, updates, inserts, table by table in the order the save first wrote them (the save writes
//          children before parents for deletes and parents before children for inserts, so foreign keys hold),
//   Down - the same reversed: the tables in reverse order, inserted rows deleted, updates put back, deleted rows inserted again.
// Every statement is guarded - an UPDATE / DELETE names the row by its key and the values it expects (the columns it changes, or the
// whole row), an INSERT fails on a key that exists - and is preceded by a "-- @rows N" line: applying a migration (applyMigrationSql)
// runs it in one transaction and refuses it, rolling back, unless every statement changed exactly that many rows. So a rollback
// cannot undo a row someone changed since; it says which statement did not match.
//
// SQL as RasaEdit's backends run it: identifiers in double quotes, text literals with only ' doubled - SQLite takes it as it is, MySQL
// needs sql_mode ANSI_QUOTES and NO_BACKSLASH_ESCAPES (applyMigrationSql sets both on its session). The same statements in MySQL's
// own dialect (mySqlDialect: identifiers in backticks, \ escaped in text) run on MySQL / MariaDB with the default sql_mode; a save
// writes both: <name>.up.sql / .down.sql and <name>.mysql.up.sql / .mysql.down.sql. The values are the same either way, so a migration
// made on one engine applies on the other (a World made by Rasa.NET's migrations has the same tables and columns on both).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class DbConnection;

// The table a statement writes ("" for anything but INSERT INTO / UPDATE / DELETE FROM).
std::string writtenTable(const std::string& sql);

class ChangeCapture {
public:
    ChangeCapture(DbConnection& db, bool mySql) : db_(db), mySql_(mySql) {}
    // Before a statement runs: the first write to a table snapshots it (tables in `ignored` are not captured).
    void beforeWrite(const std::string& sql);
    struct Result {
        std::string up, down;                        // the statements, each after its "-- @rows N" line
        int inserted = 0, updated = 0, deleted = 0;  // rows Up inserts / changes / deletes
        std::vector<std::string> tables;             // tables Up changes, in order
        bool empty() const { return inserted + updated + deleted == 0; }
    };
    // After the save's writes, inside its transaction: the diff of every captured table.
    Result finish();
    void ignore(const std::string& table) { ignored_.push_back(table); }

    struct Snapshot {
        std::string table;
        std::vector<std::string> columns;
        std::vector<int> key;                        // primary key column indices, in key order (empty: keyless)
        std::vector<std::vector<std::string>> rows;  // SQL literals ("NULL", 42, 0.5, 'text')
        std::vector<std::vector<bool>> real;         // per value: a real number (compared within a tolerance)
    };

private:
    Snapshot read(const std::string& table);
    DbConnection& db_;
    bool mySql_ = false;
    std::vector<std::string> ignored_;
    std::vector<Snapshot> before_;                   // in the order the save first wrote the tables
};

// One statement of a migration file: its SQL and the rows it must change.
struct MigrationStatement {
    std::string sql;
    int64_t rows = 0;
};
std::vector<MigrationStatement> parseMigrationSql(const std::string& text);   // statements after "-- @rows N" lines; comments skipped

// Runs a migration's statements in one transaction on `db`; false (rolled back) with `error` naming the first statement that failed
// or changed another number of rows. With `inTransaction` the caller has begun the transaction and commits / rolls back itself.
bool applyMigrationSql(DbConnection& db, bool mySql, const std::string& text, std::string& error, bool inTransaction = false);

// The statements in MySQL's own dialect: "identifiers" -> `identifiers`, \ in text literals doubled; comment lines kept.
std::string mySqlDialect(const std::string& portable);

// The header a migration file starts with (mySql: for the .mysql file).
std::string migrationHeader(const std::string& name, const std::string& direction, const std::string& database, const std::string& createdUtc,
                            const ChangeCapture::Result& result, bool mySql = false);
