#include "AssetPreview.h"

#include "EditorContext.h"
#include "GpuTexture.h"
#include "particles.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

std::string lowerExtension(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : tr::toLowerAscii(name.substr(dot));
}

bool isTextExtension(const std::string& ext) {
    for (const char* e : {".xml", ".sky", ".ltmp", ".txt", ".cfg", ".ini", ".py", ".csv", ".lua", ".htm", ".html"})
        if (ext == e) return true;
    return false;
}

const char* primitiveName(uint8_t p) {
    switch (p) {
        case 0: return "point";
        case 1: return "line";
        case 2: return "sprite";
        case 3: return "aligned sprite";
        case 4: return "oriented sprite";
        default: return "?";
    }
}

}  // namespace

AssetPreview::~AssetPreview() { release(); }

void AssetPreview::release() {
    releaseModel();
    renderer_.reset();
    releaseTexture();
    releaseSound();
    packageSound_.release();
    if (mixer_) MIX_DestroyMixer(mixer_);      // destroys its tracks too
    mixer_ = nullptr;
    track_ = nullptr;
    if (mixerInitialised_) MIX_Quit();
    mixerInitialised_ = false;
}

void AssetPreview::releaseModel() {
    scene_.reset();            // before the map it refers to
    map_.reset();
    modelFitted_ = false;
    modelVisible_ = false;
}

void AssetPreview::releaseTexture() {
    if (texture_ && device_) SDL_ReleaseGPUTexture(device_, texture_);
    texture_ = nullptr;
    pixels_.clear();
    textureWidth_ = textureHeight_ = 0;
}

void AssetPreview::stopSound() {
    if (track_) MIX_StopTrack(track_, 0);
}

void AssetPreview::releaseSound() {
    stopSound();
    if (track_) MIX_SetTrackAudio(track_, nullptr);
    if (sound_) MIX_DestroyAudio(sound_);
    sound_ = nullptr;
    soundFile_.clear();
    soundFrames_ = -1;
}

