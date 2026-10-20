#pragma once

// Preview of one game asset in the Assets panel (Client workspace):
// - .geo: the model on its own, drawn by a SceneView over a one-entity map with the viewport's renderer code (materials,
//   skinning, X.anm animation, contact-point particles and lights) into a second Renderer; drag to orbit, wheel to zoom.
// - .dds: the top mip level decoded, shown as colour, alpha or colour over a checkerboard.
// - .wav / .ogg / .mp3: played through SDL_mixer (WAV built in, Ogg Vorbis via libvorbis, MP3 via mpg123).
// - text assets (.xml, .sky, .ltmp, ...): the text.
// - .anm: a summary, and played on a model that plays it in the game (setAnimationModels: the links the other way round).
// - .pfx: the particle system on its own at the origin, and its summary.
// - .pkg: the package played on a body (an object whose usable state attaches it, else the effect proxy particle_proxy.geo):
//   accessories, particle attaches, lights and beams - timed ones for their duration - with Restart; its sound events' audio sets
//   listed with Play buttons.

#include "ModelAnimations.h"
#include "SoundPlayer.h"
#include "Renderer.h"
#include "SceneView.h"
#include "assetlibrary.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <memory>
#include <string>

class EditorContext;

class AssetPreview {
public:
    AssetPreview() = default;
    ~AssetPreview();
    AssetPreview(const AssetPreview&) = delete;
    AssetPreview& operator=(const AssetPreview&) = delete;

    void select(EditorContext& ctx, const std::string& name);   // "" clears
    // The animations tied to the selected model (listed under the model view: a click plays one); nullptr = none. Call after select.
    void setAnimationLinks(const ModelAnimationLinks* links) { links_ = links; }
    // For a selected .anm: the models it plays on (the list to pick from; the first is shown); "still looking" while the models'
    // own-animation index is being built. Call after select, again when the list grows.
    void setAnimationModels(EditorContext& ctx, const std::vector<ModelUse>& models, bool stillLooking);
    void draw(EditorContext& ctx);                                // inside the details child window
    void render(EditorContext& ctx, SDL_GPUCommandBuffer* commands);   // records the model preview (after draw)
    void release();                                               // GPU and audio resources (game closed / shutdown)
    const std::string& name() const { return name_; }
    const std::string& summary() const { return summary_; }

    // Opens the chosen package body (packageBody_) and plays the package on it.
    void openPackageBody(EditorContext& ctx);

private:
    enum class Kind { None, Model, Texture, Sound, Text, Info };
    void releaseModel();
    void openScene(EditorContext& ctx, const std::string& mesh);   // the preview scene: one entity of this model, or none ("")
    void drawAnimationModels(EditorContext& ctx);
    void releaseTexture();
    void stopSound();
    void releaseSound();
    void buildTextureView();
    void drawModel(EditorContext& ctx);
    void drawPackage(EditorContext& ctx);
    void drawTexture();
    void drawSound();

    // Decoded contents of a structured asset (.cat, .map, .vis, .mkr): titled tables, filtered together; a cell that names an asset
    // the archives hold is a link that opens it (EditorContext::showAssetRequest).
    struct ContentRow {
        std::vector<std::string> cells;
        uint32_t links = 0;                   // bit n: cell n opens an asset
    };
    struct ContentSection {
        std::string title;
        std::vector<std::string> columns;
        std::vector<ContentRow> rows;
        std::vector<int> shown;               // rows passing the filter
    };
    void addContentSection(EditorContext& ctx, std::string title, std::vector<std::string> columns,
                           std::vector<std::vector<std::string>> rows, std::vector<int> assetColumns = {});
    void filterContents();
    void drawContents(EditorContext& ctx);
    std::vector<ContentSection> contents_;
    char contentsFilter_[128] = {};

    Kind kind_ = Kind::None;
    std::string name_;
    std::string summary_;                     // text above the preview (size, archive, parsed details)
    std::string text_;                        // Kind::Text

    // Model: its own renderer and scene; the scene's map is a single entity of class 1 = the model.
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<tr::MapBundle> map_;
    std::unique_ptr<SceneView> scene_;
    bool modelFitted_ = false;
    tr::Vec3 target_{0, 1, 0};
    float distance_ = 5.0f;
    float yaw_ = 2.6f;                        // radians; models are authored facing -z, so look at the front first
    float pitch_ = -0.25f;
    bool modelVisible_ = false;               // drawn this frame (render() records only then)
    // The front-end camera (UI_FE_* models, the login / character screens' 3D backgrounds): client/gameui.py SetupFrontEndSceneView
    // puts the camera at (0, 0.9, 0) in FrontEnd/BkgdView (fieldOfView 1.047, near 0.1, far 10000, no orientation = looking +z) and
    // clears to black; the view fills the screen, so the image takes the screen's shape (frontEndAspect_: 0 = 16:9, 1 = 4:3).
    bool frontEndCamera_ = false;
    int frontEndAspect_ = 0;
    float frontEndFovY(float aspect) const;
    float animationTime_ = 0;
    const ModelAnimationLinks* links_ = nullptr;
    std::string playing_;                     // the animation chosen in the list ("" = the model's own)
    std::string animationFile_;               // a selected .anm
    std::vector<ModelUse> animationModels_;   // the models it plays on
    bool animationModelsLooking_ = false;
    std::string animationModel_;              // the one it plays on now
    bool particlesOnly_ = false;              // a selected .pfx (the scene holds no entity)
    // A selected .pkg: the bodies it can play on (label, mesh) - objects whose usable state attaches it, then the effect proxy - the
    // one in use, and its sound events (audio set ids).
    bool package_ = false;
    std::vector<std::pair<std::string, std::string>> packageBodies_;
    int packageBody_ = 0;
    std::vector<int32_t> packageSounds_;
    SoundPlayer packageSound_;

    // Texture: decoded top level and the texture shown.
    SDL_GPUDevice* device_ = nullptr;
    std::vector<tr::Rgba8> pixels_;
    uint32_t textureWidth_ = 0, textureHeight_ = 0;
    SDL_GPUTexture* texture_ = nullptr;
    int textureMode_ = 0;                     // 0 colour over checkerboard, 1 colour opaque, 2 alpha
    float textureZoom_ = 0;                   // 0 = fit

    // Sound (SDL_mixer): the file stays in memory, decoded while playing; the mixer opens the audio device on first play.
    tr::Bytes soundFile_;
    MIX_Audio* sound_ = nullptr;
    SDL_AudioSpec soundSpec_{};
    int64_t soundFrames_ = -1;                // MIX_DURATION_UNKNOWN when the decoder cannot tell
    bool mixerInitialised_ = false;
    MIX_Mixer* mixer_ = nullptr;
    MIX_Track* track_ = nullptr;
};
