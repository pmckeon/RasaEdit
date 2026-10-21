#include "Migrations.h"

#include "DbConnection.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {

std::string quoteIdentifier(const std::string& name) { return "\"" + name + "\""; }

std::string textLiteral(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

// The shortest text that reads back as the same double (std::to_chars), with a decimal point so it stays a real: 17 digits read back
// a unit in the last place off in some SQLite versions' own conversion.
std::string realLiteral(double v) {
    if (!std::isfinite(v)) return "NULL";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, v);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

bool startsWithNoCase(const std::string& text, size_t at, const char* word) {
    for (size_t k = 0; word[k]; ++k) {
        if (at + k >= text.size()) return false;
        char c = text[at + k];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c != word[k]) return false;
    }
    return true;
}

// A condition that the column holds the literal: IS NULL, = value, or within a tolerance for real numbers (a float column of MySQL
// keeps fewer digits than the literal carries).
std::string condition(const std::string& column, const std::string& literal, bool real) {
    if (literal == "NULL") return quoteIdentifier(column) + " IS NULL";
    if (real) {
        const double v = std::strtod(literal.c_str(), nullptr);
        char tolerance[32];
        std::snprintf(tolerance, sizeof tolerance, "%.3g", std::max(1e-6, std::fabs(v) * 1e-6));
        return "ABS(" + quoteIdentifier(column) + " - (" + literal + ")) <= " + tolerance;
    }
    return quoteIdentifier(column) + " = " + literal;
}

struct Writer {
    std::string text;
    void statement(int64_t rows, const std::string& sql) { text += "-- @rows " + std::to_string(rows) + "\n" + sql + ";\n"; }
};

std::string keyOf(const ChangeCapture::Snapshot& s, size_t row) {
    std::string key;
    for (int c : s.key) key += s.rows[row][size_t(c)] + '\x1f';
    return key;
}

std::string wholeRow(const ChangeCapture::Snapshot& s, size_t row) {
    std::string all;
    for (const std::string& v : s.rows[row]) all += v + '\x1f';
    return all;
}

std::string whereAll(const ChangeCapture::Snapshot& s, size_t row) {
    std::string w;
    for (size_t c = 0; c < s.columns.size(); ++c) w += (w.empty() ? "" : " AND ") + condition(s.columns[c], s.rows[row][c], s.real[row][c]);
    return w;
}

std::string insertRow(const ChangeCapture::Snapshot& s, size_t row) {
    std::string names, values;
    for (size_t c = 0; c < s.columns.size(); ++c) {
        names += (c ? ", " : "") + quoteIdentifier(s.columns[c]);
        values += (c ? ", " : "") + s.rows[row][c];
    }
    return "INSERT INTO " + quoteIdentifier(s.table) + " (" + names + ") VALUES (" + values + ")";
}

// UPDATE from row `from` of `a` to row `to` of `b`, guarded by the key and the old values of the changed columns.
std::string updateRow(const ChangeCapture::Snapshot& a, size_t from, const ChangeCapture::Snapshot& b, size_t to) {
    std::string sets, where;
    for (int c : a.key) where += (where.empty() ? "" : " AND ") + condition(a.columns[size_t(c)], a.rows[from][size_t(c)], a.real[from][size_t(c)]);
    for (size_t c = 0; c < a.columns.size(); ++c) {
        if (a.rows[from][c] == b.rows[to][c]) continue;
        sets += (sets.empty() ? "" : ", ") + quoteIdentifier(a.columns[c]) + " = " + b.rows[to][c];
        where += " AND " + condition(a.columns[c], a.rows[from][c], a.real[from][c]);
    }
    return "UPDATE " + quoteIdentifier(a.table) + " SET " + sets + " WHERE " + where;
}

}  // namespace

