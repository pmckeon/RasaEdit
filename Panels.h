#pragma once

// Panels shared by the workspaces. Each workspace owns its own instances (their state, such as
// filters or the overview's zoom, is per workspace) and draws them under workspace-specific window
// IDs, so each workspace keeps its own docking layout.

#include "AssetPreview.h"
#include "EditorContext.h"
#include "WorldDatabase.h"

#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

bool containsIgnoreCase(const std::string& text, const std::string& needleLower);
float yawDegrees(const tr::Quat& q);                 // yaw of an upright orientation (rotation about +y)
ImU32 hueColour(const tr::HueColour& c);

void drawGamePanel(EditorContext& ctx, const char* title);
void drawMapInfoPanel(EditorContext& ctx, const char* title);

class MapsPanel {
public:
    void draw(EditorContext& ctx, const char* title);
private:
    char filter_[128] = "";
};

class AssetsPanel {
public:
    void reset(EditorContext& ctx);                  // after a game folder was opened
    void draw(EditorContext& ctx, const char* title);
    void render(EditorContext& ctx, SDL_GPUCommandBuffer* commands) { preview_.render(ctx, commands); }   // the model preview
    void releaseGpuResources() { preview_.release(); }
private:
    void rebuild(EditorContext& ctx);
    std::vector<std::string> names_;
    std::vector<size_t> visible_;
    char filter_[128] = "";
    int selected_ = -1;
    AssetPreview preview_;
};

// What the overview shows and may select, supplied by the workspace each frame.
struct OverviewLayers {
    Selection* selection = nullptr;
    bool selectEntities = true;                      // map entities can be clicked
    bool dimEntities = false;                        // draw map entities faintly (context only)
    const WorldContent* server = nullptr;            // server objects (nullptr: none)
    const WorldDatabase* database = nullptr;
    bool selectServer = false;
    bool showServer = false;
    bool showMissionAreas = false;
    bool showRegions = false;
    std::function<std::string(const Selection&)> label;            // tooltip text for server objects
    std::function<bool(const Selection&, tr::Vec3&)> position;     // world position of a selection
    std::function<void(const Selection&)> showInViewport;          // double-click on a marker: select it and focus the 3D view
};

class OverviewPanel {
public:
    void onMapLoaded(const EditorContext& ctx);      // fit the view, prefer the radar map
    void requestFocus() { focusPending_ = true; }    // centre on the selection next frame
    void draw(EditorContext& ctx, const char* title, const OverviewLayers& layers);
private:
    int backgroundMode_ = 0;                         // 0 = game radar map, 1 = terrain relief
    int32_t radarChoice_ = -1;                       // the radar region shown: -1 = follow the camera (the client's rule)
    bool fitPending_ = true;
    bool focusPending_ = false;
    float centreX_ = 0, centreZ_ = 0, zoom_ = 0.25f;
};

// Hooks for a workspace whose selection can be moved, rotated and deleted in the viewport.
class ViewportEditHooks {
public:
    virtual ~ViewportEditHooks() = default;
    virtual bool editable() const = 0;
    virtual tr::Vec3* selectedPosition() = 0;        // nullptr: the selection cannot move
    virtual float* selectedRotation() = 0;           // nullptr: the selection cannot rotate
    virtual void beginEdit() = 0;                    // snapshot for undo
    virtual void contentChanged() = 0;
    virtual void deleteSelected() = 0;
    virtual bool& snapToGround() = 0;
};

// Per-workspace viewport layers; everything else about the view is shared (EditorContext::view).
struct ViewportOptions {
    bool showServer = false;
    bool showMissionAreas = false;
    bool showRegions = false;
};

class ViewportPanel {
public:
    // serverControls: show the server layer toggles and the snap option in the toolbar.
    explicit ViewportPanel(bool serverControls) : serverControls_(serverControls) {}
    ViewportOptions options;
    // Draws the panel. pickEntities / pickServer choose what a click selects; hooks may be nullptr.
    void draw(EditorContext& ctx, const char* title, Selection& selection, bool pickEntities, bool pickServer, ViewportEditHooks* hooks);
    // Records the scene into the frame's command buffer if the panel was drawn this frame.
    void render(EditorContext& ctx, SDL_GPUCommandBuffer* commands, const Selection& selection);
    // Flies the camera to the selection and brings the panel to the front.
    void focusOn(EditorContext& ctx, const Selection& selection);
private:
    void applyOptions(const ViewSettings& view, SceneView& scene) const;
    bool serverControls_;
    bool visible_ = false;
    bool raiseWindow_ = false;
    bool dragCandidate_ = false;
    bool dragging_ = false;
    tr::Vec3 dragOffset_;
    float dragY_ = 0;
};