void AssetPreview::select(EditorContext& ctx, const std::string& name) {
    // Selection happens while ImGui is building the frame; the previous model's renderer target may still be referenced
    // by draw lists of earlier frames only, which have been submitted - SDL defers the release until they finish.
    releaseModel();
    releaseTexture();
    releaseSound();
    links_ = nullptr;
    playing_.clear();
    animationFile_.clear();
    animationModels_.clear();
    animationModelsLooking_ = false;
    animationModel_.clear();
    particlesOnly_ = false;
    package_ = false;
    packageBodies_.clear();
    packageBody_ = 0;
    packageSounds_.clear();
    packageSound_.stop();
    kind_ = Kind::None;
    text_.clear();
    summary_.clear();
    name_ = name;
    device_ = ctx.device;
    if (name.empty() || !ctx.assets) return;

    const auto location = ctx.assets->locate(name);
    if (location && location->inArchive) {
        char line[512];
        std::snprintf(line, sizeof line, "%s\n%u bytes (%u stored%s)\n",
                      std::filesystem::path(ctx.assets->archives()[location->archiveIndex].archive->path()).filename().string().c_str(),
                      location->entry->rawSize, location->entry->storedSize, location->entry->flags ? ", zlib" : "");
        summary_ = line;
    } else if (location) {
        summary_ = location->filePath + "\n";
    }
    tr::Bytes data;
    if (!ctx.assets->tryRead(name, data)) {
        summary_ += "\n(could not be read)\n";
        return;
    }
    const std::string ext = lowerExtension(name);
    try {
        if (ext == ".geo") {
            const tr::GeoModel model = tr::readGeo(data);
            size_t vertices = 0, triangles = 0;
            for (const auto& p : model.pieces) {
                vertices += p.vertices.size();
                triangles += p.indices.size() / 3;
            }
            summary_ += "\n" + std::to_string(model.pieces.size()) + " pieces, " + std::to_string(model.bones.size()) + " bones, " +
                        std::to_string(vertices) + " vertices, " + std::to_string(triangles) + " triangles, " +
                        std::to_string(model.contactPoints.size()) + " contact points\n";
            for (const auto& p : model.pieces)
                summary_ += "  " + std::string(p.skinned ? "skinned " : "") + p.effectName + "  " + p.diffuseTexture + "\n";
            const std::string anm = name.substr(0, name.size() - 4) + ".anm";
            if (ctx.assets->exists(anm)) summary_ += "plays " + anm + "\n";
            openScene(ctx, name);
        } else if (ext == ".dds") {
            tr::DdsHeader header;
            pixels_ = tr::decodeDds(data, header);
            textureWidth_ = header.width;
            textureHeight_ = header.height;
            summary_ += "\n" + tr::ddsFormatName(header) + ", " + std::to_string(header.width) + " x " + std::to_string(header.height) + ", " +
                        std::to_string(header.mipCount) + " mip levels\n";
            buildTextureView();
            kind_ = Kind::Texture;
        } else if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") {
            // Decoded on the fly while playing, from the file kept in memory (the stream reads soundFile_).
            if (!mixerInitialised_) mixerInitialised_ = MIX_Init();
            soundFile_ = std::move(data);
            SDL_IOStream* io = mixerInitialised_ ? SDL_IOFromConstMem(soundFile_.data(), soundFile_.size()) : nullptr;
            sound_ = io ? MIX_LoadAudio_IO(mixer_, io, false, true) : nullptr;
            if (sound_ && MIX_GetAudioFormat(sound_, &soundSpec_)) {
                soundFrames_ = MIX_GetAudioDuration(sound_);
                char line[160];
                if (soundFrames_ >= 0)
                    std::snprintf(line, sizeof line, "\n%d Hz, %d channel%s, %.2f s\n", soundSpec_.freq, soundSpec_.channels,
                                  soundSpec_.channels == 1 ? "" : "s", double(MIX_AudioFramesToMS(sound_, soundFrames_)) / 1000.0);
                else
                    std::snprintf(line, sizeof line, "\n%d Hz, %d channel%s, length unknown\n", soundSpec_.freq, soundSpec_.channels,
                                  soundSpec_.channels == 1 ? "" : "s");
                summary_ += line;
                kind_ = Kind::Sound;
            } else {
                summary_ += std::string("\nnot playable: ") + SDL_GetError() + "\n";
                releaseSound();
            }
        } else if (isTextExtension(ext)) {
            text_.assign(data.begin(), data.end());
            kind_ = Kind::Text;
        } else if (ext == ".anm") {
            const tr::Animation a = tr::readAnimation(data);
            char line[160];
            std::snprintf(line, sizeof line, "\n%.2f s, %zu bone tracks, %zu material tracks\n", a.duration, a.tracks.size(), a.materialTracks.size());
            summary_ += line;
            for (const tr::AnimTrack& t : a.tracks) summary_ += "  " + t.name + " (" + std::to_string(t.keys.size()) + " keys)\n";
            for (const tr::MaterialTrack& t : a.materialTracks) summary_ += "  material: " + t.group + " / " + t.parameter + "\n";
            animationFile_ = name;
            kind_ = Kind::Info;                       // until a model to play it on is chosen (setAnimationModels)
        } else if (ext == ".pfx") {
            const tr::ParticleSystem s = tr::readParticleSystem(data);
            summary_ += "\n" + std::to_string(s.groups.size()) + " particle groups\n";
            for (const tr::ParticleGroup& g : s.groups)
                summary_ += "  " + g.name + ": " + primitiveName(g.primitive) + ", max " + std::to_string(g.maxParticles) + ", " +
                            (g.texture.empty() ? std::string("no texture") : g.texture) + (g.enabled ? "" : " (disabled)") + "\n";
            openScene(ctx, "");
            scene_->setStandaloneParticles(name);
            particlesOnly_ = true;
        } else if (ext == ".pkg") {
            const tr::SfxPackage p = tr::readSfxPackage(data);
            summary_ += "\n" + std::to_string(p.events.size()) + " events: " + std::to_string(p.accessories.size()) + " accessories, " +
                        std::to_string(p.particles.size()) + " particle attaches, " + std::to_string(p.lights.size()) + " lights\n";
            for (const tr::SfxAccessory& a : p.accessories)
                summary_ += "  accessory " + a.mesh + (a.animation.empty() ? "" : " + " + a.animation) + (a.package.empty() ? "" : " -> " + a.package) + "\n";
            for (const tr::SfxParticleAttach& a : p.particles) summary_ += "  particles " + a.file + "\n";
            for (const tr::SfxLight& l : p.lights)
                summary_ += "  light " + std::string(l.spot ? "spot" : "point") + ", " + (l.durationMs ? std::to_string(l.durationMs) + " ms" : std::string("persistent")) + "\n";
            for (const tr::SfxBeam& b : p.beams)
                summary_ += "  beam " + b.texture + ", " + (b.durationMs ? std::to_string(b.durationMs) + " ms" : std::string("persistent")) + "\n";
            for (const tr::SfxParticleTowards& t : p.towards)
                summary_ += "  particles towards " + t.file + (t.sourceTarget == 0 && t.targetTarget == 0 ? "" : " (aimed at another body: not played)") + "\n";
            for (const tr::SfxTracer& t : p.tracers)
                summary_ += "  tracer " + t.asset + " (a trail swept by two moving points: not drawn)\n";
            for (const tr::SfxProjectile& pr : p.projectiles)
                summary_ += "  projectile " + (pr.assets.empty() ? std::string() : pr.assets.front()) + " (not drawn)\n";
            for (const tr::SfxSound& s : p.sounds) packageSounds_.push_back(int32_t(s.audioSet));
            // The bodies it plays on: objects whose usable state (or a transition into it) attaches it, then the effect proxy.
            if (ctx.gameData) {
                for (const tr::GameData::UsablePackageUse& u : ctx.gameData->usablePackageUsers(name)) {
                    const std::string mesh = ctx.gameData->classMeshName(u.classId);
                    if (mesh.empty() || mesh == "default.geo") continue;
                    std::string label = ctx.gameData->className(u.classId) + " (state " + std::to_string(u.stateId) +
                                        (u.previousState == 97 ? "" : ", from " + std::to_string(u.previousState)) + ") - " + mesh;
                    bool seen = false;
                    for (const auto& b : packageBodies_) seen = seen || b.second == mesh;
                    if (!seen) packageBodies_.emplace_back(label, mesh);
                }
            }
            packageBodies_.emplace_back("the effect proxy (particle_proxy.geo: every HPS_ point at its origin)", "particle_proxy.geo");
            package_ = true;
            openPackageBody(ctx);
        }
    } catch (const std::exception& e) {
        summary_ += std::string("\nparse error: ") + e.what() + "\n";
        releaseModel();
        kind_ = Kind::None;
    }
}

