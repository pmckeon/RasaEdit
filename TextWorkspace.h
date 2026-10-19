#pragma once

#include "Workspace.h"
#include "gamedata.h"

#include <map>
#include <set>
#include <string>
#include <vector>

// The Text workspace: the client's static text - the language modules of data/game.zip (generated/client/language/<language>/
// <name>language.pyo, e.g. missiontextlanguage, npcgreetinglanguage, itemtemplatelanguage). Every one holds a single
// `lookup` dict: id -> u'text', (id, gender) -> [u'text'] (gender 1 male / 2 female character), or (serverselection) id ->
// (name, region, port). A language dropdown picks the language; the Modules panel a module (or all of them, for searching);
// the Texts panel lists and searches the entries; the Entry panel edits the chosen entry's strings (both genders, or one each),
// adds and deletes entries, and lists where mission texts are used (the client's missionconversation / missionobjective /
// objectiveconversation tables). Undo / redo per change. Saving writes each edited module as Python source (tr::moduleSource,
// non-ASCII as \uXXXX) into a patch zip, data/rasaedit_text.zip, that tabula_rasa.cfg's [Data] puts before game.zip (added on the
// first save), with forwarding package __init__.py files - or, when chosen ("Save to", setting textSave = game), into game.zip
// itself (first save keeps game.zip.rasaedit-original; compiled copies removed - Python's zip import takes a .pyo before a .py).
class TextWorkspace : public Workspace {
public:
    explicit TextWorkspace(EditorContext& ctx);

    const char* name() const override { return "Text"; }
    const char* dockspaceName() const override { return "TextDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer*) override {}
    void onGameOpened() override;
    bool hasUnsavedChanges() const override { return version_ != savedVersion_; }
    bool saveChanges() override;
    void handleShortcuts() override;
    void drawEditMenu() override;
    void revertChanges();
    void showEntry(const std::string& module, int64_t id);     // switch to the module's language, select and scroll to the entry

    // For tests: the modules of the chosen language, choosing one, the shown rows and editing an entry's strings.
    struct ModuleInfo {
        std::string module;            // dotted, e.g. generated.client.language.english.missiontextlanguage
        std::string label;             // "missiontext"
        bool gendered = false;         // (id, gender) keys
        size_t ids = 0;                // distinct ids
    };
    const std::vector<std::string>& languages() const { return languages_; }
    void setLanguage(const std::string& language);
    const std::vector<ModuleInfo>& modules() const { return modules_; }
    void setModule(int index);         // -1 = all modules
    void setFilter(const std::string& filter);
    size_t rowCount();
    bool setText(const std::string& module, int64_t id, int gender, size_t item, const std::string& utf8);   // gender 0 = plain key
    std::string text(const std::string& module, int64_t id, int gender, size_t item) const;
    void undo();
    void redo();
    // Edits of any game.zip module's `lookup` from other panels (the Missions panel's client mission texts): one undo step (typing
    // in one field coalesces when coalesce is true and the label repeats), saved with the workspace's other changes. value nullptr
    // removes the entry. currentLookup: the module's lookup with pending edits (nullptr when the module is missing).
    struct EntryEdit {
        std::string module;
        tr::py::Ref key, value;
    };
    void editEntries(const std::string& label, const std::vector<EntryEdit>& edits, bool coalesce);
    const tr::py::Object* currentLookup(const std::string& module) const;   // out of line: ui_smoke_test reaches privates by #define
    // Mission texts (missiontextlanguage): the English text (male character's) with pending edits; a new id - one past the largest id
    // of every language's module, pending edits included.
    std::string missionText(int64_t id) const;
    int64_t newMissionTextId() const;
    // Tests: save to this file instead (the patch zip, or with "Save to" game.zip a copy of game.zip; the game data is not reloaded
    // and tabula_rasa.cfg is not changed) - "" for the real save.
    void setSavePathForTests(const std::string& path) { savePathForTests_ = path; }
    bool saveToGameZip() const;          // the "Save to" choice (setting textSave)
    std::string patchZipPath() const;    // data/rasaedit_text.zip beside game.zip
    static constexpr const char* kTextPatchName = "rasaedit_text.zip";

private:
    struct Row {
        int module = 0;                // index into modules_
        int64_t id = 0;
        std::string preview;           // first string (gender 1), one line
        bool gendersDiffer = false;
    };
    struct Change {                    // one entry of one module: its value before and after (nullptr = absent)
        std::string module;
        tr::py::Ref key, before, after;
    };
    struct Step {
        std::vector<Change> changes;
        std::string label;
    };

    void scanModules();
    void rebuildRows();
    const tr::py::Object* lookupOf(const std::string& module) const;
    tr::py::Object& editableLookup(const std::string& module);
    tr::py::Ref keyOf(const ModuleInfo& m, int64_t id, int gender) const;
    const tr::py::Object* valueOf(const ModuleInfo& m, int64_t id, int gender) const;
    void put(const std::string& module, const tr::py::Ref& key, const tr::py::Ref& value);    // nullptr removes
    void record(Step step, bool coalesce);
    void applyStep(const Step& step, bool forward);
    int moduleIndex(const std::string& module) const;
    void buildMissionReferences();

    void drawSaveTarget();
    std::string patchState() const;     // the patch zip's [Data] entry, as a line for the panel
    void drawModules();
    void drawTexts();
    void drawEntry();
    void addEntry();
    void deleteEntry();

    std::vector<std::string> languages_;
    std::string language_;
    std::vector<ModuleInfo> modules_;
    int module_ = -1;                  // -1 = all
    char filter_[128] = "";
    std::vector<Row> rows_;
    bool rowsDirty_ = true;
    int selectedModule_ = -1;
    int64_t selectedId_ = 0;
    bool scrollToSelection_ = false;
    bool sameForBoth_ = true;

    std::map<std::string, tr::py::Ref> edited_;          // module -> its globals with edits (copy of the game's)
    std::vector<Step> undo_, redo_;
    bool coalescing_ = false;                            // the text field being typed in extends the last step
    uint64_t version_ = 0, savedVersion_ = 0;
    std::multimap<int64_t, std::string> missionReferences_;
    std::string savePathForTests_;   // missiontext id -> "mission 428: name text" etc.
};
