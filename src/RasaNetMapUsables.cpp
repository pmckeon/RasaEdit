#include "RasaNetMapUsables.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace rasanet {
namespace {

bool readFile(const fs::path& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

// Removes // comments (the table's lines carry them) so they cannot be read as code.
std::string stripComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {                              // keep string literals as they are (names may hold "//")
            const size_t end = text.find('"', i + 1);
            const size_t stop = end == std::string::npos ? text.size() : end + 1;
            out.append(text, i, stop - i);
            i = stop - 1;
            continue;
        }
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// The arguments of a call whose '(' is at `open`: split at top-level commas (nested calls such as new Vector3(...) kept whole).
std::vector<std::string> callArguments(const std::string& text, size_t open, size_t& close) {
    std::vector<std::string> args;
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            const size_t end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            i = end;
            continue;
        }
        if (c == '(') ++depth;
        else if (c == ')') {
            if (--depth == 0) {
                args.push_back(trim(text.substr(start, i - start)));
                close = i;
                return args;
            }
        } else if (c == ',' && depth == 1) {
            args.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    close = std::string::npos;
    return {};
}

// A helper `private static Usable Name(params) => new Usable(mapContextId, entityId, ..., state, ...)`: which of its parameters
// hold the map, the entity and the state, or the fixed state its body names.
struct Helper {
    int mapParam = -1, entityParam = -1, stateParam = -1;
    std::string fixedState;                  // UseObjectState member, when the body names one
};

}  // namespace

std::string findSource(const std::string& configured, const std::string& databasePath) {
    auto check = [](const fs::path& p) -> std::string {
        for (const fs::path& candidate : {p / "src" / "Rasa.Game" / "Managers" / "MapUsables.cs", p / "Rasa.Game" / "Managers" / "MapUsables.cs",
                                          p / "Managers" / "MapUsables.cs"}) {
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) return candidate.parent_path().parent_path().string();   // the Rasa.Game folder
        }
        return {};
    };
    if (!configured.empty()) return check(fs::u8path(configured));
    if (databasePath.empty() || databasePath.rfind("mysql://", 0) == 0) return {};
    std::error_code ec;
    fs::path dir = fs::absolute(fs::u8path(databasePath), ec).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up) {
        const std::string found = check(dir);
        if (!found.empty()) return found;
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

MapUsableStates mapUsableStates(const std::string& sourceRoot, uint32_t mapContextId) {
    MapUsableStates out;
    if (sourceRoot.empty()) return out;
    const fs::path game = fs::u8path(sourceRoot);
    std::string enumText, tableText;
    if (!readFile(game / "Data" / "UseObjectState.cs", enumText)) {
        out.error = "Rasa.NET source: Data/UseObjectState.cs not found";
        return out;
    }
    const fs::path tablePath = game / "Managers" / "MapUsables.cs";
    if (!readFile(tablePath, tableText)) {
        out.error = "Rasa.NET source: Managers/MapUsables.cs not found";
        return out;
    }
    out.source = tablePath.string();
    enumText = stripComments(enumText);
    tableText = stripComments(tableText);

    std::map<std::string, int32_t> stateValues;           // UseObjectState member -> value
    {
        const std::regex member(R"((\w+)\s*=\s*(\d+))");
        for (auto it = std::sregex_iterator(enumText.begin(), enumText.end(), member); it != std::sregex_iterator(); ++it)
            stateValues[(*it)[1]] = std::stoi((*it)[2]);
    }
    std::map<std::string, uint64_t> constants;            // public const uint Name = value;
    {
        const std::regex constant(R"(const\s+u?int\s+(\w+)\s*=\s*(\d+))");
        for (auto it = std::sregex_iterator(tableText.begin(), tableText.end(), constant); it != std::sregex_iterator(); ++it)
            constants[(*it)[1]] = std::stoull((*it)[2]);
    }
    auto number = [&](const std::string& s, uint64_t& v) {
        const std::string t = trim(s);
        if (t.empty()) return false;
        if (std::isdigit(static_cast<unsigned char>(t[0]))) {
            v = std::stoull(t);
            return true;
        }
        const auto c = constants.find(t);
        if (c == constants.end()) return false;
        v = c->second;
        return true;
    };
    auto stateOf = [&](const std::string& s, int32_t& v) {
        const std::string t = trim(s);
        const std::string prefix = "UseObjectState.";
        if (t.rfind(prefix, 0) != 0) return false;
        const auto it = stateValues.find(t.substr(prefix.size()));
        if (it == stateValues.end()) return false;
        v = it->second;
        return true;
    };

    // The helpers.
    std::map<std::string, Helper> helpers;
    {
        const std::regex head(R"(static\s+Usable\s+(\w+)\s*\()");
        for (auto it = std::sregex_iterator(tableText.begin(), tableText.end(), head); it != std::sregex_iterator(); ++it) {
            const size_t open = size_t(it->position(0) + it->length(0) - 1);
            size_t close = 0;
            const std::vector<std::string> params = callArguments(tableText, open, close);
            if (close == std::string::npos) continue;
            const size_t body = tableText.find("new Usable", close);
            const size_t bodyOpen = body == std::string::npos ? std::string::npos : tableText.find('(', body);
            if (bodyOpen == std::string::npos) continue;
            size_t bodyClose = 0;
            const std::vector<std::string> args = callArguments(tableText, bodyOpen, bodyClose);
            if (args.size() < 5) continue;
            auto paramIndex = [&](const std::string& arg) {
                for (size_t k = 0; k < params.size(); ++k) {
                    const size_t space = params[k].find_last_of(" \t");
                    if (trim(space == std::string::npos ? params[k] : params[k].substr(space + 1)) == trim(arg)) return int(k);
                }
                return -1;
            };
            Helper h;
            h.mapParam = paramIndex(args[0]);
            h.entityParam = paramIndex(args[1]);
            int32_t fixed = 0;
            if (stateOf(args[4], fixed)) h.fixedState = trim(args[4]);
            else h.stateParam = paramIndex(args[4]);
            helpers[(*it)[1]] = h;
        }
    }

    // The table: `All = new[] { ... };`.
    const size_t all = tableText.find("IReadOnlyList<Usable> All");
    const size_t begin = all == std::string::npos ? std::string::npos : tableText.find('{', all);
    const size_t end = begin == std::string::npos ? std::string::npos : tableText.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        out.error = "Rasa.NET source: MapUsables.All not found";
        return out;
    }
    const std::string table = tableText.substr(begin, end - begin);
    const std::regex call(R"((new\s+Usable|\b[A-Z]\w*)\s*\()");
    size_t entries = 0, unread = 0;
    for (size_t pos = 0; pos < table.size();) {
        std::smatch m;
        const std::string rest = table.substr(pos);
        if (!std::regex_search(rest, m, call)) break;
        const size_t open = pos + size_t(m.position(0) + m.length(0) - 1);
        size_t close = 0;
        const std::vector<std::string> args = callArguments(table, open, close);
        if (close == std::string::npos) break;
        pos = close + 1;
        const std::string name = m[1];
        uint64_t map = 0, entity = 0;
        int32_t state = 0;
        bool ok = false;
        if (name.rfind("new", 0) == 0) {
            ok = args.size() >= 5 && number(args[0], map) && number(args[1], entity) && stateOf(args[4], state);
        } else {
            const auto h = helpers.find(name);
            if (h == helpers.end()) continue;                 // not a table entry (e.g. new Vector3 inside one is skipped above)
            const Helper& helper = h->second;
            ok = helper.mapParam >= 0 && helper.entityParam >= 0 && size_t(std::max(helper.mapParam, helper.entityParam)) < args.size() &&
                 number(args[size_t(helper.mapParam)], map) && number(args[size_t(helper.entityParam)], entity) &&
                 (helper.fixedState.empty() ? helper.stateParam >= 0 && size_t(helper.stateParam) < args.size() && stateOf(args[size_t(helper.stateParam)], state)
                                            : stateOf(helper.fixedState, state));
        }
        ++entries;
        if (!ok) {
            ++unread;
            continue;
        }
        if (map == mapContextId) out.states[entity] = state;
    }
    if (entries == 0) out.error = "Rasa.NET source: no entries read from MapUsables.All";
    else if (unread > 0) out.error = "Rasa.NET source: " + std::to_string(unread) + " of " + std::to_string(entries) + " MapUsables entries not understood";
    return out;
}

std::string navMeshFolder(const std::string& configured, const std::string& gameSourceFolder) {
    if (!configured.empty()) return configured;
    if (gameSourceFolder.empty()) return {};
    std::error_code ec;
    fs::path dir = fs::absolute(fs::u8path(gameSourceFolder), ec);
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        if (fs::is_regular_file(dir / "Rasa.NET.sln", ec)) {
            const fs::path folder = dir / "navmesh";
            if (!fs::is_directory(folder, ec)) return {};
            const auto text = folder.u8string();
            return std::string(text.begin(), text.end());
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

std::string navMeshFile(const std::string& folder, const std::string& mapName) {
    if (folder.empty()) return {};
    std::string name = mapName;
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto text = (fs::u8path(folder) / fs::u8path(name + ".nav")).u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace rasanet
