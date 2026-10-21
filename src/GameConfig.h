#pragma once
// Editing tabula_rasa.cfg: a section's entries, and registering a file under a key the game takes before the section's other keys
// (tr::configKeyOrder - [Paths] archives: the first key wins; [Data] zips: the first key comes first on Python's path).
#include <map>
#include <string>
#include <vector>

namespace gamecfg {

// A section's `key = value` lines: key, value as written (quotes kept), byte offset of the line and its length without the end of line.
struct Line {
    std::string key;
    std::string value;
    size_t at = 0;
    size_t length = 0;
};
// headerEnd: the offset just past the section's header line (npos when the section is missing).
std::vector<Line> sectionLines(const std::string& text, const std::string& section, size_t* headerEnd = nullptr);

// The key of the section's entry whose value names fileName (case-insensitive), "" if none (or no tabula_rasa.cfg).
std::string entryKey(const std::string& gameRoot, const std::string& section, const std::string& fileName);
// True when that entry exists and the game takes its key first among the section's keys.
bool entryFirst(const std::string& gameRoot, const std::string& section, const std::string& fileName);
// Makes the game take `<key> = "<value>"` first: adds the line right after the section header, or renames the existing entry's key.
// The key is baseKey, else baseKey1, baseKey2, ... - the first the game would take before every other key of the section. The first
// change keeps tabula_rasa.cfg.rasaedit-original. Returns what it did. Throws tr::FormatError.
std::string registerFirst(const std::string& gameRoot, const std::string& section, const std::string& baseKey, const std::string& value,
                          const std::string& fileName);

}  // namespace gamecfg

// The client text patch zip (CLAUDE.md "A separate Python patch zip works"): data/rasaedit_text.zip, which tabula_rasa.cfg's [Data]
// puts on Python's path before game.zip. Shared by the Text workspace's save and viewport_test's in-game hooks.
namespace textpatch {

constexpr const char* kName = "rasaedit_text.zip";
std::string pathBeside(const std::string& gameZipPath);   // the patch beside game.zip
// Writes module sources (game.zip entry stems under generated/, e.g. "generated/client/language/english/uielementlanguage" -> its
// Python source) into the patch as stored .py, merged into an existing patch (a compiled copy of a written module removed), with an
// __init__.py for every package level that adds game.zip's same folder to the package's path (Python 2.4 binds a package to the first
// path entry that has it - modules the patch lacks then still load from game.zip). Written beside and swapped in after a read-back.
// Throws tr::FormatError.
void write(const std::string& patchPath, const std::string& gameZipName, const std::map<std::string, std::string>& sources);
// Adds the [Data] entry when the game does not take the patch first yet; "" when it already does, else what was done.
std::string registerInConfig(const std::string& gameRoot, const std::string& patchPath);

}  // namespace textpatch
