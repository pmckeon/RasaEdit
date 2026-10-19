#pragma once

#include "GpuMath.h"
#include "dds.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// Vertex used by every viewport mesh. colour is RGBA8 with red in the low byte (UBYTE4_NORM).
struct MeshVertex {
    float positionX = 0, positionY = 0, positionZ = 0;
    float normalX = 0, normalY = 0, normalZ = 0;
    uint32_t colour = 0xFFFFFFFF;
    float texU = 0, texV = 0;
    float texU1 = 0, texV1 = 0;        // second UV set (glow maps; TR materials: the UV set of their second map)
    float texU2 = 0, texV2 = 0;        // TR materials: the UV set of their third map
    float texU3 = 0, texV3 = 0;        // UV set 3 (water: VolMapTexCoord; read only by the water pipeline)
    float tangentX = 0, tangentY = 0, tangentZ = 0;     // normal maps: tangent frame (model space, like the normal)
    float binormalX = 0, binormalY = 0, binormalZ = 0;
};

inline uint32_t packColour(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    return uint32_t(red) | (uint32_t(green) << 8) | (uint32_t(blue) << 16) | (uint32_t(alpha) << 24);
}

// TR* deferred materials (effects.glm G-buffer shaders, disassembled 2026-10-11; see CLAUDE.md "Material maps, stage 3").
// Map slots: 0 = DiffuseTexture on UV set 0, 1 = the second map (UV set from its *TexCoord parameter, baked into MeshVertex
// texU1/V1), 2 = the third map (texU2/V2); each slot's UV goes through TexCoordTransform<slot>:
// u' = dot((u, v, 1), transform<slot>u.xyz), v' = dot((u, v, 1), transform<slot>v.xyz), u = (e0, e4, e8), v = (e1, e5, e9).
struct TrMaterial {
    bool active = false;
    SDL_GPUTexture* slot1 = nullptr;
    SDL_GPUTexture* slot2 = nullptr;
    // x: slot 1 kind, y: slot 2 kind (1 modifier: lerp by vertex alpha, 2 detail: + map - 0.5, 3 decal: lerp by map alpha);
    // z: environment (1 = by detail.a before the decal, 2 = by detail.a after the decal, 3 = by diffuse.a);
    // w: bits - 2 = albedo x (2 x vertex colour + emissive) (DetMapDecMap), 4 = specular, 8 = emissive is glow (DiffMapDiffMap*)
    Vec4f flags = Vec4f{0, 0, 0, 0};
    Vec4f specular = Vec4f{0, 0, 0, 0};   // MatSpecular: x spec power, y intensity, z environment amount
    Vec4f emissive = Vec4f{0, 0, 0, 0};   // MatEmissive x gModulateScale (0.5)
    Vec4f transform0u = Vec4f{1, 0, 0, 0}, transform0v = Vec4f{0, 1, 0, 0};
    Vec4f transform1u = Vec4f{1, 0, 0, 0}, transform1v = Vec4f{0, 1, 0, 0};
    Vec4f transform2u = Vec4f{1, 0, 0, 0}, transform2v = Vec4f{0, 1, 0, 0};
};

// TRRefract (screen-space distortion shells; shaders/refract.hlsl): the frame after opaque geometry, offset by the normal map.
struct RefractMaterial {
    SDL_GPUTexture* diffuse = nullptr;     // UV set 0 x transform 0
    SDL_GPUTexture* normalMap = nullptr;   // UV set 1 x transform 1
    SDL_GPUTexture* fade = nullptr;        // ModifierTexture: edge fade ramp (alpha)
    float displacement = 0;                // RefractionDisplacement
    Vec4f matDiffuse = Vec4f{0.5f, 0.5f, 0.5f, 0.5f};   // MatDiffuse x gModulateScale 0.5
    Vec4f transform0u = Vec4f{1, 0, 0, 0}, transform0v = Vec4f{0, 1, 0, 0};
    Vec4f transform1u = Vec4f{1, 0, 0, 0}, transform1v = Vec4f{0, 1, 0, 0};
};

// TRWaterReflectRefract (shaders/water.hlsl; the shaders bound at a water draw in an apitrace capture, 2026-10-14): textures
// and parameters as the .geo material names them. UV sets: diffuse 0, normal map 1, normal map 1 2, volume 3 (the
// NormalMap0TexCoord / NormalMap1TexCoord / VolMapTexCoord of the traced piece; other assignments are not decoded).
struct WaterMaterial {
    SDL_GPUTexture* diffuse = nullptr;
    SDL_GPUTexture* normalMap = nullptr;
    SDL_GPUTexture* normalMap1 = nullptr;
    SDL_GPUTexture* modifier = nullptr;    // ModifierTexture: alpha by thickness
    SDL_GPUTexture* volume = nullptr;      // VolumeMapTexture (3D): colour by (UV, thickness)
    float matPower = 0;
    float clampDepth = 1;
    float refractionDisplacement = 0;
    float volumeDisplacement = 0;
    Vec4f matSpecular = Vec4f{0, 0, 0, 0};
    Vec4f matRefractColour = Vec4f{1, 1, 1, 1};
    Vec4f transform[8] = {Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}, Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0},
                          Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}, Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}};   // TexCoordTransform0..3 u, v rows
};