std::string writtenTable(const std::string& sql) {
    size_t at = sql.find_first_not_of(" \t\r\n");
    if (at == std::string::npos) return "";
    if (startsWithNoCase(sql, at, "INSERT INTO ")) at += 12;
    else if (startsWithNoCase(sql, at, "DELETE FROM ")) at += 12;
    else if (startsWithNoCase(sql, at, "UPDATE ")) at += 7;
    else return "";
    at = sql.find_first_not_of(" \t\r\n", at);
    if (at == std::string::npos) return "";
    if (sql[at] == '"' || sql[at] == '`') {
        const size_t end = sql.find(sql[at], at + 1);
        return end == std::string::npos ? "" : sql.substr(at + 1, end - at - 1);
    }
    size_t end = at;
    while (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_')) ++end;
    return sql.substr(at, end - at);
}

ChangeCapture::Snapshot ChangeCapture::read(const std::string& table) {
    Snapshot s;
    s.table = table;
    std::vector<std::pair<int, int>> keyed;
    const std::vector<DbColumnInfo> columns = db_.columns(table);
    for (size_t c = 0; c < columns.size(); ++c) {
        s.columns.push_back(columns[c].name);
        if (columns[c].keyPosition > 0) keyed.emplace_back(columns[c].keyPosition, int(c));
    }
    std::sort(keyed.begin(), keyed.end());
    for (const auto& k : keyed) s.key.push_back(k.second);
    if (s.columns.empty()) return s;
    std::string select;
    for (const std::string& c : s.columns) select += (select.empty() ? "" : ", ") + quoteIdentifier(c);
    auto statement = db_.prepare("SELECT " + select + " FROM " + quoteIdentifier(table));
    while (statement->step()) {
        std::vector<std::string> row;
        std::vector<bool> real;
        for (size_t c = 0; c < s.columns.size(); ++c) {
            switch (statement->columnType(int(c))) {
                case DbType::Null: row.push_back("NULL"); real.push_back(false); break;
                case DbType::Int: row.push_back(std::to_string(statement->integer(int(c)))); real.push_back(false); break;
                case DbType::Real: row.push_back(realLiteral(statement->realDouble(int(c)))); real.push_back(true); break;
                default: row.push_back(textLiteral(statement->text(int(c)))); real.push_back(false); break;
            }
        }
        s.rows.push_back(std::move(row));
        s.real.push_back(std::move(real));
    }
    return s;
}

void ChangeCapture::beforeWrite(const std::string& sql) {
    const std::string table = writtenTable(sql);
    if (table.empty()) return;
    const auto same = [&](const std::string& other) {
        if (other.size() != table.size()) return false;
        for (size_t k = 0; k < other.size(); ++k)
            if (std::tolower(static_cast<unsigned char>(other[k])) != std::tolower(static_cast<unsigned char>(table[k]))) return false;
        return true;
    };
    for (const std::string& t : ignored_)
        if (same(t)) return;
    for (const Snapshot& s : before_)
        if (same(s.table)) return;
    before_.push_back(read(table));
}

ChangeCapture::Result ChangeCapture::finish() {
    Result result;
    struct TableDiff {
        Snapshot before, after;
        std::vector<size_t> deleted, inserted;                       // rows of before / after
        std::vector<std::pair<size_t, size_t>> changed;              // (before row, after row)
        std::vector<std::tuple<size_t, size_t, size_t, bool>> multiset;   // keyless: (row, before count, after count, row in after)
    };
    std::vector<TableDiff> diffs;
    for (const Snapshot& b : before_) {
        TableDiff d;
        d.before = b;
        d.after = read(b.table);
        if (!b.key.empty()) {
            std::map<std::string, size_t> afterRows;
            for (size_t r = 0; r < d.after.rows.size(); ++r) afterRows[keyOf(d.after, r)] = r;
            std::map<std::string, size_t> beforeRows;
            for (size_t r = 0; r < b.rows.size(); ++r) {
                const std::string key = keyOf(b, r);
                beforeRows[key] = r;
                const auto it = afterRows.find(key);
                if (it == afterRows.end()) d.deleted.push_back(r);
                else if (b.rows[r] != d.after.rows[it->second]) d.changed.emplace_back(r, it->second);
            }
            for (size_t r = 0; r < d.after.rows.size(); ++r)
                if (!beforeRows.count(keyOf(d.after, r))) d.inserted.push_back(r);
        } else {
            std::map<std::string, std::pair<size_t, size_t>> counts;     // row -> (before, after)
            std::map<std::string, std::pair<size_t, bool>> sample;       // row -> (index, in after)
            for (size_t r = 0; r < b.rows.size(); ++r) {
                const std::string w = wholeRow(b, r);
                ++counts[w].first;
                sample.emplace(w, std::make_pair(r, false));
            }
            for (size_t r = 0; r < d.after.rows.size(); ++r) {
                const std::string w = wholeRow(d.after, r);
                ++counts[w].second;
                sample.emplace(w, std::make_pair(r, true));
            }
            for (const auto& [w, c] : counts)
                if (c.first != c.second) d.multiset.emplace_back(sample[w].first, c.first, c.second, sample[w].second);
        }
        if (d.deleted.empty() && d.inserted.empty() && d.changed.empty() && d.multiset.empty()) continue;
        result.tables.push_back(b.table);
        diffs.push_back(std::move(d));
    }

    Writer up, down;
    for (const TableDiff& d : diffs) {
        for (size_t r : d.deleted) up.statement(1, "DELETE FROM " + quoteIdentifier(d.before.table) + " WHERE " + whereAll(d.before, r));
        for (const auto& [from, to] : d.changed) up.statement(1, updateRow(d.before, from, d.after, to));
        for (size_t r : d.inserted) up.statement(1, insertRow(d.after, r));
        result.deleted += int(d.deleted.size());
        result.updated += int(d.changed.size());
        result.inserted += int(d.inserted.size());
        for (const auto& [row, b, a, inAfter] : d.multiset) {
            const Snapshot& s = inAfter ? d.after : d.before;
            if (a < b) {
                up.statement(int64_t(b), "DELETE FROM " + quoteIdentifier(s.table) + " WHERE " + whereAll(s, row));
                for (size_t k = 0; k < a; ++k) up.statement(1, insertRow(s, row));
                result.deleted += int(b - a);
            } else {
                for (size_t k = b; k < a; ++k) up.statement(1, insertRow(s, row));
                result.inserted += int(a - b);
            }
        }
    }
    for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
        const TableDiff& d = *it;
        for (size_t r : d.inserted) down.statement(1, "DELETE FROM " + quoteIdentifier(d.after.table) + " WHERE " + whereAll(d.after, r));
        for (const auto& [from, to] : d.changed) down.statement(1, updateRow(d.after, to, d.before, from));
        for (size_t r : d.deleted) down.statement(1, insertRow(d.before, r));
        for (const auto& [row, b, a, inAfter] : d.multiset) {
            const Snapshot& s = inAfter ? d.after : d.before;
            if (a > 0) down.statement(int64_t(a), "DELETE FROM " + quoteIdentifier(s.table) + " WHERE " + whereAll(s, row));
            for (size_t k = 0; k < b; ++k) down.statement(1, insertRow(s, row));
        }
    }
    result.up = up.text;
    result.down = down.text;
    before_.clear();
    return result;
}

