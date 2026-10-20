#include "GameConfig.h"

#include "assetlibrary.h"
#include "gamedata.h"
#include "trformats.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace gamecfg {

namespace {

std::string lower(const std::string& s) { return tr::toLowerAscii(s); }

fs::path configPath(const std::string& gameRoot) { return fs::u8path(gameRoot) / "tabula_rasa.cfg"; }

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool names(const Line& l, const std::string& fileName) { return lower(l.value).find(lower(fileName)) != std::string::npos; }

// A key the game takes before every other key of the section (tr::configKeyOrder): baseKey, else baseKey1, 2, ...
std::string winningKey(const std::vector<Line>& others, const std::string& baseKey) {
    std::vector<std::string> keys;
    for (const Line& l : others) keys.push_back(l.key);
    for (int n = 0; n < 100000; ++n) {
        const std::string candidate = n ? baseKey + std::to_string(n) : baseKey;
        std::vector<std::string> with = keys;
        with.push_back(candidate);
        if (tr::configKeyOrder(with).front() == candidate) return candidate;
    }
    throw tr::FormatError("no key found that the game would take first");
}

}  // namespace

std::vector<Line> sectionLines(const std::string& text, const std::string& section, size_t* headerEnd) {
    std::vector<Line> lines;
    if (headerEnd) *headerEnd = std::string::npos;
    const std::string header = "[" + lower(section) + "]";
    bool inside = false;
    for (size_t p = 0; p < text.size();) {
        size_t end = text.find('\n', p);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(p, end - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        t.erase(0, std::min(t.size(), t.find_first_not_of(" \t")));
        if (!t.empty() && t[0] == '[') {
            inside = lower(t).rfind(header, 0) == 0;
            if (inside && headerEnd) *headerEnd = end < text.size() ? end + 1 : end;
        } else if (inside && !t.empty() && t[0] != ';' && t[0] != '#') {
            const size_t eq = t.find('=');
            if (eq != std::string::npos) {
                Line l;
                l.key = t.substr(0, eq);
                l.key.erase(l.key.find_last_not_of(" \t") + 1);
                l.value = t.substr(eq + 1);
                l.at = p;
                l.length = line.size();
                lines.push_back(l);
            }
        }
        p = end + 1;
    }
    return lines;
}

std::string entryKey(const std::string& gameRoot, const std::string& section, const std::string& fileName) {
    for (const Line& l : sectionLines(readText(configPath(gameRoot)), section))
        if (names(l, fileName)) return l.key;
    return {};
}

bool entryFirst(const std::string& gameRoot, const std::string& section, const std::string& fileName) {
    std::vector<std::string> keys;
    std::string key;
    for (const Line& l : sectionLines(readText(configPath(gameRoot)), section)) {
        keys.push_back(l.key);
        if (names(l, fileName)) key = l.key;
    }
    return !key.empty() && lower(tr::configKeyOrder(keys).front()) == lower(key);
}

std::string registerFirst(const std::string& gameRoot, const std::string& section, const std::string& baseKey, const std::string& value,
                          const std::string& fileName) {
    if (entryFirst(gameRoot, section, fileName))
        return "tabula_rasa.cfg already names " + fileName + " (" + entryKey(gameRoot, section, fileName) + ", taken first)";
    const fs::path cfg = configPath(gameRoot);
    std::error_code ec;
    if (!fs::exists(cfg, ec)) throw tr::FormatError("no tabula_rasa.cfg in " + gameRoot);
    std::string text = readText(cfg);
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    size_t headerEnd = std::string::npos;
    const std::vector<Line> lines = sectionLines(text, section, &headerEnd);
    if (headerEnd == std::string::npos) throw tr::FormatError("tabula_rasa.cfg has no [" + section + "] section");
    std::vector<Line> others;
    const Line* existing = nullptr;
    for (const Line& l : lines) {
        if (names(l, fileName)) existing = &l;
        else others.push_back(l);
    }
    // The game takes a section's keys in its hash-table order (tr::configKeyOrder) - not alphabetical order - so the key is chosen to
    // come first among the keys the file has.
    const std::string key = winningKey(others, baseKey);
    const std::string newLine = key + " = \"" + value + "\"";
    std::string what;
    if (existing) {
        what = "renamed " + existing->key + " to " + key + " (the old key was not taken first)";
        text.replace(existing->at, existing->length, newLine);
    } else {
        std::string line = newLine + eol;
        if (headerEnd == text.size() && !text.empty() && text.back() != '\n') line = eol + line;
        text.insert(headerEnd, line);
        what = "added " + newLine + " to [" + section + "]";
    }
    const fs::path backup = cfg.string() + ".rasaedit-original";
    if (!fs::exists(backup, ec)) fs::copy_file(cfg, backup, ec);
    const fs::path temp = cfg.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) throw tr::FormatError("cannot write " + temp.string());
    }
    fs::rename(temp, cfg, ec);
    if (ec) throw tr::FormatError("cannot replace " + cfg.string() + ": " + ec.message());
    return what + " in " + cfg.string() + " (backup " + backup.filename().string() + ")";
}

}  // namespace gamecfg

namespace textpatch {

std::string pathBeside(const std::string& gameZipPath) { return (fs::path(gameZipPath).parent_path() / kName).string(); }

void write(const std::string& patchPath, const std::string& gameZipName, const std::map<std::string, std::string>& sources) {
    const fs::path patch = fs::path(patchPath);
    const std::string forward = "__path__.append(__path__[0].replace('" + std::string(kName) + "', '" + gameZipName + "'))\n";
    std::map<std::string, tr::Bytes> replace;
    for (const auto& [stem, source] : sources) {
        if (tr::toLowerAscii(stem).rfind("generated/", 0) != 0) throw tr::FormatError(stem + " is not under generated/");
        replace[stem + ".py"] = tr::Bytes(source.begin(), source.end());
        for (size_t slash = stem.find('/'); slash != std::string::npos; slash = stem.find('/', slash + 1))
            replace[stem.substr(0, slash) + "/__init__.py"] = tr::Bytes(forward.begin(), forward.end());
    }
    std::error_code ec;
    fs::path temp = patch;
    temp += ".rasaedit-new";
    if (fs::exists(patch, ec)) {
        tr::ZipArchive existing(patch.string());
        std::vector<std::string> remove;                      // a compiled copy in the patch would be taken first
        for (const auto& [stem, source] : sources)
            for (const tr::ZipEntry& z : existing.entries()) {
                const std::string name = tr::toLowerAscii(z.name);
                if (name == tr::toLowerAscii(stem + ".pyo") || name == tr::toLowerAscii(stem + ".pyc")) remove.push_back(z.name);
            }
        tr::writeZip(existing, temp.string(), replace, remove);
    } else {
        tr::writeZip(temp.string(), replace);
    }
    { tr::ZipArchive check(temp.string()); }                // reads back (a broken write throws here, before the swap)
    fs::rename(temp, patch, ec);
    if (ec) throw tr::FormatError("cannot replace " + patch.string() + " (is the game running?): " + ec.message());
}

std::string registerInConfig(const std::string& gameRoot, const std::string& patchPath) {
    if (gamecfg::entryFirst(gameRoot, "Data", kName)) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(patchPath), fs::path(gameRoot), ec);
    return gamecfg::registerFirst(gameRoot, "Data", "AAA_RasaEditText",
                                  ec || relative.empty() ? fs::path(patchPath).generic_string() : relative.generic_string(), kName);
}

}  // namespace textpatch
