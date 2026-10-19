#pragma once

#include "UiCanvas.h"
#include "UiSchema.h"
#include "Workspace.h"
#include "ui.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The UI workspace: the game's UI layout export (data\xml\Tabula_Rasa_UI_EXPORT.xml) - its <Widgets> tree (or the
// Definitions' <TemplateWidgets>, the trees the scripts instantiate), a canvas that draws
// the chosen branch where it sits on the 1280 x 960 logical screen (UiCanvas), and the chosen element's attributes, which can be
// edited (tr::UiDocument: the file's text is changed in place, everything untouched stays byte for byte). Edits are checked against
// what the game's loaders accept (uischema): invalid values are refused, structure edits are offered only where the game takes
// them, and saving is refused while the file has errors it did not have when it was opened (Problems panel). Choosing an element in
// the tree draws its branch; a click on the canvas selects the widget under the cursor (inside the selection: drags it).
class UiWorkspace : public Workspace {
public:
    explicit UiWorkspace(EditorContext& ctx);
    ~UiWorkspace() override;

    const char* name() const override { return "UI"; }
    const char* dockspaceName() const override { return "UIDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void beginFrame() override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer*) override {}
    void onGameOpened() override;
    void releaseGpuResources();
    bool hasUnsavedChanges() const override { return doc_ && doc_->modified(); }
    bool saveChanges() override;
    void handleShortcuts() override;
    void drawEditMenu() override;
    void revertChanges();                   // reload the file, dropping unsaved edits
    void loadExport();
    void syncWithDocument();                // after an edit / undo: canvas, selection, validation
    // Writes an attribute of the selected element after checking it (refused: status line); a default value is left out.
    void commitAttribute(const std::string& cls, const std::string& name, const std::string& value);
    // A textId: the id and its text (generated.client.uielement.bystring[id] -> uielementlanguage, the Text workspace's module), a
    // searchable browser (id or text) to pick another, and "Open in Text". As the mission texts: picked, not typed or edited here.
    // true with `picked` set when one was chosen ("" = none).
    bool textIdField(const std::string& value, std::string& picked);
    void openTextIdInText(const std::string& id);
    void addChild(const std::string& kind);  // a new element of that kind as the selection's last child
    void select(const std::vector<int32_t>& path, bool drawBranch, bool revealInTree);
    void runEdit(const std::function<void()>& edit);   // one document edit; failures go to the status line
    // Form-designer operations (also used by the smoke test).
    bool dropNewElement(const std::string& kind, int32_t x, int32_t y);   // a toolbox drop at a logical point: into the container there
    void copySelection();
    void cutSelection();
    bool pasteClipboard();
    // Into another container at that index; keepScreenPosition: new anchor offsets so it stays where it is drawn (else the old
    // offsets, now relative to the new parent). One undo step.
    bool moveSelectionInto(const std::vector<int32_t>& parent, size_t index, bool keepScreenPosition);
    tr::XmlNode newElement(const std::string& kind) const;   // what the toolbox makes: a basic widget, or a copy of an existing control
    // Several elements selected (Ctrl+click in the canvas or the tree): selected_ is the primary - the first one chosen - which
    // align and same size follow, as in Visual Studio's form designer.
    enum class Arrange { Lefts, Centres, Rights, Tops, Middles, Bottoms, SameWidth, SameHeight, SameSize, SpaceAcross, SpaceDown,
                         CentreAcross, CentreDown };
    void toggleSelect(const std::vector<int32_t>& path);   // add to / remove from the selection
    std::vector<std::vector<int32_t>> selection() const;    // primary first, then the others
    bool canArrange(Arrange op) const;
    bool arrangeSelection(Arrange op);                      // one undo step; false if nothing changed

    // Definitions (UiDefinitions.cpp): Colors, Fonts, Textures, Animations. Every edit is one undo step and is kept only when it adds
    // no error the file did not have (else undone); each returns the refusal ("" when kept), which also goes to the status line.
    std::string editDefinition(const std::function<void()>& edit);
    std::string setDefinitionAttribute(tr::UiSection section, const std::vector<int32_t>& path, const std::string& name, const std::string& value);
    std::string setDefinitionText(tr::UiSection section, const std::vector<int32_t>& path, const std::string& value);  // LoadedFontFile
    std::string setAnimationField(size_t index, const std::string& field, const std::string& value);   // id, playbackType, animType
    // A new id; with updateReferences every widget / template attribute naming the old one follows (same edit).
    std::string renameDefinition(tr::UiSection section, size_t index, const std::string& newId, bool updateReferences);
    int32_t addDefinition(tr::UiSection section, bool copySelected);  // mandatory fields filled; the new index, -1 refused
    std::string removeDefinition(tr::UiSection section, size_t index);        // refused while anything names it
    std::string addDefinitionChild(tr::UiSection section, const std::vector<int32_t>& parent);    // a FontRange, a track's frame
    std::string removeDefinitionChild(tr::UiSection section, const std::vector<int32_t>& path);
    std::string addAnimationTrack(size_t index, const std::string& track);
    std::string definitionId(tr::UiSection section, size_t index) const;
    struct Reference {
        bool templates = false;
        std::vector<int32_t> path;
        std::string attribute;
    };
    std::vector<Reference> findReferences(tr::UiSection section, const std::string& id) const;   // widget / template attributes naming it
    void selectDefinition(tr::UiSection section, int32_t index);

    // For the smoke test: choose a branch by uiids from <Widgets> ("InGame/…"); false if not found.
    bool showBranch(const std::string& uiidPath);
    const std::vector<UiCanvas::DrawnWidget>& drawnWidgets() const { return drawn_; }
    const tr::UiExport* uiExport() const { return doc_ ? &doc_->ui() : nullptr; }
    tr::UiDocument* document() { return doc_.get(); }
    // Tests: read and save this file instead of the game's (then reloads it).
    void setFilePathForTests(const std::string& path) { testFilePath_ = path; loadExport(); }

private:
    void drawTreePanel();
    void drawTreeNode(const tr::XmlNode& node, std::vector<int32_t>& path);
    void drawCanvasPanel();
    void drawElementPanel();
    std::string label(const tr::XmlNode& node) const;
    std::string pathLabel(const std::vector<int32_t>& path) const;
    const tr::UiExport* ui() const { return uiExport(); }

    // Editing.
    void drawProblemsPanel();
    void drawToolboxPanel();
    std::vector<int32_t> containerAt(int32_t x, int32_t y) const;     // the deepest drawn element there that takes free children
    // Snapping a dragged / resized rectangle (logical) to sibling and parent edges and centres, then to the grid; fills guides_.
    UiCanvas::Rect snapRect(const UiCanvas::Rect& r, bool moveX0, bool moveX1, bool moveY0, bool moveY1, float pixelsPerUnit);
    void drawStructureTools(const tr::XmlNode& n, const std::string& cls);
    void drawAttributeEditor(const tr::XmlNode& n, const std::string& cls);
    bool isFreeElement(const std::vector<int32_t>& path) const;      // not an owned part: can be moved, duplicated, deleted
    void deleteSelection();
    void duplicateSelection();
    void moveSelection(int delta);
    void nudgeSelection(int dx, int dy);
    // The selected elements an operation moves: not an element whose ancestor is selected too (it moves with that ancestor).
    std::vector<std::vector<int32_t>> movableSelection() const;
    // The attribute changes that put an element at a logical rectangle (anchor offsets; the size when it changes); false if none.
    bool placementEdit(const std::vector<int32_t>& path, const UiCanvas::Rect& r, std::vector<std::pair<std::string, std::string>>& set,
                       std::vector<std::string>& remove) const;
    void drawArrangeMenu();
    // Definitions panel.
    void drawDefinitionsPanel();
    void drawDefinitionList(tr::UiSection section);
    void drawColourEditor(size_t index);
    void drawTextureEditor(size_t index);
    void drawFontEditor(size_t index);
    void drawAnimationEditor(size_t index);
    void drawReferences(tr::UiSection section, const std::string& id);
    void definitionField(tr::UiSection section, const std::vector<int32_t>& path, const std::string& name);   // one attribute's editor
    std::string referenceLabel(const Reference& r) const;
    void updateDefinitionPreview();         // between frames: the selected textureDef's file as a GPU texture
    void validateNow();
    std::vector<int32_t> branchFor(const std::vector<int32_t>& path) const;

    std::unique_ptr<tr::UiDocument> doc_;
    std::string filePath_, testFilePath_;                  // the loose file the game reads (data\xml\Tabula_Rasa_UI_EXPORT.xml)
    size_t seenRevision_ = 0, validatedRevision_ = size_t(-1);
    double lastEditTime_ = 0;
    std::vector<uischema::Issue> issues_;
    std::multiset<std::string> originalErrors_;     // error messages the file had when it was opened (they do not block saving)
    std::map<std::string, std::string> editBuffers_;   // attribute text fields being edited
    char textBrowseFilter_[128] = "";                   // the textId browser
    struct TextIdChoice {
        std::string id, text, lowerId, lowerText;
    };
    std::vector<TextIdChoice> textBrowseAll_;
    std::vector<size_t> textBrowseShown_;
    std::string textBrowseShownFor_, textBrowsePick_;
    std::vector<int32_t> editBuffersFor_;
    std::map<std::string, std::array<float, 4>> colourBuffers_;     // colour pickers in use (by attribute prefix)
    bool editBuffersTemplates_ = false;
    bool dragging_ = false;
    ImVec2 dragStart_{};
    int dragHandle_ = -1;                   // -1 moving; 0..7 resizing by a handle (TL, T, TR, R, BR, B, BL, L)
    UiCanvas::Rect dragStartRect_;
    bool snap_ = true;
    bool keepPlaceOnMove_ = true;           // tree drags into another container keep the element's place on the screen
    int grid_ = 0;                          // grid step in logical units (0: none)
    std::vector<std::pair<bool, int32_t>> guides_;   // snap guides while dragging: (vertical, logical coordinate)
    std::vector<int32_t> dropTarget_;       // toolbox / outline drag: the container a drop would go into
    std::string saveRefusal_;
    bool problemsErrorsOnly_ = false;
    std::string loadError_;
    UiCanvas canvas_;
    UiCanvas::Options options_;
    std::vector<int32_t> branch_;           // the branch drawn on the canvas
    std::vector<int32_t> selected_;         // the element shown in the inspector and outlined
    std::vector<std::vector<int32_t>> extraSelected_;   // further selected elements (Ctrl+click), in the order chosen
    bool hasBranch_ = false;
    bool hasSelection_ = false;
    bool dirty_ = true;
    bool revealSelection_ = false;          // open the tree down to the selection and scroll to it, once
    std::vector<uint8_t> rgba_;
    std::vector<UiCanvas::DrawnWidget> drawn_;
    SDL_GPUTexture* texture_ = nullptr;
    int zoom_ = 0;                          // 0 fit, 1 50 %, 2 100 %, 3 200 %
    int screen_ = 0;                        // the screen-size choice (Options::screenWidth / screenHeight)
    // Edit mode: every widget in its exported state - no animation, no mouse effects; clicks select. Interactive mode: animations
    // play, the wires glow around the mouse, buttons highlight / push / toggle under it; clicks press buttons.
    bool interactive_ = false;
    std::set<const tr::XmlNode*> toggled_;  // interactive mode: toggle buttons switched on
    const tr::XmlNode* pushed_ = nullptr;   // interactive mode: the button the mouse went down on
    std::map<const tr::XmlNode*, float> scrollValues_;   // interactive mode: each scroll bar's / slider's thumb (0..1), once dragged
    const tr::XmlNode* thumbBar_ = nullptr;              // interactive mode: the bar whose thumb is being dragged
    int32_t grabX_ = 0, grabY_ = 0;                      // the mouse's offset into that thumb when it was grabbed
    char filter_[128] = {};
    // Definitions panel.
    tr::UiSection defSection_ = tr::UiSection::Textures;
    int32_t defIndex_ = -1;                 // the selected entry: a child index of the section's element
    bool focusDefinitions_ = false;         // bring the panel and defSection_'s tab to the front (Problems)
    bool defScroll_ = false;                // scroll the list to the selected entry, once
    bool defRenameReferences_ = true;
    char defFilter_[128] = {};
    std::map<std::string, std::string> defBuffers_;    // text fields being edited
    std::map<std::string, std::array<float, 4>> defColours_;    // colour pickers in use
    std::vector<Reference> references_;     // of the selected definition, for referencesRevision_ / Section_ / Id_
    size_t referencesRevision_ = size_t(-1);
    tr::UiSection referencesSection_ = tr::UiSection::Widgets;
    std::string referencesId_;
    std::string previewWanted_, previewLoaded_;
    SDL_GPUTexture* previewTexture_ = nullptr;
    int32_t previewWidth_ = 0, previewHeight_ = 0;
    bool regionDragging_ = false;
    ImVec2 regionDragStart_{};
    int regionHandle_ = -1;                 // -1 a new region, 0..7 an edge / corner (TL, T, TR, R, BR, B, BL, L), 8 moving it
    long regionStartL_ = 0, regionStartT_ = 0, regionStartR_ = 0, regionStartB_ = 0;
    float previewZoom_ = 0.0f;              // screen pixels per texture pixel (0: fit the view)
    ImVec2 previewScroll_{};
    bool previewScrollPending_ = false;
    bool zoomPreviewToRegion_ = false;      // once the preview is shown (RASAEDIT_UI_DEFINITION=...@zoom)     // previewScroll_ applied with the new content size before the view's next frame
};
