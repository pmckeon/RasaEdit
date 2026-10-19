// Headless viewport test: renders a map with the editor's Renderer/SceneView into an offscreen
// target and writes the frame as raw RGBA. Runs on any Vulkan driver, including Mesa's lavapipe.
//   viewport_test <game root> <game.zip> <map> <out.rgba> <width> <height> <eye x y z> <target x y z>
#include "Avatar.h"
#include "SceneView.h"
#include "PvsBuilder.h"
#include "UiCanvas.h"
#include "UiSchema.h"
#include "gamedata.h"
#include "geofile.h"
#include "image.h"
#include "ModelExchange.h"
#include "MissionValidation.h"
#include "WorldDatabase.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>

// An environment variable's value, or nullptr when it is unset or empty (PowerShell leaves cleared variables empty).
static const char* envValue(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

int main(int argc, char** argv) {
    if (argc < 13) { std::puts("usage: viewport_test root game.zip map out.rgba w h ex ey ez tx ty tz"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice* device = createGpuDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, true);
    if (!device) { std::printf("SDL_CreateGPUDevice: %s\n", SDL_GetError()); return 1; }
    std::printf("GPU driver: %s, BC1 textures %s\n", SDL_GetGPUDeviceDriver(device),
                SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "supported" : "not supported");
    const uint32_t width = uint32_t(std::atoi(argv[5])), height = uint32_t(std::atoi(argv[6]));
    std::setvbuf(stdout, nullptr, _IONBF, 0);          // keep the output if something goes wrong
    try {
        Renderer renderer(device);
        renderer.resize(width, height);
        tr::AssetLibrary assets(argv[1]);
        tr::GameData data(argv[2]);
        // RASA_UIRENDER=<uiid>/<uiid>/...[;<another branch>...] (or "*" for every root widget): draws those branches of
        // data\xml\Tabula_Rasa_UI_EXPORT.xml in turn with UiCanvas into <out.rgba> and exits; prints what could not be resolved.
        // RASA_UIHIDDEN=1 draws hidden widgets too, RASA_UIMOUSE=<x>,<y> puts the mouse there (logical), RASA_UISCREEN=<w>x<h> draws for that screen
        // (the output is then w x h, else the export's 1280 x 960), RASA_UIBACKGROUND=<raw rgba file of that size> draws over it.
        // RASA_UIVALIDATE=<xml file>: lists every validation issue of that UI export (errors, then warnings unless RASA_UIWARNINGS is
        // unset) with the element's uiid path, its root widget / template and its tag.
        if (const char* file = envValue("RASA_UIVALIDATE")) {
            const tr::UiDocument doc(tr::readFile(file));
            const auto issues = uischema::validate(doc.ui(), &assets, &data);
            const bool warnings = envValue("RASA_UIWARNINGS") != nullptr;
            size_t errors = 0;
            for (const auto& i : issues) errors += i.error ? 1 : 0;
            std::printf("%s: %zu errors, %zu warnings\n", file, errors, issues.size() - errors);
            for (int pass = 0; pass < 2; ++pass)
                for (const auto& i : issues) {
                    if (i.error != (pass == 0) || (!i.error && !warnings)) continue;
                    std::string where, root, tag;
                    if (!i.section.empty()) where = "Definitions/" + i.section;
                    else {
                        const tr::XmlNode* n = i.templates ? &doc.ui().templates : &doc.ui().widgets;
                        where = i.templates ? "TemplateWidgets" : "Widgets";
                        for (size_t k = 0; k < i.path.size() && n; ++k) {
                            const int32_t idx = i.path[k];
                            n = idx >= 0 && size_t(idx) < n->children.size() ? &n->children[size_t(idx)] : nullptr;
                            if (!n) break;
                            const std::string id = n->attribute("uiid");
                            where += "/" + (id.empty() ? n->tag + "[" + std::to_string(idx) + "]" : id);
                            if (k == 0) root = (id.empty() ? n->tag : id);
                            tag = n->tag;
                        }
                    }
                    std::printf("%s | %s | %s | <%s> | %s\n", i.error ? "ERROR" : "warning", root.c_str(), where.c_str(), tag.c_str(), i.message.c_str());
                }
            return 0;
        }
        // RASA_UIDOCTEST=1: validates the UI export, then edits it with tr::UiDocument (attribute set / add / remove, element insert /
        // duplicate / move / remove), checks each result parses and reads back, undoes everything and checks the text is the original.
        if (envValue("RASA_UIDOCTEST")) {
            const tr::Bytes xml = tr::readFile((std::filesystem::path(argv[1]) / "data" / "xml" / "Tabula_Rasa_UI_EXPORT.xml").string());
            auto t0 = std::chrono::steady_clock::now();
            tr::UiDocument doc(xml);
            std::printf("UI document: %zu bytes, parsed in %.0f ms\n", doc.text().size(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            t0 = std::chrono::steady_clock::now();
            const auto issues = uischema::validate(doc.ui(), &assets, &data);
            size_t errors = 0;
            for (const auto& i : issues) errors += i.error ? 1 : 0;
            std::printf("validation: %zu errors, %zu warnings in %.0f ms\n", errors, issues.size() - errors,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            size_t shown = 0;
            for (int pass = 0; pass < 2; ++pass)
                for (const auto& i : issues)
                    if (i.error == (pass == 0) && shown++ < 25)
                    std::printf("  %s %s%s %s: %s\n", i.error ? "ERROR" : "warn ", i.section.c_str(), i.templates ? "T" : "",
                                [&] { std::string s; for (int32_t k : i.path) s += "/" + std::to_string(k); return s; }().c_str(), i.message.c_str());
            int failures = 0;
            auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += ok ? 0 : 1; };
            const std::string original = doc.text();
            // FrontEnd/LogOutWindow (root 1, its LogOutWindow child).
            std::vector<int32_t> window;
            for (size_t k = 0; k < doc.ui().widgets.children.size(); ++k)
                if (doc.ui().widgets.children[k].attribute("uiid") == "FrontEnd")
                    for (size_t j = 0; j < doc.ui().widgets.children[k].children.size(); ++j)
                        if (doc.ui().widgets.children[k].children[j].attribute("uiid") == "LogOutWindow") window = {int32_t(k), int32_t(j)};
            check(!window.empty(), "LogOutWindow found");
            doc.setAttribute(false, window, "anchorOffsetX", "17");
            check(doc.node(false, window)->attribute("anchorOffsetX") == "17", "attribute changed");
            doc.setAttribute(false, window, "colorG", "128");
            const auto& attrs = doc.node(false, window)->attributes;
            size_t g = 0, s0 = 0;
            for (size_t k = 0; k < attrs.size(); ++k) { if (attrs[k].first == "colorG") g = k; if (attrs[k].first == "sizeW") s0 = k; }
            check(doc.node(false, window)->attribute("colorG") == "128" && g > s0, "new attribute placed in the export's order");
            doc.setAttribute(false, window, "uiid", "Logout\"<&>");
            check(doc.node(false, window)->attribute("uiid") == "Logout\"<&>", "special characters escaped and read back");
            doc.removeAttribute(false, window, "colorG");
            check(!doc.node(false, window)->hasAttribute("colorG"), "attribute removed");
            tr::XmlNode bitmap;
            bitmap.tag = "Bitmap";
            bitmap.attributes = {{"uiid", "NewBitmap"}, {"sizeW", "32"}, {"sizeH", "32"}, {"textureDef", "DEFAULT"}};
            const size_t before = doc.node(false, window)->children.size();
            const auto added = doc.insertElement(false, window, 1, bitmap);
            check(doc.node(false, window)->children.size() == before + 1 && doc.node(false, added)->attribute("uiid") == "NewBitmap", "element inserted");
            const auto copy = doc.duplicateElement(false, added);
            check(doc.node(false, copy)->attribute("uiid") == "NewBitmap" && doc.node(false, window)->children.size() == before + 2, "element duplicated");
            const auto moved = doc.moveElement(false, copy, 1);
            check(moved.back() == copy.back() + 1 && doc.node(false, moved)->attribute("uiid") == "NewBitmap", "element moved down");
            doc.removeElement(false, moved);
            check(doc.node(false, window)->children.size() == before + 1, "element removed");
            // Into a self-closing element: the first Bitmap without children.
            std::vector<int32_t> leaf = window;
            leaf.push_back(int32_t(before));
            for (size_t k = 0; k < doc.node(false, window)->children.size(); ++k)
                if (doc.node(false, window)->children[k].selfClosing) { leaf.back() = int32_t(k); break; }
            const auto nested = doc.insertElement(false, leaf, 0, bitmap);
            check(!doc.node(false, leaf)->selfClosing && doc.node(false, nested), "child inserted into a self-closing element");
            const auto after = uischema::validate(doc.ui(), &assets, &data);
            size_t afterErrors = 0;
            for (const auto& i : after) afterErrors += i.error ? 1 : 0;
            check(afterErrors > errors, "validation flags a Bitmap inside a Bitmap");
            while (doc.canUndo()) doc.undo();
            check(doc.text() == original, "undo restores the original text byte for byte");
            while (doc.canRedo()) doc.redo();
            check(doc.node(false, nested) != nullptr, "redo replays the edits");
            std::printf("%d failure(s)\n", failures);
            return failures == 0 ? 0 : 1;
        }
        if (const char* uiPaths = envValue("RASA_UIRENDER")) {
            const tr::Bytes xml = tr::readFile((std::filesystem::path(argv[1]) / "data" / "xml" / "Tabula_Rasa_UI_EXPORT.xml").string());
            const tr::UiExport ui = tr::readUiExport(xml);
            std::printf("UI export: %d x %d, %zu colours, %zu fonts, %zu texture defs, %zu root widgets\n", ui.logicalWidth, ui.logicalHeight, ui.colours.size(),
                        ui.fonts.size(), ui.textures.size(), ui.widgets.children.size());
            UiCanvas canvas;
            canvas.setSources(&ui, &assets, &data, argv[1]);
            UiCanvas::Options options;
            options.showHidden = envValue("RASA_UIHIDDEN") != nullptr;
            canvas.setTemplates(envValue("RASA_UITEMPLATES") != nullptr);    // paths from Definitions/TemplateWidgets
            if (const char* state = envValue("RASA_UIBUTTONS")) {        // up / highlighted / down / disabled / pressed
                const std::string s = state;
                options.buttonState = s == "up" ? 1 : s == "highlighted" ? 2 : s == "down" ? 3 : s == "disabled" ? 4 : s == "pressed" ? 5 : 0;
            }
            options.toggledOn = envValue("RASA_UITOGGLED") != nullptr;
            if (const char* progress = envValue("RASA_UIPROGRESS")) options.progress = std::strtof(progress, nullptr);   // 0..1
            if (const char* scroll = envValue("RASA_UISCROLL")) options.scroll = std::strtof(scroll, nullptr);           // 0..1
            if (const char* animTime = envValue("RASA_UIANIM")) options.animTime = std::strtof(animTime, nullptr);       // ms
            if (const char* mouse = envValue("RASA_UIMOUSE")) {           // logical x,y: the window wires glow around it
                options.mouseHighlight = std::sscanf(mouse, "%f,%f", &options.mouseX, &options.mouseY) == 2;
            }
            if (const char* uiScreen = envValue("RASA_UISCREEN")) std::sscanf(uiScreen, "%dx%d", &options.screenWidth, &options.screenHeight);
            std::vector<uint8_t> rgba, background;
            if (const char* bg = envValue("RASA_UIBACKGROUND")) {
                const tr::Bytes b = tr::readFile(bg);
                background.assign(b.begin(), b.end());
                options.backgroundRgba = &background;
            }
            std::stringstream branches(uiPaths);
            std::string uiPath;
            while (std::getline(branches, uiPath, ';')) {
                std::vector<int32_t> path;
                if (uiPath != "*") {
                    std::stringstream parts(uiPath);
                    std::string part;
                    const tr::XmlNode* n = canvas.root();
                    while (std::getline(parts, part, '/')) {
                        int32_t found = -1;
                        for (size_t k = 0; k < n->children.size() && found < 0; ++k)
                            if (n->children[k].attribute("uiid") == part) found = int32_t(k);
                        if (found < 0) { std::printf("UI widget %s not found\n", part.c_str()); return 1; }
                        path.push_back(found);
                        n = &n->children[size_t(found)];
                    }
                }
                const auto t0 = std::chrono::steady_clock::now();
                const auto drawn = canvas.render(path, options, rgba);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                size_t visual = 0;
                for (const auto& d : drawn) visual += d.visual ? 1 : 0;
                const UiCanvas::Rect r = canvas.layout(path);
                std::printf("UI branch %s at (%d %d)-(%d %d): %zu widgets laid out, %zu drawn something, %.1f ms\n", uiPath.c_str(), r.x0, r.y0, r.x1, r.y1,
                            drawn.size(), visual, ms);
                background = rgba;                    // the next branch draws over this one
                options.backgroundRgba = &background;
            }
            for (const auto& t : canvas.missingTextures()) std::printf("UI missing texture %s\n", t.c_str());
            for (const auto& f : canvas.missingFonts()) std::printf("UI missing font face %s\n", f.c_str());
            for (const auto& t : canvas.missingTextIds()) std::printf("UI unknown text id %s\n", t.c_str());
            std::FILE* out = std::fopen(argv[4], "wb");
            if (out) {
                std::fwrite(rgba.data(), 1, rgba.size(), out);
                std::fclose(out);
            }
            return 0;
        }
        const tr::MapBundle map = tr::loadMap(assets, argv[3]);
        // RASA_GLTFIMPORT with RASA_GLTFPREVIEW=1: the import's files override the archives' before the scene is built (the scene
        // reads each entity's model properties - its animation - when it is created); the comparison below is then skipped.
        if (envValue("RASA_GLTFIMPORT") && envValue("RASA_GLTFPREVIEW")) {
            try {
                ModelImportOptions options;
                if (const char* src = envValue("RASA_GLTFSOURCE")) options.source = src;
                if (const char* name = envValue("RASA_GLTFMODELNAME")) options.modelName = name;
                options.renameChangedTextures = envValue("RASA_GLTFRENAME") != nullptr;   // changed textures as <model>_<texture>
                const ModelImportResult result = importModelGltf(assets, envValue("RASA_GLTFIMPORT"), options);
                for (const ImportedFile& f : result.files) assets.setOverride(f.name, f.data);
                for (const std::string& w : result.warnings) std::printf("gltf import warning: %s\n", w.c_str());
                std::printf("gltf preview: %zu file(s) override the archives (%s)\n", result.files.size(),
                            result.files.empty() ? "" : result.files[0].name.c_str());
            } catch (const std::exception& e) {
                std::printf("gltf import FAILED: %s\n", e.what());
            }
        }
        SceneView scene(renderer, assets, map, [&](int32_t id) { return data.classMeshName(id); },
                        [&](int32_t id) { return !data.className(id).empty(); }, [&](int32_t id) { return data.classCullingLayer(id); },
                        envValue("RASA_TERRAINSTEP") ? uint32_t(std::atoi(envValue("RASA_TERRAINSTEP"))) : 0u);   // 0 = automatic
        {
            std::vector<std::pair<int32_t, std::string>> regionSkies;
            for (const tr::MapRegion& r : map.map.regions)
                if (r.sky > 0) regionSkies.emplace_back(r.regionId, data.stringById(r.sky));
            if (envValue("RASA_LISTHUES")) {             // distinct per-entity map hues (hue1 / hue2 RGBA bytes)
                std::map<std::string, int> counts;
                for (const tr::MapEntity& e : map.map.entities) {
                    char key[64];
                    std::snprintf(key, sizeof key, "%3u %3u %3u %3u / %3u %3u %3u %3u", e.hue1.red, e.hue1.green, e.hue1.blue, e.hue1.alpha,
                                  e.hue2.red, e.hue2.green, e.hue2.blue, e.hue2.alpha);
                    ++counts[key];
                }
                for (const auto& c : counts) std::printf("map hue %s: %d entities\n", c.first.c_str(), c.second);
            }
            if (envValue("RASA_LISTREGIONS"))
                for (const tr::MapRegion& r : map.map.regions) {
                    const auto name = [&](int32_t id) -> std::string {
                        if (id <= 0) return "";
                        try { return data.stringById(id); } catch (const std::exception&) { return "?"; }
                    };
                    std::string display;
                    if (r.displayName > 0) {
                        try { display = data.regionDisplayName(r.displayName); } catch (const std::exception& e) { display = std::string("? ") + e.what(); }
                    }
                    std::printf("region %d priority %d flags 0x%x env '%s' sky '%s' name %d '%s'\n", r.regionId, r.priority, unsigned(r.enabledFlags),
                                name(r.environmentMap).c_str(), name(r.sky).c_str(), r.displayName, display.c_str());
                }
            scene.setRegionSkies(regionSkies);
            std::vector<std::pair<int32_t, std::string>> regionEnvironmentMaps;
            for (const tr::MapRegion& r : map.map.regions)
                regionEnvironmentMaps.emplace_back(r.regionId, r.environmentMap > 0 ? data.stringById(r.environmentMap) : std::string());
            scene.setRegionEnvironmentMaps(regionEnvironmentMaps);
            scene.setClassHues([&](int32_t classId, tr::HueColour& hue1, tr::HueColour& hue2) { return data.classHue(classId, hue1, hue2); });
            scene.setUsableStatePackages([&](int32_t classId, int32_t stateId) { return data.usableStatePackage(classId, stateId); });
            scene.setStringLookup([&](int32_t stringId) { return data.stringById(stringId); });
            scene.setUsableStateAnimations([&](int32_t classId, int32_t stateId) { return data.usableStateAnimation(classId, stateId); });
            // RASA_TIME=<fraction of the day>, RASA_SKY=<index>, RASA_NOSKY=1 / RASA_NOFOG=1 for lighting checks.
            if (const char* t = envValue("RASA_TIME")) scene.timeOfDay = float(std::atof(t));
            if (const char* k = envValue("RASA_SKY")) { scene.skyIndex = size_t(std::atoi(k)); scene.skyByRegion = false; }
            if (envValue("RASA_NOSKY")) scene.skyLighting = false;
            if (envValue("RASA_NOSKYDOME")) scene.showSkyDome = false;
            if (envValue("RASA_NOFOG")) scene.showFog = false;
            if (envValue("RASA_NOSUN")) scene.showSun = false;
            if (envValue("RASA_NOSHADOWS")) scene.showShadows = false;
            if (const char* g = envValue("RASA_LIGHTGAMMA")) scene.lightColourGamma = float(std::atof(g));
            if (envValue("RASA_NOLIGHTS")) scene.showMapLights = false;
            if (envValue("RASA_NOLOCALSHADOWS")) scene.localShadows = false;
            if (envValue("RASA_NOTERRAINCASTERS")) scene.localShadowTerrain = false;
            if (envValue("RASA_NOCOOKIES")) scene.spotCookies = false;
            if (const char* only = envValue("RASA_ONLYLIGHT")) scene.onlyLight = std::atoi(only);
            // The game's placed-light shadow limits: client.cfg, or RASA_SHADOWMAX / RASA_SHADOWDISTANCE (the settings' 0..100 values).
            scene.shadowMaxSimultaneous = clientConfigValue(argv[1], "Client.Graphics.ShadowMaxSimultaneous", 100.0f) / 100.0f;
            scene.shadowDistance = clientConfigValue(argv[1], "Client.Graphics.ShadowDistance", 100.0f) / 100.0f;
            if (const char* v = envValue("RASA_SHADOWMAX")) scene.shadowMaxSimultaneous = float(std::atof(v)) / 100.0f;
            if (const char* v = envValue("RASA_SHADOWDISTANCE")) scene.shadowDistance = float(std::atof(v)) / 100.0f;
            if (const char* v = envValue("RASA_RQSSHADOWDISTANCE")) scene.rqsShadowDistance = std::atoi(v) != 0;
            if (envValue("RASA_NOSHADOWCACHE")) scene.cacheShadowMaps = false;
            if (envValue("RASA_SHOWLIGHT")) scene.showLightBuffer = true;
            if (envValue("RASA_NOENV")) scene.environmentMaps = false;
            if (const char* lt = envValue("RASA_LIGHTTIME")) scene.lightTime = float(std::atof(lt));   // light modifier time, s
            // RASA_LIGHTSTEPS=<seconds>: advance the clock frame by frame as the app does (advanceLights at 30 Hz) instead of setting it.
            if (const char* ls = envValue("RASA_LIGHTSTEPS")) for (int k = 0; k < int(std::atof(ls) * 30.0f); ++k) scene.advanceLights(1.0f / 30.0f);
            if (envValue("RASA_NOLIGHTMODS")) scene.animateLights = false;
            if (envValue("RASA_SHOWNORMALS")) scene.debugView = 2;                  // like the game's /dl_shownormals
            if (envValue("RASA_SHOWSPEC")) scene.debugView = 3;
            if (envValue("RASA_SHOWSPECPOWER")) scene.debugView = 5;                // like the game's /dl_showspecpower
            if (envValue("RASA_SHOWDIFFUSE")) scene.debugView = 4;                  // like the game's /dl_showdiffuse                     // like the game's /dl_showspecintensity     // like the game's /dl_showlight
            if (const char* a = envValue("RASA_AMBIENT")) scene.ambientScale = float(std::atof(a));
            if (envValue("RASA_NOMAPS")) scene.materialMaps = false;                // normal / gloss maps off   // scales the sky hemi light
            std::printf("map lights: %zu%s%s\n", scene.mapLights().size(), scene.mapLightsError().empty() ? "" : " - ", scene.mapLightsError().c_str());
            for (size_t k = 0; k < scene.skies().size(); ++k) {
                const SceneView::SkyChoice& c = scene.skies()[k];
                std::string regions;
                for (int32_t r : c.regions) regions += (regions.empty() ? "" : " ") + std::to_string(r);
                std::printf("sky %zu%s: %s (regions: %s)%s%s\n", k, k == scene.skyIndex ? " *" : "", c.name.c_str(), regions.c_str(),
                            c.loaded ? "" : " - not usable: ", c.error.c_str());
            }
            const SceneLighting l = scene.lighting();
            std::printf("time %.3f: hemi top %.2f %.2f %.2f bottom %.2f %.2f %.2f, sun %.2f %.2f %.2f towards %.2f %.2f %.2f, fog %s from %.0f m density %.4f, %d placed lights in use\n",
                        scene.timeOfDay, l.hemiTop.x, l.hemiTop.y, l.hemiTop.z, l.hemiBottom.x, l.hemiBottom.y, l.hemiBottom.z, l.sunFront.x,
                        l.sunFront.y, l.sunFront.z, l.towardsLight.x, l.towardsLight.y, l.towardsLight.z, l.fog ? "on" : "off", l.fogStart, l.fogDensity, l.localCount);
        }
        // RASA_GEOINFO=<model.geo>: per piece its vertex elements, vertex colour range (ARGB bytes) and material parameters.
        if (const char* geoName = envValue("RASA_GEOINFO")) {
            const tr::GeoModel geo = tr::readGeo(assets.read(geoName));
            for (const tr::GeoBone& b : geo.bones) {
                // det of the inverse bind's 3 x 3 (rows X, Y, Z): < 0 = the stored matrix mirrors
                const tr::Vec3 &x = b.inverseBindX, &y = b.inverseBindY, &z = b.inverseBindZ;
                const float det = x.x * (y.y * z.z - y.z * z.y) - x.y * (y.x * z.z - y.z * z.x) + x.z * (y.x * z.y - y.y * z.x);
                std::printf("bone %s parent %d: t (%.3f %.3f %.3f) r (%.3f %.3f %.3f %.3f) s (%.4f %.4f %.4f)%s ibT (%.3f %.3f %.3f) ibX (%.4f %.4f %.4f) "
                            "ibY (%.4f %.4f %.4f) ibZ (%.4f %.4f %.4f) det %.4f\n",
                            b.name.c_str(), b.parent, b.translation.x, b.translation.y, b.translation.z, b.rotation.x, b.rotation.y,
                            b.rotation.z, b.rotation.w, b.scale.x, b.scale.y, b.scale.z, b.hasInverseBind ? " inverse bind" : "",
                            b.inverseBindT.x, b.inverseBindT.y, b.inverseBindT.z, x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z, det);
            }
            for (const tr::GeoContactPoint& c : geo.contactPoints) {
                std::printf("contact point %u bone %d at (%.3f %.3f %.3f) rotation (%.3f %.3f %.3f %.3f):", c.id, c.bone, c.position.x, c.position.y, c.position.z,
                            c.rotation.x, c.rotation.y, c.rotation.z, c.rotation.w);
                for (const tr::GeoParameter& kv : c.values) std::printf(" %s=%s", kv.key.c_str(), kv.value.c_str());
                std::printf("\n");
            }
            for (size_t k = 0; k < geo.pieces.size(); ++k) {
                const tr::GeoPiece& piece = geo.pieces[k];
                uint32_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                tr::Vec3 pmin{1e30f, 1e30f, 1e30f}, pmax{-1e30f, -1e30f, -1e30f};
                for (const tr::GeoVertex& v : piece.vertices) {
                    pmin = tr::Vec3{std::min(pmin.x, v.position.x), std::min(pmin.y, v.position.y), std::min(pmin.z, v.position.z)};
                    pmax = tr::Vec3{std::max(pmax.x, v.position.x), std::max(pmax.y, v.position.y), std::max(pmax.z, v.position.z)};
                }
                for (const tr::GeoVertex& v : piece.vertices)
                    for (int c = 0; c < 4; ++c) {
                        const uint32_t byte = (v.colour >> (24 - 8 * c)) & 0xFF;
                        lo[c] = std::min(lo[c], byte);
                        hi[c] = std::max(hi[c], byte);
                    }
                std::printf("piece %zu %s (%zu vertices): colour A %u-%u R %u-%u G %u-%u B %u-%u; elements", k, piece.effectName.c_str(),
                            piece.vertices.size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
                for (const tr::VertexElement& e : piece.layout) std::printf(" usage%u.%u type%u", e.usage, e.usageIndex, e.type);
                std::printf("\n    bone %s, stored positions (%.2f %.2f %.2f) - (%.2f %.2f %.2f)\n", piece.boneName.c_str(), pmin.x, pmin.y, pmin.z,
                            pmax.x, pmax.y, pmax.z);
                // RASA_GEOVERTS=<piece>: every vertex of that piece (position, normal, uv, uv1, ARGB colour).
                if (const char* dump = envValue("RASA_GEOVERTS"); dump && size_t(std::atoi(dump)) == k)
                    for (const tr::GeoVertex& v : piece.vertices)
                        std::printf("    v %.3f %.3f %.3f n %.3f %.3f %.3f uv %.3f %.3f uv1 %.3f %.3f c %08x\n", v.position.x, v.position.y, v.position.z,
                                    v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y, v.uv1.x, v.uv1.y, v.colour);
                // Winding: Direct3D's default cull (CCW) keeps triangles that are clockwise on screen, whose face normal
                // in this left-handed space is cross(b - a, c - a). Count those facing away from the piece's centre and
                // those agreeing with the stored vertex normals.
                const tr::Vec3 centre = scaled(add(pmin, pmax), 0.5f);
                size_t outward = 0, agree = 0, triangles = 0;
                for (size_t t = 0; t + 2 < piece.indices.size(); t += 3) {
                    const uint32_t i0 = piece.indices[t], i1 = piece.indices[t + 1], i2 = piece.indices[t + 2];
                    if (i0 >= piece.vertices.size() || i1 >= piece.vertices.size() || i2 >= piece.vertices.size()) continue;
                    const tr::Vec3& a = piece.vertices[i0].position;
                    const tr::Vec3& b = piece.vertices[i1].position;
                    const tr::Vec3& c = piece.vertices[i2].position;
                    const tr::Vec3 n = cross(subtract(b, a), subtract(c, a));
                    const tr::Vec3 mid = scaled(add(add(a, b), c), 1.0f / 3.0f);
                    const tr::Vec3 stored = add(add(piece.vertices[i0].normal, piece.vertices[i1].normal), piece.vertices[i2].normal);
                    ++triangles;
                    if (dot(n, subtract(mid, centre)) > 0) ++outward;
                    if (dot(n, stored) > 0) ++agree;
                }
                std::printf("    triangles %zu: %zu face away from the centre, %zu agree with the vertex normals\n", triangles, outward, agree);
                for (const tr::GeoParameter& p : piece.parameters) std::printf("    %s = %s\n", p.key.c_str(), p.value.c_str());
            }
        }
        // RASA_PKGSCAN=1: parse every special-FX package; counts of accessory / particle / light events and failures.
        if (envValue("RASA_PKGSCAN")) {
            size_t packages = 0, failed = 0, accessories = 0, particles = 0, lights = 0;
            for (const std::string& name : assets.namesWithExtension(".pkg")) {
                ++packages;
                try {
                    const tr::SfxPackage p = tr::readSfxPackage(assets.read(name));
                    accessories += p.accessories.size();
                    particles += p.particles.size();
                    lights += p.lights.size();
                } catch (const std::exception& e) {
                    if (++failed <= 5) std::printf("pkg %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("pkg scan: %zu packages, %zu failed; %zu accessories, %zu particle attaches, %zu lights\n", packages, failed,
                        accessories, particles, lights);
        }
        // RASA_TEXTWRITETEST=<folder>: every language module written as Python source (tr::moduleSource) and read back
        // (tr::evaluateSource) must give the same values; the sources are left in the folder (for a check with Python itself). Then
        // game.zip is rewritten with english missiontextlanguage as source (its .pyo removed, one text changed): the copy must hold
        // every other entry byte for byte and give the changed module.
        if (const char* folder = envValue("RASA_TEXTWRITETEST")) {
            size_t modules = 0, same = 0, entries = 0;
            for (const std::string& name : data.moduleNames()) {
                if (name.find(".language.") == std::string::npos) continue;
                ++modules;
                const tr::py::Object& globals = data.module(name);
                const std::string source = tr::py::moduleSource(globals);
                const tr::py::Ref back = tr::evaluateSource(tr::Bytes(source.begin(), source.end()), name);
                bool equal = tr::py::moduleSource(*back) == source;
                for (const auto& g : globals.entries) {
                    const std::string key = tr::py::asString(g.first.get());
                    if (key.compare(0, 2, "__") == 0) continue;
                    const tr::py::Object* other = tr::py::dictGet(back.get(), key);
                    equal = equal && other && tr::py::sourceLiteral(*other) == tr::py::sourceLiteral(*g.second);
                }
                same += equal ? 1 : 0;
                if (const tr::py::Object* lookup = tr::py::dictGet(&globals, "lookup")) entries += tr::py::length(lookup);
                if (!equal) std::printf("text write test: %s differs after the round trip\n", name.c_str());
                std::ofstream(std::string(folder) + "/" + name + ".py", std::ios::binary) << source;
            }
            std::printf("text write test: %zu language modules, %zu read back equal, %zu entries\n", modules, same, entries);
            try {
                const std::string module = "generated.client.language.english.missiontextlanguage";
                const std::string entry = data.moduleEntryName(module);
                tr::py::Ref edited = tr::py::deepCopy(std::shared_ptr<tr::py::Object>(&const_cast<tr::py::Object&>(data.module(module)), [](tr::py::Object*) {}));
                tr::py::Object* lookup = const_cast<tr::py::Object*>(tr::py::dictGet(edited.get(), "lookup"));
                const tr::py::Ref key = tr::py::makeTuple({tr::py::makeInt(1), tr::py::makeInt(1)});
                tr::py::dictPut(*lookup, key, tr::py::makeList({tr::py::makeText("Edited \xe2\x80\x99text\xc3\xa9 \xe6\xbc\xa2")}));
                const std::string source = tr::py::moduleSource(*edited);
                const std::string zipPath = std::string(folder) + "/game_test.zip";
                tr::ZipArchive original(data.path());
                const std::string sourceEntry = entry.substr(0, entry.size() - 4) + ".py";
                tr::writeZip(original, zipPath, {{sourceEntry, tr::Bytes(source.begin(), source.end())}}, {entry});
                tr::ZipArchive copy(zipPath);
                size_t identical = 0, compared = 0;
                for (const tr::ZipEntry& e : copy.entries()) {
                    if (e.name == sourceEntry) continue;
                    for (const tr::ZipEntry& o : original.entries())
                        if (o.name == e.name) {
                            ++compared;
                            identical += copy.readStored(e) == original.readStored(o) && copy.read(e) == original.read(o) ? 1 : 0;
                        }
                }
                tr::GameData reread(zipPath);
                const tr::py::Object* text = tr::py::item(tr::py::dictGetTuple(reread.global(module, "lookup"), {1, 1}), 0);
                std::printf("text write test: zip %zu -> %zu entries, %zu of %zu others identical, module from %s, text 1/1 \"%s\", %zu entries\n",
                            original.entries().size(), copy.entries().size(), identical, compared, reread.moduleEntryName(module).c_str(),
                            tr::py::asString(text).c_str(), tr::py::length(reread.global(module, "lookup")));
            } catch (const std::exception& e) {
                std::printf("text write test: FAILED: %s\n", e.what());
            }
        }
        // RASA_GLMTEST=<out.glm>: write an archive of a few game files (a texture, a model, an animation, a text file, an empty file),
        // open it with GlmArchive and compare every entry; then rewrite one shipped archive's every entry and compare again.
        if (const char* out = envValue("RASA_GLMTEST")) {
            try {
                std::vector<tr::GlmSource> sources;
                for (const char* name : {"npc_hum_m_officer_v01_df.dds", "prop_hum_lamp_v02.geo", "m_idle_non_combat_v01.anm",
                                         "sky_foreas_bootcamp_d11.sky"})
                    sources.push_back(tr::GlmSource{name, assets.read(name), true, 0});
                sources.push_back(tr::GlmSource{"Empty_File.txt", {}, true, 0});
                sources.push_back(tr::GlmSource{"stored.bin", assets.read("prop_hum_lamp_v02.geo"), false, 1234567890});
                const std::vector<tr::GlmSource> expected = sources;
                tr::writeGlmArchive(out, sources);
                tr::GlmArchive glm(out);
                size_t matching = 0, compressed = 0;
                bool ordered = true, aligned = true;
                for (size_t k = 0; k < glm.entries().size(); ++k) {
                    const tr::GlmEntry& e = glm.entries()[k];
                    if (k && tr::toLowerAscii(glm.entries()[k - 1].name) >= tr::toLowerAscii(e.name)) ordered = false;
                    if (e.offset % 1024) aligned = false;
                    if (e.flags) ++compressed;
                    for (const tr::GlmSource& s : expected)
                        if (s.name == e.name && glm.read(e) == s.data && (s.timestamp == 0 || s.timestamp == e.timestamp)) ++matching;
                }
                const tr::GlmEntry* stored = glm.find("STORED.BIN");
                std::printf("glm write test: %zu entries, %zu read back equal, %zu compressed, %s, %s, stored entry %s\n", glm.entries().size(), matching,
                            compressed, ordered ? "sorted" : "NOT sorted", aligned ? "1 KiB aligned" : "NOT aligned",
                            stored && stored->flags == 0 ? "kept stored" : "WRONG");
                bool duplicateRefused = false;
                try {
                    tr::buildGlmArchive({tr::GlmSource{"a.txt", {1}, true, 0}, tr::GlmSource{"A.TXT", {2}, true, 0}});
                } catch (const tr::FormatError&) {
                    duplicateRefused = true;
                }
                // A whole shipped archive rewritten: every entry read back the same.
                const std::string source = assets.root() + "/data/sky.glm";
                tr::GlmArchive original(source);
                std::vector<tr::GlmSource> all;
                for (const tr::GlmEntry& e : original.entries()) all.push_back(tr::GlmSource{e.name, original.read(e), e.flags != 0, e.timestamp});
                const std::string copy = std::string(out) + ".sky.glm";
                tr::writeGlmArchive(copy, all);
                tr::GlmArchive rewritten(copy);
                size_t same = 0;
                for (const tr::GlmEntry& e : original.entries())
                    if (const tr::GlmEntry* r = rewritten.find(e.name); r && rewritten.read(*r) == original.read(e)) ++same;
                std::printf("glm rewrite test: sky.glm %zu entries, %zu equal after rewriting; duplicate names %s\n", original.entries().size(), same,
                            duplicateRefused ? "refused" : "NOT refused");
            } catch (const std::exception& e) {
                std::printf("glm write test FAILED: %s\n", e.what());
            }
        }
        // RASA_MKRSCAN=1: parse every animation marker file; markers per type, files whose markers lie past their animation's end.
        if (envValue("RASA_MKRSCAN")) {
            size_t files = 0, failed = 0, markers = 0, withAnim = 0, inside = 0, rewritten = 0;
            std::map<int, size_t> byType;
            for (const std::string& name : assets.namesWithExtension(".mkr")) {
                ++files;
                try {
                    const tr::Bytes data = assets.read(name);
                    const tr::AnimationMarkers m = tr::readAnimationMarkers(data);
                    if (tr::writeAnimationMarkers(m, tr::Bytes(data.begin(), data.begin() + 8)) == data) ++rewritten;
                    else if (++failed <= 5) std::printf("mkr %s: rewrite differs\n", name.c_str());
                    markers += m.markers.size();
                    // RASA_MKRLIST=<prefix>: lists the files with markers whose names start with it.
                    if (envValue("RASA_MKRLIST") && name.rfind(envValue("RASA_MKRLIST"), 0) == 0 && !m.markers.empty())
                        std::printf("mkr %s: %zu markers\n", name.c_str(), m.markers.size());
                    for (const tr::AnimMarker& k : m.markers) ++byType[k.type];
                    const std::string anim = name.substr(0, name.size() - 4) + ".anm";
                    if (assets.exists(anim)) {
                        ++withAnim;
                        const tr::Animation a = tr::readAnimation(assets.read(anim));
                        bool all = true;
                        for (const tr::AnimMarker& k : m.markers) all = all && k.timeMs <= uint32_t(a.duration * 1000.0f + 50.0f);
                        if (all) ++inside;
                    }
                } catch (const std::exception& e) {
                    if (++failed <= 5) std::printf("mkr %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("mkr scan: %zu files, %zu failed, %zu markers; %zu with an .anm, %zu of them with every marker inside it; %zu rewritten "
                        "byte-identical\n", files, failed, markers, withAnim, inside, rewritten);
            for (const auto& [type, count] : byType) std::printf("  %2d %-20s %zu\n", type, tr::animMarkerTypeName(type), count);
        }
        // RASA_CONSTANTS=<module>: a game data module's integer constants (GameData::moduleConstants).
        if (const char* module = envValue("RASA_CONSTANTS")) {
            try {
                const auto constants = data.moduleConstants(module);
                std::printf("%s: %zu constants\n", module, constants.size());
                for (size_t k = 0; k < constants.size() && k < 8; ++k) std::printf("  %d %s\n", constants[k].first, constants[k].second.c_str());
            } catch (const std::exception& e) {
                std::printf("%s: %s\n", module, e.what());
            }
        }
        // RASA_PKGINFO=<name.pkg>: one special-FX package's events (accessories, particle attaches with target / contact point, lights).
        if (const char* pkg = envValue("RASA_PKGINFO")) {
            try {
                const tr::SfxPackage p = tr::readSfxPackage(assets.read(pkg));
                for (const tr::SfxAccessory& a : p.accessories)
                    std::printf("pkg %s accessory mesh %s anim %s package %s\n", pkg, a.mesh.c_str(), a.animation.c_str(), a.package.c_str());
                for (const tr::SfxParticleAttach& a : p.particles)
                    std::printf("pkg %s particles %s target %u contact point %u (v%u, unknown0 %u, unknown2 %u, flag %d)\n", pkg, a.file.c_str(), a.target,
                                a.contactPoint, a.version, a.unknown0, a.unknown2, int(a.flag));
                std::printf("pkg %s: %zu lights\n", pkg, p.lights.size());
                for (const tr::SfxLight& l : p.lights) {
                    std::printf("  light duration %u ms target %u contact point %u spot %d shadows %d priority %.2f\n", l.durationMs,
                                l.target, l.contactPoint, int(l.spot), int(l.castsShadows), l.priority);
                    static const char* names[9] = {"colourR", "colourG", "colourB", "colourA", "range", "falloff", "intensity", "penumbra", "cone"};
                    for (int k = 0; k < 9; ++k) {
                        const tr::SfxCurve& c = tr::sfxLightCurve(l, k);
                        std::printf("    %-10s loop %d:", names[k], int(c.loop));
                        for (const auto& key : c.keys) std::printf(" (%.3f s, %.3f)", key.first, key.second);
                        std::printf("\n");
                    }
                }
            } catch (const std::exception& e) {
                std::printf("pkg %s: %s\n", pkg, e.what());
            }
        }
        // RASA_GEOROUNDTRIP=1: every .geo read with tr::readGeoFile and written back must be byte-identical, and decodeGeoFile must
        // give what readGeo gives (piece / vertex / bone counts).
        if (envValue("RASA_GEOROUNDTRIP")) {
            size_t models = 0, identical = 0, failed = 0;
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                ++models;
                try {
                    const tr::Bytes original = assets.read(name);
                    const tr::GeoFile file = tr::readGeoFile(original);
                    const tr::Bytes written = tr::writeGeoFile(file);
                    if (written == original) {
                        ++identical;
                    } else {
                        size_t at = 0;
                        while (at < written.size() && at < original.size() && written[at] == original[at]) ++at;
                        std::printf("geo round trip DIFFERS: %s (sizes %zu / %zu, first difference at %zu)\n", name.c_str(), original.size(),
                                    written.size(), at);
                    }
                } catch (const std::exception& e) {
                    ++failed;
                    std::printf("geo round trip FAILED: %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("geo round trip: %zu of %zu models byte-identical, %zu not read\n", identical, models, failed);
            // Bone collision volumes: decoded and written back byte-identical; which definition the BVOL radius follows.
            {
                size_t bones = 0, same = 0;
                std::map<std::string, size_t> radiusRule;
                for (const std::string& name : assets.namesWithExtension(".geo")) {
                    tr::GeoFile file;
                    try { file = tr::readGeoFile(assets.read(name)); } catch (const std::exception&) { continue; }
                    for (const tr::GeoFileBone& b : file.bones) {
                        if (b.boundingVolume.empty()) continue;
                        ++bones;
                        try {
                            const std::vector<tr::GeoBoneVolume> volumes = tr::readBoneVolumes(b.boundingVolume);
                            if (tr::writeBoneVolumes(volumes) == b.boundingVolume) ++same;
                            else std::printf("bone volume DIFFERS: %s bone %s\n", name.c_str(), b.name.c_str());
                            for (const tr::GeoBoneVolume& v : volumes) {
                                const std::string kind = tr::tagName(v.kind);
                                auto close = [&](float x) { return std::fabs(x - v.radius) <= 1e-3f * std::max(1.0f, v.radius); };
                                std::vector<std::pair<const char*, float>> candidates;
                                if (kind == "BVWS" && !v.vertices.empty()) {
                                    tr::Vec3 lo = v.vertices[0], hi = v.vertices[0];
                                    float origin = 0;
                                    for (const tr::Vec3& p : v.vertices) {
                                        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                                        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                                        origin = std::max(origin, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z));
                                    }
                                    const tr::Vec3 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
                                    float fromCentre = 0;
                                    for (const tr::Vec3& p : v.vertices) {
                                        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
                                        fromCentre = std::max(fromCentre, std::sqrt(dx * dx + dy * dy + dz * dz));
                                    }
                                    const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
                                    candidates = {{"farthest from the origin", origin}, {"farthest from the box centre", fromCentre},
                                                  {"half the box diagonal", 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz)}};
                                } else if (kind == "BVBX") {
                                    const tr::Vec3& h = v.halfExtents;
                                    const float half = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
                                    const float c = std::sqrt(v.centre.x * v.centre.x + v.centre.y * v.centre.y + v.centre.z * v.centre.z);
                                    candidates = {{"half diagonal", half}, {"centre distance + half diagonal", c + half}};
                                } else if (kind == "BVSP") {
                                    candidates = {{"sphere radius", v.sphereRadius}};
                                }
                                std::string rule = "none";
                                for (const auto& cand : candidates)
                                    if (close(cand.second)) {
                                        rule = cand.first;
                                        break;
                                    }
                                ++radiusRule[kind + ": " + rule];
                            }
                        } catch (const std::exception& e) {
                            std::printf("bone volume FAILED: %s bone %s: %s\n", name.c_str(), b.name.c_str(), e.what());
                        }
                    }
                }
                std::printf("bone volumes: %zu of %zu bones byte-identical\n", same, bones);
                for (const auto& r : radiusRule) std::printf("  radius %s: %zu\n", r.first.c_str(), r.second);
            }
            // Animations: read / write byte-identical, and every bone / material track's keys decoded and encoded again in the
            // same key chunk kind reproduce the stored bytes (the half / int16 / float encoders).
            size_t anims = 0, animIdentical = 0, animFailed = 0, tracks = 0, tracksReencoded = 0;
            float maxKeyError = 0;
            for (const std::string& name : assets.namesWithExtension(".anm")) {
                ++anims;
                try {
                    const tr::Bytes original = assets.read(name);
                    tr::AnimFile file = tr::readAnimFile(original);
                    if (tr::writeAnimFile(file) == original) ++animIdentical;
                    else std::printf("anm round trip DIFFERS: %s\n", name.c_str());
                    for (tr::AnimFileTrack& t : file.tracks) {
                        if (t.target != tr::fourcc("SKEL") && t.target != tr::fourcc("EFCT")) continue;
                        ++tracks;
                        tr::AnimFileTrack again = t;
                        if (t.target == tr::fourcc("SKEL")) tr::encodeAnimKeys(again, t.keyTag, tr::decodeAnimKeys(t));
                        else tr::encodeMaterialKeys(again, tr::decodeMaterialKeys(t));
                        if (again.keyData == t.keyData) {
                            ++tracksReencoded;
                        } else if (t.target == tr::fourcc("SKEL")) {
                            // Rotations are normalised on decoding, so int16 rotations may come back one step off: measure it.
                            const std::vector<tr::AnimKey> a = tr::decodeAnimKeys(t), b = tr::decodeAnimKeys(again);
                            for (size_t k = 0; k < a.size(); ++k) {
                                const float d[] = {a[k].rotation.x - b[k].rotation.x, a[k].rotation.y - b[k].rotation.y, a[k].rotation.z - b[k].rotation.z,
                                                   a[k].rotation.w - b[k].rotation.w, a[k].translation.x - b[k].translation.x, a[k].translation.y - b[k].translation.y,
                                                   a[k].translation.z - b[k].translation.z, a[k].scale.x - b[k].scale.x, a[k].time - b[k].time};
                                for (float x : d) maxKeyError = std::max(maxKeyError, std::fabs(x));
                            }
                        } else {
                            std::printf("anm material keys re-encode DIFFER: %s track %s\n", name.c_str(), t.name.c_str());
                        }
                    }
                } catch (const std::exception& e) {
                    ++animFailed;
                    std::printf("anm round trip FAILED: %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("anm round trip: %zu of %zu animations byte-identical, %zu not read; keys re-encoded identically in %zu of %zu tracks, "
                        "largest decoded difference in the others %.6f\n",
                        animIdentical, anims, animFailed, tracksReencoded, tracks, maxKeyError);
        }
        // RASA_BLENDERSCHEMA=<file.json>: writes the Blender add-on's schema (every effect's parameters, connection points, ...).
        if (const char* where = envValue("RASA_BLENDERSCHEMA")) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string error;
            BlenderSchemaStats stats;
            const bool ok = writeBlenderSchema(assets, where, error, &stats);
            std::printf("blender schema: %s - %zu models, %zu pieces, %zu effects, %.1f s%s%s\n", ok ? "written" : "FAILED", stats.models,
                        stats.pieces, stats.effects, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                        error.empty() ? "" : ": ", error.c_str());
        }
        // RASA_GLTFEXPORT=<model.geo>[,<anim.anm>...] with RASA_GLTFOUT=<file.gltf>: exports the model (and the animations; none
        // named = the model's own) with ModelExchange.
        if (const char* what = envValue("RASA_GLTFEXPORT")) {
            std::vector<std::string> parts;
            std::stringstream list(what);
            for (std::string part; std::getline(list, part, ',');)
                if (!part.empty()) parts.push_back(part);
            const char* out = envValue("RASA_GLTFOUT");
            if (!parts.empty() && out) {
                std::vector<std::string> anims(parts.begin() + 1, parts.end());
                if (anims.empty()) anims = modelOwnAnimations(assets, parts[0]);
                try {
                    const auto start = std::chrono::steady_clock::now();
                    const ModelExchangeReport report = exportModelGltf(assets, parts[0], anims, out);
                    std::printf("gltf export: %s with %zu animation(s) -> %zu files in %.2f s\n", parts[0].c_str(), anims.size(), report.written.size(),
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                    for (const std::string& w : report.warnings) std::printf("gltf export warning: %s\n", w.c_str());
                } catch (const std::exception& e) {
                    std::printf("gltf export FAILED: %s\n", e.what());
                }
            }
        }
        // RASA_TANGENTCHECK=<n>: for the first n shipped pieces with tangent frames (TANGENT0 / TANGENT1), computeTangentFrames from
        // their positions, normals and UV set 0 against the stored vectors (mean dot product; 1 = same direction, -1 = opposite).
        if (const char* count = envValue("RASA_TANGENTCHECK")) {
            const size_t wanted = size_t(std::max(1, std::atoi(count)));
            size_t pieces = 0;
            double sumT = 0, sumB = 0, n = 0;
            size_t flippedB = 0, sameB = 0;
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                if (pieces >= wanted) break;
                tr::GeoModel geo;
                try { geo = tr::readGeo(assets.read(name)); } catch (const std::exception&) { continue; }
                for (const tr::GeoPiece& piece : geo.pieces) {
                    bool hasT = false;
                    for (const tr::VertexElement& e : piece.layout) hasT = hasT || (e.usage == 6 && e.usageIndex == 0);
                    if (!hasT || piece.vertices.empty() || pieces >= wanted) continue;
                    ++pieces;
                    std::vector<tr::Vec3> p, nn, t, b;
                    std::vector<tr::Vec2> uv;
                    for (const tr::GeoVertex& v : piece.vertices) {
                        p.push_back(v.position);
                        nn.push_back(v.normal);
                        uv.push_back(v.uv);
                    }
                    computeTangentFrames(p, nn, uv, piece.indices, t, b);
                    double pt = 0, pb = 0;
                    for (size_t k = 0; k < piece.vertices.size(); ++k) {
                        const tr::GeoVertex& v = piece.vertices[k];
                        auto unit = [](tr::Vec3 a) { const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); return l > 0 ? tr::Vec3{a.x / l, a.y / l, a.z / l} : a; };
                        const tr::Vec3 st = unit(v.tangent), sb = unit(v.binormal);
                        pt += st.x * t[k].x + st.y * t[k].y + st.z * t[k].z;
                        pb += sb.x * b[k].x + sb.y * b[k].y + sb.z * b[k].z;
                    }
                    pt /= double(piece.vertices.size());
                    pb /= double(piece.vertices.size());
                    sumT += pt;
                    sumB += pb;
                    n += 1;
                    if (pb < 0) ++flippedB; else ++sameB;
                }
            }
            std::printf("tangent check: %zu pieces, mean dot tangent %.3f, binormal %.3f (pieces with the binormal mostly opposite: %zu, same: %zu)\n", pieces,
                        n ? sumT / n : 0.0, n ? sumB / n : 0.0, flippedB, sameB);
        }
        // RASA_GLTFIMPORT=<file.gltf|.glb>: imports with ModelExchange (RASA_GLTFSOURCE names the source model for a file without one),
        // lists the files it would write and compares the model with its source: per piece of the same index, vertex count, the largest
        // distance from each source vertex to the nearest imported vertex, UVs, normals, skin bones by name; bones; contact points;
        // and each animation's keys against the source .anm (bind-adjusted where a component was left out).
        if (const char* importPath = envValue("RASA_GLTFPREVIEW") ? nullptr : envValue("RASA_GLTFIMPORT")) {
            try {
                ModelImportOptions options;
                if (const char* src = envValue("RASA_GLTFSOURCE")) options.source = src;
                // RASA_GLTFMODELNAME: write the model under this name (e.g. an existing map entity's model, to see a new model in
                // place); RASA_GLTFPREVIEW=1: the imported files override the archives' for this run (AssetLibrary::setOverride).
                if (const char* name = envValue("RASA_GLTFMODELNAME")) options.modelName = name;
                options.renameChangedTextures = envValue("RASA_GLTFRENAME") != nullptr;
                const auto start = std::chrono::steady_clock::now();
                const ModelImportResult result = importModelGltf(assets, importPath, options);
                std::printf("gltf import: %zu file(s) in %.2f s\n", result.files.size(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                for (const ImportedFile& f : result.files) std::printf("  %s (%zu bytes): %s\n", f.name.c_str(), f.data.size(), f.note.c_str());
                for (const std::string& w : result.warnings) std::printf("gltf import warning: %s\n", w.c_str());
                // RASA_GLTFPATCH=1: the imported files into the game's patch archive (data/rasaedit_patch.glm), registered in
                // tabula_rasa.cfg - what the Blender panel's two buttons do. Changes the game folder.
                if (envValue("RASA_GLTFPATCH") && !result.files.empty()) {
                    std::printf("patch archive: %s\n", addToPatchArchive(argv[1], result.files, &assets).c_str());
                    std::printf("tabula_rasa.cfg: %s\n", registerPatchArchive(argv[1]).c_str());
                    // The library re-mounted the archive: reads in this process see the new files.
                    size_t same = 0;
                    for (const ImportedFile& f : result.files) {
                        tr::Bytes back;
                        same += assets.tryRead(f.name, back) && back == f.data ? 1 : 0;
                    }
                    std::printf("patch archive: %zu of %zu files read back equal in this session\n", same, result.files.size());
                }
                if (const char* dump = envValue("RASA_GLTFIMPORTDIR"))
                    for (const ImportedFile& f : result.files) {
                        std::ofstream o(std::filesystem::path(dump) / f.name, std::ios::binary);
                        o.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
                    }
                if (!result.files.empty()) {
                    const tr::GeoModel built = tr::readGeo(result.files[0].data);
                    tr::Bytes sourceData;
                    const std::string sourceName = result.files[0].name;
                    if (assets.tryRead(sourceName, sourceData)) {
                        const tr::GeoModel original = tr::readGeo(sourceData);
                        std::printf("compare: pieces %zu / %zu, bones %zu / %zu, contact points %zu / %zu\n", original.pieces.size(), built.pieces.size(),
                                    original.bones.size(), built.bones.size(), original.contactPoints.size(), built.contactPoints.size());
                        size_t sameBones = 0;
                        for (size_t b = 0; b < std::min(original.bones.size(), built.bones.size()); ++b) {
                            const tr::GeoBone &x = original.bones[b], &y = built.bones[b];
                            if (x.name == y.name && x.parent == y.parent && std::memcmp(&x.rotation, &y.rotation, sizeof x.rotation) == 0 &&
                                std::memcmp(&x.translation, &y.translation, sizeof x.translation) == 0)
                                ++sameBones;
                        }
                        std::printf("compare: %zu bones identical (name, parent, bind)\n", sameBones);
                        for (size_t k = 0; k < std::min(original.pieces.size(), built.pieces.size()); ++k) {
                            const tr::GeoPiece &a = original.pieces[k], &b = built.pieces[k];
                            float worst = 0, worstUv = 0, worstNormal = 0;
                            size_t skinMismatch = 0;
                            const bool sameOrder = a.vertices.size() == b.vertices.size();   // RasaEdit's own export keeps the order
                            for (size_t vi = 0; vi < a.vertices.size(); ++vi) {
                                const tr::GeoVertex& va = a.vertices[vi];
                                float best = 1e30f;
                                const tr::GeoVertex* nearest = sameOrder ? &b.vertices[vi] : nullptr;
                                if (sameOrder) {
                                    const float dx = va.position.x - nearest->position.x, dy = va.position.y - nearest->position.y, dz = va.position.z - nearest->position.z;
                                    // Billboard centres (POSITION1) count too.
                                    const float cx = va.position1.x - nearest->position1.x, cy = va.position1.y - nearest->position1.y,
                                                cz = va.position1.z - nearest->position1.z;
                                    best = dx * dx + dy * dy + dz * dz + cx * cx + cy * cy + cz * cz;
                                }
                                static const std::vector<tr::GeoVertex> none;
                                const std::vector<tr::GeoVertex>& candidates = sameOrder ? none : b.vertices;
                                for (const tr::GeoVertex& vb : candidates) {
                                    const float dx = va.position.x - vb.position.x, dy = va.position.y - vb.position.y, dz = va.position.z - vb.position.z;
                                    // Vertices at one position (seams, hard edges) are told apart by their UVs and normals.
                                    const float nd = 1.0f - (va.normal.x * vb.normal.x + va.normal.y * vb.normal.y + va.normal.z * vb.normal.z);
                                    const float d = dx * dx + dy * dy + dz * dz + 1e-7f * (nd + std::fabs(va.uv.x - vb.uv.x) + std::fabs(va.uv.y - vb.uv.y));
                                    if (d < best) { best = d; nearest = &vb; }
                                }
                                if (!nearest) continue;
                                worst = std::max(worst, std::sqrt(std::max(0.0f, best)));
                                worstUv = std::max(worstUv, std::max(std::fabs(va.uv.x - nearest->uv.x), std::fabs(va.uv.y - nearest->uv.y)));
                                const float dot = va.normal.x * nearest->normal.x + va.normal.y * nearest->normal.y + va.normal.z * nearest->normal.z;
                                const float la = std::sqrt(va.normal.x * va.normal.x + va.normal.y * va.normal.y + va.normal.z * va.normal.z);
                                worstNormal = std::max(worstNormal, la > 0 ? 1.0f - dot / la : 0.0f);
                                if (a.skinned && b.skinned) {
                                    const uint32_t ia = tr::blendIndex(va, 0), ib = tr::blendIndex(*nearest, 0);
                                    const std::string na = ia < a.skinBones.size() ? a.skinBones[ia] : "", nb = ib < b.skinBones.size() ? b.skinBones[ib] : "";
                                    if (tr::toLowerAscii(na) != tr::toLowerAscii(nb) && tr::blendWeight(va, 0) > 0.6f) {
                                        if (++skinMismatch <= 2) {
                                            auto describe = [](const tr::GeoPiece& p, const tr::GeoVertex& v) {
                                                std::string s;
                                                for (uint32_t i = 0; i < 3; ++i) {
                                                    const uint32_t slot = tr::blendIndex(v, i);
                                                    s += (slot < p.skinBones.size() ? p.skinBones[slot] : "?") + "=" + std::to_string(tr::blendWeight(v, i)) + " ";
                                                }
                                                return s;
                                            };
                                            std::printf("    skin differs at (%.3f %.3f %.3f): source %s/ imported %s\n", va.position.x, va.position.y, va.position.z,
                                                        describe(a, va).c_str(), describe(b, *nearest).c_str());
                                        }
                                    }
                                }
                            }
                            std::printf("  piece %zu %-28s %-40s verts %zu / %zu, tris %zu / %zu, max dist %.5f, uv %.5f, normal 1-cos %.5f, main bone differs %zu\n", k,
                                        a.pieceName.c_str(), a.effectName.c_str(), a.vertices.size(), b.vertices.size(), a.indices.size() / 3, b.indices.size() / 3,
                                        worst, worstUv, worstNormal, skinMismatch);
                        }
                        float worstCp = 0;
                        for (const tr::GeoContactPoint& ca : original.contactPoints)
                            for (const tr::GeoContactPoint& cb : built.contactPoints)
                                if (ca.id == cb.id) {
                                    const float dx = ca.position.x - cb.position.x, dy = ca.position.y - cb.position.y, dz = ca.position.z - cb.position.z;
                                    worstCp = std::max(worstCp, std::sqrt(dx * dx + dy * dy + dz * dz));
                                }
                        std::printf("compare: contact points largest position difference %.5f m\n", worstCp);
                        // Collision volumes per bone: kind, counts, the largest distance from a source point to the nearest built one.
                        {
                            const tr::GeoFile fa = tr::readGeoFile(sourceData), fb = tr::readGeoFile(result.files[0].data);
                            size_t withA = 0, withB = 0, same = 0;
                            float worst = 0, worstRadius = 0;
                            for (const tr::GeoFileBone& ba : fa.bones) {
                                if (!ba.boundingVolume.empty()) ++withA;
                                for (const tr::GeoFileBone& bb : fb.bones) {
                                    if (tr::toLowerAscii(ba.name) != tr::toLowerAscii(bb.name)) continue;
                                    if (ba.boundingVolume.empty() || bb.boundingVolume.empty()) continue;
                                    if (ba.boundingVolume == bb.boundingVolume) ++same;
                                    const auto va = tr::readBoneVolumes(ba.boundingVolume), vb = tr::readBoneVolumes(bb.boundingVolume);
                                    if (va.size() != vb.size() || va.empty()) continue;
                                    worstRadius = std::max(worstRadius, std::fabs(va[0].radius - vb[0].radius));
                                    if (va[0].kind != vb[0].kind) std::printf("  collision of %s changed kind %s -> %s\n", ba.name.c_str(),
                                                                              tr::tagName(va[0].kind).c_str(), tr::tagName(vb[0].kind).c_str());
                                    for (const tr::Vec3& p : va[0].vertices) {
                                        float best = 1e30f;
                                        for (const tr::Vec3& q : vb[0].vertices)
                                            best = std::min(best, (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
                                        worst = std::max(worst, std::sqrt(best));
                                    }
                                    const float dc = std::fabs(va[0].centre.x - vb[0].centre.x) + std::fabs(va[0].centre.y - vb[0].centre.y) +
                                                     std::fabs(va[0].centre.z - vb[0].centre.z) + std::fabs(va[0].halfExtents.x - vb[0].halfExtents.x) +
                                                     std::fabs(va[0].halfExtents.y - vb[0].halfExtents.y) + std::fabs(va[0].halfExtents.z - vb[0].halfExtents.z);
                                    worst = std::max(worst, dc);
                                    if (va[0].kind == tr::fourcc("BVWS"))
                                        std::printf("  collision %s: surface %zu / %zu vertices, %zu / %zu triangles\n", ba.name.c_str(), va[0].vertices.size(),
                                                    vb[0].vertices.size(), va[0].indices.size() / 3, vb[0].indices.size() / 3);
                                }
                            }
                            for (const tr::GeoFileBone& bb : fb.bones) {
                                withB += bb.boundingVolume.empty() ? 0 : 1;
                                for (const tr::GeoBoneVolume& v : tr::readBoneVolumes(bb.boundingVolume))
                                    std::printf("  imported collision on %s: %s centre (%.4f %.4f %.4f) half (%.4f %.4f %.4f) sphere %.4f, %zu vertices, "
                                                "%zu triangles\n", bb.name.c_str(), tr::tagName(v.kind).c_str(), v.centre.x, v.centre.y, v.centre.z,
                                                v.halfExtents.x, v.halfExtents.y, v.halfExtents.z, v.sphereRadius, v.vertices.size(), v.indices.size() / 3);
                            }
                            std::printf("compare: collision on %zu / %zu bones, %zu byte-identical, largest point difference %.5f m, radius %.5f\n", withA, withB, same,
                                        worst, worstRadius);
                        }
                    }
                    for (size_t f = 1; f < result.files.size(); ++f) {
                        if (tr::toLowerAscii(result.files[f].name).find(".anm") == std::string::npos) continue;
                        tr::Bytes origAnim;
                        if (!assets.tryRead(result.files[f].name, origAnim)) continue;
                        const tr::Animation a = tr::readAnimation(origAnim), b = tr::readAnimation(result.files[f].data);
                        float worstT = 0, worstR = 0;
                        for (const tr::AnimTrack& ta : a.tracks) {
                            const tr::AnimTrack* tb = tr::findTrack(b, ta.name);
                            if (!tb) continue;
                            const tr::GeoBone* bone = nullptr;
                            for (const tr::GeoBone& x : built.bones)
                                if (tr::toLowerAscii(x.name) == tr::toLowerAscii(ta.name)) bone = &x;
                            for (const tr::AnimKey& ka : ta.keys) {
                                const tr::AnimKey kb = tr::sampleTrack(*tb, ka.time);
                                const tr::Vec3 tA = ta.hasTranslation ? ka.translation : (bone ? bone->translation : ka.translation);
                                const tr::Vec3 tB = tb->hasTranslation ? kb.translation : (bone ? bone->translation : kb.translation);
                                const tr::Quat rA = ta.hasRotation ? ka.rotation : (bone ? bone->rotation : ka.rotation);
                                const tr::Quat rB = tb->hasRotation ? kb.rotation : (bone ? bone->rotation : kb.rotation);
                                worstT = std::max(worstT, std::max(std::fabs(tA.x - tB.x), std::max(std::fabs(tA.y - tB.y), std::fabs(tA.z - tB.z))));
                                const float dot = std::min(1.0f, std::fabs(rA.x * rB.x + rA.y * rB.y + rA.z * rB.z + rA.w * rB.w));
                                worstR = std::max(worstR, 2.0f * std::acos(dot) * 57.29578f);
                            }
                        }
                        std::printf("compare animation %s: tracks %zu / %zu, material tracks %zu / %zu, duration %.3f / %.3f s, largest translation %.5f, rotation %.3f deg\n",
                                    result.files[f].name.c_str(), a.tracks.size(), b.tracks.size(), a.materialTracks.size(), b.materialTracks.size(), a.duration,
                                    b.duration, worstT, worstR);
                    }
                }
            } catch (const std::exception& e) {
                std::printf("gltf import FAILED: %s\n", e.what());
            }
        }
        // RASA_IMAGETEST=<n>: the first n shipped DXT1 / DXT3 / DXT5 / uncompressed .dds textures: PNG written and read back must
        // be identical; re-encoded as DDS in their own format, decoded again, the error against the original's decode (PSNR).
        if (const char* count = envValue("RASA_IMAGETEST")) {
            const size_t wanted = size_t(std::max(1, std::atoi(count)));
            size_t tested = 0, pngSame = 0;
            double worst = 1e9, total = 0;
            std::map<std::string, size_t> formats;
            for (const std::string& name : assets.namesWithExtension(".dds")) {
                if (tested >= wanted) break;
                try {
                    const tr::Bytes dds = assets.read(name);
                    const tr::DdsHeader header = tr::readDdsHeader(dds);
                    if (header.depth > 1 || header.width < 4 || header.height < 4) continue;
                    const tr::RgbaImage image = tr::ddsImage(dds);
                    const tr::RgbaImage png = tr::readPng(tr::writePng(image));
                    bool same = png.width == image.width && png.height == image.height;
                    for (size_t k = 0; same && k < image.pixels.size(); ++k)
                        same = std::memcmp(&png.pixels[k], &image.pixels[k], 4) == 0;
                    if (same) ++pngSame;
                    const tr::DdsEncoding encoding = tr::ddsEncodingLike(header);
                    const tr::RgbaImage again = tr::ddsImage(tr::encodeDds(image, encoding));
                    double sum = 0;
                    for (size_t k = 0; k < image.pixels.size(); ++k) {
                        const int d[4] = {image.pixels[k].red - again.pixels[k].red, image.pixels[k].green - again.pixels[k].green,
                                          image.pixels[k].blue - again.pixels[k].blue, image.pixels[k].alpha - again.pixels[k].alpha};
                        for (int x : d) sum += double(x) * x;
                    }
                    const double mse = sum / (4.0 * double(image.pixels.size()));
                    const double psnr = mse <= 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
                    worst = std::min(worst, psnr);
                    total += psnr;
                    ++formats[tr::ddsEncodingName(encoding)];
                    ++tested;
                    if (psnr < 30) std::printf("image test: %s (%s %ux%u) re-encoded PSNR %.1f dB\n", name.c_str(), tr::ddsEncodingName(encoding),
                                               header.width, header.height, psnr);
                } catch (const std::exception& e) {
                    std::printf("image test: %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("image test: %zu textures, PNG identical in %zu; DDS re-encode PSNR mean %.1f dB, worst %.1f dB (", tested, pngSame,
                        tested ? total / double(tested) : 0.0, worst);
            for (const auto& f : formats) std::printf(" %s %zu", f.first.c_str(), f.second);
            std::printf(" )\n");
        }
        // RASA_GEOSCAN=1: every model's pieces whose vertex colours are all zero (colour and alpha), with names.
        if (envValue("RASA_GEOSCAN")) {
            std::map<std::string, size_t> byName;
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                tr::GeoModel geo;
                try {
                    geo = tr::readGeo(assets.read(name));
                } catch (const std::exception&) {
                    continue;
                }
                for (const tr::GeoPiece& piece : geo.pieces) {
                    if (piece.vertices.empty()) continue;
                    bool hasColour = false, allZero = true;
                    for (const tr::VertexElement& e : piece.layout) hasColour = hasColour || (e.usage == 10 && e.usageIndex == 0);
                    for (const tr::GeoVertex& v : piece.vertices) allZero = allZero && v.colour == 0;
                    if (!hasColour || !allZero) continue;
                    ++byName[tr::toLowerAscii(piece.boneName) + " | " + piece.effectName];
                    std::printf("zero-colour piece: %s bone '%s' group '%s' %s (%zu vertices)\n", name.c_str(), piece.boneName.c_str(),
                                piece.swapGroup.c_str(), piece.effectName.c_str(), piece.vertices.size());
                }
            }
            for (const auto& n : byName) std::printf("zero-colour summary: %4zu  %s\n", n.second, n.first.c_str());
        }
        // RASA_NEAR=x,z,radius: map entities within the radius (horizontal), with model, position and scale.
        if (const char* near = envValue("RASA_NEAR")) {
            float x = 0, z = 0, radius = 20;
            std::sscanf(near, "%f,%f,%f", &x, &z, &radius);
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                const tr::MapEntity& e = map.map.entities[k];
                const float dx = e.location.x - x, dz = e.location.z - z;
                if (dx * dx + dz * dz > radius * radius) continue;
                std::printf("near %zu %s at %.1f %.1f %.1f scale %.2f orientation %.3f %.3f %.3f %.3f\n", k, data.classMeshName(e.classId).c_str(), e.location.x,
                            e.location.y, e.location.z, e.scale, e.orientation.x, e.orientation.y, e.orientation.z, e.orientation.w);
            }
        }
        // RASA_FADEDUMP=<multiplier>: per map entity the game's distance fade (tabula_rasa.exe FUN_00856c00, see CLAUDE.md
        // "Distance fade") from the eye in argv 7-9 with the given view multiplier and Far CullLayer Scalar 1: index, culling layer,
        // bounding-sphere centre distance, scaled radius, alpha (1 opaque, 0 culled), world position.
        if (const char* fade = envValue("RASA_FADEDUMP")) {
            const float multiplier = std::max(float(std::atof(fade)), 1.0f);
            const tr::Vec3 eye{float(std::atof(argv[7])), float(std::atof(argv[8])), float(std::atof(argv[9]))};
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                const tr::MapEntity& e = map.map.entities[k];
                const int layer = e.cullLayer != 0 ? e.cullLayer : data.classCullingLayer(e.classId);
                const float f = 1.0f;
                const float layerDistance = layer == 2 ? 100 + 200 * f : layer == 3 ? 100 : layer == 4 ? 10 : layer == 5 ? 25 : layer == 7 ? 50
                                          : layer == 8 ? 100 + 50 * f : layer == 9 ? 100 + 100 * f : layer == 10 ? 100 + 150 * f : layer == 11 ? 75 : 0;
                const tr::Vec3 c{(e.bounds.min.x + e.bounds.max.x) * 0.5f, (e.bounds.min.y + e.bounds.max.y) * 0.5f, (e.bounds.min.z + e.bounds.max.z) * 0.5f};
                const Mat4 world = placementMatrix(e.location, e.orientation, e.scale);
                const tr::Vec3 centre = transformPoint(world, c);
                const float radius = length(subtract(e.bounds.max, e.bounds.min)) * 0.5f * e.scale;
                const float d = length(subtract(centre, eye));
                float alpha = 1.0f;
                if (layer == 6) alpha = 0.0f;
                else if (layerDistance > 0) {
                    const float maxDistance = std::min(layerDistance * multiplier, 250.0f);
                    const float start = 0.9f * maxDistance;
                    if (d > start) alpha = std::clamp(1.0f - (d - radius - start) / (maxDistance - start), 0.0f, 1.0f);
                }
                std::printf("fade %zu layer %d d %.1f r %.1f alpha %.3f at %.2f %.2f %.2f\n", k, layer, d, radius, alpha, e.location.x, e.location.y,
                            e.location.z);
            }
        }
        // RASA_FINDFEATURES=1: map entities whose models use material features worth checking in game - TR texture transforms
        // with an offset in the third row, TR environment reflection (MatSpecular.z > 0), modifier blending, decals on UV
        // set 2, TRHair, Pal*Dec* decal maps. One line per (feature, model) with the nearest-to-first entity position and the entity count.
        if (envValue("RASA_FINDFEATURES")) {
            std::map<std::string, tr::GeoModel> geos;
            std::map<std::string, std::pair<size_t, tr::Vec3>> found;      // "feature | model | detail" -> count, first position
            for (const tr::MapEntity& e : map.map.entities) {
                const std::string mesh = tr::toLowerAscii(data.classMeshName(e.classId));
                if (mesh.empty() || mesh == "default.geo") continue;
                auto it = geos.find(mesh);
                if (it == geos.end()) {
                    tr::GeoModel geo;
                    try { geo = tr::readGeo(assets.read(mesh)); } catch (const std::exception&) {}
                    it = geos.emplace(mesh, std::move(geo)).first;
                }
                for (const tr::GeoPiece& piece : it->second.pieces) {
                    const std::string effect = tr::toLowerAscii(piece.effectName);
                    const bool palDecal = effect.rfind("paldiff", 0) == 0 && effect.find("dec") != std::string::npos;
                    if ((effect.rfind("tr", 0) != 0 && !palDecal) || effect.find("additive") != std::string::npos) continue;
                    std::string phase;
                    for (const tr::GeoParameter& gp : piece.parameters)
                        if (gp.key == "Phase") phase = tr::toLowerAscii(gp.value);
                    if (phase == "additive" || phase == "translucent") continue;
                    std::vector<std::string> features;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key.rfind("TexCoordTransform", 0) == 0) {
                            float m[16] = {};
                            std::istringstream values(gp.value);
                            for (float& x : m) values >> x;
                            if (m[8] != 0 || m[9] != 0) features.push_back("offset transform " + gp.key + " = " + gp.value);
                        }
                        if (gp.key == "MatSpecular" && effect.find("env") != std::string::npos) {
                            float s[4] = {};
                            std::istringstream values(gp.value);
                            for (float& x : s) values >> x;
                            if (s[2] > 0.2f) features.push_back("environment reflection, MatSpecular " + gp.value);
                        }
                        if (gp.key == "DecalTexCoord" && gp.value == "2") features.push_back("decal on UV set 2");
                    }
                    if (effect.find("diffmapdiffmap") != std::string::npos) features.push_back("modifier blend (second diffuse by vertex alpha)");
                    if (effect.rfind("trhair", 0) == 0) features.push_back("hair cut-out");
                    if (palDecal)
                        for (const tr::GeoParameter& gp : piece.parameters)
                            if (gp.key == "DecalTexture" && !gp.value.empty()) features.push_back("Pal decal " + gp.value);
                    for (const std::string& f : features) {
                        auto& slot = found[f.substr(0, f.find(',') == std::string::npos ? f.size() : f.size()) + " | " + mesh + " | " + piece.effectName];
                        if (slot.first++ == 0) slot.second = e.location;
                    }
                }
            }
            for (const auto& f : found)
                std::printf("feature %s | %zu entities, e.g. at %.1f %.1f %.1f\n", f.first.c_str(), f.second.first, f.second.second.x, f.second.second.y,
                            f.second.second.z);
        }
        // RASA_STATICBONESCAN=1: models with static pieces whose bone's world transform (the bind chain) differs from its local
        // transform by more than 1 cm at the piece's centre - the pieces whose placement changed when static pieces moved to
        // the bone world (2026-10-14).
        if (envValue("RASA_STATICBONESCAN")) {
            size_t models = 0, pieces = 0;
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                tr::GeoModel geo;
                try { geo = tr::readGeo(assets.read(name)); } catch (const std::exception&) { continue; }
                auto local = [](const tr::GeoBone& bone) {
                    Mat4 m = rotationMatrix(bone.rotation);
                    const tr::Vec3 bs = bone.scale;
                    m.row0 = {m.row0.x * bs.x, m.row0.y * bs.y, m.row0.z * bs.z, bone.translation.x};
                    m.row1 = {m.row1.x * bs.x, m.row1.y * bs.y, m.row1.z * bs.z, bone.translation.y};
                    m.row2 = {m.row2.x * bs.x, m.row2.y * bs.y, m.row2.z * bs.z, bone.translation.z};
                    return m;
                };
                std::vector<Mat4> world(geo.bones.size());
                for (size_t b = 0; b < geo.bones.size(); ++b) {
                    const int p = geo.bones[b].parent;
                    world[b] = p >= 0 && size_t(p) < b ? multiply(world[size_t(p)], local(geo.bones[b])) : local(geo.bones[b]);
                }
                size_t changed = 0;
                for (const tr::GeoPiece& piece : geo.pieces) {
                    if (piece.skinned || piece.boneName.empty()) continue;
                    for (size_t b = 0; b < geo.bones.size(); ++b) {
                        if (geo.bones[b].name != piece.boneName) continue;
                        tr::Vec3 c{0, 0, 0};
                        for (const tr::GeoVertex& v : piece.vertices) c = tr::Vec3{c.x + v.position.x, c.y + v.position.y, c.z + v.position.z};
                        const float n = piece.vertices.empty() ? 1.0f : float(piece.vertices.size());
                        c = tr::Vec3{c.x / n, c.y / n, c.z / n};
                        const tr::Vec3 a = transformPoint(world[b], c), l = transformPoint(local(geo.bones[b]), c);
                        const float dx = a.x - l.x, dy = a.y - l.y, dz = a.z - l.z;
                        if (dx * dx + dy * dy + dz * dz > 1e-4f) ++changed;
                        break;
                    }
                }
                if (changed) {
                    ++models;
                    pieces += changed;
                    std::printf("static bone scan: %s: %zu piece(s) move\n", name.c_str(), changed);
                }
            }
            std::printf("static bone scan: %zu models, %zu pieces\n", models, pieces);
        }
        // RASA_PARAMSCAN=<effect prefix>: value counts of every material parameter of pieces whose effect starts with the
        // prefix (lower case, e.g. "trdiff"), over every model; texture names are summarised as "<texture>".
        if (const char* prefix = envValue("RASA_PARAMSCAN")) {
            std::map<std::string, std::map<std::string, size_t>> counts;     // "effect | key" -> value -> pieces
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                tr::GeoModel geo;
                try {
                    geo = tr::readGeo(assets.read(name));
                } catch (const std::exception&) {
                    continue;
                }
                for (const tr::GeoPiece& piece : geo.pieces) {
                    const std::string effect = tr::toLowerAscii(piece.effectName);
                    if (effect.rfind(prefix, 0) != 0) continue;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        const bool isTexture = gp.key.size() > 7 && gp.key.compare(gp.key.size() - 7, 7, "Texture") == 0;
                        ++counts[effect + " | " + gp.key][isTexture ? (gp.value.empty() ? "<none>" : "<texture>") : gp.value];
                    }
                }
            }
            for (const auto& c : counts) {
                std::vector<std::pair<size_t, std::string>> values;
                for (const auto& v : c.second) values.emplace_back(v.second, v.first);
                std::sort(values.rbegin(), values.rend());
                std::printf("%s:", c.first.c_str());
                for (size_t k = 0; k < values.size() && k < 6; ++k) std::printf("  [%zu] %s", values[k].first, values[k].second.c_str());
                if (values.size() > 6) std::printf("  (+%zu more)", values.size() - 6);
                std::printf("\n");
            }
        }
        // RASA_LISTPARTICLES=1: the map entities whose models carry particle systems, with their positions.
        if (envValue("RASA_LISTPARTICLES")) {
            std::map<std::string, std::vector<tr::ParticleAttachment>> byMesh;
            for (const tr::MapEntity& e : map.map.entities) {
                const std::string mesh = data.classMeshName(e.classId);
                if (mesh.empty() || byMesh.count(mesh)) continue;
                try {
                    byMesh[mesh] = tr::particleAttachments(tr::readGeo(assets.read(mesh)));
                } catch (const std::exception&) {
                    byMesh[mesh] = {};
                }
            }
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                const tr::MapEntity& e = map.map.entities[k];
                const auto found = byMesh.find(data.classMeshName(e.classId));
                if (found == byMesh.end()) continue;
                for (const tr::ParticleAttachment& a : found->second)
                    std::printf("particles: entity %zu %s at %.1f %.1f %.1f: cp %u %s\n", k, found->first.c_str(), e.location.x, e.location.y,
                                e.location.z, a.contactPoint, a.file.c_str());
            }
        }
        // Optional server content: set RASA_DB to a Rasa.NET World database.
        std::unique_ptr<WorldDatabase> database;
        WorldContent content;
        if (const char* dbPath = envValue("RASA_DB")) {
            const char* base = SDL_GetBasePath();
            database = std::make_unique<WorldDatabase>(dbPath, defaultMySqlPluginDir(base ? base : ""));
            content = database->loadMap(database->contextIdForMap(map.name));
            scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                   [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            // The client's map-screen markers, as the Server workspace sets them (drawn with the server layer).
            try {
                const int32_t templateId = int32_t(tr::py::asInt(
                    tr::py::item(tr::py::dictGet(data.global("generated.client.gamecontext", "lookup"), content.mapContextId), 3), -1));
                std::vector<SceneView::ClientMapMarker> drawn;
                for (const tr::MapScreenMarker& m : data.factionedMapMarkers(templateId))
                    drawn.push_back(SceneView::ClientMapMarker{m.entityId, m.type, tr::Vec3{m.x, m.y, m.z}});
                std::printf("client map-screen markers: %zu\n", drawn.size());
                scene.setClientMapMarkers(std::move(drawn));
            } catch (const std::exception& e) {
                std::printf("client map-screen markers: %s\n", e.what());
            }
            // RASA_SAVETEST=1 (use a COPY of the database): edit the arrival points - move the first, add one to its pool, delete
            // the last - save, reload and check the rows; then restore and save again.
            if (envValue("RASA_SAVETEST") && content.arrivals.size() >= 2) {
                const WorldContent original = content;
                WorldContent edited = content;
                edited.arrivals.front().position.x += 1.5f;
                edited.arrivals.front().comment += " (moved)";
                SpawnArrival added = edited.arrivals.front();
                added.id = 0;
                added.kind = 2;
                added.entityId = 1234567;
                added.comment = "save test";
                edited.arrivals.push_back(added);
                const int64_t deletedId = edited.arrivals[edited.arrivals.size() - 2].id;
                edited.arrivals.erase(edited.arrivals.end() - 2);
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                bool moved = false, addedFound = false, deletedGone = true;
                for (const SpawnArrival& r : reloaded.arrivals) {
                    if (r.id == original.arrivals.front().id) moved = std::fabs(r.position.x - edited.arrivals.front().position.x) < 1e-3f &&
                                                                    r.comment == edited.arrivals.front().comment;
                    if (r.comment == "save test" && r.kind == 2 && r.entityId == 1234567 && r.poolId == added.poolId) addedFound = true;
                    if (r.id == deletedId) deletedGone = false;
                }
                std::printf("save test: saved %d (%d added, %d changed, %d deleted%s%s), moved %d, added %d, deleted %d, %zu -> %zu arrivals\n", saved,
                            summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(), moved, addedFound,
                            deletedGone, original.arrivals.size(), reloaded.arrivals.size());
                // Undo: delete the added row, restore the moved and deleted ones.
                WorldContent restore = reloaded;
                restore.arrivals.erase(std::remove_if(restore.arrivals.begin(), restore.arrivals.end(),
                                                      [](const SpawnArrival& r) { return r.comment == "save test"; }),
                                       restore.arrivals.end());
                for (SpawnArrival& r : restore.arrivals)
                    if (r.id == original.arrivals.front().id) r = original.arrivals.front();
                for (const SpawnArrival& r : original.arrivals)
                    if (r.id == deletedId) { SpawnArrival back = r; back.id = 0; restore.arrivals.push_back(back); }
                const bool restored = database->save(reloaded, restore, summary, error);
                std::printf("save test restore: %d (%d added, %d changed, %d deleted)\n", restored, summary.inserted, summary.updated, summary.deleted);
                content = database->loadMap(content.mapContextId);
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST with patrols / poses (Rasa.NET upstream tables; Bootcamp pool 400009): insert a step after the first
            // (the later steps renumber up), move the first, drop the facing of the last, change the first pose and delete
            // another; save, reload, compare the patrol and pose lists exactly, then restore the original rows.
            if (envValue("RASA_SAVETEST") && !content.patrolSteps.empty()) {
                const WorldContent original = content;
                WorldContent edited = content;
                const int64_t pool = edited.patrolSteps.front().poolId;
                const int32_t first = edited.patrolSteps.front().step;
                for (PatrolStep& s : edited.patrolSteps)
                    if (s.poolId == pool && s.step > first) ++s.step;
                PatrolStep inserted = edited.patrolSteps.front();
                inserted.step = first + 1;
                inserted.position.z += 2.0f;
                inserted.hasFacing = false;
                inserted.pauseMs = 1234;
                edited.patrolSteps.insert(edited.patrolSteps.begin() + 1, inserted);
                edited.patrolSteps.front().position.x += 0.75f;
                edited.patrolSteps.back().hasFacing = false;
                if (!edited.poses.empty()) edited.poses.front().pose = 6;
                if (edited.poses.size() >= 2) edited.poses.pop_back();
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                auto samePatrols = [](const std::vector<PatrolStep>& a, const std::vector<PatrolStep>& b) {
                    if (a.size() != b.size()) return false;
                    for (size_t k = 0; k < a.size(); ++k)
                        if (a[k].poolId != b[k].poolId || a[k].step != b[k].step || std::fabs(a[k].position.x - b[k].position.x) > 1e-3f ||
                            std::fabs(a[k].position.z - b[k].position.z) > 1e-3f || a[k].hasFacing != b[k].hasFacing ||
                            (a[k].hasFacing && std::fabs(a[k].facing - b[k].facing) > 1e-4f) || a[k].pauseMs != b[k].pauseMs)
                            return false;
                    return true;
                };
                auto samePoses = [](const std::vector<PoolPose>& a, const std::vector<PoolPose>& b) {
                    if (a.size() != b.size()) return false;
                    for (size_t k = 0; k < a.size(); ++k)
                        if (a[k].poolId != b[k].poolId || a[k].pose != b[k].pose) return false;
                    return true;
                };
                std::printf("patrol save test: saved %d (%d added, %d changed, %d deleted%s%s), patrol %s (%zu -> %zu steps), poses %s (%zu -> %zu)\n",
                            saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                            samePatrols(edited.patrolSteps, reloaded.patrolSteps) ? "matches" : "DIFFERS", original.patrolSteps.size(),
                            reloaded.patrolSteps.size(), samePoses(edited.poses, reloaded.poses) ? "match" : "DIFFER", original.poses.size(),
                            reloaded.poses.size());
                const bool restored = database->save(reloaded, original, summary, error);
                const WorldContent back = database->loadMap(content.mapContextId);
                std::printf("patrol save test restore: %d (%d added, %d changed, %d deleted), %s\n", restored, summary.inserted, summary.updated,
                            summary.deleted, samePatrols(original.patrolSteps, back.patrolSteps) && samePoses(original.poses, back.poses) ? "original back" : "NOT RESTORED");
                content = back;
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST with the upstream object tables: move the first ambient NPC, add a world usable and a world destructible
            // with every column set, change the first wormhole lock and add one; save, reload and compare, then restore.
            if (envValue("RASA_SAVETEST") && database->hasTable("world_usable") && database->hasTable("wormhole_lock")) {
                const WorldContent original = content;
                WorldContent edited = content;
                for (PlacedObject& o : edited.objects)
                    if (o.kind == ObjectKind::AmbientNpc) { o.position.x += 0.5f; o.rotation += 0.25f; o.name += " (moved)"; break; }
                PlacedObject usable;
                usable.kind = ObjectKind::WorldUsable;
                usable.classId = 3895;
                usable.position = tr::Vec3{1, 2, 3};
                usable.rotation = 0.5f;
                usable.name = "save test usable";
                usable.usable = UsableSettings{1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 2.5f, 3};
                edited.objects.push_back(usable);
                PlacedObject destructible;
                destructible.kind = ObjectKind::WorldDestructible;
                destructible.classId = 3896;
                destructible.position = tr::Vec3{4, 5, 6};
                destructible.name = "save test destructible";
                destructible.destructible = DestructibleSettings{100, 22, 3, 1};
                edited.objects.push_back(destructible);
                const bool emitters = database->hasTable("map_emitter");
                if (emitters) {
                    PlacedObject emitter;
                    emitter.kind = ObjectKind::Emitter;
                    emitter.classId = kEmitterClassId;
                    emitter.position = tr::Vec3{7, 8, 9};
                    emitter.rotation = 1.25f;
                    emitter.name = "save test emitter";
                    emitter.emitter = EmitterSettings{4242, false};
                    edited.objects.push_back(emitter);
                }
                if (!edited.wormholeLocks.empty()) edited.wormholeLocks.front().value += 1;
                WormholeLock lock;
                lock.fromTeleporterId = 0;
                lock.toTeleporterId = 4242;
                lock.kind = 2;
                lock.value = 30;
                edited.wormholeLocks.push_back(lock);           // comment NULL
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                bool ambientOk = true, usableOk = false, destructibleOk = false, lockOk = false, lockEdited = edited.wormholeLocks.size() < 2;
                bool emitterOk = !emitters;
                for (const PlacedObject& o : edited.objects)
                    if (o.kind == ObjectKind::AmbientNpc && o.id != 0) {
                        bool found = false;
                        for (const PlacedObject& r : reloaded.objects)
                            if (r.kind == o.kind && r.id == o.id)
                                found = std::fabs(r.position.x - o.position.x) < 1e-3f && std::fabs(r.rotation - o.rotation) < 1e-4f && r.name == o.name;
                        ambientOk = ambientOk && found;
                    }
                for (const PlacedObject& r : reloaded.objects) {
                    if (r.kind == ObjectKind::WorldUsable && r.name == "save test usable") {
                        const UsableSettings& u = r.usable;
                        usableOk = r.classId == 3895 && u.state == 1 && u.usable == 1 && u.itemTemplateId == 2 && u.itemClassId == 3 && u.itemCount == 4 &&
                                   u.resetSeconds == 5 && u.intervalMinSeconds == 6 && u.intervalMaxSeconds == 7 && u.damageMin == 8 && u.damageMax == 9 &&
                                   std::fabs(u.radius - 2.5f) < 1e-4f && u.actionArg == 3;
                    }
                    if (r.kind == ObjectKind::Emitter && r.name == "save test emitter")
                        emitterOk = r.classId == kEmitterClassId && r.emitter.packageId == 4242 && !r.emitter.isOn && std::fabs(r.rotation - 1.25f) < 1e-4f &&
                                    std::fabs(r.position.z - 9.0f) < 1e-4f;
                    if (r.kind == ObjectKind::WorldDestructible && r.name == "save test destructible")
                        destructibleOk = r.classId == 3896 && r.destructible.hitPoints == 100 && r.destructible.creatureId == 22 &&
                                         r.destructible.creatureCount == 3 && r.destructible.side == 1;
                }
                for (const WormholeLock& r : reloaded.wormholeLocks) {
                    if (r.toTeleporterId == 4242 && r.kind == 2 && r.value == 30 && !r.hasComment) lockOk = true;
                    if (!edited.wormholeLocks.empty() && r.id == edited.wormholeLocks.front().id) lockEdited = r.value == edited.wormholeLocks.front().value;
                }
                std::printf("object save test: saved %d (%d added, %d changed, %d deleted%s%s), ambient %s, usable %s, destructible %s, lock added %s, "
                            "lock changed %s, emitter %s\n", saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                            ambientOk ? "ok" : "FAILED", usableOk ? "ok" : "FAILED", destructibleOk ? "ok" : "FAILED", lockOk ? "ok" : "FAILED",
                            lockEdited ? "ok" : "FAILED", emitterOk ? "ok" : "FAILED");
                // Restore: the original lists (new rows deleted by id, edited rows written back).
                const bool restored = database->save(reloaded, original, summary, error);
                const WorldContent back = database->loadMap(content.mapContextId);
                std::printf("object save test restore: %d (%d added, %d changed, %d deleted), %zu objects / %zu locks (originally %zu / %zu)\n", restored,
                            summary.inserted, summary.updated, summary.deleted, back.objects.size(), back.wormholeLocks.size(), original.objects.size(),
                            original.wormholeLocks.size());
                content = back;
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_MARKERS=1: the client's map-screen markers of this map (uimapmarker.factionedmarkers) and their map_marker bindings, with
            // the distance from the marker to the bound object recomputed against the stored match_distance.
            if (envValue("RASA_MARKERS")) {
                const int32_t mapTemplateId = int32_t(tr::py::asInt(
                    tr::py::item(tr::py::dictGet(data.global("generated.client.gamecontext", "lookup"), content.mapContextId), 3), -1));   // gamecontext [3]
                const std::vector<tr::MapScreenMarker> markers = data.factionedMapMarkers(mapTemplateId);
                size_t bound = 0, agree = 0;
                for (const tr::MapScreenMarker& m : markers) {
                    std::string binding = "unbound";
                    for (const MapMarkerBinding& b : content.mapMarkers) {
                        if (b.markerEntityId != m.entityId) continue;
                        ++bound;
                        tr::Vec3 p{};
                        bool found = false;
                        if (b.objectKind == 1)
                            for (const Teleporter& t : content.teleporters)
                                if (t.id == b.objectId) p = t.position, found = true;
                        if (b.objectKind == 2)
                            for (const PlacedObject& o : content.objects)
                                if (o.kind == ObjectKind::Kraftwerk && o.id == b.objectId) p = o.position, found = true;
                        const double d = std::sqrt(double(p.x - m.x) * (p.x - m.x) + double(p.y - m.y) * (p.y - m.y) + double(p.z - m.z) * (p.z - m.z));
                        if (found && std::fabs(d - b.matchDistance) < 0.02) ++agree;
                        char line[160];
                        std::snprintf(line, sizeof line, "kind %d id %lld, stored %.2f m, recomputed %.2f m", b.objectKind, static_cast<long long>(b.objectId),
                                      b.matchDistance, found ? d : -1.0);
                        binding = line;
                    }
                    std::printf("marker %lld type %d %s (%.1f %.1f %.1f): %s\n", static_cast<long long>(m.entityId), m.type, m.name.c_str(), m.x, m.y, m.z,
                                binding.c_str());
                }
                std::printf("markers: %zu client, %zu bound, %zu bindings in map_marker, %zu distances agree\n", markers.size(), bound,
                            content.mapMarkers.size(), agree);
            }
            // RASA_SAVETEST with map markers (maps with bindings): unbind the first, rebind the second to another object with a new
            // distance and comment, save, reload, compare, restore.
            if (envValue("RASA_SAVETEST") && database->hasTable("map_marker") && content.mapMarkers.size() >= 2) {
                const WorldContent original = content;
                WorldContent edited = content;
                const MapMarkerBinding dropped = edited.mapMarkers.front();
                edited.mapMarkers.erase(edited.mapMarkers.begin());
                MapMarkerBinding& changed = edited.mapMarkers.front();
                changed.objectId = 4242;
                changed.matchDistance = 7.25;
                changed.hasComment = true;
                changed.comment = "save test marker";
                const MapMarkerBinding expect = changed;
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                bool ok = reloaded.mapMarkers.size() == edited.mapMarkers.size();
                for (const MapMarkerBinding& m : reloaded.mapMarkers) {
                    if (m.markerEntityId == dropped.markerEntityId) ok = false;
                    if (m.markerEntityId == expect.markerEntityId)
                        ok = ok && m.objectId == 4242 && std::fabs(m.matchDistance - 7.25) < 1e-9 && m.hasComment && m.comment == "save test marker";
                }
                const bool restored = database->save(reloaded, original, summary, error);
                const WorldContent back = database->loadMap(content.mapContextId);
                std::printf("map marker save test: saved %d%s%s, %s; restored %d, %zu bindings (originally %zu)\n", saved, error.empty() ? "" : ": ",
                            error.c_str(), ok ? "bindings match" : "FAILED", restored, back.mapMarkers.size(), original.mapMarkers.size());
                content = back;
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST with control point links (maps with control points): drop the first link, add a boss link, save, reload,
            // compare as sets, then restore.
            if (envValue("RASA_SAVETEST") && database->hasTable("control_point_link") && !content.controlPointLinks.empty()) {
                const WorldContent original = content;
                WorldContent edited = content;
                const ControlPointLink dropped = edited.controlPointLinks.front();
                edited.controlPointLinks.erase(edited.controlPointLinks.begin());
                const ControlPointLink added{dropped.controlPointId, 5, 999001};
                edited.controlPointLinks.push_back(added);
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                const auto has = [](const std::vector<ControlPointLink>& list, const ControlPointLink& l) {
                    for (const ControlPointLink& x : list)
                        if (x.controlPointId == l.controlPointId && x.kind == l.kind && x.objectId == l.objectId) return true;
                    return false;
                };
                const bool ok = reloaded.controlPointLinks.size() == edited.controlPointLinks.size() && has(reloaded.controlPointLinks, added) &&
                                !has(reloaded.controlPointLinks, dropped);
                const bool restored = database->save(reloaded, original, summary, error);
                const WorldContent back = database->loadMap(content.mapContextId);
                std::printf("control point link save test: saved %d%s%s, %s; restored %d, %zu links (originally %zu)\n", saved, error.empty() ? "" : ": ",
                            error.c_str(), ok ? "links match" : "FAILED", restored, back.controlPointLinks.size(), original.controlPointLinks.size());
                content = back;
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST with control points (worlds with marker_entity_id / default_owner): flip the first point's owner and add a
            // point (id = largest + 1), save, reload; link the new point, save; then restore, which deletes the new point and its link.
            const auto firstPoint = std::find_if(content.objects.begin(), content.objects.end(), [](const PlacedObject& o) { return o.kind == ObjectKind::ControlPoint; });
            if (envValue("RASA_SAVETEST") && database->hasColumn("control_point", "marker_entity_id") && database->hasColumn("control_point", "default_owner") &&
                firstPoint != content.objects.end()) {
                const WorldContent original = content;
                const auto largestId = [&] {
                    const DbTable t = database->loadTable("control_point");
                    int64_t m = 0;
                    for (const auto& r : t.rows) m = std::max<int64_t>(m, r[size_t(t.column("id"))].asInt());
                    return m;
                };
                const int64_t before = largestId();
                WorldContent edited = content;
                PlacedObject& first = *std::find_if(edited.objects.begin(), edited.objects.end(), [](const PlacedObject& o) { return o.kind == ObjectKind::ControlPoint; });
                first.controlPoint.defaultOwner = first.controlPoint.defaultOwner == 1 ? 0 : 1;
                PlacedObject added = first;
                added.id = 0;
                added.name = "save test point";
                added.position.x += 3.0f;
                added.controlPoint.markerEntityId = 0;
                edited.objects.push_back(added);
                WorldDatabase::SaveSummary summary;
                std::string error;
                bool ok = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                const PlacedObject* newPoint = nullptr;
                const PlacedObject* flipped = nullptr;
                for (const PlacedObject& o : reloaded.objects) {
                    if (o.kind == ObjectKind::ControlPoint && o.name == "save test point") newPoint = &o;
                    if (o.kind == ObjectKind::ControlPoint && o.id == first.id) flipped = &o;
                }
                ok = ok && newPoint && newPoint->id == before + 1 && newPoint->classId == added.classId && newPoint->controlPoint.markerEntityId == 0 &&
                     newPoint->controlPoint.defaultOwner == added.controlPoint.defaultOwner && flipped &&
                     flipped->controlPoint.defaultOwner == first.controlPoint.defaultOwner;
                const int64_t newId = newPoint ? newPoint->id : 0;
                WorldContent linked = reloaded;
                if (newId) linked.controlPointLinks.push_back(ControlPointLink{newId, 5, 999002});
                ok = database->save(reloaded, linked, summary, error) && ok;
                const WorldContent withLink = database->loadMap(content.mapContextId);
                bool linkSaved = false;
                for (const ControlPointLink& l : withLink.controlPointLinks) linkSaved = linkSaved || (l.controlPointId == newId && l.objectId == 999002);
                const bool restored = database->save(withLink, original, summary, error);
                const DbTable links = database->loadTable("control_point_link");
                bool linkGone = true;
                for (const auto& r : links.rows) linkGone = linkGone && r[size_t(links.column("control_point_id"))].asInt() != newId;
                const bool pointGone = largestId() == before;
                const WorldContent back = database->loadMap(content.mapContextId);
                const PlacedObject* firstBack = nullptr;
                for (const PlacedObject& o : back.objects)
                    if (o.kind == ObjectKind::ControlPoint && o.id == first.id) firstBack = &o;
                const bool ownerBack = firstBack && firstBack->controlPoint.defaultOwner == firstPoint->controlPoint.defaultOwner;
                std::printf("control point save test: %s (new id %lld after %lld, owner flipped and read back, link %s); restored %d: point %s, its links %s, "
                            "owner %s%s%s\n",
                            ok ? "saved" : "FAILED", static_cast<long long>(newId), static_cast<long long>(before), linkSaved ? "saved" : "NOT saved", restored,
                            pointGone ? "deleted" : "STILL THERE", linkGone ? "deleted" : "STILL THERE", ownerBack ? "back" : "NOT back",
                            error.empty() ? "" : ": ", error.c_str());
                content = back;
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST also round-trips the creature definitions: change the first creature with stats and appearance (level,
            // stat row removed, a slot recoloured), add a new creature, save, reload and check (the copy keeps the changes).
            if (envValue("RASA_SAVETEST")) {
                const std::vector<CreatureDefinition> before = database->creatureDefinitions();
                std::vector<CreatureDefinition> edited = before;
                CreatureDefinition* target = nullptr;
                for (CreatureDefinition& d : edited)
                    if (d.hasStat && !d.appearance.empty()) { target = &d; break; }
                if (target) {
                    const int32_t id = target->creature.id;
                    target->creature.level += 1;
                    target->creature.action8 = database->creatureActions().empty() ? 0 : database->creatureActions().front().id;
                    target->hasStat = false;
                    target->appearance.front().colour = 0xff112233;
                    const int32_t expectLevel = target->creature.level, expectAction8 = target->creature.action8;
                    CreatureDefinition added = *target;
                    added.creature.id = edited.back().creature.id + 1;
                    added.creature.comment = "save test creature";
                    added.hasStat = true;
                    added.stat = CreatureStat{1, 2, 3, 4, 5};
                    edited.push_back(added);
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveCreatures(before, edited, summary, error);
                    const Creature* c = database->creature(id);
                    const Creature* n = database->creature(added.creature.id);
                    const CreatureStat* ns = database->creatureStat(added.creature.id);
                    const bool ok = c && c->level == expectLevel && c->action8 == expectAction8 && !database->creatureStat(id) &&
                                    !database->appearance(id).empty() && database->appearance(id).front().colour == 0xff112233u && n &&
                                    n->comment == "save test creature" && ns && ns->armor == 5 &&
                                    database->appearance(added.creature.id).size() == added.appearance.size();
                    std::printf("creature save test: saved %d (%d added, %d changed, %d deleted%s%s), checks %s\n", saved, summary.inserted,
                                summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(), ok ? "passed" : "FAILED");
                }
                // Generic tables (missions): edit, delete and add rows of two tables, save, reload and compare row for row.
                {
                    const DbTable objectives = database->loadTable("mission_objective_definition");
                    const DbTable triggers = database->loadTable("mission_trigger");
                    DbTable objectives2 = objectives, triggers2 = triggers;
                    const int comment = objectives2.column("comment"), objectiveId = objectives2.column("objective_id");
                    objectives2.rows.front()[size_t(comment)] = DbValue::text("generic save test");
                    std::vector<DbValue> added = objectives2.rows.front();
                    added[size_t(objectiveId)] = DbValue::integer(9999);
                    objectives2.rows.push_back(added);
                    triggers2.rows.erase(triggers2.rows.begin());
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveTables({{&objectives, &objectives2}, {&triggers, &triggers2}}, summary, error);
                    const DbTable objectives3 = database->loadTable("mission_objective_definition");
                    const DbTable triggers3 = database->loadTable("mission_trigger");
                    auto sameRows = [](const DbTable& a, const DbTable& b) {
                        if (a.rows.size() != b.rows.size()) return false;
                        for (const auto& row : a.rows)
                            if (std::find(b.rows.begin(), b.rows.end(), row) == b.rows.end()) return false;
                        return true;
                    };
                    std::printf("generic table save test: saved %d (%d added, %d changed, %d deleted%s%s), objectives %s, triggers %s; key %zu columns\n",
                                saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                                sameRows(objectives2, objectives3) ? "match" : "DIFFER", sameRows(triggers2, triggers3) ? "match" : "DIFFER",
                                objectives.key.size());
                    // Restore (the test can then run again on the same database - a MySQL server is not a fresh copy each time).
                    if (saved) {
                        const bool restored = database->saveTables({{&objectives3, &objectives}, {&triggers3, &triggers}}, summary, error);
                        std::printf("generic table save test restore: %d, %s\n", restored,
                                    sameRows(objectives, database->loadTable("mission_objective_definition")) &&
                                            sameRows(triggers, database->loadTable("mission_trigger"))
                                        ? "original back"
                                        : "FAILED");
                    }
                }
                // Foreign keys (PRAGMA foreign_keys = ON in WorldDatabase): deleting a mission that still has a mission_area row
                // (ON DELETE RESTRICT) must fail and leave the mission in place.
                {
                    const DbTable missions = database->loadTable("mission_content_definition");
                    const DbTable areas = database->loadTable("mission_area");
                    if (!areas.rows.empty()) {
                        const int64_t mission = areas.rows.front()[size_t(areas.column("mission_id"))].asInt();
                        const std::string revision = areas.rows.front()[size_t(areas.column("content_revision"))].t;
                        DbTable missions2 = missions;
                        const int idColumn = missions2.column("mission_id"), revisionColumn = missions2.column("content_revision");
                        missions2.rows.erase(std::remove_if(missions2.rows.begin(), missions2.rows.end(),
                                                            [&](const std::vector<DbValue>& r) {
                                                                return r[size_t(idColumn)].asInt() == mission && r[size_t(revisionColumn)].t == revision;
                                                            }),
                                             missions2.rows.end());
                        WorldDatabase::SaveSummary summary;
                        std::string error;
                        const bool saved = database->saveTables({{&missions, &missions2}}, summary, error);
                        const DbTable missions3 = database->loadTable("mission_content_definition");
                        std::printf("foreign key test: deleting mission %lld/%s with an area %s (%s); missions %zu -> %zu\n", static_cast<long long>(mission),
                                    revision.c_str(), saved ? "SAVED (not enforced)" : "refused", error.c_str(), missions.rows.size(), missions3.rows.size());
                    }
                }
                // NPC / vendor rows: a vendor gets a flat price, loses its first stock item and gains one, a greeting and a package.
                std::vector<CreatureDefinition> before2 = database->creatureDefinitions();
                std::vector<CreatureDefinition> edited2 = before2;
                for (CreatureDefinition& d : edited2) {
                    if (!d.hasVendor || d.vendorItems.size() < 2 || d.hasVendorPrice) continue;
                    const int32_t id = d.creature.id;
                    std::vector<int32_t> stock(d.vendorItems.begin() + 1, d.vendorItems.end());
                    stock.push_back(13066);
                    d.vendorItems = stock;
                    d.hasVendorPrice = true;
                    d.vendorPrice = 77;
                    d.hasGreeting = true;
                    d.greetingId = 120;
                    d.greetingImportant = database->hasColumn("npc_greeting", "important");
                    d.hasNpcPackage = true;
                    d.npcPackageId = 113;
                    d.npcPackageComment = "save test package";
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveCreatures(before2, edited2, summary, error);
                    bool ok = false;
                    for (const CreatureDefinition& r : database->creatureDefinitions())
                        if (r.creature.id == id)
                            ok = r.hasVendorPrice && r.vendorPrice == 77 && r.vendorItems == stock && r.hasGreeting && r.greetingId == 120 &&
                                 r.greetingImportant == database->hasColumn("npc_greeting", "important") &&
                                 r.hasNpcPackage && r.npcPackageId == 113 && r.npcPackageComment == "save test package";
                    std::printf("vendor save test (creature %d): saved %d (%d added, %d changed, %d deleted%s%s), checks %s; greeting 120 = \"%s\"\n", id,
                                saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                                ok ? "passed" : "FAILED", data.npcGreeting(120).c_str());
                    break;
                }
            }
            // Avatar assembly of the first few spawned creatures that wear swap-set items.
            int listed = 0;
            for (const SpawnPool& pool : content.spawnPools) {
                const Creature* c = database->creature(pool.slot1.creatureId);
                if (!c || listed >= 4) continue;
                const AvatarLook look = resolveAvatar(data, *database, *c);
                if (look.parts.empty()) continue;
                ++listed;
                std::printf("avatar %d %s: base %s, skin %08x, pose %s\n", c->id, c->comment.c_str(), look.baseMesh.c_str(), look.skinColour,
                            look.poseAnimation.empty() ? "(bind)" : look.poseAnimation.c_str());
                for (const AvatarPart& part : look.parts)
                    std::printf("    slot %d -> swap set %u (%s) from %s, colour %08x\n", part.slotId, part.swapsetId,
                                data.swapsetName(int32_t(part.swapsetId)).c_str(), part.mesh.c_str(), part.colour);
                for (const AvatarAccessory& a : look.accessories)
                    std::printf("    accessory %s on connection point %d (%s)\n", a.mesh.c_str(), a.connectionPoint,
                                tr::connectionPointIdName(uint32_t(a.connectionPoint)).c_str());
            }
            std::printf("server content: %zu spawn pools, %zu teleporters, %zu objects, %zu links\n", content.spawnPools.size(),
                        content.teleporters.size(), content.objects.size(), content.mapLinks.size());
        }
        scene.loadPending(1e9);
        {
            size_t layer6 = 0;
            for (size_t k = 0; k < map.map.entities.size(); ++k) layer6 += scene.entityCullingLayer(k) == 6 ? 1 : 0;
            std::printf("entities on culling layer 6 (never drawn): %zu\n", layer6);
        }
        std::printf("models: %zu loaded, %zu failed, %zu total; textures %zu\n", scene.modelsLoaded(), scene.modelsFailed(), scene.modelsTotal(), scene.texturesLoaded());
        ViewCamera& cam = scene.camera();
        cam.position = tr::Vec3{float(std::atof(argv[7])), float(std::atof(argv[8])), float(std::atof(argv[9]))};
        const tr::Vec3 to = subtract(tr::Vec3{float(std::atof(argv[10])), float(std::atof(argv[11])), float(std::atof(argv[12]))}, cam.position);
        cam.yaw = std::atan2(to.x, to.z);
        cam.pitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
        if (const char* fov = envValue("RASA_FOV")) cam.fovY = float(std::atof(fov)) * 3.14159265f / 180.0f;   // vertical, degrees
        if (argc > 13) scene.showPlaceholders = false;
        if (!scene.skies().empty()) {
            const SceneLighting active = scene.lighting();
            std::printf("sky at the camera: %s%s; hemi top %.2f %.2f %.2f, sun %.2f %.2f %.2f\n", scene.skies()[std::min(scene.activeSkyIndex(), scene.skies().size() - 1)].name.c_str(),
                        scene.skyFromRegion() ? " (by region)" : "", active.hemiTop.x, active.hemiTop.y, active.hemiTop.z, active.sunFront.x, active.sunFront.y, active.sunFront.z);
        }
        // RASA_LISTLIGHTS=1: the placed lights chosen for the frame (position, range, colour x intensity, kind from localSpot.w).
        if (envValue("RASA_LISTLIGHTS")) {
            const SceneLighting active = scene.lighting();
            for (int i = 0; i < active.localCount; ++i)
                std::printf("light %d at %.1f %.1f %.1f range %.1f colour %.3f %.3f %.3f fade from %.1f kind %s globe %.0f spot %.3f %.3f %.3f %.4f inner %.4f\n", i, active.localPosition[i].x,
                            active.localPosition[i].y, active.localPosition[i].z, active.localPosition[i].w, active.localColour[i].x, active.localColour[i].y,
                            active.localColour[i].z, active.localColour[i].w, active.localSpot[i].w == -2.0f ? "point" : active.localSpot[i].w == -3.0f ? "plane" : "spot",
                            active.localSpotInner[i].y, active.localSpot[i].x, active.localSpot[i].y, active.localSpot[i].z, active.localSpot[i].w, active.localSpotInner[i].x);
            for (int i = 0; i < active.localCount; ++i) {          // the light's frame (localRotation): its x / y / z axes in world space
                const Vec4f& q = active.localRotation[i];
                const Mat4 r = rotationMatrix(tr::Quat{q.x, q.y, q.z, q.w});
                std::printf("light %d frame x %.4f %.4f %.4f y %.4f %.4f %.4f z %.4f %.4f %.4f\n", i, r.row0.x, r.row1.x, r.row2.x, r.row0.y, r.row1.y,
                            r.row2.y, r.row0.z, r.row1.z, r.row2.z);
            }
            for (int i = 0; i < active.localCount; ++i) {
                const Vec4f& b = active.localShadowBase[i];
                if (b.x > 0.5f)
                    std::printf("light %d shadow: %.0f view(s) from %.0f (shadow light %.0f)%s%s\n", i, b.y, b.x - 1.0f, b.z,
                                active.localSpot[i].w == -2.0f ? ", point (cube)" : active.localSpot[i].w == -3.0f ? ", plane (orthographic)" : "",
                                b.w > 0.5f ? ", not static: characters cast" : "");
                if (active.localSpot[i].w > -1.5f && active.localSpotInner[i].w > 0.5f) std::printf("light %d cookie %.0f\n", i, active.localSpotInner[i].w);
            }
            std::printf("characters in placed-light shadow maps: %d\n", scene.lastActorShadowCasters);
            std::printf("shadow maps drawn in the last frame: %d\n", scene.lastShadowMapsDrawn);
            std::printf("shadow limits: ShadowMaxSimultaneous %.2f, ShadowDistance %.2f\n", scene.shadowMaxSimultaneous, scene.shadowDistance);
            for (const SceneView::ShadowCandidateInfo& c : scene.lastShadowCandidates)
                std::printf("shadow candidate %.2f at %.1f %.1f %.1f range %.1f%s\n", c.value, c.position.x, c.position.y, c.position.z, c.range,
                            c.kept ? "" : "  (dropped)");
            std::printf("globe errors: %s\n", scene.globeErrors().c_str());
        }
        std::printf("environment cube at the camera: %s\n", scene.activeEnvironmentMap().c_str());
        for (const auto& e : scene.environmentMapErrors()) std::printf("environment cube %s failed: %s\n", e.first.c_str(), e.second.c_str());
        // Particle systems: started (with their warm-up) and run for RASA_PARTICLES seconds more (default 0.5);
        // RASA_NOPARTICLES=1 leaves them out.
        if (envValue("RASA_NOPARTICLES")) scene.showParticles = false;
        if (envValue("RASA_NOSTATEFX")) scene.showStateEffects = false;   // no use-state particles / lights of server objects
        if (envValue("RASA_NOTERRAIN")) scene.showTerrain = false;
        scene.terrainBuildsPerFrame = SIZE_MAX;      // a single frame: every chunk at its distance's detail
        if (envValue("RASA_TWOSIDED")) renderer.cullBackFaces = false;
        if (envValue("RASA_NOENTITIES")) scene.showEntities = false;
        const float particleSeconds = envValue("RASA_PARTICLES") ? float(std::atof(envValue("RASA_PARTICLES"))) : 0.5f;
        // Terrain clutter: every cell around the camera at once, models loaded before the frame (RASA_NOCLUTTER=1: none).
        if (envValue("RASA_NOCLUTTER")) scene.showClutter = false;
        if (envValue("RASA_NOGAMEDISTANCES")) scene.gameDistances = false;   // draw every entity within the editor's draw distance
        // RASA_NOSERVER=1: hide the server layer (markers, creatures) while RASA_DB still supplies the regions (sky, environment map).
        if (envValue("RASA_NOSERVER")) { scene.showServer = false; scene.showMissionAreas = false; }
        // RASA_PVSTEST=<rows>: the PVS builder (PvsBuilder.h) against the shipped .vis - every leaf scouted for camera points (a leaf the
        // game found none for has no row data), then <rows> computed rows (spread over the leaves with shipped data, or the leaves in
        // RASA_PVSLEAVES=a,b,...) compared bit for bit with the shipped ones.
        if (const char* pvsTest = envValue("RASA_PVSTEST")) {
            PvsBuilder builder(device, scene, map, map.vis);
            if (const char* th = envValue("RASA_PVSTHRESHOLD")) builder.pixelThreshold = uint32_t(std::atoi(th));
            const uint32_t leaves = builder.leafCount();
            if (!envValue("RASA_PVSNOSCOUT")) {
                size_t both = 0, noneBoth = 0, shippedOnly = 0, builtOnly = 0, cameraSum = 0;
                const auto t0 = std::chrono::steady_clock::now();
                for (uint32_t leaf = 0; leaf < leaves; ++leaf) {
                    const size_t n = builder.cameras(leaf).size();
                    cameraSum += n;
                    const bool shipped = leaf < map.vis.rows.size() && tr::pvsHasData(map.vis.rows[leaf]);
                    if (shipped && n) ++both;
                    else if (!shipped && !n) ++noneBoth;
                    else if (shipped) ++shippedOnly;
                    else ++builtOnly;
                }
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("PVS scout: %u leaves in %.1f s, %zu camera points; cameras and shipped data %zu, neither %zu, shipped data but no camera %zu, "
                            "cameras but no shipped data %zu\n", leaves, s, cameraSum, both, noneBoth, shippedOnly, builtOnly);
            }
            std::vector<uint32_t> sample;
            if (const char* list = envValue("RASA_PVSLEAVES")) {
                for (const char* q = list; *q;) {
                    sample.push_back(uint32_t(std::strtoul(q, nullptr, 10)));
                    while (*q && *q != ',') ++q;
                    if (*q == ',') ++q;
                }
            } else {
                std::vector<uint32_t> withData;
                for (uint32_t leaf = 0; leaf < leaves && leaf < map.vis.rows.size(); ++leaf)
                    if (tr::pvsHasData(map.vis.rows[leaf])) withData.push_back(leaf);
                const size_t want = std::min<size_t>(size_t(std::atoi(pvsTest)), withData.size());
                for (size_t k = 0; k < want; ++k) sample.push_back(withData[k * withData.size() / std::max<size_t>(want, 1)]);
            }
            size_t totalShipped = 0, totalBuilt = 0, totalBoth = 0;
            for (uint32_t leaf : sample) {
                const std::vector<uint32_t> row = builder.computeRow(leaf);
                size_t shipped = 0, built = 0, common = 0;
                const bool hasShipped = leaf < map.vis.rows.size() && tr::pvsHasData(map.vis.rows[leaf]);
                for (uint32_t other = 0; other < leaves; ++other) {
                    const bool s = hasShipped && tr::pvsLeafVisible(map.vis.rows[leaf], other);
                    const bool b = !row.empty() && ((row[other >> 5] >> (other & 0x1F)) & 0x1) != 0;
                    shipped += s; built += b; common += s && b;
                }
                totalShipped += shipped; totalBuilt += built; totalBoth += common;
                const int32_t node = builder.leafNode(leaf);
                const tr::Bounds& box = map.vis.nodes[size_t(node)].bounds;
                std::printf("PVS leaf %u (%.0f %.0f %.0f .. %.0f %.0f %.0f): %u views, scout %.2f s, views %.2f s; shipped %zu, built %zu, both %zu, "
                            "shipped only %zu, built only %zu\n", leaf, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z, builder.lastViews,
                            builder.lastScoutSeconds, builder.lastViewSeconds, shipped, built, common, shipped - common, built - common);
            }
            if (!sample.empty())
                std::printf("PVS rows: %zu leaves; shipped bits %zu, built %zu, both %zu - recall %.3f, precision %.3f\n", sample.size(), totalShipped, totalBuilt,
                            totalBoth, totalShipped ? double(totalBoth) / double(totalShipped) : 0.0, totalBuilt ? double(totalBoth) / double(totalBuilt) : 0.0);
            return 0;
        }
        if (const char* flora = envValue("RASA_FLORA")) scene.clutterDensity = float(std::atof(flora));   // density factor
        else {
            scene.clutterCellsPerFrame = 1000;
            scene.updateClutterCells();
            scene.loadPending(1e9);
            std::printf("terrain clutter: %zu instances around the camera\n", scene.clutterInstances());
            // RASA_LISTCLUTTER=<metres>: every clutter instance within that distance (horizontally) of the camera.
            if (const char* within = envValue("RASA_LISTCLUTTER")) {
                const float r = float(std::atof(within));
                const tr::Terrain& t = map.terrain;
                const uint32_t cellsX = tr::tribbleCellsX(t), cellsZ = tr::tribbleCellsZ(t);
                for (uint32_t cell = 0; cell < cellsX * cellsZ; ++cell) {
                    const float cx0 = t.header.bounds.min.x + float(cell % cellsX) * 32.0f * t.header.spacing;
                    const float cz0 = t.header.bounds.min.z + float(cell / cellsX) * 32.0f * t.header.spacing;
                    const tr::Vec3& c = scene.camera().position;
                    if (c.x < cx0 - r - 1 || c.x > cx0 + 33 + r || c.z < cz0 - r - 1 || c.z > cz0 + 33 + r) continue;
                    for (const tr::TribbleInstance& in : tr::placeTribbleCell(t, map.textureLayers, map.tribbleSets, cell)) {
                        const float dx = in.position.x - c.x, dz = in.position.z - c.z;
                        if (dx * dx + dz * dz > r * r) continue;
                        std::printf("clutter cell %u %s: %.2f %.2f %.2f scale %.2f set %s layer %u entry %u rank %u of %u\n", cell, map.tribbleSets[in.set].models[in.model].model.c_str(),
                                    in.position.x, in.position.y, in.position.z, in.scale, map.tribbleSets[in.set].name.c_str(), in.layer, in.model, in.rank, in.containerSize);
                    }
                }
            }
        }
        // RASA_SKYFADEFROM=x,y,z with RASA_SKYFADEAT=<s>: the camera starts at the point (outside a region), the sky is settled there,
        // then the camera goes to the frame's position and the sky clock runs for <s> seconds: the render shows the cross-fade at that time.
        if (const char* from = envValue("RASA_SKYFADEFROM")) {
            float fx = 0, fy = 0, fz = 0;
            std::sscanf(from, "%f,%f,%f", &fx, &fy, &fz);
            const tr::Vec3 target = scene.camera().position;
            scene.camera().position = tr::Vec3{fx, fy, fz};
            for (int k = 0; k < 30; ++k) scene.advanceLights(0.1f);
            scene.camera().position = target;
            const float seconds = envValue("RASA_SKYFADEAT") ? float(std::atof(envValue("RASA_SKYFADEAT"))) : 0.0f;
            // RASA_SKYFADEIN=x,y,z: the point the region test uses while the clock runs (the render camera is restored after).
            tr::Vec3 inside = target;
            if (const char* in = envValue("RASA_SKYFADEIN")) std::sscanf(in, "%f,%f,%f", &inside.x, &inside.y, &inside.z);
            scene.camera().position = inside;
            for (int k = 0; k < int(seconds * 10.0f + 0.5f); ++k) scene.advanceLights(0.1f);
            scene.camera().position = target;
            std::printf("sky fade at +%.1f s:", seconds);
            for (const SceneView::SkyWeight& w : scene.skyWeights())
                std::printf(" %s x%.2f", scene.skies()[w.index].name.c_str(), w.weight);
            std::printf("\n");
        }
        scene.advanceParticles(0.0f);
        for (float t = 0; t < particleSeconds; t += 1.0f / 30.0f) scene.advanceParticles(1.0f / 30.0f);
        std::printf("particle systems running: %zu, %zu particles\n", scene.liveParticleSystems(), scene.liveParticles());
        if (const char* pool = envValue("RASA_POOLFIGURES")) {   // <spawn pool index in the map's content>: its figures
            const std::vector<std::string> lines = scene.debugServerFigures(Selection{SelectionKind::SpawnPool, size_t(std::atoi(pool))});
            std::printf("spawn pool %s: %zu figures\n", pool, lines.size());
            for (const std::string& line : lines) std::printf("%s\n", line.c_str());
        }
        if (const char* pose = envValue("RASA_POSEDUMP")) {     // x,z: the idle pose of the nearest posed server placement
            float x = 0, z = 0;
            std::sscanf(pose, "%f,%f", &x, &z);
            for (const std::string& line : scene.debugServerPose(x, z)) std::printf("%s\n", line.c_str());
        }
        if (envValue("RASA_LISTSERVEREFFECTS"))       // server objects whose models bring their own lights / particle systems
            for (const std::string& line : scene.debugServerBodyEffects()) std::printf("%s\n", line.c_str());
        if (envValue("RASA_ACCESSORIES"))            // RASA_ACCESSORIES=1: the accessories drawn on server placements (teleporter columns)
            for (const std::string& line : scene.debugServerAccessories()) std::printf("%s\n", line.c_str());
        for (const auto& e : scene.textureErrors()) std::printf("texture error: %s: %s\n", e.first.c_str(), e.second.c_str());
        // RASA_SELECT=<map entity index>: draw that entity highlighted, as the editor shows a selection.
        Selection highlighted{};
        if (const char* group = envValue("RASA_HIDEGROUP")) scene.hiddenGroup = group;   // skip model pieces of one GPCE group
        if (const char* only = envValue("RASA_ONLY")) scene.onlyEntity = std::atoi(only);   // draw one map entity alone
        if (const char* range = envValue("RASA_ENTITYRANGE")) std::sscanf(range, "%d,%d", &scene.entityFirst, &scene.entityLast);   // bisecting
        if (const char* sel = envValue("RASA_SELECT")) highlighted = Selection{SelectionKind::MapEntity, size_t(std::atoi(sel))};
        // RASA_SURFACE=1: where Add would put a new object - the view centre's ray on the ground alone and on the nearest surface.
        if (envValue("RASA_SURFACE")) {
            tr::Vec3 origin, direction, ground, surface;
            scene.viewRay(0, 0, origin, direction);
            const bool g = scene.groundPoint(origin, direction, origin.y - 10.0f, ground);
            const bool s = scene.surfacePoint(origin, direction, origin.y - 10.0f, surface);
            std::printf("view centre: ground %s (%.2f %.2f %.2f), surface %s (%.2f %.2f %.2f)\n", g ? "hit" : "none", ground.x, ground.y, ground.z,
                        s ? "hit" : "none", surface.x, surface.y, surface.z);
        }
        // RASA_SNAPAT=x,y,z[;x,y,z...]: what Snap to ground gives there (first surface below y + 2 m) and the raw terrain height.
        if (const char* spec = envValue("RASA_SNAPAT")) {
            for (const char* p = spec; p && *p;) {
                float x = 0, y = 0, z = 0;
                if (std::sscanf(p, "%f,%f,%f", &x, &y, &z) != 3) break;
                float terrain = 0, solid = 0;
                const bool hasTerrain = map.hasTerrain && map.terrain.heightAtWorld(x, z, terrain);
                const bool isSolid = scene.solidTerrainHeight(x, z, solid);
                std::printf("snap at (%.2f %.2f %.2f): %.2f   terrain %s%.2f%s\n", x, y, z, scene.surfaceHeightBelow(x, z, y + 2.0f, y),
                            hasTerrain ? "" : "none ", terrain, hasTerrain && !isSolid ? " (hole)" : "");
                p = std::strchr(p, ';');
                if (p) ++p;
            }
        }
        // RASA_SOUNDMARKERS=1: the map's sound emitters as marker boxes; RASA_SELECTSOUND=<emitter index> selects one (its distances
        // drawn) and frames it, overriding the camera.
        scene.showSoundMarkers = envValue("RASA_SOUNDMARKERS") != nullptr;
        if (const char* sound = envValue("RASA_SELECTSOUND")) {
            highlighted = Selection{SelectionKind::AudioEmitter, size_t(std::atoi(sound))};
            scene.focusOn(highlighted);
            if (highlighted.index < scene.audioEmitters().size()) {
                const tr::AudioEmitter& e = scene.audioEmitters()[highlighted.index];
                std::printf("sound emitter %zu: %s, audio set %u, at %.1f %.1f %.1f, distances %.1f / %.1f\n", highlighted.index, e.name.c_str(), e.audioSetId,
                            e.position.x, e.position.y, e.position.z, e.minDistance, e.maxDistance);
            }
        }
        // RASA_GARRISONS=<0|1|2>: the garrison shown at every control point (SceneView::garrisonViews: holder / Bane / AFS);
        // RASA_SELECTPOINT=<control point id>: draw with that point selected (its link lines shown).
        if (const char* g = envValue("RASA_GARRISONS"))
            for (const PlacedObject& o : content.objects)
                if (o.kind == ObjectKind::ControlPoint) scene.garrisonViews[o.id] = std::atoi(g);
        if (const char* point = envValue("RASA_SELECTPOINT"))
            for (size_t k = 0; k < content.objects.size(); ++k)
                if (content.objects[k].kind == ObjectKind::ControlPoint && content.objects[k].id == std::atoll(point))
                    highlighted = Selection{SelectionKind::Object, k};
        // RASA_LIGHTMARKERS=1: the map's lights as marker boxes; RASA_SELECTLIGHT=<light index> highlights one (its reach drawn)
        // and checks that a click on its box's top near edge picks it; RASA_SELECTLIGHT=all checks every light that way from a
        // camera 5 m away (other geometry may be in front - use RASA_NOENTITIES / RASA_NOTERRAIN) and restores the camera.
        scene.showLightMarkers = envValue("RASA_LIGHTMARKERS") != nullptr;
        auto pickLight = [&](size_t index) {
            const tr::PlacedLight light = scene.displayedMapLight(index);
            const ViewCamera& cam = scene.camera();
            const tr::Vec3 top = add(light.position, tr::Vec3{0, 0.25f, 0});
            tr::Vec3 toward = subtract(cam.position, top);
            toward.y = 0;
            const float m = std::max(std::fabs(toward.x), std::fabs(toward.z));
            const tr::Vec3 nearEdge = m > 1e-4f ? add(top, scaled(toward, 0.25f / m)) : top;
            const tr::Vec3 v = subtract(nearEdge, cam.position);
            const float depth = dot(v, cam.forward());
            const float aspect = float(width) / float(height), tanHalf = std::tan(cam.fovY * 0.5f);
            const Selection hit = scene.pick(dot(v, cam.right()) / depth / (tanHalf * aspect), dot(v, cam.up()) / depth / tanHalf);
            return sameSelection(hit, Selection{SelectionKind::MapLight, index});
        };
        if (const char* sel = envValue("RASA_SELECTLIGHT")) {
            if (std::strcmp(sel, "all") == 0) {
                const ViewCamera saved = scene.camera();
                size_t ok = 0;
                for (size_t k = 0; k < scene.mapLights().size(); ++k) {
                    ViewCamera& cam = scene.camera();
                    const tr::Vec3 p = scene.displayedMapLight(k).position;
                    cam.position = add(p, tr::Vec3{3.0f, 2.0f, 3.6f});
                    const tr::Vec3 f = normalized(subtract(p, cam.position));
                    cam.yaw = std::atan2(f.x, f.z);
                    cam.pitch = std::asin(f.y);
                    if (pickLight(k)) ++ok;
                    else std::printf("light %zu (%s): MISSED\n", k, scene.mapLights()[k].name.c_str());
                }
                scene.camera() = saved;
                std::printf("light markers: %zu of %zu picked at their box edge\n", ok, scene.mapLights().size());
            } else {
                const size_t index = size_t(std::atoi(sel));
                highlighted = Selection{SelectionKind::MapLight, index};
                if (scene.showLightMarkers && index < scene.mapLights().size())
                    std::printf("light %zu (%s): pick at its box edge %s\n", index, scene.mapLights()[index].name.c_str(),
                                pickLight(index) ? "OK" : "MISSED (or something is in front)");
            }
        }
        // RASA_SKINTIMING=1: time 20 frames of queueDraws with the animation clock advancing (object animations re-skinned each
        // frame) against the same with the clock stopped (SceneView::animate off).
        // RASA_SKINDUMP=<entity index>: per skinned piece of the entity's model, its palette (GSKN bone list) as world skin
        // matrices (3 rows of 4, like the game's gWorldMatrixArray) at RASA_LIGHTTIME - for comparison with an apitrace capture.
        if (const char* dumpEntity = envValue("RASA_SKINDUMP")) {
            const size_t index = size_t(std::atoi(dumpEntity));
            const auto matrices = scene.debugWorldSkinMatrices(index, scene.lightTime);
            if (index < map.map.entities.size() && !matrices.empty()) {
                const tr::GeoModel geo = tr::readGeo(assets.read(data.classMeshName(map.map.entities[index].classId)));
                for (const tr::GeoPiece& piece : geo.pieces) {
                    if (!piece.skinned) continue;
                    std::printf("skin dump: piece %s (%zu vertices), %zu palette bones\n", piece.pieceName.c_str(), piece.vertices.size(), piece.skinBones.size());
                    for (size_t b = 0; b < piece.skinBones.size(); ++b)
                        for (const auto& [name, m] : matrices)
                            if (name == piece.skinBones[b])
                                std::printf("  %2zu %-24s %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f\n", b, name.c_str(),
                                            m.row0.x, m.row0.y, m.row0.z, m.row0.w, m.row1.x, m.row1.y, m.row1.z, m.row1.w, m.row2.x, m.row2.y, m.row2.z,
                                            m.row2.w);
                }
            }
        }
        // RASA_MISSIONCHECK=1 (with RASA_DB): the server's mission content checks (MissionValidation.h) on the database - counts and
        // every diagnostic, one per line ("mission revision code: message"), for comparison with the Game server's own validator.
        if (envValue("RASA_MISSIONCHECK") && database) {
            std::vector<DbTable> loaded;
            for (const char* name : {"mission_content_definition", "mission_channel_policy", "mission_repeat_policy", "mission_prerequisite",
                                     "mission_objective_definition", "mission_objective_transition", "mission_trigger", "mission_action", "mission_indicator",
                                     "mission_reward_definition", "mission_reward_item", "mission_spawn_group", "mission_spawn", "mission_scenario",
                                     "mission_scenario_step", "mission_area", "mission_scene_binding"})
                if (database->hasTable(name)) loaded.push_back(database->loadTable(name));
            std::map<std::string, const DbTable*> byName;
            for (const DbTable& t : loaded) byName[t.name] = &t;
            const MissionReferences references = MissionReferences::load(*database);
            const auto start = std::chrono::steady_clock::now();
            const MissionValidationResult v = validateMissionContent(byName, references);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("mission check: %zu enabled, %zu left out, %zu diagnostics, start-up %s, readiness %s (%.1f ms)\n", v.enabledMissions,
                        v.inactiveMissions.size(), v.diagnostics.size(), v.startupFails ? "FAILS" : "ok", v.blocksReadiness ? "BLOCKED" : "ok", ms);
            for (const MissionDiagnostic& d : v.diagnostics)
                std::printf("  mission %lld %s o%lld t%lld g%lld a%lld s%lld p%lld %s: %s\n", static_cast<long long>(d.missionId), d.revision.c_str(),
                            static_cast<long long>(d.objectiveId), static_cast<long long>(d.transitionId), static_cast<long long>(d.triggerId),
                            static_cast<long long>(d.actionId), static_cast<long long>(d.scenarioId), static_cast<long long>(d.stepId), d.code.c_str(),
                            d.message.c_str());
        }
        // RASA_SKYFADETEST=1 (Concordia with RASA_DB): walk into Ranja Cavern at the mouth (the in-game positions where the region
        // switched) and print the sky weights and hemisphere light every 0.5 s - the 4 s cross-fade (SceneView::skyTransitions).
        if (envValue("RASA_SKYFADETEST")) {
            const ViewCamera saved = scene.camera();
            auto hemi = [&] {
                const SceneLighting l = scene.lighting();
                return 0.3f * l.hemiTop.x + 0.59f * l.hemiTop.y + 0.11f * l.hemiTop.z;
            };
            auto weights = [&] {
                std::string out;
                for (const SceneView::SkyWeight& w : scene.skyWeights())
                    out += std::string(out.empty() ? "" : " + ") + scene.skies()[w.index].name + " x" + std::to_string(w.weight).substr(0, 4);
                return out;
            };
            scene.camera().position = tr::Vec3{-489.92090f, 213.91177f, -405.16742f};
            for (int k = 0; k < 3; ++k) scene.advanceLights(0.1f);
            std::printf("sky fade test: outside, hemi top %.3f, %s\n", hemi(), weights().c_str());
            scene.camera().position = tr::Vec3{-475.07239f, 214.95740f, -403.85135f};
            for (int k = 0; k <= 10; ++k) {
                scene.advanceLights(k == 0 ? 0.0f : 0.5f);
                std::printf("  inside %.1f s: hemi top %.3f, %s\n", 0.5f * k, hemi(), weights().c_str());
            }
            // Walk out, and back in half way through that fade: the outdoor sky (fading in, at 0.5) fades out from where it is, the
            // cave sky (fading out) is dropped and fades in again from 0 - as FUN_00822ac0 / FUN_008238d0 do.
            scene.camera().position = tr::Vec3{-489.92090f, 213.91177f, -405.16742f};
            scene.advanceLights(0.0f);
            for (int k = 0; k < 4; ++k) scene.advanceLights(0.5f);
            std::printf("  out again, 2 s: %s\n", weights().c_str());
            scene.camera().position = tr::Vec3{-475.07239f, 214.95740f, -403.85135f};
            scene.advanceLights(0.0f);
            std::printf("  back in: %s\n", weights().c_str());
            scene.advanceLights(1.0f);
            std::printf("  1 s later: %s\n", weights().c_str());
            scene.camera() = saved;
        }
        // RASA_PROFILE=<frames>: the editor's per-frame work at this camera (Editor: loadPending(0.010); ViewportPanel::render:
        // advanceParticles, advanceLights, queueDraws, record + submit) for that many 60 Hz frames, the camera turning 0.5 degrees
        // per frame as when looking around; per section the mean and worst time and the frames that took over twice the median.
        // RASA_MAPROUNDTRIP=1: every map under <game root>/data/maps read and written back (tr::writeMap) must be byte-identical.
        if (envValue("RASA_MAPROUNDTRIP")) {
            size_t maps = 0, identical = 0;
            std::error_code ec;
            for (const auto& dir : std::filesystem::directory_iterator(std::filesystem::path(argv[1]) / "data" / "maps", ec)) {
                for (const auto& file : std::filesystem::directory_iterator(dir.path(), ec)) {
                    if (tr::toLowerAscii(file.path().extension().string()) != ".map") continue;
                    ++maps;
                    try {
                        const tr::Bytes original = tr::readFile(file.path().string());
                        const tr::Bytes written = tr::writeMap(tr::readMap(original));
                        if (written == original) ++identical;
                        else {
                            size_t at = 0;
                            while (at < written.size() && at < original.size() && written[at] == original[at]) ++at;
                            std::printf("map round trip DIFFERS: %s (sizes %zu / %zu, first difference at %zu)\n", file.path().filename().string().c_str(),
                                        original.size(), written.size(), at);
                        }
                    } catch (const std::exception& e) {
                        std::printf("map round trip FAILED: %s: %s\n", file.path().filename().string().c_str(), e.what());
                    }
                }
            }
            std::printf("map round trip: %zu of %zu maps byte-identical\n", identical, maps);
        }
        // RASA_LIGHTSEDITTEST=1: for every map's lights.xml - tr::LightsDocument finds as many lights as readLights; moving light 0,
        // duplicating it (with its modifiers) and removing the copy read back as expected; an untouched document is byte-identical.
        if (envValue("RASA_LIGHTSEDITTEST")) {
            size_t files = 0, passed = 0, modTracksTotal = 0;
            std::error_code ec;
            for (const auto& dir : std::filesystem::directory_iterator(std::filesystem::path(argv[1]) / "data" / "maps", ec)) {
                const auto path = dir.path() / "lights.xml";
                if (!std::filesystem::exists(path, ec)) continue;
                ++files;
                try {
                    const tr::Bytes xml = tr::readFile(path.string());
                    tr::LightsDocument doc(xml);
                    const auto lights = tr::readLights(xml);
                    bool ok = doc.bytes() == xml && doc.count() == lights.size();
                    if (ok && doc.count() > 0) {
                        const size_t modsBefore = tr::readLightModifiers(xml).size();
                        const float p[3] = {lights[0].position.x + 3.0f, lights[0].position.y, lights[0].position.z - 2.0f};
                        doc.setProperty(0, "Position", tr::LightsDocument::formatVector(p, 3));
                        const auto moved = tr::readLights(doc.bytes());
                        ok = moved.size() == lights.size() && std::fabs(moved[0].position.x - p[0]) < 1e-4f && std::fabs(moved[0].position.z - p[2]) < 1e-4f;
                        const size_t copy = doc.duplicate(0, 999999999999ull, "RasaEdit copy");
                        const auto duplicated = tr::readLights(doc.bytes());
                        const auto modsCopy = tr::readLightModifiers(doc.bytes());
                        size_t copyMods = 0, origMods = 0;
                        for (const auto& m : modsCopy) {
                            if (m.lightId == 999999999999ull) ++copyMods;
                            if (m.lightId == lights[0].id && lights[0].id != 0) ++origMods;
                        }
                        ok = ok && copy == lights.size() && duplicated.size() == lights.size() + 1 && duplicated.back().name == "RasaEdit copy" &&
                             duplicated.back().id == 999999999999ull && copyMods == origMods && modsCopy.size() == modsBefore + copyMods;
                        doc.remove(copy);
                        const auto removed = tr::readLights(doc.bytes());
                        ok = ok && removed.size() == lights.size() && tr::readLightModifiers(doc.bytes()).size() == modsBefore;
                    }
                    // Modifier tracks: writing every light's tracks back unchanged is byte-identical; a key edit, an added track (on a
                    // light without one: a new <Mod>, made with its ID) and its removal read back through readLightModifiers.
                    if (ok) {
                        tr::LightsDocument mods(xml);
                        size_t tracks = 0;
                        for (size_t l = 0; l < mods.count(); ++l) {
                            const std::vector<tr::LightModTrack> t = mods.modTracks(l);
                            for (size_t k = 0; k < t.size(); ++k) mods.setModTrack(l, k, t[k]);
                            tracks += t.size();
                        }
                        ok = mods.bytes() == xml;
                        if (!ok) std::printf("  modifier tracks of %s not written back unchanged\n", dir.path().filename().string().c_str());
                        modTracksTotal += tracks;
                        size_t withTrack = SIZE_MAX, without = SIZE_MAX;
                        for (size_t l = 0; l < mods.count(); ++l) {
                            const bool hasId = std::strtoull(mods.property(l, "ID").c_str(), nullptr, 10) != 0;
                            if (hasId && !mods.modTracks(l).empty() && withTrack == SIZE_MAX) withTrack = l;
                            if (hasId && mods.modTracks(l).empty() && without == SIZE_MAX) without = l;
                        }
                        if (ok && withTrack != SIZE_MAX) {
                            tr::LightModTrack t = mods.modTracks(withTrack)[0];
                            t.keys[0].second = "0.123";
                            mods.setModTrack(withTrack, 0, t);
                            const uint64_t id = std::strtoull(mods.property(withTrack, "ID").c_str(), nullptr, 10);
                            bool found = false;
                            for (const auto& m : tr::readLightModifiers(mods.bytes()))
                                if (m.lightId == id && !m.tracks.empty() && !m.tracks[0].keys.empty()) found = std::fabs(m.tracks[0].keys[0].second - 0.123f) < 1e-6f;
                            ok = found;
                            if (!ok) std::printf("  key edit on %s not read back\n", dir.path().filename().string().c_str());
                        }
                        if (ok && without != SIZE_MAX) {
                            const size_t before = tr::readLightModifiers(mods.bytes()).size();
                            tr::LightModTrack t;
                            t.className = "IntensityModifier";
                            t.properties = {{"Enabled", "true", "bool"}, {"Intensity1", "0.5000000", "float"}, {"Intensity2", "1.0000000", "float"}};
                            t.keys = {{"0", "0"}, {"1", "1"}};
                            const tr::Bytes beforeAdd = mods.bytes();
                            ok = mods.addModTrack(without, t) && tr::readLightModifiers(mods.bytes()).size() == before + 1 && mods.modTracks(without).size() == 1 &&
                                 tr::readLights(mods.bytes()).size() == lights.size();
                            ok = ok && mods.removeModTrack(without, 0) && mods.bytes() == beforeAdd;
                            if (!ok) std::printf("  add / remove of a track on %s failed\n", dir.path().filename().string().c_str());
                        }
                    }
                    if (ok) ++passed;
                    else std::printf("lights edit test FAILED: %s\n", dir.path().filename().string().c_str());
                } catch (const std::exception& e) {
                    std::printf("lights edit test ERROR: %s: %s\n", dir.path().filename().string().c_str(), e.what());
                }
            }
            std::printf("lights edit test: %zu of %zu lights.xml files passed (%zu modifier tracks written back unchanged)\n", passed, files, modTracksTotal);
        }
        // RASA_VISROUNDTRIP=1: every map's .vis - written back unchanged it is byte-identical; with every computed row encoded again by
        // setPvsRow (zlib level 1, as the game's SetPVS) it is still byte-identical (damaged rows kept as stored).
        if (envValue("RASA_VISROUNDTRIP")) {
            size_t files = 0, same = 0, reencoded = 0, rowsTotal = 0;
            std::error_code ec;
            for (const auto& dir : std::filesystem::directory_iterator(std::filesystem::path(argv[1]) / "data" / "maps", ec)) {
                const auto path = dir.path() / (dir.path().filename().string() + ".vis");
                if (!std::filesystem::exists(path, ec)) continue;
                ++files;
                try {
                    const tr::Bytes data = tr::readFile(path.string());
                    tr::VisFile vis = tr::readVis(data);
                    const bool unchanged = tr::writeVis(vis) == data;
                    same += unchanged ? 1 : 0;
                    for (uint32_t k = 0; k < vis.rows.size(); ++k)
                        if (!vis.rows[k].stored.empty() && !vis.rows[k].damaged) {
                            const std::vector<uint32_t> words = vis.rows[k].words;
                            tr::setPvsRow(vis, k, words, vis.rows[k].timestamp);
                            ++rowsTotal;
                        }
                    const bool again = tr::writeVis(vis) == data;
                    reencoded += again ? 1 : 0;
                    if (!unchanged || !again) std::printf("vis round trip FAILED: %s (unchanged %d, rows re-encoded %d)\n", path.filename().string().c_str(), unchanged, again);
                } catch (const std::exception& e) {
                    std::printf("vis round trip ERROR: %s: %s\n", path.filename().string().c_str(), e.what());
                }
            }
            std::printf("vis round trip: %zu of %zu files byte-identical, %zu identical with all %zu computed rows re-encoded\n", same, files, reencoded, rowsTotal);
        }
        // RASA_AUDIOEDITTEST=1: every map's audioemitters.xml - the document finds as many emitters as readAudioEmitters and writes
        // back byte-identical untouched; moving emitter 0, duplicating it (new UID) and removing the copy read back; adding one to an
        // empty file (<AudioEmitters />) and removing it gives the file back.
        if (envValue("RASA_AUDIOEDITTEST")) {
            size_t files = 0, passed = 0, emittersTotal = 0;
            std::error_code ec;
            for (const auto& dir : std::filesystem::directory_iterator(std::filesystem::path(argv[1]) / "data" / "maps", ec)) {
                const auto path = dir.path() / "audioemitters.xml";
                if (!std::filesystem::exists(path, ec)) continue;
                ++files;
                try {
                    const tr::Bytes xml = tr::readFile(path.string());
                    tr::LightsDocument doc = tr::audioEmittersDocument(xml);
                    const auto emitters = tr::readAudioEmitters(xml);
                    emittersTotal += emitters.size();
                    bool ok = doc.bytes() == xml && doc.count() == emitters.size();
                    if (ok && doc.count() > 0) {
                        const float p[3] = {emitters[0].position.x + 3.0f, emitters[0].position.y, emitters[0].position.z};
                        doc.setProperty(0, "Position", tr::LightsDocument::formatVector(p, 3));
                        const size_t copy = doc.duplicate(0, 999999999999ull, "RasaEdit copy");
                        const auto after = tr::readAudioEmitters(doc.bytes());
                        ok = after.size() == emitters.size() + 1 && std::fabs(after[0].position.x - p[0]) < 1e-4f && after.back().uid == "999999999999" &&
                             after.back().name == "RasaEdit copy" && after.back().audioSetId == emitters[0].audioSetId;
                        doc.remove(copy);
                        ok = ok && tr::readAudioEmitters(doc.bytes()).size() == emitters.size();
                    }
                    if (ok && emitters.empty()) {
                        tr::LightsDocument add = tr::audioEmittersDocument(xml);
                        const size_t i = add.addElement("AudioEmitterProperties", {{"Name", "New", "string"}, {"UID", "1", "string"}, {"AudioSetID", "30", "uint"},
                                                                                    {"Position", "1.0000000, 2.0000000, 3.0000000", "string"}});
                        const auto one = tr::readAudioEmitters(add.bytes());
                        ok = i == 0 && one.size() == 1 && one[0].audioSetId == 30 && one[0].position.z == 3.0f;
                        add.remove(0);
                        // the section stays open (<AudioEmitters> </AudioEmitters>): an equal list, not equal bytes
                        ok = ok && tr::readAudioEmitters(add.bytes()).empty();
                    }
                    if (ok) ++passed;
                    else std::printf("audio emitter edit test FAILED: %s\n", dir.path().filename().string().c_str());
                } catch (const std::exception& e) {
                    std::printf("audio emitter edit test ERROR: %s: %s\n", dir.path().filename().string().c_str(), e.what());
                }
            }
            std::printf("audio emitter edit test: %zu of %zu files passed (%zu emitters)\n", passed, files, emittersTotal);
        }
        if (const char* frames = envValue("RASA_PROFILE")) {
            const int count = std::max(1, std::atoi(frames));
            struct Times { double load, particles, lights, queue, record, gpu, total; };
            double sections[7] = {};
            std::vector<Times> times;
            auto now = [] { return std::chrono::steady_clock::now(); };
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            for (int f = 0; f < count; ++f) {
                Times t{};
                const auto t0 = now();
                scene.loadPending(0.010);
                const auto t1 = now();
                scene.advanceParticles(1.0f / 60.0f);
                const auto t2 = now();
                scene.advanceLights(1.0f / 60.0f);
                const auto t3 = now();
                scene.queueDraws(highlighted);
                const auto t4 = now();
                for (int k = 0; k < 7; ++k) sections[k] += scene.queueSectionMs(k);
                SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device);
                renderer.record(cb);
                const auto t5 = now();
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
                if (fence) {
                    SDL_WaitForGPUFences(device, true, &fence, 1);
                    SDL_ReleaseGPUFence(device, fence);
                }
                const auto t6 = now();
                t = Times{ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), ms(t5, t6), ms(t0, t6)};
                times.push_back(t);
                scene.camera().yaw += 0.5f * 3.14159265f / 180.0f;
            }
            std::vector<double> totals;
            for (const Times& t : times) totals.push_back(t.total);
            std::vector<double> sorted = totals;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            auto report = [&](const char* name, double Times::*field) {
                double sum = 0, worst = 0;
                for (const Times& t : times) { sum += t.*field; worst = std::max(worst, t.*field); }
                std::printf("profile %-9s mean %7.2f ms  worst %7.2f ms\n", name, sum / double(times.size()), worst);
            };
            report("load", &Times::load);
            report("particles", &Times::particles);
            report("lights", &Times::lights);
            report("queue", &Times::queue);
            report("record", &Times::record);
            report("gpu", &Times::gpu);
            report("total", &Times::total);
            std::printf("profile queue sections (mean ms): terrain+setup %.2f (terrain %.2f)  entities %.2f  clutter %.2f  particles %.2f  sky %.2f  server %.2f;"
                        " entities drawn %zu, clutter drawn %zu, draws queued %zu\n", sections[0] / count, sections[6] / count, sections[1] / count, sections[2] / count,
                        sections[3] / count, sections[4] / count, sections[5] / count, scene.drawnEntities(), scene.drawnClutter(), renderer.queuedDraws());
            std::printf("profile median frame %.2f ms; spikes (> 2x median):\n", median);
            for (size_t f = 0; f < times.size(); ++f)
                if (times[f].total > 2.0 * median)
                    std::printf("  frame %3zu total %7.2f  load %6.2f particles %6.2f lights %6.2f queue %6.2f record %6.2f gpu %6.2f\n", f,
                                times[f].total, times[f].load, times[f].particles, times[f].lights, times[f].queue, times[f].record, times[f].gpu);
        }
        // RASA_SHADOWCACHETEST=x,y,z: kept placed-light shadow maps follow edits. Renders this view twice (the second from the kept maps),
        // moves the map entity nearest to (x, y, z) 0.8 m east (refreshEntity), renders with the cache and again with every map redrawn:
        // the last two must be equal and differ from the first.
        if (const char* at = envValue("RASA_SHADOWCACHETEST")) {
            tr::Vec3 p{};
            std::sscanf(at, "%f,%f,%f", &p.x, &p.y, &p.z);
            auto frame = [&] {
                scene.queueDraws(highlighted);
                SDL_GPUTransferBufferCreateInfo ti = {};
                ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
                ti.size = width * height * 4;
                SDL_GPUTransferBuffer* dl = SDL_CreateGPUTransferBuffer(device, &ti);
                SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device);
                renderer.record(cb);
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cb);
                SDL_GPUTextureRegion src = {};
                src.texture = renderer.colourTarget();
                src.w = width;
                src.h = height;
                src.d = 1;
                SDL_GPUTextureTransferInfo dst = {};
                dst.transfer_buffer = dl;
                dst.pixels_per_row = width;
                dst.rows_per_layer = height;
                SDL_DownloadFromGPUTexture(copy, &src, &dst);
                SDL_EndGPUCopyPass(copy);
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
                const uint8_t* px = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device, dl, false));
                std::vector<uint8_t> image(px, px + size_t(width) * height * 4);
                SDL_UnmapGPUTransferBuffer(device, dl);
                SDL_ReleaseGPUTransferBuffer(device, dl);
                return std::make_pair(image, scene.lastShadowMapsDrawn);
            };
            auto differing = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                size_t n = 0;
                for (size_t k = 0; k + 3 < a.size(); k += 4)
                    if (std::abs(int(a[k]) - int(b[k])) > 2 || std::abs(int(a[k + 1]) - int(b[k + 1])) > 2 || std::abs(int(a[k + 2]) - int(b[k + 2])) > 2) ++n;
                return n;
            };
            size_t nearest = 0;
            float best = 1e30f;
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                if (data.className(map.map.entities[k].classId).rfind("ABV", 0) == 0) continue;     // invisible collision boxes
                const tr::Vec3& l = map.map.entities[k].location;
                const float d = (l.x - p.x) * (l.x - p.x) + (l.y - p.y) * (l.y - p.y) + (l.z - p.z) * (l.z - p.z);
                if (d < best) { best = d; nearest = k; }
            }
            const auto a = frame();
            const auto a2 = frame();
            tr::MapBundle& editable = const_cast<tr::MapBundle&>(map);     // as the Client workspace edits the bundle the scene shows
            editable.map.entities[nearest].location.x += 0.8f;
            scene.refreshEntity(nearest);
            const auto b = frame();
            scene.cacheShadowMaps = false;
            const auto c = frame();
            scene.cacheShadowMaps = true;
            editable.map.entities[nearest].location.x -= 0.8f;
            scene.refreshEntity(nearest);
            std::printf("shadow cache test: entity %zu (%s); maps drawn %d / %d (kept) / %d (after the edit) / %d (no cache)\n", nearest,
                        data.className(map.map.entities[nearest].classId).c_str(), a.second, a2.second, b.second, c.second);
            std::printf("shadow cache test: kept vs first %zu px, edited vs first %zu px, edited cached vs redrawn %zu px -> %s\n",
                        differing(a.first, a2.first), differing(a.first, b.first), differing(b.first, c.first),
                        differing(a.first, a2.first) == 0 && differing(b.first, c.first) == 0 && differing(a.first, b.first) > 0 ? "PASS" : "FAIL");
        }
        if (envValue("RASA_SKINTIMING")) {
            for (int pass = 0; pass < 3; ++pass) {        // on, off, on: the first pass also pays one-time costs
                scene.animate = pass != 1;
                const auto start = std::chrono::steady_clock::now();
                for (int f = 0; f < 20; ++f) {
                    scene.advanceLights(1.0f / 30.0f);
                    scene.queueDraws(highlighted);
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20.0;
                std::printf("skin timing: %s %.2f ms per frame\n", pass != 1 ? "playing" : "stopped", ms);
            }
            scene.animate = true;
        }
        scene.queueDraws(highlighted);
        std::printf("draws queued: %zu (entities drawn %zu, fading forward pieces %zu), particle vertices %zu\n", renderer.queuedDraws(), scene.drawnEntities(), scene.fadingForwardPieces(), renderer.queuedParticleVertices());
        std::printf("clutter drawn: %zu\n", scene.drawnClutter());
        std::printf("terrain chunks: %zu, %zu at full detail\n", scene.terrainChunkCount(), scene.terrainChunksAtFullDetail());
        {
            tr::Vec3 rayOrigin, rayDirection, ground;
            scene.viewRay(0.0f, 0.0f, rayOrigin, rayDirection);
            if (scene.groundPoint(rayOrigin, rayDirection, 0.0f, ground))
            {
                std::printf("ground under the view centre: %.2f, %.2f, %.2f (terrain height there %.2f)\n", ground.x, ground.y, ground.z,
                            scene.groundHeight(ground.x, ground.z, -9999.0f));
                // The sample there: layers (texlayers.tab name) with stored weight and the client's coverage.
                const tr::Terrain& t = map.terrain;
                const long column = std::lround((ground.x - t.worldX(0)) / t.header.spacing);
                const long row = std::lround((ground.z - t.worldZ(0)) / t.header.spacing);
                if (map.hasTerrain && column >= 0 && row >= 0 && column < long(t.columns) && row < long(t.rows)) {
                    const tr::TerrainSample& s = t.sampleAt(uint32_t(row), uint32_t(column));
                    std::printf("terrain sample (row %ld, column %ld), category %d:", row, column, int(tr::surfaceCategory(s.word)));
                    for (int k = 0; k < 4; ++k) {
                        const tr::LayerBlend& b = tr::sampleLayer(s, k);
                        const int entry = tr::textureLayerOf(b);
                        std::printf(" [%d %s w%d c%d]", int(b.layerIndex),
                                    entry >= 0 && size_t(entry) < map.textureLayers.size() ? map.textureLayers[entry].name.c_str() : "-", int(b.weight),
                                    int(tr::slotCoverage(s, k)));
                    }
                    std::printf("\n");
                }
            }
        }
        const Selection hit = scene.pick(0.0f, 0.0f);    // centre of the view
        const int picked = hit.kind == SelectionKind::MapEntity ? int(hit.index) : -1;
        if (hit.kind != SelectionKind::MapEntity && hit.kind != SelectionKind::None)
            std::printf("pick at centre: server object kind %d index %zu\n", int(hit.kind), hit.index);
        else if (picked >= 0)
            std::printf("pick at centre: entity %d, class %d (%s) at %.1f, %.1f, %.1f\n", picked, map.map.entities[size_t(picked)].classId,
                        data.className(map.map.entities[size_t(picked)].classId).c_str(), map.map.entities[size_t(picked)].location.x,
                        map.map.entities[size_t(picked)].location.y, map.map.entities[size_t(picked)].location.z);
        else
            std::puts("pick at centre: nothing");

        SDL_GPUTransferBufferCreateInfo info = {};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = width * height * 4;
        SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &info);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
        renderer.record(commands);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion source = {};
        source.texture = renderer.colourTarget();
        source.w = width;
        source.h = height;
        source.d = 1;
        SDL_GPUTextureTransferInfo destination = {};
        destination.transfer_buffer = download;
        destination.pixels_per_row = width;
        destination.rows_per_layer = height;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        const void* pixels = SDL_MapGPUTransferBuffer(device, download, false);
        FILE* f = std::fopen(argv[4], "wb");
        std::fwrite(pixels, 1, width * height * 4, f);
        std::fclose(f);
        SDL_UnmapGPUTransferBuffer(device, download);
        SDL_ReleaseGPUTransferBuffer(device, download);

        // Focus check: from a camera looking 30 degrees up (the old failure case), "show in viewport" must not put
        // the camera under the terrain. Targets below the terrain surface (caves, interiors) are counted apart.
        {
            size_t tried = 0, underground = 0, caveTargets = 0, listed = 0;
            auto check = [&](const Selection& s) {
                tr::Vec3 target;
                float distance = 0, targetGround = 0;
                if (!scene.focusTarget(s, target, distance)) return;
                if (!map.hasTerrain || !map.terrain.heightAtWorld(target.x, target.z, targetGround)) return;
                if (target.y < targetGround - 3.0f) { ++caveTargets; return; }
                cam.yaw = 0.7f;
                cam.pitch = 0.5f;
                scene.focusOn(s);
                ++tried;
                float h = 0;
                if (map.terrain.heightAtWorld(cam.position.x, cam.position.z, h) && cam.position.y < h + 0.5f) {
                    ++underground;
                    if (listed++ < 5)
                        std::printf("  under: kind %d #%zu target %.1f %.1f %.1f (ground %.1f), camera %.1f %.1f %.1f (ground %.1f), distance %.1f\n",
                                    int(s.kind), s.index, target.x, target.y, target.z, targetGround, cam.position.x, cam.position.y,
                                    cam.position.z, h, distance);
                }
            };
            for (size_t k = 0; k < map.map.entities.size(); ++k) check(Selection{SelectionKind::MapEntity, k});
            for (size_t k = 0; k < content.spawnPools.size(); ++k) check(Selection{SelectionKind::SpawnPool, k});
            std::printf("focus check: %zu targets, camera under the terrain for %zu (%zu targets below the surface skipped)\n",
                        tried, underground, caveTargets);
        }
        SDL_WaitForGPUIdle(device);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());               // report instead of aborting (a Debug Error dialog)
        SDL_DestroyGPUDevice(device);
        SDL_Quit();
        return 2;
    }
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::puts("done");
    return 0;
}