std::vector<MigrationStatement> parseMigrationSql(const std::string& text) {
    std::vector<MigrationStatement> out;
    std::istringstream in(text);
    std::string line;
    bool open = false;
    const auto close = [&]() {
        if (!open) return;
        std::string& sql = out.back().sql;
        while (!sql.empty() && (sql.back() == '\n' || sql.back() == '\r' || sql.back() == ' ')) sql.pop_back();
        if (!sql.empty() && sql.back() == ';') sql.pop_back();
        open = false;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("-- @rows ", 0) == 0) {
            close();
            out.push_back(MigrationStatement{"", std::strtoll(line.c_str() + 9, nullptr, 10)});
            open = true;
        } else if (open) {
            out.back().sql += line + "\n";
        }
    }
    close();
    return out;
}

bool applyMigrationSql(DbConnection& db, bool mySql, const std::string& text, std::string& error, bool inTransaction) {
    const std::vector<MigrationStatement> statements = parseMigrationSql(text);
    std::string oldMode;
    if (mySql) {
        auto mode = db.prepare("SELECT @@SESSION.sql_mode");
        if (mode->step()) oldMode = mode->text(0);
        db.execute("SET SESSION sql_mode = CONCAT(@@SESSION.sql_mode, ',ANSI_QUOTES,NO_BACKSLASH_ESCAPES')");
    }
    const auto restoreMode = [&]() {
        if (mySql) {
            try { db.execute("SET SESSION sql_mode = " + textLiteral(oldMode)); } catch (const std::exception&) {}
        }
    };
    try {
        if (!inTransaction) db.begin();
        for (size_t k = 0; k < statements.size(); ++k) {
            db.execute(statements[k].sql);
            const int64_t changed = db.changes();
            if (changed != statements[k].rows) {
                std::string shown = statements[k].sql.substr(0, 240);
                throw std::runtime_error("statement " + std::to_string(k + 1) + " of " + std::to_string(statements.size()) + " changed " +
                                         std::to_string(changed) + " row(s), not " + std::to_string(statements[k].rows) +
                                         " - the data is no longer as the migration expects: " + shown);
            }
        }
        if (!inTransaction) db.commit();
    } catch (const std::exception& e) {
        error = e.what();
        if (!inTransaction) {
            try { db.rollback(); } catch (const std::exception&) {}
        }
        restoreMode();
        return false;
    }
    restoreMode();
    return true;
}

