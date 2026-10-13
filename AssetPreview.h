#pragma once

// Preview of one game asset in the Assets panel (Client workspace):
// - .geo: the model on its own, drawn by a SceneView over a one-entity map with the viewport's renderer code (materials,
//   skinning, X.anm animation, contact-point particles and lights) into a second Renderer; drag to orbit, wheel to zoom.
// - .dds: the top mip level decoded, shown as colour, alpha or colour over a checkerboard.
// - .wav / .ogg / .mp3: played through SDL_mixer (WAV built in, Ogg Vorbis via libvorbis, MP3 via mpg123).
// - text assets (.xml, .sky, .ltmp, ...): the text.
// - .anm / .pfx / .pkg: a summary.

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
    void draw(EditorContext& ctx);                                // inside the details child window
    void render(EditorContext& ctx, SDL_GPUCommandBuffer* commands);   // records the model preview (after draw)
    void release();                                               // GPU and audio resources (game closed / shutdown)
    const std::string& name() const { return name_; }
    const std::string& summary() const { return summary_; }

private:
    enum class Kind { None, Model, Texture, Sound, Text, Info };
    void releaseModel();
    void releaseTexture();
    void stopSound();
    void releaseSound();
    void buildTextureView();
    void drawModel(EditorContext& ctx);
    void drawTexture();
    void drawSound();

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
    float animationTime_ = 0;

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
