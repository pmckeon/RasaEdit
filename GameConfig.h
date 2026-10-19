#pragma once
// Editing tabula_rasa.cfg: a section's entries, and registering a file under a key the game takes before the section's other keys
// (tr::configKeyOrder - [Paths] archives: the first key wins; [Data] zips: the first key comes first on Python's path).
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