// The shown texture for the current mode (ImGui draws textures alpha-blended, so the modes are separate images).
void AssetPreview::buildTextureView() {
    if (texture_ && device_) SDL_ReleaseGPUTexture(device_, texture_);
    texture_ = nullptr;
    if (pixels_.empty() || !device_) return;
    std::vector<tr::Rgba8> view = pixels_;
    for (size_t k = 0; k < view.size(); ++k) {
        tr::Rgba8& p = view[k];
        if (textureMode_ == 1) {
            p.alpha = 255;
        } else if (textureMode_ == 2) {
            p.red = p.green = p.blue = p.alpha;
            p.alpha = 255;
        } else {
            // Over a checkerboard of 8-pixel squares, so transparency shows.
            const uint32_t x = uint32_t(k % textureWidth_), y = uint32_t(k / textureWidth_);
            const uint32_t back = ((x >> 3) ^ (y >> 3)) & 0x1 ? 0x99 : 0x66;
            auto over = [&](uint8_t c) { return uint8_t((c * p.alpha + back * (255 - p.alpha)) / 255); };
            p.red = over(p.red);
            p.green = over(p.green);
            p.blue = over(p.blue);
            p.alpha = 255;
        }
    }
    texture_ = uploadRgbaTexture(device_, view.data(), textureWidth_, textureHeight_);
}

