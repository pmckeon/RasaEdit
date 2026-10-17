// PVS builder (PvsBuilder.h): what the game's BuildPVS (tabula_rasa.exe FUN_0085a1d0) learns from one 512 x 512 view with
// occlusion queries - each leaf's box drawn with the depth test over the scene, a leaf seen when more than 26 pixels pass
// (FUN_0085b7b0). Back faces are culled (PalGlobalEffect's CCW baseline), so a pixel counts for a box when the view ray
// enters the box behind the near plane and no further than the scene depth there (LESS_OR_EQUAL). The BSP's leaves tile
// the root exactly (every child is its parent's half on the split - all 991,522 children in the 77 shipped .vis files),
// so walking the tree along each pixel's ray finds exactly the boxes the ray enters, in order.
//
// Compute resources as SDL GPU binds them: space0 = sampled textures then read-only storage buffers, space1 = read-write
// storage buffers, space2 = uniforms (DXC maps register spaces to SPIR-V descriptor sets, so one source serves both).

struct PvsNode {
    float4 boundsMin;    // w: split position (-d of the file)
    float4 boundsMax;
    int4 info;           // x: split axis (3 = leaf), y: child 0 (the half at or above the split), z: child 1 (below), w: leaf index
};

[[vk::combinedImageSampler]] Texture2D<float> depthTexture : register(t0, space0);
[[vk::combinedImageSampler]] SamplerState depthSampler : register(s0, space0);
StructuredBuffer<PvsNode> nodes : register(t1, space0);
RWStructuredBuffer<uint> counts : register(u0, space1);   // pixels per leaf in this view
RWStructuredBuffer<uint> bits : register(u1, space1);     // the row being built: leaf i = bit (i & 31) of word (i >> 5)

cbuffer PvsUniforms : register(b0, space2)
{
    row_major float4x4 inverseViewProjection;
    float4 eye;          // xyz camera position
    uint4 sizes;         // x width, y height, z leaf count, w pixel threshold (26)
};

float3 unproject(float2 ndc, float depth)
{
    const float4 p = mul(inverseViewProjection, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}

// Entry distance of the ray into a box (slab test); returns a value beyond tMax when it misses.
float boxEntry(float3 origin, float3 inverseDirection, float3 lo, float3 hi, float tMax)
{
    const float3 t0 = (lo - origin) * inverseDirection;
    const float3 t1 = (hi - origin) * inverseDirection;
    const float3 tSmall = min(t0, t1);
    const float3 tBig = max(t0, t1);
    const float enter = max(max(tSmall.x, tSmall.y), tSmall.z);
    const float leave = min(min(tBig.x, tBig.y), tBig.z);
    return enter <= leave ? enter : tMax + 1.0;
}

[numthreads(8, 8, 1)]
void csCount(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= sizes.x || id.y >= sizes.y) return;
    const float2 ndc = float2((float(id.x) + 0.5) / float(sizes.x) * 2.0 - 1.0, 1.0 - (float(id.y) + 0.5) / float(sizes.y) * 2.0);
    const float depth = depthTexture.Load(int3(id.xy, 0));
    const float3 nearPoint = unproject(ndc, 0.0);
    const float3 scenePoint = unproject(ndc, depth);
    const float3 origin = eye.xyz;
    const float3 toNear = nearPoint - origin;
    const float3 direction = normalize(scenePoint - origin);
    const float tNear = dot(toNear, direction);
    const float tScene = length(scenePoint - origin);
    const float3 inverseDirection = 1.0 / direction;      // +-inf on axis-parallel rays: the slab tests still work

    // Walk the tree over [tNear, tScene]; a small stack of (node, t0, t1).
    uint stackNode[48];
    float stackT0[48];
    float stackT1[48];
    int top = 0;
    {
        const PvsNode root = nodes[0];
        const float3 t0 = (root.boundsMin.xyz - origin) * inverseDirection;
        const float3 t1 = (root.boundsMax.xyz - origin) * inverseDirection;
        const float3 tSmall = min(t0, t1);
        const float3 tBig = max(t0, t1);
        const float a = max(max(max(tSmall.x, tSmall.y), tSmall.z), tNear);
        const float b = min(min(min(tBig.x, tBig.y), tBig.z), tScene);
        if (a > b) return;
        stackNode[0] = 0;
        stackT0[0] = a;
        stackT1[0] = b;
        top = 1;
    }
    [loop]
    while (top > 0) {
        --top;
        uint index = stackNode[top];
        float a = stackT0[top];
        float b = stackT1[top];
        [loop]
        for (uint guard = 0; guard < 64; ++guard) {
            const PvsNode node = nodes[index];
            if (node.info.x == 3) {
                // The leaf's own box: counted when the ray enters it behind the near plane, in front of the scene.
                const float enter = boxEntry(origin, inverseDirection, node.boundsMin.xyz, node.boundsMax.xyz, tScene);
                if (enter > tNear && enter <= tScene && node.info.w >= 0 && uint(node.info.w) < sizes.z)
                    InterlockedAdd(counts[node.info.w], 1u);
                break;
            }
            const uint axis = uint(node.info.x);
            const float o = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
            const float d = axis == 0 ? direction.x : axis == 1 ? direction.y : direction.z;
            const float split = node.boundsMin.w;
            const uint above = uint(node.info.y), below = uint(node.info.z);
            // The half holding the ray's start first; the other only if the ray crosses the split inside [a, b].
            const bool startAbove = o > split || (o == split && d >= 0.0);
            const uint first = startAbove ? above : below;
            const uint second = startAbove ? below : above;
            if (d == 0.0) { index = first; continue; }
            const float tSplit = (split - o) / d;
            if (tSplit >= b || tSplit < 0.0) { index = first; continue; }
            if (tSplit <= a) { index = second; continue; }
            if (top < 48) {
                stackNode[top] = second;
                stackT0[top] = tSplit;
                stackT1[top] = b;
                ++top;
            }
            index = first;
            b = tSplit;
        }
    }
}

[numthreads(64, 1, 1)]
void csMark(uint3 id : SV_DispatchThreadID)
{
    const uint leaf = id.x;
    if (leaf >= sizes.z) return;
    if (counts[leaf] > sizes.w) InterlockedOr(bits[leaf >> 5], 1u << (leaf & 31u));
    counts[leaf] = 0;
}