// Material of the game's TRAdditiveAnisoSpecMapDiffMap[DetMapDecMap]VertColor effects (shader in effects.glm, verified by disassembly):
//   albedo = lerp(diffuse + detail - 0.5, decal, decal.a);  alpha = diffuse.a * vertex.a * MatDiffuse.a
//   colour = 4 * alpha * albedo * (albedo * vertex.rgb + LightTexture(0.5, max(N.V, 0)).a * MatSpecular)
//   (N.V per vertex in the game's vertex shader: oT0 = (0.5, max(N.V, 0)))
struct AnisoMaterial {
    SDL_GPUTexture* diffuse = nullptr;     // UV set 0
    SDL_GPUTexture* detail = nullptr;      // null: no detail layer (diffuse used as is)
    SDL_GPUTexture* decal = nullptr;       // null: no decal
    SDL_GPUTexture* light = nullptr;       // LightTexture: alpha ramp indexed by N.V
    Vec4f specular = Vec4f{0, 0, 0, 0};    // MatSpecular
    Vec4f diffuseAlpha = Vec4f{1, 1, 1, 1};  // MatDiffuse
    // TexCoordTransform0..2 (u row, v row as in TrMaterial): diffuse on UV set 0, detail on UV set DetailTexCoord (baked into
    // MeshVertex texU1/V1), decal on UV set DecalTexCoord (texU2/V2) - vs2x_AnisoDetDec[DetailTexCoord x 3 + DecalTexCoord],
    // disassembled 2026-10-13 (element layout; the index expression itself is not decoded).
    Vec4f transform0u = Vec4f{1, 0, 0, 0}, transform0v = Vec4f{0, 1, 0, 0};
    Vec4f transform1u = Vec4f{1, 0, 0, 0}, transform1v = Vec4f{0, 1, 0, 0};
    Vec4f transform2u = Vec4f{1, 0, 0, 0}, transform2v = Vec4f{0, 1, 0, 0};
    // The facing term's normal matrix (scene.hlsl VertexUniforms::anisoNormal): 3 x 3 applied to the vertex normal, not normalised.
    Mat4 normalMatrix = identityMatrix();
    // plain: the TRAdditiveDiffMap[DetMapDecMap]VertColor[Hue] effects (forward pixel shaders PSAdditiveCompiled /
    // PSAdditiveDetDet[Hue]Compiled, disassembled 2026-10-14; the same vertex shader layout and per-vertex fog):
    //   albedo = lerp(diffuse + detail - 0.5 [+ Hue - 0.5], decal, decal.a);  alpha = diffuse.a * vertex.a * MatDiffuse.a
    //   colour = albedo * (2 * vertex.rgb)^2 * alpha, written with that alpha and blended SRCALPHA / ONE (no specular / facing term).
    bool plain = false;
    Vec4f hue = Vec4f{0, 0, 0, 0};           // plain + Hue effect: w = 1, rgb = the material's Hue colour (added unweighted, - 0.5)
};

// GPU skinning: per vertex, up to 4 bones of the model's skeleton (index into the bone palette given by setSkin) and their
// weights; matches shaders/scene.hlsl SkinVertex (uint4 + float4).
struct SkinVertex {
    uint32_t bone0 = 0, bone1 = 0, bone2 = 0, bone3 = 0;
    float weight0 = 0, weight1 = 0, weight2 = 0, weight3 = 0;
};

struct GpuMesh {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    uint32_t indexCount = 0;
    bool lines = false;                 // line list (two indices per segment) instead of triangles
};

// Inputs of the terrain shader (see shaders/terrain.hlsl).
struct TerrainMaterial {
    SDL_GPUTexture* layers = nullptr;           // 2D array, one RGBA layer per texture layer, mipmapped
    SDL_GPUTexture* layerIndices = nullptr;     // one texel per terrain sample: its four layer indices
    SDL_GPUTexture* layerWeights = nullptr;     // one texel per terrain sample: its four weights
    float originX = 0;                          // world x of sample column 0
    float originZ = 0;                          // world z of sample row 0
    float inverseSpacing = 1;
    uint32_t columns = 1;
    uint32_t rows = 1;
    std::vector<float> tilings;                 // metres per repeat, per layer (up to 128)
};