void AssetPreview::openScene(EditorContext& ctx, const std::string& mesh) {
    releaseModel();
    // The scene: one entity of class 1 at the origin (none for a particle system); everything else empty.
    if (!renderer_) renderer_ = std::make_unique<Renderer>(ctx.device);
    map_ = std::make_unique<tr::MapBundle>();
    map_->name = "preview";
    if (!mesh.empty()) {
        tr::MapEntity entity;
        entity.classId = 1;
        map_->map.entities.push_back(entity);
    }
    scene_ = std::make_unique<SceneView>(*renderer_, *ctx.assets, *map_, [mesh](int32_t) { return mesh; }, [](int32_t) { return true; });
    scene_->showPlaceholders = false;
    // The game's distance culling (culling layers: 100 m for most objects, never past 250 m) is about the world; a preview frames the
    // model from as far as its size needs (the 260 m Bane mining drill from ~300 m), so it is off here.
    scene_->gameDistances = false;
    scene_->setRegionEnvironmentMaps({});        // Env1.dds, the client's default cube
    target_ = tr::Vec3{0, 1, 0};
    distance_ = 5.0f;
    yaw_ = 2.6f;
    pitch_ = -0.25f;
    animationTime_ = 0;
    modelFitted_ = false;
    frontEndCamera_ = tr::toLowerAscii(mesh).rfind("ui_fe_", 0) == 0;
    kind_ = Kind::Model;
}

// gfxView's projection (tabula_rasa.exe FUN_005e7360, traced 2026-10-20): s = 1 / tan(fieldOfView / 2) is the scale of the view's
// LONGER side and the other is s x long / short, so a wide view's fieldOfView is horizontal (1.047 = 60 degrees across).
float AssetPreview::frontEndFovY(float aspect) const {
    const float halfAcross = 1.047f * 0.5f;
    return aspect >= 1.0f ? 2.0f * std::atan(std::tan(halfAcross) / aspect) : 2.0f * halfAcross;
}

void AssetPreview::setAnimationModels(EditorContext& ctx, const std::vector<ModelUse>& models, bool stillLooking) {
    if (animationFile_.empty()) return;
    animationModels_ = models;
    animationModelsLooking_ = stillLooking;
    if (animationModel_.empty() && !animationModels_.empty()) {
        // The first model it plays on, playing it from the start.
        animationModel_ = animationModels_.front().model;
        openScene(ctx, animationModel_);
        scene_->setEntityAnimation(0, animationFile_);
    }
}