std::string mySqlDialect(const std::string& portable) {
    std::string out;
    out.reserve(portable.size() + portable.size() / 16);
    enum class State { Normal, Text, Identifier } state = State::Normal;
    bool lineStart = true;
    for (size_t k = 0; k < portable.size(); ++k) {
        const char c = portable[k];
        if (state == State::Normal && lineStart && c == '-' && k + 1 < portable.size() && portable[k + 1] == '-') {
            const size_t end = portable.find('\n', k);                  // a comment line, as it is
            const size_t stop = end == std::string::npos ? portable.size() : end + 1;
            out.append(portable, k, stop - k);
            k = stop - 1;
            lineStart = true;
            continue;
        }
        lineStart = c == '\n';
        switch (state) {
            case State::Normal:
                if (c == '"') { out += '`'; state = State::Identifier; }
                else {
                    out += c;
                    if (c == '\'') state = State::Text;
                }
                break;
            case State::Identifier:
                if (c == '"') { out += '`'; state = State::Normal; }
                else out += c;
                break;
            case State::Text:
                if (c == '\\') out += "\\\\";
                else if (c == '\'') {
                    out += c;
                    if (k + 1 < portable.size() && portable[k + 1] == '\'') out += portable[++k];   // '' inside the text
                    else state = State::Normal;
                } else out += c;
                break;
        }
    }
    return out;
}

std::string migrationHeader(const std::string& name, const std::string& direction, const std::string& database, const std::string& createdUtc,
                            const ChangeCapture::Result& result, bool mySql) {
    std::string tables;
    for (const std::string& t : result.tables) tables += (tables.empty() ? "" : ", ") + t;
    char counts[160];
    std::snprintf(counts, sizeof counts, "%d inserted, %d updated, %d deleted", result.inserted, result.updated, result.deleted);
    return "-- RasaEdit migration " + name + " (" + direction + ")\n" +
           "-- Database: " + database + "\n" +
           "-- Created (UTC): " + createdUtc + "\n" +
           "-- Up changes rows: " + counts + "; tables: " + tables + "\n" +
           "-- RasaEdit applies it (Server workspace, Migrations panel) in one transaction: every statement follows a \"-- @rows N\" line and\n"
           "-- must change exactly N rows, else nothing is applied.\n" +
           (mySql ? "-- MySQL / MariaDB dialect (backtick identifiers, backslashes escaped): runs as it is with the default sql_mode. The\n"
                    "-- statements are guarded (each names the rows it expects), so check that each reports the rows above when run by hand.\n\n"
                  : "-- Portable dialect (double-quoted identifiers, only ' escaped): SQLite as it is; MySQL / MariaDB needs\n"
                    "-- SET SESSION sql_mode = CONCAT(@@SESSION.sql_mode, ',ANSI_QUOTES,NO_BACKSLASH_ESCAPES'); or the .mysql file.\n\n");
}