// Frame lighting (see shaders/lighting.hlsli): hemispheric light + one directional light, linear fog.
// The defaults are the plain editor lighting used when sky lighting is off.
struct SceneLighting {
    tr::Vec3 towardsLight{-0.35f, 0.85f, -0.4f};
    Vec4f sunFront{0.65f, 0.65f, 0.65f, 1};
    Vec4f sunBack{0, 0, 0, 1};
    Vec4f hemiTop{0.4f, 0.4f, 0.4f, 1};
    Vec4f hemiBottom{0.3f, 0.3f, 0.3f, 1};
    bool fog = false;
    Vec4f fogColour{0.55f, 0.66f, 0.78f, 1};
    float fogStart = 0;
    float fogDensity = 0;                        // exp-squared fog, see shaders/lighting.hlsli
    bool showLight = false;                      // debug view like the game's /dl_showlight: 0.2 x light, no albedo
    int debugView = 0;                           // 2: G-buffer normals (N x 0.5 + 0.5, /dl_shownormals), 3: specular intensity
                                                 // (/dl_showspecintensity), 4: albedo (/dl_showdiffuse)
    // Sun shadows: a shadow map from the sun over the area around the camera; visibility = 1 - shadowOpacity x occlusion
    // (TRDirLight's shadowed pass, g_ShadowOpacity), shadowOpacity = the sun key track's ShadowOpacity at the time of day
    // (tr::skyLightShadowOpacity; e.g. 0.75 at 12:00 in sky_foreas_wilderness_default_tranz.sky).
    bool sunShadows = false;
    float shadowOpacity = 1.0f;
    // g_ShadowSoftness: the PCF taps' offset in half texels (TRDirLight ps3dir[1]); its runtime value is NOT traced (1 assumed).
    float shadowSoftness = 1.0f;
    tr::Vec3 cameraForward{0, 0, 1};    // the shadow bias grows with the camera view depth
    // Further directional sky lights (every enabled layer light applies, not only the sun: e.g. the second "Light" of
    // sky_foreas_wilderness_default_tranz.sky, orange at dawn / dusk, ShadowOpacity 0). Drawn without the shadow map
    // (exact for ShadowOpacity 0; UNVERIFIED for others).
    static constexpr int maxExtraDirectional = 3;
    // The forward light (gDirLight[0] of forward-drawn pieces: water, forward-phase pieces, billboards): the engine combines every
    // directional light into one TRasa::CombinedDirLight (FUN_00883040 / FUN_00883120) - colour = their sum, direction =
    // normalize((0, -1, 0) + sum of each light's travel direction x the luminance of the colour summed so far). See SceneView::lighting.
    tr::Vec3 forwardTowards{-0.35f, 0.85f, -0.4f};
    Vec4f forwardColour{0.65f, 0.65f, 0.65f, 1};
    int extraDirectionalCount = 0;
    tr::Vec3 extraTowards[maxExtraDirectional];
    Vec4f extraFront[maxExtraDirectional] = {};
    Vec4f extraBack[maxExtraDirectional] = {};
    SDL_FColor clearColour{0.55f, 0.66f, 0.78f, 1.0f};   // background (the sky colour with sky lighting)
    // Placed lights, laid out as the shaders' arrays (shaders/lighting.hlsli): position xyz + range; colour rgb *
    // intensity + fade share; spot direction + cos(outer half angle) (-2 = point); x = cos(inner half angle).
    static constexpr int maxLocalLights = 32;
    int localCount = 0;
    Vec4f localPosition[maxLocalLights] = {};
    Vec4f localColour[maxLocalLights] = {};
    Vec4f localSpot[maxLocalLights] = {};
    Vec4f localSpotInner[maxLocalLights] = {};    // x: cos(inner half angle), y: globe cube index (0 = none)
    Vec4f localRotation[maxLocalLights] = {};     // light orientation quaternion x y z w (for the globe lookup)
    // Placed-light shadows (lights with CastsShadows the game's selection keeps; see SceneView::lighting): each shadow VIEW is a
    // square of the shadow atlas (Renderer::shadowAtlasSize) holding one depth map - a spot light has one (perspective), a plane
    // light one (orthographic), a point light six (the cube faces +x, -x, +y, -y, +z, -z). localShadowBase[light]: x = its first view
    // + 1 (0 = no shadow), y = its view count, z = its shadow-light index (the casters' mask bit), w = 1 when its map holds
    // characters (a non-static light: tr::PlacedLight::isStatic).
    static constexpr int maxLocalShadows = 72;      // views: at most 12 shadow lights (the game's cap) x 6 faces
    int localShadowCount = 0;
    Vec4f localShadowBase[maxLocalLights] = {};
    Vec4f localBackColour[maxLocalLights] = {};     // placed light i's LightBackColor x intensity (plane lights; in the slot buffer)
    // Shadow lights (bit = shadow-light index) whose views are drawn this frame; the others keep what the atlas holds (a static
    // light's map is drawn once, as the game does - see SceneView::lighting).
    uint32_t localShadowRedraw = 0xFFFFFFFFu;
    Mat4 localShadowMatrix[maxLocalShadows];      // world -> the view's clip space
    Vec4f localShadowView[maxLocalShadows] = {};  // xyz: the view's axis, w: -dot(axis, eye) - linear depth = dot(axis, p) + w
    Vec4f localShadowRect[maxLocalShadows] = {};  // x, y: the square's first texel in the atlas, z: its size in texels, w: shadow-light index
    Vec4f localShadowParams[maxLocalShadows] = {};   // x near, y far, z ShadowOpacity, w ShadowSoftness
    Vec4f localShadowBias[maxLocalShadows] = {};     // x scale, y min, z max of the depth bias; w: 1 = orthographic (camera-depth bias)
    Vec4f localShadowCamera[maxLocalShadows] = {};   // orthographic views: the camera's forward, w -dot(forward, eye)
};