void AssetPreview::drawAnimationModels(EditorContext& ctx) {
    if (animationFile_.empty()) return;
    if (animationModels_.empty()) {
        ImGui::TextDisabled(animationModelsLooking_ ? "Looking for the models it plays on..." : "No model plays it in the game's files.");
        return;
    }
    if (!ImGui::CollapsingHeader("Plays on", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("%zu model(s)%s; a click shows it on that one", animationModels_.size(), animationModelsLooking_ ? " so far" : "");
    if (ImGui::BeginChild("animationmodels", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>(animationModels_.size(), 8) + 4),
                          ImGuiChildFlags_Borders)) {
        for (size_t k = 0; k < animationModels_.size(); ++k) {
            const ModelUse& u = animationModels_[k];
            ImGui::PushID(int(k));
            if (ImGui::Selectable((u.model + "  (" + u.why + ")").c_str(), tr::toLowerAscii(u.model) == tr::toLowerAscii(animationModel_))) {
                animationModel_ = u.model;
                openScene(ctx, u.model);
                scene_->setEntityAnimation(0, animationFile_);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void AssetPreview::draw(EditorContext& ctx) {
    modelVisible_ = false;
    if (name_.empty()) {
        ImGui::TextDisabled("Select an asset.");
        return;
    }
    ImGui::TextWrapped("%s", name_.c_str());
    drawAnimationModels(ctx);
    switch (kind_) {
        case Kind::Model: drawModel(ctx); break;
        case Kind::Texture: drawTexture(); break;
        case Kind::Sound: drawSound(); break;
        default: break;
    }
    ImGui::TextWrapped("%s", summary_.c_str());
    if (kind_ == Kind::Text) {
        ImGui::InputTextMultiline("##assettext", text_.data(), text_.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
    }
}

void AssetPreview::openPackageBody(EditorContext& ctx) {
    const std::string& mesh = packageBodies_[size_t(std::clamp(packageBody_, 0, int(packageBodies_.size()) - 1))].second;
    openScene(ctx, mesh);
    scene_->previewTimedEffects = true;
    scene_->setEntityPackage(0, name_);
    if (mesh == "particle_proxy.geo") {          // nothing to fit to: frame the origin
        target_ = tr::Vec3{0, 1, 0};
        distance_ = 6.0f;
        modelFitted_ = true;
    }
}

// The package's controls: the body it plays on, Restart (the package clock, timed lights and beams again), its sounds.
void AssetPreview::drawPackage(EditorContext& ctx) {
    if (!package_ || !scene_) return;
    if (packageBodies_.size() > 1) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##pkgbody", ("On " + packageBodies_[size_t(packageBody_)].first).c_str())) {
            for (size_t k = 0; k < packageBodies_.size(); ++k)
                if (ImGui::Selectable(packageBodies_[k].first.c_str(), int(k) == packageBody_) && int(k) != packageBody_) {
                    packageBody_ = int(k);
                    openPackageBody(ctx);
                }
            ImGui::EndCombo();
        }
    }
    if (!scene_) return;
    if (ImGui::SmallButton("Restart package")) scene_->setEntityPackage(0, name_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop")) scene_->stopPackage();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ends what runs and fires marker 24 (Stop), as a STOP event does.");
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f s", scene_->packageTime());
    if (const size_t timers = scene_->packageTimersRunning(); timers > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu timer%s running", timers, timers == 1 ? "" : "s");
    }
    // The markers its events wait for (tr::SfxPackage): starting fires 0 and TIME events fire Timer markers; the rest come from the
    // body's animation or an ability script in the game - fired here by hand.
    const std::vector<uint32_t> markers = scene_->packageMarkers();
    bool any = false;
    for (uint32_t m : markers) {
        if (m == 0) continue;
        if (!any) {
            ImGui::TextUnformatted("Fire marker:");
            any = true;
        }
        ImGui::SameLine();
        ImGui::PushID(int(m));
        if (ImGui::SmallButton(tr::animMarkerTypeName(int(m)))) scene_->firePackageMarker(m);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Marker %u: the events waiting for it trigger.", m);
        ImGui::PopID();
    }
    if (!scene_->packageFired().empty()) {
        std::string fired;
        const auto& log = scene_->packageFired();
        for (size_t k = log.size() > 8 ? log.size() - 8 : 0; k < log.size(); ++k) {
            char item[64];
            std::snprintf(item, sizeof(item), "%s%s %.1f s", fired.empty() ? "" : ", ", tr::animMarkerTypeName(int(log[k].second)), log[k].first);
            fired += item;
        }
        ImGui::TextDisabled("Fired: %s", fired.c_str());
    }
    if (packageSounds_.empty() || !ctx.gameData) return;
    if (ImGui::TreeNode("pkgsounds", "Sounds (%zu)", packageSounds_.size())) {
        for (size_t k = 0; k < packageSounds_.size(); ++k) {
            const int32_t set = packageSounds_[k];
            ImGui::PushID(int(k));
            ImGui::Text("audio set %d %s", set, ctx.gameData->audioSetName(set).c_str());
            const std::vector<tr::GameData::AudioSetSound> members = ctx.gameData->audioSetSounds(set);
            for (size_t m = 0; m < members.size(); ++m) {
                ImGui::PushID(int(m));
                const bool playing = packageSound_.playing(members[m].file);
                if (ImGui::SmallButton(playing ? "Stop" : "Play")) {
                    if (playing) packageSound_.stop();
                    else if (ctx.assets && ctx.assets->exists(members[m].file)) packageSound_.play(members[m].file, ctx.assets->read(members[m].file));
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(members[m].file.c_str());
                ImGui::PopID();
            }
            if (members.empty()) ImGui::TextDisabled("  (no sounds in this set)");
            ImGui::PopID();
        }
        if (!packageSound_.error().empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", packageSound_.error().c_str());
        ImGui::TreePop();
    }
}

void AssetPreview::drawModel(EditorContext& ctx) {
    drawPackage(ctx);
    if (!scene_ || !renderer_) return;
    if (ImGui::Checkbox("Front-end camera", &frontEndCamera_) && !frontEndCamera_) modelFitted_ = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("As the login / character screens show it: camera at (0, 0.9, 0) looking +z, 60 degrees across, black behind\n"
                          "(client/gameui.py SetupFrontEndSceneView, FrontEnd/BkgdView).");
    if (frontEndCamera_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        const char* shapes[] = {"16:9", "4:3"};
        ImGui::Combo("##feaspect", &frontEndAspect_, shapes, 2);
    }
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size = frontEndCamera_ ? ImVec2(width, width / (frontEndAspect_ == 0 ? 16.0f / 9.0f : 4.0f / 3.0f))
                                        : ImVec2(width, std::max(160.0f, std::min(width, ImGui::GetContentRegionAvail().y * 0.6f)));
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    renderer_->resize(uint32_t(size.x * scale), uint32_t(size.y * scale));     // before the image is referenced
    ImGui::InvisibleButton("modelpreview", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef((ImTextureID)(intptr_t)renderer_->colourTarget()), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    if (!frontEndCamera_ && ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        yaw_ += d.x * 0.01f;
        pitch_ = std::clamp(pitch_ - d.y * 0.01f, -1.5f, 1.5f);
    }
    // Middle drag moves the camera (and the point it turns about) in the view plane, the model following the cursor.
    if (!frontEndCamera_ && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        const ViewCamera& camera = scene_->camera();
        const float metresPerPixel = 2.0f * distance_ * std::tan(camera.fovY * 0.5f) / size.y;
        target_ = add(target_, add(scaled(camera.right(), -d.x * metresPerPixel), scaled(camera.up(), d.y * metresPerPixel)));
    }
    if (!frontEndCamera_ && ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) distance_ *= std::pow(0.85f, ImGui::GetIO().MouseWheel);
    modelVisible_ = true;
    const size_t total = scene_->modelsTotal(), loaded = scene_->modelsLoaded(), failed = scene_->modelsFailed();
    if (failed > 0) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The model failed to load.");
    else if (loaded < total) ImGui::TextDisabled("Loading...");
    else if (frontEndCamera_) ImGui::TextDisabled("The game's front-end camera (fixed).");
    else ImGui::TextDisabled("Drag to turn, middle drag to move, wheel to zoom.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view")) modelFitted_ = false;
    if (particlesOnly_ || !animationFile_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart")) {
            if (particlesOnly_) scene_->setStandaloneParticles(name_);
            else scene_->setEntityAnimation(0, animationFile_);
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Textures", &scene_->showTextures);
    ImGui::SameLine();
    ImGui::Checkbox("Effects", &scene_->showEffects);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &scene_->showParticles);
    // The animations the game's files tie to this model (ModelAnimations.h): a click plays one, looping, from its start.
    if (links_ && !links_->groups.empty() && ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Playing: %s", playing_.empty() ? "the model's own animation (if any)" : playing_.c_str());
        if (!playing_.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Back to its own")) {
                playing_.clear();
                scene_->setEntityAnimation(0, "");
            }
        }
        drawAnimationGroups(*links_, [&](const AnimationChoice& c) {
            const std::string label = c.frequency > 0 ? c.name + "  (weight " + std::to_string(c.frequency) + ")" : c.name;
            if (ImGui::Selectable(label.c_str(), tr::toLowerAscii(c.name) == tr::toLowerAscii(playing_))) {
                playing_ = c.name;
                scene_->setEntityAnimation(0, c.name);
            }
        });
        ImGui::Spacing();
    }
}

void AssetPreview::render(EditorContext&, SDL_GPUCommandBuffer* commands) {
    if (kind_ != Kind::Model || !modelVisible_ || !scene_ || !renderer_ || !renderer_->colourTarget()) return;
    scene_->loadPending(0.05);
    if (particlesOnly_) {
        // A particle system: framed on where its particles are once it has run two seconds (most systems are steady by then), then left alone.
        tr::Vec3 centre;
        float radius = 0;
        if (!modelFitted_ && scene_->standaloneParticleTime() >= 2.0f && scene_->standaloneParticleBounds(centre, radius)) {
            target_ = centre;
            distance_ = std::max(1.0f, std::max(radius, 0.5f) / std::sin(scene_->camera().fovY * 0.5f) * 1.1f);
            modelFitted_ = true;
        }
    } else if (!modelFitted_ && scene_->fitEntityToModel(0)) {
        // Frame the model's bounds: centre, and far enough that its bounding sphere fits the view.
        tr::Vec3 centre;
        float radius = 1;
        scene_->entityBounds(0, centre, radius);
        target_ = centre;
        distance_ = std::max(0.5f, radius / std::sin(scene_->camera().fovY * 0.5f) * 1.05f);
        yaw_ = 2.6f;
        pitch_ = -0.25f;
        modelFitted_ = true;
    }
    ViewCamera& camera = scene_->camera();
    scene_->blackBackground = frontEndCamera_;
    if (frontEndCamera_) {
        camera.yaw = 0;
        camera.pitch = 0;
        camera.position = tr::Vec3{0, 0.9f, 0};
        camera.fovY = frontEndFovY(frontEndAspect_ == 0 ? 16.0f / 9.0f : 4.0f / 3.0f);
        camera.nearZ = 0.1f;
        camera.farZ = 10000.0f;
    } else {
        camera.fovY = 1.0f;
        camera.yaw = yaw_;
        camera.pitch = pitch_;
        camera.position = subtract(target_, scaled(camera.forward(), distance_));
        camera.nearZ = std::max(0.01f, distance_ * 0.01f);
        camera.farZ = std::max(100.0f, distance_ * 20.0f);
    }
    scene_->drawDistance = camera.farZ;
    scene_->particleDistance = camera.farZ;
    const float dt = ImGui::GetIO().DeltaTime;
    scene_->advanceParticles(dt);
    scene_->advanceLights(dt);
    scene_->queueDraws(Selection{});
    renderer_->record(commands);
}

void AssetPreview::drawTexture() {
    if (!texture_) return;
    const char* modes[] = {"Colour + alpha", "Colour", "Alpha"};
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##texmode", &textureMode_, modes, 3)) buildTextureView();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##texzoom", &textureZoom_, 0.0f, 4.0f, textureZoom_ <= 0.0f ? "fit" : "%.2fx");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) textureZoom_ = 0;
    const float available = std::max(32.0f, ImGui::GetContentRegionAvail().x);
    const float zoom = textureZoom_ > 0.0f ? textureZoom_ : std::min(1.0f, available / float(textureWidth_));
    const ImVec2 size(float(textureWidth_) * zoom, float(textureHeight_) * zoom);
    if (ImGui::BeginChild("texview", ImVec2(0, std::min(size.y + 16, std::max(160.0f, ImGui::GetContentRegionAvail().y * 0.6f))), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)texture_), size);
    ImGui::EndChild();
}

void AssetPreview::drawSound() {
    if (!sound_) return;
    const bool playing = track_ && MIX_TrackPlaying(track_);
    if (ImGui::Button(playing ? "Restart" : "Play")) {
        // The audio device is opened on the first play only (headless tests and silent sessions never open one).
        if (!mixer_) mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (mixer_ && !track_) track_ = MIX_CreateTrack(mixer_);
        const bool started = track_ && MIX_SetTrackAudio(track_, sound_) && MIX_SetTrackPlaybackPosition(track_, 0) && MIX_PlayTrack(track_, 0);
        if (!started) summary_ += std::string("audio: ") + SDL_GetError() + "\n";
    }
    if (!playing) return;
    ImGui::SameLine();
    if (ImGui::Button("Stop")) stopSound();
    ImGui::SameLine();
    const int64_t position = MIX_GetTrackPlaybackPosition(track_);
    if (soundFrames_ > 0) {
        char label[64];
        std::snprintf(label, sizeof label, "%.1f / %.1f s", double(MIX_TrackFramesToMS(track_, position)) / 1000.0,
                      double(MIX_AudioFramesToMS(sound_, soundFrames_)) / 1000.0);
        ImGui::ProgressBar(float(std::max<int64_t>(position, 0)) / float(soundFrames_), ImVec2(-1, 0), label);
    } else {
        ImGui::Text("%.1f s", double(MIX_TrackFramesToMS(track_, position)) / 1000.0);
    }
}