// Creates the GPU device, preferring Vulkan (the same SPIR-V path on Windows, Linux and macOS via MoltenVK); falls back
// to SDL's own choice (Direct3D 12 on Windows) if Vulkan is unavailable. SDL_GPU_DRIVER, when set, still wins.
SDL_GPUDevice* createGpuDevice(SDL_GPUShaderFormat formats, bool debug);

// Offscreen scene renderer for the viewport: colour + depth targets; textured-mesh, line and terrain pipelines.
class Renderer {
public:
    explicit Renderer(SDL_GPUDevice* device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    SDL_GPUDevice* device() const { return device_; }
    // (Re)creates the render targets when the size changes. Call before the frame's draw lists
    // reference colourTarget().
    void resize(uint32_t width, uint32_t height);
    SDL_GPUTexture* colourTarget() const { return colour_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    GpuMesh createMesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, bool lines);
    void destroyMesh(GpuMesh& mesh);
    // How many placed-light shadow passes this renderer has recorded: SceneView counts a cached map as drawn once this moves on.
    uint64_t localShadowPassesRecorded() const { return localShadowPasses_; }
    // GPU skinning: a storage buffer of SkinVertex per mesh vertex; the frame's bone palettes (addBones, per frame, returns the
    // palette's first row); setSkin applies to every draw call until clearSkin (skinNormals false: the normal stays as stored,
    // e.g. TRAdditiveAnisoSpec pieces, whose game shader skins only the position).
    SDL_GPUBuffer* createSkinBuffer(const std::vector<SkinVertex>& vertices);
    void destroyBuffer(SDL_GPUBuffer*& buffer);
    uint32_t addBones(const std::vector<Mat4>& skinMatrices);
    void setSkin(SDL_GPUBuffer* skinVertices, uint32_t boneBase, bool skinNormals);
    void clearSkin() { currentSkin_ = nullptr; }

    // Frame: begin, queue draws, then record everything into a command buffer.
    void beginFrame(const Mat4& viewProjection, const tr::Vec3& cameraPosition, const SceneLighting& lighting);
    // The view's axes in world space (refraction works in view space); call after beginFrame.
    void setCameraAxes(const tr::Vec3& right, const tr::Vec3& up, const tr::Vec3& forward) { cameraRight_ = right; cameraUp_ = up; cameraForward_ = forward; }
    // A refraction shell, drawn after the opaque geometry over a copy of the frame (alpha blended, depth written, two-sided).
    void drawRefract(const GpuMesh& mesh, const Mat4& model, const RefractMaterial& material);
    // A water surface (TRWaterReflectRefract), drawn after the opaque geometry over a copy of the frame, sampling the scene depth.
    void drawWater(const GpuMesh& mesh, const Mat4& model, const WaterMaterial& material);
    // The view's projection (for view depths from the depth buffer); call after beginFrame.
    void setProjection(const Mat4& projection) { projection_ = projection; }
    // texture nullptr draws untextured (white); alphaTest discards texels with alpha below alphaReference.
    // hue (w = 1) adds hue.rgb weighted by the texture alpha, as the game's "Hue" effects do (avatar skin, hair, clothing).
    // glow (sampled with the second UV set) is added after lighting, as the game's glow buffer is.
    // twoSided: no back-face culling for this draw (effects whose passes set CULLMODE = NONE, see SceneView).
    // Whether later draw() calls go into the sun shadow map (default on).
    void setShadowCasting(bool on) { shadowCasting_ = on; }
    void draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture = nullptr, bool alphaTest = false,
              const Vec4f& hue = Vec4f{0, 0, 0, 0}, SDL_GPUTexture* glow = nullptr, bool twoSided = false, float alphaReference = 0.5f,
              SDL_GPUTexture* normalMap = nullptr, SDL_GPUTexture* glossMap = nullptr, bool environment = false,
              const TrMaterial* trMaterial = nullptr, bool hairCutout = false, SDL_GPUTexture* palDecal = nullptr);
    // Back faces of opaque lit draws are culled when true, as in the game: PalGlobalEffect.fx sets CULLMODE = CCW and
    // is begun without state saving (D3DXFX_DONOTSAVESTATE), so it is the baseline; material effects leave it alone
    // except the few that set NONE. false draws everything two-sided (comparison / debugging).
    bool cullBackFaces = true;
    // Alpha-blended (opacity = tint.w x vertex alpha x texture alpha), lit, drawn after all opaque draws, without depth
    // writes: water, and pieces in the game's Translucent phase (SRCALPHA / INVSRCALPHA, PhaseTranslucent.fx).
    // detail: the forward pieces' detail layer (added around 0.5), sampled with UV set detailSet (0 / 1 / 2).
    // uvTransforms (forward pieces): diffuse = UV set 0 x transform 0, detail = UV set detailSet x transform detailSet
    // (vsBasicStatic2TexVertColor); only its transform fields are read. Null: the UVs as stored.
    // cullBack (translucent, additive, aniso): back faces culled when cullBackFaces is on - every phase and TRAdditive*
    // effect sets no CULLMODE, so PalGlobalEffect's CCW applies (fxcull.py, 2026-10-13); water (TRWaterReflectRefract) and
    // the four NONE effects are two-sided.
    void drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture = nullptr,
                         SDL_GPUTexture* detail = nullptr, int detailSet = 0, bool cullBack = false,
                         const TrMaterial* uvTransforms = nullptr);
    // A TR piece drawn with its forward technique while its body is in the distance fade band (SceneView, "fading" in
    // drawModelPieces): the translucent pipeline (SRCALPHA / INVSRCALPHA, no depth writes), sky lights only (no placed lights),
    // alpha = lerp(diffuse.a, modifier.a, vertex alpha) - TrMaterial flags.w bit 0x10.
    void drawFadingForward(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, const TrMaterial& material, bool cullBack);
    // Additive (one, one), unlit, faded by fog, drawn last without depth writes: the game's TRAdditive* effects.
    // lit: a forward-phase piece in the Additive phase (lit per vertex, opacity x tint.w = MatDiffuse alpha).
    void drawAdditive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, bool lit = false,
                      SDL_GPUTexture* detail = nullptr, int detailSet = 0, bool cullBack = false,
                      const TrMaterial* uvTransforms = nullptr);
    // The game's TRAdditiveAnisoSpec* effects (holograms): additive, with detail/decal layers and a view-angle light ramp.
    void drawAnisoAdditive(const GpuMesh& mesh, const Mat4& model, const AnisoMaterial& material, bool cullBack = false, bool sky = false);
    // Sky layers (TRasa::SkyLayer bodies in the Sky phase, see SceneView::queueSky): drawn after the opaque geometry and before
    // water / translucent / additive pieces, only on background pixels - the game's Sky.fx pass tests stencil EQUAL 255; here
    // the vertex shader puts the sky at the far plane (z = w) and the LESS_OR_EQUAL depth test passes only where the depth
    // buffer still holds its clear value. drawSkyEmissive: the TREmissive* forward shader (psNormal): rgb = texture x vertex colour x
    // tint.rgb (= MatDiffuse x 4; MatEmissive's term is 0), alpha = texture alpha x vertex alpha x tint.a (MatDiffuse.a), unlit, no
    // fog, blended SRCALPHA / INVSRCALPHA without alpha test (the device state at the game's dome draw, fountain trace call 37333047).
    void drawSkyEmissive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, bool cullBack);
    void drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material);
    // Particle quads (world-space triangle list, see ParticleSim.h). blendType as tr::particleBlendType: 0 additive (the
    // game's SRCALPHA/ONE/ADD), 3 subtractive (reverse subtract); 1 multiply and 2 diffuse are alpha blended. Unlit,
    // faded by fog, drawn last in queue order without depth writes.
    // fog false: drawn without fog (TRSphBillboard sprites: their pass sets FOGENABLE 0).
    void drawParticles(const std::vector<MeshVertex>& vertices, SDL_GPUTexture* texture, uint32_t blendType, bool fog = true);
    // Placed lights' globe maps (GlobeMapFile cube maps), cube k at index k + 1 (index 0 = white = no globe); 6 faces
    // per cube. Call between frames (releases the previous texture).
    void setGlobeMaps(const std::vector<std::vector<tr::Rgba8>>& faces, uint32_t size);
    // Spot lights' 2D globe maps (a GlobeMapFile that is a plain 2D texture, e.g. globemap_human_flashlight.dds: the TRSpotLight globe
    // variants project it - uv = (x / z, -y / z) x LightProjDistance + 0.5 in the light's frame), layer k at index k + 1 (0 = white).
    void setCookieMaps(const std::vector<std::vector<tr::Rgba8>>& layers, uint32_t size);
    // A shadow caster for placed lights this frame (the map's bodies within a shadowed light's range; SceneView chooses them):
    // lightMask bit s = drawn into slot s. The GPU skin set by setSkin applies. alphaTexture non-null: texels below alphaReference cut.
    // Back faces are culled (the game's shadow pass has CULLMODE CCW, as its main pass: the replayed device state at a flashlight's map,
    // tabula_rasa.trace 2026-10-17) except for two-sided pieces and terrain (Terrain_ShadowMap sets NONE).
    void drawLocalShadowCaster(const GpuMesh& mesh, const Mat4& model, SDL_GPUTexture* alphaTexture, float alphaReference, bool terrain, bool twoSided,
                               uint32_t lightMask);
    static constexpr uint32_t shadowAtlasSize = 4096;     // shadow views packed in 128-texel units (SceneView::lighting)
    // The global environment cube (the game's gGlobalEnvTexture) that Pal*Env* pieces reflect; null = a black cube.
    // Not owned (SceneView keeps its region cubes); set each frame before record().
    void setEnvironmentMap(SDL_GPUTexture* cube) { environment_ = cube; }
    // Sun shadow map (depth from the sun, orthographic, centred on the camera, texel-snapped).
    static constexpr uint32_t shadowSize = 2048;
    // The game's sun shadow map (fitted to 3,868 frames of an apitrace capture at Alia Das, tabula_rasa.3.trace, 2026-10-14, with
    // Client.Graphics.ShadowDistance 100): orthographic, 2048 x 2048, half width 35.84 m (constant in every frame), depth range
    // 400 m (|row 2| = 0.0025), centred in the light's plane on the camera + 27.7 m along its HORIZONTAL view direction (pitch
    // ignored: fitted over both Alia Das captures, a level camera and one pitched up 39 degrees - median residual 0.34 m, max 0.89
    // as the stepped sun direction changes; the projected 3D view direction leaves 6 - 8 m), not texel-snapped.
    static constexpr float shadowExtent = 35.84f;   // half width in metres
    static constexpr float shadowDepth = 400.0f;
    static constexpr float shadowAhead = 27.7f;     // centre: metres ahead of the camera, horizontally
    void record(SDL_GPUCommandBuffer* commands);

    // PVS builder (PvsBuilder.h, shaders/pvs.hlsl): one view of the game's BuildPVS. setPvsTree uploads the .vis BSP and sizes the
    // per-leaf buffers; recordPvsView draws the queued opaque draws (the shadow pass's set: lit, triangles - terrain holes and
    // alpha cut-outs discard) depth-only into the depth target, back faces culled except two-sided pieces, then counts per leaf the
    // pixels whose ray enters its box in front of the scene and sets the row bit of every leaf over the threshold (26, the game's
    // occlusion query test). The row accumulates over views until clearPvsRow; downloadPvsRow waits for the GPU.
    struct PvsNodeGpu {
        float minX, minY, minZ, split;
        float maxX, maxY, maxZ, pad;
        int32_t axis, child0, child1, leaf;
    };
    void setPvsTree(const std::vector<PvsNodeGpu>& nodes, uint32_t leafCount);
    void recordPvsView(const Mat4& inverseViewProjection, const tr::Vec3& eye, uint32_t threshold);
    void clearPvsRow();
    std::vector<uint32_t> downloadPvsRow();

    size_t queuedDraws() const { return draws_.size() + translucentDraws_.size() + additiveDraws_.size() + particleBatches_.size(); }
    size_t queuedParticleVertices() const { return particleVertices_.size(); }

private:
    struct DrawItem {
        const GpuMesh* mesh;
        Mat4 model;
        Vec4f tint;
        bool lit;
        SDL_GPUTexture* texture;
        bool alphaTest;
        const TerrainMaterial* terrain;          // non-null: terrain draw
        bool translucent;
        Vec4f hue;
        SDL_GPUTexture* glow;                    // never null for scene draws (black_ when none)
        bool additive = false;
        SDL_GPUTexture* detail = nullptr;        // aniso additive layers (null: defaults bound)
        SDL_GPUTexture* decal = nullptr;
        SDL_GPUTexture* light = nullptr;
        Vec4f specular = Vec4f{0, 0, 0, 0};
        Vec4f layers = Vec4f{0, 0, 0, 0};
        bool aniso = false;
        bool anisoPlain = false;                 // aniso: AnisoMaterial::plain
        bool forwardLit = false;                 // lit additive (forward-phase piece)
        bool twoSided = false;
        SDL_GPUBuffer* skinVertices = nullptr;   // GPU skinning (setSkin); null = as stored
        Mat4 anisoNormal = identityMatrix();     // aniso: AnisoMaterial::normalMatrix
        uint32_t boneBase = 0;
        bool skinNormals = true;
        bool cullBlended = false;                // translucent / additive draw with back faces culled (see drawTranslucent)
        float alphaReference = 0.5f;             // alpha test: texels below it are discarded
        SDL_GPUTexture* normalMap = nullptr;     // Pal* materials: normal map (tangent space) and gloss map
        SDL_GPUTexture* glossMap = nullptr;
        bool environment = false;                // Pal*Env* material: reflects the environment cube by gloss.b
        TrMaterial tr;                           // TR* deferred material (tr.active)
        bool trHairCutout = false;               // TRHair: kill where the normal map's alpha < 0.5
        bool castsShadow = true;                 // drawn into the sun shadow map (see setShadowCasting)
        bool sky = false;                        // sky layer: at the far plane (drawSkyEmissive / drawAnisoAdditive(..., sky))
        bool forwardUv = false;                  // forward piece with TexCoordTransforms in tr (trFlags.w bit 0x20)
    };
    SDL_GPUShader* createShader(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                const char* entryPoint, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t storageBuffers = 0);
    SDL_GPUGraphicsPipeline* createPipeline(SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader, SDL_GPUPrimitiveType primitive,
                                            int blend = 0, bool cullBack = false, bool cullFront = false, bool depthTarget = true);
    void recordItem(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass, const DrawItem& item, SDL_GPUGraphicsPipeline*& bound);
    // Direct3D 12 descriptor budget (SDL 3.4's D3D12 backend: one 2,048-sampler / 65,536-view GPU descriptor heap pair per
    // command buffer, and its overflow handling corrupts the frame - see CLAUDE.md "D3D12 crash"). Before each draw: true when the
    // draw's descriptors would not fit, after which the pass continues in a new command buffer (the caller rebinds its pipeline).
    bool reserveDescriptors(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass, uint32_t samplers, uint32_t storageBuffers);
    SDL_GPURenderPass* beginPass(SDL_GPUCommandBuffer* commands, const SDL_GPUColorTargetInfo* colour, SDL_GPUDepthStencilTargetInfo* depth);
    bool splitCommandBuffers_ = false;         // Direct3D 12 only
    uint32_t samplerDescriptors_ = 0, viewDescriptors_ = 0;
    SDL_GPUColorTargetInfo passColour_ = {};
    SDL_GPUDepthStencilTargetInfo passDepth_ = {};
    bool passHasColour_ = false, passHasDepth_ = false;

    SDL_GPUDevice* device_;
    SDL_GPUShader* sceneVertex_ = nullptr;
    SDL_GPUShader* sceneFragment_ = nullptr;
    SDL_GPUShader* terrainVertex_ = nullptr;
    SDL_GPUShader* terrainFragment_ = nullptr;
    SDL_GPUGraphicsPipeline* trianglePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* culledTrianglePipeline_ = nullptr;   // back faces culled
    SDL_GPUGraphicsPipeline* linePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* terrainPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* culledTerrainPipeline_ = nullptr;   // back faces culled
    SDL_GPUGraphicsPipeline* translucentPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* refractPipeline_ = nullptr;
    SDL_GPUShader* refractFragment_ = nullptr;
    SDL_GPUTexture* frameCopy_ = nullptr;      // the colour target after opaque geometry, sampled by refraction
    struct RefractItem {
        const GpuMesh* mesh;
        Mat4 model;
        RefractMaterial material;
        SDL_GPUBuffer* skinVertices = nullptr;
        uint32_t boneBase = 0;
        bool skinNormals = true;
    };
    void uploadBones(SDL_GPUCommandBuffer* commands);
    void uploadLocalShadows(SDL_GPUCommandBuffer* commands);
    SDL_GPUBuffer* localShadowBuffer_ = nullptr;           // placed-light shadow slots (fragment storage buffer)
    SDL_GPUTransferBuffer* localShadowTransfer_ = nullptr;
    void bindSkin(SDL_GPURenderPass* pass, SDL_GPUBuffer* skinVertices);
    std::vector<Vec4f> bones_;                 // this frame's bone palettes, 3 rows per bone
    SDL_GPUBuffer* boneBuffer_ = nullptr;
    SDL_GPUTransferBuffer* boneTransfer_ = nullptr;
    uint32_t boneCapacity_ = 0;                // rows
    SDL_GPUBuffer* skinDummy_ = nullptr;       // bound for draws that are not skinned
    SDL_GPUBuffer* currentSkin_ = nullptr;
    uint32_t currentBoneBase_ = 0;
    bool currentSkinNormals_ = true;
    std::vector<RefractItem> refractDraws_;
    struct WaterItem {
        const GpuMesh* mesh;
        Mat4 model;
        WaterMaterial material;
    };
    std::vector<WaterItem> waterDraws_;
    SDL_GPUGraphicsPipeline* waterPipeline_ = nullptr;
    SDL_GPUShader* waterVertex_ = nullptr;
    SDL_GPUShader* waterFragment_ = nullptr;
    SDL_GPUSampler* volumeSampler_ = nullptr;
    SDL_GPUTexture* whiteVolume_ = nullptr;  // linear, wrap u / v, clamp w (the water's volume map)
    Mat4 projection_ = identityMatrix();
    void recordWater(SDL_GPUCommandBuffer*& commands);
    tr::Vec3 cameraRight_{1, 0, 0}, cameraUp_{0, 1, 0}, cameraForward_{0, 0, 1};
    SDL_GPUGraphicsPipeline* additivePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* culledAdditivePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* culledTranslucentPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* subtractivePipeline_ = nullptr;   // particles: destination - source
    // Particles: this frame's vertices (uploaded in record()) and their draws.
    struct ParticleBatch {
        uint32_t first = 0;
        uint32_t count = 0;
        SDL_GPUTexture* texture = nullptr;
        uint32_t blendType = 0;
        bool fog = true;
    };
    std::vector<MeshVertex> particleVertices_;
    std::vector<ParticleBatch> particleBatches_;
    SDL_GPUBuffer* particleBuffer_ = nullptr;
    SDL_GPUTransferBuffer* particleTransfer_ = nullptr;
    uint32_t particleCapacity_ = 0;            // vertices
    void uploadParticles(SDL_GPUCommandBuffer* commands);
    void recordParticles(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass);
    SDL_GPUShader* shadowFragment_ = nullptr;
    SDL_GPUGraphicsPipeline* shadowPipeline_ = nullptr;
    SDL_GPUTexture* shadowMap_ = nullptr;
    SDL_GPUTexture* globes_ = nullptr;            // cube map array; cube 0 white
    SDL_GPUTexture* cookies_ = nullptr;           // 2D array; layer 0 white
    SDL_GPUTexture* shadowAtlas_ = nullptr;       // placed-light shadow maps (depth)
    SDL_GPUGraphicsPipeline* localShadowPipeline_ = nullptr;           // back faces culled
    SDL_GPUGraphicsPipeline* localShadowTwoSidedPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* localShadowClearPipeline_ = nullptr;      // depth 1 over a view's square (ALWAYS, depth clamped)
    GpuMesh localShadowClearQuad_;
    bool localShadowAtlasCleared_ = false;      // the atlas starts undefined: cleared by the first shadow pass
    uint64_t localShadowPasses_ = 0;            // shadow passes recorded (localShadowPassesRecorded)
    struct LocalCaster {
        const GpuMesh* mesh;
        Mat4 model;
        SDL_GPUTexture* alphaTexture;
        float alphaReference;
        bool terrain;
        bool twoSided;
        uint32_t lightMask;
        SDL_GPUBuffer* skinVertices;
        uint32_t boneBase;
    };
    std::vector<LocalCaster> localCasters_;
    void recordLocalShadows(SDL_GPUCommandBuffer*& commands);
    SDL_GPUTexture* environment_ = nullptr;       // not owned
    SDL_GPUTexture* blackCube_ = nullptr;         // stand-in when there is no environment cube
    SDL_GPUSampler* shadowSampler_ = nullptr;     // comparison sampler
    Mat4 shadowMatrix_;
    bool shadowsActive_ = false;
    void recordShadows(SDL_GPUCommandBuffer*& commands);
    SDL_GPUSampler* linearSampler_ = nullptr;   // trilinear, repeat
    SDL_GPUSampler* pointSampler_ = nullptr;    // nearest, clamp (terrain data textures)
    SDL_GPUSampler* clampSampler_ = nullptr;    // trilinear, clamp (lookup ramps)
    SDL_GPUTexture* white_ = nullptr;
    SDL_GPUTexture* black_ = nullptr;          // default glow
    SDL_GPUTextureFormat colourFormat_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUTexture* colour_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    Mat4 viewProjection_;
    Vec4f towardsLight_;
    SDL_FColor clearColour_ = {0, 0, 0, 1};
    SceneLighting lighting_;
    Vec4f cameraPosition_;
    std::vector<DrawItem> draws_;
    bool shadowCasting_ = true;
    std::vector<DrawItem> translucentDraws_;
    std::vector<DrawItem> additiveDraws_;
    std::vector<DrawItem> skyDraws_;

    // PVS builder state (created by setPvsTree).
    SDL_GPUGraphicsPipeline* pvsDepthPipeline_ = nullptr;          // back faces culled
    SDL_GPUGraphicsPipeline* pvsDepthTwoSidedPipeline_ = nullptr;
    SDL_GPUComputePipeline* pvsCountPipeline_ = nullptr;
    SDL_GPUComputePipeline* pvsMarkPipeline_ = nullptr;
    SDL_GPUBuffer* pvsNodes_ = nullptr;
    SDL_GPUBuffer* pvsCounts_ = nullptr;
    SDL_GPUBuffer* pvsBits_ = nullptr;
    SDL_GPUTransferBuffer* pvsDownload_ = nullptr;
    uint32_t pvsLeafCount_ = 0;
    void createPvsPipelines();
    SDL_GPUComputePipeline* createComputePipeline(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                                  const char* entryPoint, uint32_t samplers, uint32_t readOnlyBuffers, uint32_t readWriteBuffers,
                                                  uint32_t threadsX, uint32_t threadsY);
    void uploadBuffer(SDL_GPUBuffer* buffer, const void* data, uint32_t size);
};
