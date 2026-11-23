#include "city_renderer.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace city {

using namespace DirectX;

namespace {

struct SkyVertex {
    XMFLOAT3 pos;
};

struct CityVertex {
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    XMFLOAT4 color;
    XMFLOAT4 meta; // x = building_id, y = part_type, z = b_height, w = category_flag
};

struct GroundVertex {
    XMFLOAT3 pos;
    XMFLOAT2 uv;
};

struct ParticleVertex {
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    XMFLOAT4 color;
    XMFLOAT4 meta; // x = kind_or_softness, y = emissive_boost
};

struct alignas(16) FrameCB {
    XMFLOAT4X4 view_proj;
    XMFLOAT4 cam_pos;
    XMFLOAT4 time_and_fog;  // x = time, y = fog_start, z = fog_end, w = 0
    XMFLOAT4 highlight_ids; // x = hovered_id, y = selected_id, z = 0, w = 0
    XMFLOAT4 strikes[4];    // x = building_id, y = impact_y, z = burn_intensity, w = collapse_t
};

const char* kSkyShaderSrc = R"(
cbuffer FrameCB : register(b0) {
    float4x4 gViewProj;
    float4 gCamPos;
    float4 gTimeFog;
    float4 gHighlightIds;
    float4 gStrikes[4];
};

struct VSIn {
    float3 pos : POSITION;
};

struct PSIn {
    float4 sv_pos : SV_POSITION;
    float3 dir    : TEXCOORD0;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    float3 worldPos = input.pos * 440.0f + gCamPos.xyz;
    o.sv_pos = mul(float4(worldPos, 1.0f), gViewProj);
    o.sv_pos.z = o.sv_pos.w * 0.99999f;
    o.dir = input.pos;
    return o;
}

float Hash31(float3 p) {
    p = frac(p * float3(443.8975f, 397.2973f, 491.1871f));
    p += dot(p, p.yxz + 19.19f);
    return frac((p.x + p.y) * p.z);
}

float Hash11(float p) {
    p = frac(p * 0.1031f);
    p *= p + 33.33f;
    p *= p + p;
    return frac(p);
}

float4 PSMain(PSIn input) : SV_TARGET {
    float3 d = normalize(input.dir);
    float h = saturate(d.y);

    float3 zenith  = float3(0.012f, 0.020f, 0.048f);
    float3 midSky  = float3(0.032f, 0.056f, 0.115f);
    float3 horizon = float3(0.075f, 0.125f, 0.210f);

    float3 col = lerp(horizon, midSky, smoothstep(0.0f, 0.28f, h));
    col = lerp(col, zenith, smoothstep(0.22f, 0.88f, h));

    // Subtle urban glow near horizon
    float cityGlow = exp(-abs(d.y) * 16.0f);
    col += float3(0.06f, 0.14f, 0.24f) * cityGlow * 0.65f;

    // Moon & atmospheric halo
    float3 moonDir = normalize(float3(0.45f, 0.56f, -0.35f));
    float mDot = saturate(dot(d, moonDir));
    float moonDisc = smoothstep(0.9991f, 0.9996f, mDot);
    float moonHalo = pow(mDot, 24.0f) * 0.25f + pow(mDot, 140.0f) * 0.55f;
    col += float3(0.70f, 0.84f, 1.00f) * moonHalo;
    col += float3(0.95f, 0.97f, 1.00f) * moonDisc * 1.4f;

    // Stars in upper hemisphere
    if (d.y > 0.04f) {
        float3 grid = floor(d * 210.0f);
        float rnd = Hash31(grid);
        if (rnd > 0.992f) {
            float3 centerDir = normalize(grid + 0.5f);
            float dist = length(d - centerDir) * 210.0f;
            float star = smoothstep(0.42f, 0.06f, dist);
            float twinkle = 0.65f + 0.35f * sin(gTimeFog.x * (1.8f + rnd * 5.0f) + rnd * 100.0f);
            col += float3(0.75f, 0.86f, 1.0f) * star * twinkle * smoothstep(0.04f, 0.22f, d.y);
        }
    }

    // Distant horizon city skyline silhouette
    if (d.y > -0.02f && d.y < 0.075f) {
        float ang = atan2(d.z, d.x);
        float b1 = floor(ang * 48.0f);
        float b2 = floor(ang * 95.0f);
        float h1 = Hash11(b1) * 0.048f + 0.008f;
        float h2 = Hash11(b2) * 0.032f + 0.005f;
        float silH = max(h1, h2);
        if (d.y < silH) {
            float3 silCol = float3(0.018f, 0.028f, 0.052f);
            float winMask = step(0.84f, Hash11(b2 * 13.0f + floor(d.y * 420.0f)));
            silCol += float3(0.35f, 0.55f, 0.85f) * winMask * 0.16f * step(0.005f, d.y);
            float fade = smoothstep(-0.01f, 0.01f, d.y);
            col = lerp(col, silCol, fade * 0.78f);
        }
    }

    return float4(col, 1.0f);
}
)";

const char* kBuildingShaderSrc = R"(
cbuffer FrameCB : register(b0) {
    float4x4 gViewProj;
    float4 gCamPos;
    float4 gTimeFog;
    float4 gHighlightIds;
    float4 gStrikes[4];
};

struct VSIn {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
    float4 meta   : TEXCOORD1;
};

struct PSIn {
    float4 sv_pos   : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float4 color    : COLOR0;
    float4 meta     : TEXCOORD3;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    o.sv_pos = mul(float4(input.pos, 1.0f), gViewProj);
    o.worldPos = input.pos;
    o.normal = input.normal;
    o.uv = input.uv;
    o.color = input.color;
    o.meta = input.meta;
    return o;
}

float Hash21(float2 p) {
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(PSIn input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 L = normalize(float3(0.45f, 0.72f, -0.35f));
    float3 V = normalize(gCamPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float bId = input.meta.x;
    float partType = input.meta.y;
    float bHeight = input.meta.z;
    float catFlag = input.meta.w;

    // Emissive fast path (neon trims, beacons, headlights, taillights, lamps, jet afterglow)
    if (partType > 1.5f && partType < 2.5f) {
        float3 em = input.color.rgb * 2.1f;
        if (bId > 0.5f && catFlag > 0.5f && catFlag < 1.5f) {
            em *= (0.65f + 0.45f * sin(gTimeFog.x * 5.5f + bId));
        }
        float dist = length(gCamPos.xyz - input.worldPos);
        float fog = saturate((dist - gTimeFog.y) / max(1.0f, gTimeFog.z - gTimeFog.y));
        float3 fogCol = float3(0.055f, 0.095f, 0.165f);
        return float4(lerp(ACESFilm(em), fogCol, fog * fog), 1.0f);
    }

    float ndotl = saturate(dot(N, L));
    float skyFill = 0.5f + 0.5f * N.y;
    float ao = lerp(0.48f, 1.0f, saturate(input.worldPos.y / 3.2f));
    float3 streetBounce = float3(0.08f, 0.14f, 0.24f) * saturate(1.0f - input.worldPos.y / 14.0f) * saturate(-N.y * 0.5f + 0.6f);

    float3 lighting = float3(0.18f, 0.23f, 0.34f) * skyFill + float3(0.85f, 0.90f, 1.00f) * ndotl * 0.78f + streetBounce;
    float3 baseCol = input.color.rgb * lighting * ao;
    float3 emissive = float3(0.0f, 0.0f, 0.0f);

    float fresnel = pow(1.0f - saturate(dot(N, V)), 3.0f);

    if (partType < 0.5f) {
        // Skyscraper curtain-wall facade
        float spec = pow(saturate(dot(N, H)), 48.0f) * 0.42f;
        float3 skyRefl = lerp(float3(0.06f, 0.12f, 0.22f), float3(0.22f, 0.42f, 0.68f), fresnel);
        baseCol = lerp(baseCol * 0.65f, skyRefl, 0.28f + 0.45f * fresnel) + float3(0.7f, 0.85f, 1.0f) * spec;

        if (abs(N.y) < 0.5f && bId > 0.5f) {
            float2 grid = float2(input.uv.x * 1.45f, input.uv.y * 1.22f);
            float2 cell = floor(grid);
            float2 f = frac(grid);

            // Mechanical dark floor band every 10 floors
            float mechFloor = step(0.1f, frac(cell.y / 10.0f));
            float inWin = step(0.18f, f.x) * step(f.x, 0.82f) * step(0.22f, f.y) * step(f.y, 0.80f) * mechFloor;
            float aboveLobby = step(1.1f, input.worldPos.y);

            float rnd = Hash21(cell + float2(bId * 13.7f, bId * 3.1f));
            float rnd2 = Hash21(cell * 1.7f + float2(bId * 5.3f, 9.2f));
            float lit = step(0.40f, rnd) * inWin * aboveLobby;

            // Subtle office blinds gradient inside lit windows
            float blindGrad = lerp(0.65f, 1.15f, 1.0f - f.y);
            float flicker = 0.88f + 0.12f * sin(gTimeFog.x * 2.1f + rnd * 45.0f);

            float3 warmWin = float3(1.00f, 0.84f, 0.56f);
            float3 coolWin = float3(0.55f, 0.88f, 1.00f);
            float3 accentWin = input.color.rgb * 1.65f + 0.20f;
            float3 winCol = lerp(warmWin, coolWin, step(0.48f, rnd2));
            winCol = lerp(winCol, accentWin, 0.35f);

            if (catFlag > 0.5f && catFlag < 1.5f) {
                float pulse = 0.65f + 0.35f * sin(gTimeFog.x * 4.5f + bId);
                winCol = lerp(float3(1.0f, 0.42f, 0.16f), float3(1.0f, 0.72f, 0.22f), rnd2) * pulse;
            } else if (catFlag > 2.5f && catFlag < 3.5f) {
                winCol = lerp(winCol, float3(0.20f, 0.96f, 0.85f), 0.55f);
            } else if (catFlag > 3.5f) {
                winCol = lerp(winCol, float3(0.95f, 0.45f, 1.00f), 0.48f);
            }
            emissive += winCol * lit * blindGrad * flicker * 0.92f;

            // Ground lobby glass band
            if (input.worldPos.y < 1.15f && input.worldPos.y > 0.22f) {
                float lobbyWin = step(0.15f, f.x) * step(f.x, 0.85f);
                emissive += float3(0.95f, 0.88f, 0.72f) * lobbyWin * 0.45f;
            }
        }
    } else if (partType < 1.5f) {
        // Architectural concrete / steel / podium / HVAC / street poles
        float spec = pow(saturate(dot(N, H)), 24.0f) * 0.18f;
        baseCol += float3(0.45f, 0.55f, 0.70f) * spec + float3(0.08f, 0.14f, 0.24f) * fresnel * 0.25f;
    } else if (partType > 2.5f && partType < 3.5f) {
        // Commercial jetliner metallic aircraft paint
        float spec = pow(saturate(dot(N, H)), 64.0f) * 0.65f;
        baseCol = input.color.rgb * (0.42f + 0.72f * ndotl + 0.22f * skyFill);
        baseCol += float3(0.85f, 0.92f, 1.0f) * (spec + fresnel * 0.35f);
    } else if (partType > 3.5f && partType < 4.5f) {
        // Urban foliage
        float wrap = saturate(dot(N, L) * 0.5f + 0.5f);
        baseCol = input.color.rgb * (0.35f + 0.65f * wrap) * ao;
    } else {
        // Solid debris / charred rubble
        float rough = Hash21(floor(input.worldPos.xz * 4.0f));
        baseCol *= (0.65f + 0.45f * rough);
        emissive += float3(0.85f, 0.22f, 0.04f) * step(0.75f, rough) * 0.35f;
    }

    // Active impact breach fire & scorch on targeted building
    if (bId > 0.5f) {
        [unroll]
        for (int k = 0; k < 4; ++k) {
            if (gStrikes[k].x > 0.5f && abs(bId - gStrikes[k].x) < 0.25f) {
                float impY = gStrikes[k].y;
                float burn = gStrikes[k].z;
                float dy = abs(input.worldPos.y - impY);
                if (burn > 0.01f && dy < 4.5f) {
                    float band = saturate(1.0f - dy / 4.5f);
                    float noise = Hash21(floor(input.worldPos.xz * 2.2f + input.worldPos.y * 1.5f) + floor(gTimeFog.x * 14.0f));
                    baseCol *= lerp(1.0f, 0.20f, band * burn);
                    float3 fireCol = lerp(float3(1.0f, 0.20f, 0.03f), float3(1.0f, 0.82f, 0.25f), noise);
                    emissive += fireCol * band * band * burn * (2.2f + 1.4f * noise);
                }
            }
        }
    }

    // Hover / Selected rim highlight (GPU-driven, zero VB rebuild!)
    if (bId > 0.5f) {
        bool isSel = abs(bId - gHighlightIds.y) < 0.25f;
        bool isHov = abs(bId - gHighlightIds.x) < 0.25f;
        if (isSel || isHov) {
            float3 hCol = isSel ? float3(1.0f, 0.84f, 0.26f) : float3(0.45f, 0.90f, 1.00f);
            float pulse = 0.78f + 0.22f * sin(gTimeFog.x * 5.5f);
            float scan = isSel ? pow(saturate(1.0f - abs(frac(input.worldPos.y * 0.06f - gTimeFog.x * 0.65f) - 0.5f) * 6.0f), 2.0f) * 0.45f : 0.0f;
            emissive += hCol * (0.18f + 0.78f * fresnel + scan) * pulse;
        }
    }

    float3 finalCol = ACESFilm(baseCol + emissive);

    // Distance fog into horizon glow
    float dist = length(gCamPos.xyz - input.worldPos);
    float fog = saturate((dist - gTimeFog.y) / max(1.0f, gTimeFog.z - gTimeFog.y));
    float3 fogCol = float3(0.055f, 0.095f, 0.165f);
    finalCol = lerp(finalCol, fogCol, fog * fog);

    return float4(finalCol, 1.0f);
}
)";

const char* kGroundShaderSrc = R"(
cbuffer FrameCB : register(b0) {
    float4x4 gViewProj;
    float4 gCamPos;
    float4 gTimeFog;
    float4 gHighlightIds;
    float4 gStrikes[4];
};

struct VSIn {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSIn {
    float4 sv_pos   : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    o.sv_pos = mul(float4(input.pos, 1.0f), gViewProj);
    o.worldPos = input.pos;
    return o;
}

float Hash21(float2 p) {
    p = frac(p * float2(234.34f, 435.345f));
    p += dot(p, p + 34.23f);
    return frac(p.x * p.y);
}

float4 PSMain(PSIn input) : SV_TARGET {
    float2 p = input.worldPos.xz;
    float2 fw = max(fwidth(p), float2(0.001f, 0.001f));

    // Base urban concrete / block ground
    float grain = Hash21(floor(p * 3.5f)) * 0.015f;
    float3 col = float3(0.036f, 0.048f, 0.074f) + grain;

    // Subtle city block tile grid
    float2 gBlock = abs(frac(p / 4.0f - 0.5f) - 0.5f) / (fw / 4.0f);
    float blockLine = 1.0f - min(min(gBlock.x, gBlock.y), 1.0f);
    col += float3(0.025f, 0.055f, 0.095f) * blockLine * 0.35f;

    // Road distances
    float dxMain = abs(p.x);
    float dzMain = abs(p.y);

    float dxSec = min(min(abs(abs(p.x) - 32.0f), abs(abs(p.x) - 64.0f)), abs(abs(p.x) - 96.0f));
    float dzSec = min(min(abs(abs(p.y) - 32.0f), abs(abs(p.y) - 64.0f)), abs(abs(p.y) - 96.0f));

    float inMainRoadX = 1.0f - smoothstep(3.75f, 3.90f, dxMain);
    float inMainRoadZ = 1.0f - smoothstep(3.75f, 3.90f, dzMain);
    float inSecRoadX  = 1.0f - smoothstep(2.25f, 2.38f, dxSec);
    float inSecRoadZ  = 1.0f - smoothstep(2.25f, 2.38f, dzSec);

    float inSideMainX = (1.0f - smoothstep(5.30f, 5.45f, dxMain)) * (1.0f - inMainRoadX);
    float inSideMainZ = (1.0f - smoothstep(5.30f, 5.45f, dzMain)) * (1.0f - inMainRoadZ);
    float inSideSecX  = (1.0f - smoothstep(3.50f, 3.65f, dxSec)) * (1.0f - inSecRoadX);
    float inSideSecZ  = (1.0f - smoothstep(3.50f, 3.65f, dzSec)) * (1.0f - inSecRoadZ);

    float inAnyRoad = max(max(inMainRoadX, inMainRoadZ), max(inSecRoadX, inSecRoadZ));
    float inAnySidewalk = saturate(max(max(inSideMainX, inSideMainZ), max(inSideSecX, inSideSecZ))) * (1.0f - inAnyRoad);

    // Sidewalks
    float3 sidewalkCol = float3(0.085f, 0.105f, 0.145f) + grain * 0.8f;
    col = lerp(col, sidewalkCol, inAnySidewalk * 0.92f);

    // Asphalt roads with subtle wet reflection
    float3 V = normalize(gCamPos.xyz - input.worldPos);
    float fresnel = pow(1.0f - saturate(V.y), 3.0f);
    float3 asphaltCol = float3(0.018f, 0.024f, 0.036f) + grain * 0.6f;
    asphaltCol += float3(0.05f, 0.12f, 0.22f) * fresnel * 0.55f;
    col = lerp(col, asphaltCol, inAnyRoad);

    // Double cyan/amber median & dashed lane lines on main boulevards
    float inIntersection = max(inMainRoadX, inSecRoadX) * max(inMainRoadZ, inSecRoadZ);
    float dashZ = step(0.45f, frac(p.y * 0.22f));
    float dashX = step(0.45f, frac(p.x * 0.22f));

    // Main boulevard glowing fiber-optic center line + data pulse
    float pulseZ = exp(-abs(frac(abs(p.y) * 0.035f + gTimeFog.x * 0.45f) - 0.5f) * 14.0f);
    float pulseX = exp(-abs(frac(abs(p.x) * 0.035f + gTimeFog.x * 0.45f) - 0.5f) * 14.0f);

    float centerMainX = (1.0f - smoothstep(0.10f, 0.18f, dxMain)) * (1.0f - inIntersection);
    float centerMainZ = (1.0f - smoothstep(0.10f, 0.18f, dzMain)) * (1.0f - inIntersection);
    col += float3(0.12f, 0.62f, 0.95f) * centerMainX * (0.45f + 0.95f * pulseZ);
    col += float3(0.12f, 0.62f, 0.95f) * centerMainZ * (0.45f + 0.95f * pulseX);

    // Lane dashes on main boulevards
    float laneMainX = (1.0f - smoothstep(0.05f, 0.11f, abs(dxMain - 1.85f))) * dashZ * (1.0f - inIntersection);
    float laneMainZ = (1.0f - smoothstep(0.05f, 0.11f, abs(dzMain - 1.85f))) * dashX * (1.0f - inIntersection);
    col += float3(0.42f, 0.48f, 0.58f) * (laneMainX + laneMainZ) * 0.48f;

    // Secondary avenue center dashes
    float centerSecX = (1.0f - smoothstep(0.05f, 0.11f, dxSec)) * dashZ * (1.0f - inIntersection);
    float centerSecZ = (1.0f - smoothstep(0.05f, 0.11f, dzSec)) * dashX * (1.0f - inIntersection);
    col += float3(0.55f, 0.46f, 0.20f) * (centerSecX + centerSecZ) * 0.45f;

    // Crosswalks near main intersections
    float zebraX = step(0.5f, frac(p.x * 1.4f));
    float zebraZ = step(0.5f, frac(p.y * 1.4f));
    float cwMainZ = inMainRoadX * step(4.1f, dzMain) * step(dzMain, 5.3f) * zebraX;
    float cwMainX = inMainRoadZ * step(4.1f, dxMain) * step(dxMain, 5.3f) * zebraZ;
    col += float3(0.45f, 0.50f, 0.60f) * max(cwMainZ, cwMainX) * 0.42f;

    // District boundary glow around [-96, 96]
    float boxDist = max(abs(p.x), abs(p.y));
    float border = smoothstep(0.9f, 0.0f, abs(boxDist - 96.0f));
    col += float3(0.15f, 0.68f, 0.98f) * border * 0.65f;

    float dist = length(gCamPos.xyz - input.worldPos);
    float fog = saturate((dist - gTimeFog.y) / max(1.0f, gTimeFog.z - gTimeFog.y));
    float3 fogCol = float3(0.055f, 0.095f, 0.165f);
    col = lerp(col, fogCol, fog * fog);

    return float4(col, 1.0f);
}
)";

const char* kParticleShaderSrc = R"(
cbuffer FrameCB : register(b0) {
    float4x4 gViewProj;
    float4 gCamPos;
    float4 gTimeFog;
    float4 gHighlightIds;
    float4 gStrikes[4];
};

struct VSIn {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR0;
    float4 meta   : TEXCOORD1;
};

struct PSIn {
    float4 sv_pos   : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float4 color    : COLOR0;
    float4 meta     : TEXCOORD3;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    o.sv_pos = mul(float4(input.pos, 1.0f), gViewProj);
    o.worldPos = input.pos;
    o.normal = input.normal;
    o.uv = input.uv;
    o.color = input.color;
    o.meta = input.meta;
    return o;
}

float4 PSMain(PSIn input) : SV_TARGET {
    float mode = input.meta.x; // 0 = volumetric puff, 1 = shockwave/ring, 2 = spark/flash
    float boost = input.meta.y;

    float2 c = input.uv - 0.5f;
    float r = length(c) * 2.0f;
    float alpha = input.color.a;
    float3 rgb = input.color.rgb * boost;

    if (mode < 0.5f) {
        // Volumetric smoke/fireball puff
        float edge = saturate(1.0f - r * 0.72f);
        float3 N = normalize(input.normal);
        float ndotl = saturate(dot(N, normalize(float3(0.45f, 0.8f, -0.35f))));
        rgb *= (0.65f + 0.35f * ndotl);
        alpha *= edge;
    } else if (mode < 1.5f) {
        // Ring / selection bracket
        float ring = smoothstep(0.62f, 0.82f, r) * (1.0f - smoothstep(0.88f, 1.0f, r));
        alpha *= ring;
    } else {
        // Bright spark / flash core
        float core = saturate(1.0f - r * 0.55f);
        rgb *= (1.0f + core * 1.5f);
        alpha *= core;
    }

    float dist = length(gCamPos.xyz - input.worldPos);
    float fog = saturate((dist - gTimeFog.y) / max(1.0f, gTimeFog.z - gTimeFog.y));
    float3 fogCol = float3(0.055f, 0.095f, 0.165f);
    rgb = lerp(rgb, fogCol, fog * fog * 0.75f);

    return float4(rgb, saturate(alpha));
}
)";

XMFLOAT4 CategoryBaseColor(NodeCategory cat, bool is_protected) {
    if (is_protected) return {0.20f, 0.34f, 0.56f, 1.0f};
    switch (cat) {
        case NodeCategory::Junk:   return {0.88f, 0.28f, 0.16f, 1.0f};
        case NodeCategory::Media:  return {0.62f, 0.28f, 0.84f, 1.0f};
        case NodeCategory::Dev:    return {0.12f, 0.70f, 0.64f, 1.0f};
        case NodeCategory::System: return {0.20f, 0.34f, 0.56f, 1.0f};
        case NodeCategory::Normal: return {0.18f, 0.50f, 0.80f, 1.0f};
    }
    return {0.18f, 0.50f, 0.80f, 1.0f};
}

XMFLOAT4 CategoryTrimColor(NodeCategory cat, bool is_protected) {
    if (is_protected) return {0.35f, 0.72f, 1.00f, 1.0f};
    switch (cat) {
        case NodeCategory::Junk:   return {1.00f, 0.42f, 0.14f, 1.0f};
        case NodeCategory::Media:  return {0.92f, 0.38f, 1.00f, 1.0f};
        case NodeCategory::Dev:    return {0.22f, 0.96f, 0.86f, 1.0f};
        case NodeCategory::System: return {0.35f, 0.72f, 1.00f, 1.0f};
        case NodeCategory::Normal: return {0.36f, 0.78f, 1.00f, 1.0f};
    }
    return {0.36f, 0.78f, 1.00f, 1.0f};
}

float CategoryFlag(NodeCategory cat, bool is_junk, bool is_protected) {
    if (is_junk || cat == NodeCategory::Junk) return 1.0f;
    if (is_protected || cat == NodeCategory::System) return 2.0f;
    if (cat == NodeCategory::Dev) return 3.0f;
    if (cat == NodeCategory::Media) return 4.0f;
    return 0.0f;
}

XMFLOAT3 Cross3(const XMFLOAT3& a, const XMFLOAT3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

XMFLOAT3 Sub3(const XMFLOAT3& a, const XMFLOAT3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

XMFLOAT3 Add3(const XMFLOAT3& a, const XMFLOAT3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XMFLOAT3 Mul3(const XMFLOAT3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

float Dot3(const XMFLOAT3& a, const XMFLOAT3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

XMFLOAT3 Normalize3(const XMFLOAT3& v) {
    float l = std::sqrt(Dot3(v, v));
    if (l < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / l, v.y / l, v.z / l};
}

void AddQuadChecked(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices,
                    XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2, XMFLOAT3 p3,
                    XMFLOAT3 n, XMFLOAT2 uv0, XMFLOAT2 uv1, XMFLOAT2 uv2, XMFLOAT2 uv3,
                    const XMFLOAT4& col, const XMFLOAT4& meta) {
    XMFLOAT3 e1 = Sub3(p1, p0);
    XMFLOAT3 e2 = Sub3(p2, p0);
    XMFLOAT3 c = Cross3(e1, e2);
    if (Dot3(c, n) < 0.0f) {
        std::swap(p1, p3);
        std::swap(uv1, uv3);
    }
    uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back({p0, n, uv0, col, meta});
    verts.push_back({p1, n, uv1, col, meta});
    verts.push_back({p2, n, uv2, col, meta});
    verts.push_back({p3, n, uv3, col, meta});

    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

void AppendBox(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices,
               float x0, float y0, float z0, float x1, float y1, float z1,
               const XMFLOAT4& col, const XMFLOAT4& meta) {
    if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
    float w = x1 - x0;
    float h = y1 - y0;
    float d = z1 - z0;

    AddQuadChecked(verts, indices, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0},
                   {0, 0, -1}, {0, 0}, {0, h}, {w, h}, {w, 0}, col, meta);
    AddQuadChecked(verts, indices, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1},
                   {0, 0, 1}, {0, 0}, {0, h}, {w, h}, {w, 0}, col, meta);
    AddQuadChecked(verts, indices, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0},
                   {-1, 0, 0}, {0, 0}, {0, h}, {d, h}, {d, 0}, col, meta);
    AddQuadChecked(verts, indices, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1},
                   {1, 0, 0}, {0, 0}, {0, h}, {d, h}, {d, 0}, col, meta);
    AddQuadChecked(verts, indices, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0},
                   {0, 1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 0}, col, meta);
    AddQuadChecked(verts, indices, {x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1},
                   {0, -1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 0}, col, meta);
}

void AppendOrientedBox(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices,
                       const XMFLOAT3& center,
                       const XMFLOAT3& right, const XMFLOAT3& up, const XMFLOAT3& fwd,
                       float hx, float hy, float hz,
                       const XMFLOAT4& col, const XMFLOAT4& meta) {
    auto pt = [&](float sx, float sy, float sz) -> XMFLOAT3 {
        return Add3(center, Add3(Mul3(right, sx * hx), Add3(Mul3(up, sy * hy), Mul3(fwd, sz * hz))));
    };
    XMFLOAT3 p000 = pt(-1, -1, -1);
    XMFLOAT3 p001 = pt(-1, -1,  1);
    XMFLOAT3 p010 = pt(-1,  1, -1);
    XMFLOAT3 p011 = pt(-1,  1,  1);
    XMFLOAT3 p100 = pt( 1, -1, -1);
    XMFLOAT3 p101 = pt( 1, -1,  1);
    XMFLOAT3 p110 = pt( 1,  1, -1);
    XMFLOAT3 p111 = pt( 1,  1,  1);

    XMFLOAT3 nr = Normalize3(right);
    XMFLOAT3 nu = Normalize3(up);
    XMFLOAT3 nf = Normalize3(fwd);

    AddQuadChecked(verts, indices, p000, p010, p110, p100, Mul3(nf, -1.0f), {0,0},{0,1},{1,1},{1,0}, col, meta);
    AddQuadChecked(verts, indices, p101, p111, p011, p001, nf,              {0,0},{0,1},{1,1},{1,0}, col, meta);
    AddQuadChecked(verts, indices, p001, p011, p010, p000, Mul3(nr, -1.0f), {0,0},{0,1},{1,1},{1,0}, col, meta);
    AddQuadChecked(verts, indices, p100, p110, p111, p101, nr,              {0,0},{0,1},{1,1},{1,0}, col, meta);
    AddQuadChecked(verts, indices, p010, p011, p111, p110, nu,              {0,0},{0,1},{1,1},{1,0}, col, meta);
    AddQuadChecked(verts, indices, p001, p000, p100, p101, Mul3(nu, -1.0f), {0,0},{0,1},{1,1},{1,0}, col, meta);
}

void AppendOctaPrismLocal(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices,
                          const XMFLOAT3& origin, const XMFLOAT3& right, const XMFLOAT3& up, const XMFLOAT3& fwd,
                          float z0, float z1, float r0_x, float r0_y, float r1_x, float r1_y,
                          float y_off0, float y_off1,
                          const XMFLOAT4& col, const XMFLOAT4& meta, bool cap0, bool cap1) {
    constexpr int kSegs = 8;
    constexpr float kTwoPi = 6.283185307f;
    XMFLOAT3 ring0[kSegs]{};
    XMFLOAT3 ring1[kSegs]{};

    auto to_world = [&](float lx, float ly, float lz) -> XMFLOAT3 {
        return Add3(origin, Add3(Mul3(right, lx), Add3(Mul3(up, ly), Mul3(fwd, lz))));
    };

    for (int i = 0; i < kSegs; ++i) {
        float ang = (static_cast<float>(i) + 0.5f) * (kTwoPi / static_cast<float>(kSegs));
        float ca = std::cos(ang);
        float sa = std::sin(ang);
        ring0[i] = to_world(ca * r0_x, sa * r0_y + y_off0, z0);
        ring1[i] = to_world(ca * r1_x, sa * r1_y + y_off1, z1);
    }

    for (int i = 0; i < kSegs; ++i) {
        int ni = (i + 1) % kSegs;
        XMFLOAT3 e1 = Sub3(ring1[i], ring0[i]);
        XMFLOAT3 e2 = Sub3(ring0[ni], ring0[i]);
        XMFLOAT3 n = Normalize3(Cross3(e1, e2));
        XMFLOAT3 radial = Sub3(Mul3(Add3(ring0[i], ring0[ni]), 0.5f), to_world(0.0f, y_off0, z0));
        if (Dot3(n, radial) < 0.0f) n = Mul3(n, -1.0f);
        AddQuadChecked(verts, indices, ring0[i], ring1[i], ring1[ni], ring0[ni],
                       n, {0, 0}, {0, 1}, {1, 1}, {1, 0}, col, meta);
    }

    if (cap0) {
        XMFLOAT3 c0 = to_world(0.0f, y_off0, z0);
        XMFLOAT3 nf = Mul3(Normalize3(fwd), -1.0f);
        for (int i = 0; i < kSegs; ++i) {
            int ni = (i + 1) % kSegs;
            AddQuadChecked(verts, indices, c0, ring0[ni], ring0[i], c0, nf,
                           {0, 0}, {1, 0}, {1, 1}, {0, 1}, col, meta);
        }
    }
    if (cap1) {
        XMFLOAT3 c1 = to_world(0.0f, y_off1, z1);
        XMFLOAT3 nf = Normalize3(fwd);
        for (int i = 0; i < kSegs; ++i) {
            int ni = (i + 1) % kSegs;
            AddQuadChecked(verts, indices, c1, ring1[i], ring1[ni], c1, nf,
                           {0, 0}, {1, 0}, {1, 1}, {0, 1}, col, meta);
        }
    }
}

void AppendBuildingArchitecture(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices,
                                const Building& b, float y_clip_min, float y_clip_max,
                                float off_x, float off_z, float vertical_scale) {
    float x0 = b.x0 + off_x;
    float z0 = b.z0 + off_z;
    float x1 = b.x1 + off_x;
    float z1 = b.z1 + off_z;
    float w = x1 - x0;
    float d = z1 - z0;
    float cx = (x0 + x1) * 0.5f;
    float cz = (z0 + z1) * 0.5f;
    float full_h = b.target_height * vertical_scale;
    if (full_h <= 0.1f) return;

    XMFLOAT4 facade_col = CategoryBaseColor(b.category, b.is_protected);
    XMFLOAT4 trim_col = CategoryTrimColor(b.category, b.is_protected);
    XMFLOAT4 concrete_col = {0.14f, 0.17f, 0.23f, 1.0f};
    XMFLOAT4 steel_col = {0.22f, 0.26f, 0.34f, 1.0f};
    XMFLOAT4 dark_mech_col = {0.09f, 0.11f, 0.15f, 1.0f};

    float cflag = CategoryFlag(b.category, b.is_junk, b.is_protected);
    XMFLOAT4 meta_facade{static_cast<float>(b.id), 0.0f, full_h, cflag};
    XMFLOAT4 meta_arch{static_cast<float>(b.id), 1.0f, full_h, cflag};
    XMFLOAT4 meta_emissive{static_cast<float>(b.id), 2.0f, full_h, cflag};

    auto add_clipped_box = [&](float bx0, float by0, float bz0, float bx1, float by1, float bz1,
                               const XMFLOAT4& col, const XMFLOAT4& meta) {
        float cy0 = std::max(by0, y_clip_min);
        float cy1 = std::min(by1, y_clip_max);
        if (cy1 <= cy0 + 0.02f) return;
        AppendBox(verts, indices, bx0, cy0, bz0, bx1, cy1, bz1, col, meta);
    };

    // 1. Concrete Plaza / Sidewalk Plinth
    add_clipped_box(x0 - 0.55f, 0.0f, z0 - 0.55f, x1 + 0.55f, 0.24f, z1 + 0.55f, concrete_col, meta_arch);

    // 2. Ground Podium / Lobby
    float podium_h = std::clamp(full_h * 0.12f, 0.9f, 3.6f);
    if (full_h < 2.2f) podium_h = full_h * 0.35f;
    add_clipped_box(x0 - 0.18f, 0.24f, z0 - 0.18f, x1 + 0.18f, podium_h, z1 + 0.18f, steel_col, meta_facade);
    add_clipped_box(x0 - 0.28f, podium_h - 0.22f, z0 - 0.28f, x1 + 0.28f, podium_h + 0.06f, z1 + 0.28f, concrete_col, meta_arch);

    // Entrance canopy on -Z side
    float canopy_w = std::min(w * 0.45f, 3.6f);
    add_clipped_box(cx - canopy_w * 0.5f, 1.05f, z0 - 0.95f, cx + canopy_w * 0.5f, 1.28f, z0 - 0.15f, steel_col, meta_arch);
    add_clipped_box(cx - canopy_w * 0.44f, 1.02f, z0 - 0.92f, cx + canopy_w * 0.44f, 1.07f, z0 - 0.84f, trim_col, meta_emissive);

    // 3. Main Tower Shaft Variants
    uint32_t style = b.style_seed % 5u;
    if (b.is_junk && (b.style_seed % 2u == 0u)) style = 4; // bias junk toward industrial vault

    float roof_y = full_h;
    float roof_x0 = x0;
    float roof_z0 = z0;
    float roof_x1 = x1;
    float roof_z1 = z1;

    if (style == 0 && full_h > 8.0f) {
        // Classic Setback Skyscraper (WTC/Empire-inspired tiers)
        float mid_y = podium_h + (full_h - podium_h) * 0.62f;
        float mech_y0 = mid_y - 0.45f;
        float mech_y1 = mid_y + 0.45f;
        add_clipped_box(x0, podium_h, z0, x1, mech_y0, z1, facade_col, meta_facade);
        add_clipped_box(x0 + 0.08f, mech_y0, z0 + 0.08f, x1 - 0.08f, mech_y1, z1 - 0.08f, dark_mech_col, meta_arch);

        float inset = std::min(w, d) * 0.14f;
        roof_x0 = x0 + inset;
        roof_z0 = z0 + inset;
        roof_x1 = x1 - inset;
        roof_z1 = z1 - inset;
        add_clipped_box(roof_x0, mech_y1, roof_z0, roof_x1, full_h, roof_z1, facade_col, meta_facade);

        // Corner vertical pilasters on lower shaft
        float pw = std::clamp(w * 0.07f, 0.20f, 0.55f);
        add_clipped_box(x0 - 0.08f, podium_h, z0 - 0.08f, x0 + pw, mech_y0, z0 + pw, steel_col, meta_arch);
        add_clipped_box(x1 - pw, podium_h, z0 - 0.08f, x1 + 0.08f, mech_y0, z0 + pw, steel_col, meta_arch);
        add_clipped_box(x0 - 0.08f, podium_h, z1 - pw, x0 + pw, mech_y0, z1 + 0.08f, steel_col, meta_arch);
        add_clipped_box(x1 - pw, podium_h, z1 - pw, x1 + 0.08f, mech_y0, z1 + 0.08f, steel_col, meta_arch);
    } else if (style == 1) {
        // Twin-Core / Buttressed Glass Tower
        add_clipped_box(x0, podium_h, z0, x1, full_h, z1, facade_col, meta_facade);
        float rib = std::clamp(std::min(w, d) * 0.08f, 0.22f, 0.65f);
        add_clipped_box(x0 - 0.12f, podium_h, z0 - 0.12f, x0 + rib, full_h + 0.25f, z0 + rib, steel_col, meta_arch);
        add_clipped_box(x1 - rib, podium_h, z0 - 0.12f, x1 + 0.12f, full_h + 0.25f, z0 + rib, steel_col, meta_arch);
        add_clipped_box(x0 - 0.12f, podium_h, z1 - rib, x0 + rib, full_h + 0.25f, z1 + 0.12f, steel_col, meta_arch);
        add_clipped_box(x1 - rib, podium_h, z1 - rib, x1 + 0.12f, full_h + 0.25f, z1 + 0.12f, steel_col, meta_arch);

        // Vertical neon data strip on Dev/Media/System towers
        if (b.category == NodeCategory::Dev || b.category == NodeCategory::Media || b.is_protected) {
            add_clipped_box(cx - 0.14f, podium_h, z0 - 0.08f, cx + 0.14f, full_h - 0.1f, z0 + 0.04f, trim_col, meta_emissive);
            add_clipped_box(cx - 0.14f, podium_h, z1 - 0.04f, cx + 0.14f, full_h - 0.1f, z1 + 0.08f, trim_col, meta_emissive);
        }
    } else if (style == 2 && full_h > 6.0f) {
        // Modern Cantilever / Stepped Terrace Tower
        float split_y = podium_h + (full_h - podium_h) * 0.50f;
        add_clipped_box(x0, podium_h, z0, x1, split_y, z1, facade_col, meta_facade);
        add_clipped_box(x0 - 0.1f, split_y - 0.15f, z0 - 0.1f, x1 + 0.1f, split_y + 0.08f, z1 + 0.1f, concrete_col, meta_arch);

        float shift_x = (b.style_seed & 1u) ? w * 0.14f : -w * 0.14f;
        roof_x0 = std::max(x0, x0 + shift_x * 0.5f + w * 0.08f);
        roof_x1 = std::min(x1, x1 + shift_x * 0.5f - w * 0.08f);
        roof_z0 = z0 + d * 0.08f;
        roof_z1 = z1 - d * 0.08f;
        add_clipped_box(roof_x0, split_y + 0.08f, roof_z0, roof_x1, full_h, roof_z1, facade_col, meta_facade);

        // Glowing terrace edge strip
        add_clipped_box(x0, split_y + 0.06f, z0, x1, split_y + 0.14f, z0 + 0.12f, trim_col, meta_emissive);
    } else if (style == 3 && w > 5.0f && d > 5.0f) {
        // Chamfered Cross-Footprint Citadel
        float cw = w * 0.18f;
        float cd = d * 0.18f;
        add_clipped_box(x0 + cw, podium_h, z0, x1 - cw, full_h, z1, facade_col, meta_facade);
        add_clipped_box(x0, podium_h, z0 + cd, x1, full_h * 0.92f, z1 - cd, facade_col, meta_facade);
        roof_x0 = x0 + cw;
        roof_x1 = x1 - cw;
    } else {
        // Industrial / Vault Block (external conduits + hazard bands)
        add_clipped_box(x0, podium_h, z0, x1, full_h, z1, facade_col, meta_facade);
        float pipe_r = std::clamp(w * 0.045f, 0.14f, 0.36f);
        add_clipped_box(x0 - pipe_r, podium_h, cz - d * 0.25f, x0 + 0.05f, full_h * 0.88f, cz - d * 0.25f + pipe_r, steel_col, meta_arch);
        add_clipped_box(x0 - pipe_r, podium_h, cz + d * 0.25f - pipe_r, x0 + 0.05f, full_h * 0.88f, cz + d * 0.25f, steel_col, meta_arch);
        if (b.is_junk) {
            add_clipped_box(x0 - 0.06f, full_h - 0.55f, z0 - 0.06f, x1 + 0.06f, full_h - 0.28f, z1 + 0.06f, trim_col, meta_emissive);
        }
    }

    // 4. Rooftop Parapet, Crown Neon, HVAC, Helipad & Spire
    float rw = roof_x1 - roof_x0;
    float rd = roof_z1 - roof_z0;
    float rcx = (roof_x0 + roof_x1) * 0.5f;
    float rcz = (roof_z0 + roof_z1) * 0.5f;
    if (rw > 1.8f && rd > 1.8f && full_h > 2.4f) {
        float pw = 0.22f;
        float ph = 0.38f;
        add_clipped_box(roof_x0, roof_y, roof_z0, roof_x1, roof_y + ph, roof_z0 + pw, steel_col, meta_arch);
        add_clipped_box(roof_x0, roof_y, roof_z1 - pw, roof_x1, roof_y + ph, roof_z1, steel_col, meta_arch);
        add_clipped_box(roof_x0, roof_y, roof_z0 + pw, roof_x0 + pw, roof_y + ph, roof_z1 - pw, steel_col, meta_arch);
        add_clipped_box(roof_x1 - pw, roof_y, roof_z0 + pw, roof_x1, roof_y + ph, roof_z1 - pw, steel_col, meta_arch);

        // Glowing crown trim line around roof edge
        add_clipped_box(roof_x0 - 0.04f, roof_y + ph - 0.08f, roof_z0 - 0.04f, roof_x1 + 0.04f, roof_y + ph + 0.02f, roof_z0 + 0.06f, trim_col, meta_emissive);
        add_clipped_box(roof_x0 - 0.04f, roof_y + ph - 0.08f, roof_z1 - 0.06f, roof_x1 + 0.04f, roof_y + ph + 0.02f, roof_z1 + 0.04f, trim_col, meta_emissive);

        // Mechanical penthouse / HVAC units
        float hvac_w = std::min(rw * 0.32f, 2.6f);
        float hvac_d = std::min(rd * 0.28f, 2.2f);
        add_clipped_box(roof_x0 + 0.45f, roof_y, roof_z0 + 0.45f,
                        roof_x0 + 0.45f + hvac_w, roof_y + 0.85f, roof_z0 + 0.45f + hvac_d, dark_mech_col, meta_arch);

        if (rw > 6.5f && rd > 6.5f && full_h > 10.0f) {
            // Helipad deck + glowing 'H'
            float pad_r = std::min(rw, rd) * 0.24f;
            add_clipped_box(rcx - pad_r, roof_y, rcz - pad_r, rcx + pad_r, roof_y + 0.10f, rcz + pad_r, dark_mech_col, meta_arch);
            XMFLOAT4 pad_col = {0.95f, 0.82f, 0.25f, 1.0f};
            add_clipped_box(rcx - pad_r * 0.45f, roof_y + 0.10f, rcz - pad_r * 0.55f,
                            rcx - pad_r * 0.28f, roof_y + 0.14f, rcz + pad_r * 0.55f, pad_col, meta_emissive);
            add_clipped_box(rcx + pad_r * 0.28f, roof_y + 0.10f, rcz - pad_r * 0.55f,
                            rcx + pad_r * 0.45f, roof_y + 0.14f, rcz + pad_r * 0.55f, pad_col, meta_emissive);
            add_clipped_box(rcx - pad_r * 0.28f, roof_y + 0.10f, rcz - pad_r * 0.09f,
                            rcx + pad_r * 0.28f, roof_y + 0.14f, rcz + pad_r * 0.09f, pad_col, meta_emissive);
        }

        // Tall antenna spire + aviation beacon on skyscrapers
        if (full_h > 13.0f) {
            float spire_h = std::clamp(full_h * 0.16f, 2.5f, 8.5f);
            float base_y = roof_y + 0.1f;
            add_clipped_box(rcx - 0.35f, base_y, rcz - 0.35f, rcx + 0.35f, base_y + 1.1f, rcz + 0.35f, steel_col, meta_arch);
            add_clipped_box(rcx - 0.12f, base_y + 1.1f, rcz - 0.12f, rcx + 0.12f, base_y + spire_h, rcz + 0.12f, steel_col, meta_arch);
            XMFLOAT4 beacon_col = b.is_junk ? XMFLOAT4{1.0f, 0.20f, 0.12f, 1.0f} : XMFLOAT4{1.0f, 0.28f, 0.22f, 1.0f};
            add_clipped_box(rcx - 0.20f, base_y + spire_h, rcz - 0.20f, rcx + 0.20f, base_y + spire_h + 0.36f, rcz + 0.20f, beacon_col, meta_emissive);
        }
    }
}

void TransformVertexRange(std::vector<CityVertex>& verts, size_t start_idx,
                          const XMFLOAT3& pivot, const XMFLOAT3& axis, float angle,
                          const XMFLOAT3& translation) {
    if (start_idx >= verts.size()) return;
    XMVECTOR ax = XMVector3Normalize(XMLoadFloat3(&axis));
    XMMATRIX rot = XMMatrixRotationAxis(ax, angle);
    XMVECTOR piv = XMLoadFloat3(&pivot);
    XMVECTOR tr = XMLoadFloat3(&translation);

    for (size_t i = start_idx; i < verts.size(); ++i) {
        XMVECTOR p = XMLoadFloat3(&verts[i].pos) - piv;
        p = XMVector3TransformCoord(p, rot) + piv + tr;
        XMStoreFloat3(&verts[i].pos, p);

        XMVECTOR n = XMLoadFloat3(&verts[i].normal);
        n = XMVector3Normalize(XMVector3TransformNormal(n, rot));
        XMStoreFloat3(&verts[i].normal, n);
    }
}

void AppendDetailedBuilding(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices, const Building& b) {
    if (b.demolished) return;

    if (!b.collapsing) {
        float vscale = std::clamp(b.height / std::max(0.1f, b.target_height), 0.02f, 1.15f);
        AppendBuildingArchitecture(verts, indices, b, 0.0f, b.height + 12.0f, 0.0f, 0.0f, vscale);
        return;
    }

    // Collapsing building split at b.impact_y:
    // 1. Lower intact/pancaking section (0 .. b.impact_y * b.lower_scale)
    float lower_h = std::max(0.0f, b.impact_y * b.lower_scale);
    if (lower_h > 0.15f) {
        // Compress lower section cleanly from bottom
        float compress_offset = -(b.impact_y - lower_h);
        size_t v0 = verts.size();
        AppendBuildingArchitecture(verts, indices, b, b.impact_y - lower_h, b.impact_y,
                                   b.shake_offset_x, b.shake_offset_z, 1.0f);
        for (size_t i = v0; i < verts.size(); ++i) {
            verts[i].pos.y = std::max(0.0f, verts[i].pos.y + compress_offset);
        }
    }

    // 2. Upper section (b.impact_y .. b.target_height + spire): tilts and drops into the breach!
    float cx = (b.x0 + b.x1) * 0.5f;
    float cz = (b.z0 + b.z1) * 0.5f;
    size_t upper_v0 = verts.size();
    AppendBuildingArchitecture(verts, indices, b, b.impact_y, b.target_height + 12.0f, 0.0f, 0.0f, 1.0f);

    // Tilt axis perpendicular to impact direction in XZ plane
    XMFLOAT3 tilt_axis = Normalize3({-b.impact_dir_z, 0.0f, b.impact_dir_x});
    float breach_drop = (b.impact_y * (1.0f - b.lower_scale)) + b.upper_drop;
    XMFLOAT3 pivot{cx, b.impact_y, cz};
    XMFLOAT3 tr{b.shake_offset_x, -breach_drop, b.shake_offset_z};
    TransformVertexRange(verts, upper_v0, pivot, tilt_axis, b.upper_tilt, tr);
}

void AppendCityProp(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices, const CityProp& p) {
    XMFLOAT4 meta_arch{0.0f, 1.0f, 1.0f, 0.0f};
    XMFLOAT4 meta_em{0.0f, 2.0f, 1.0f, 0.0f};
    XMFLOAT4 meta_leaf{0.0f, 4.0f, 1.0f, 0.0f};

    if (p.kind == CityProp::Kind::StreetLight) {
        XMFLOAT3 right{std::cos(p.yaw), 0.0f, -std::sin(p.yaw)};
        XMFLOAT3 up{0.0f, 1.0f, 0.0f};
        XMFLOAT3 fwd{std::sin(p.yaw), 0.0f, std::cos(p.yaw)};
        float h = 3.2f * p.scale;
        XMFLOAT4 pole_col{0.16f, 0.20f, 0.26f, 1.0f};
        XMFLOAT4 lamp_col{0.85f, 0.94f, 1.00f, 1.0f};

        AppendBox(verts, indices, p.x - 0.12f, 0.0f, p.z - 0.12f, p.x + 0.12f, 0.35f, p.z + 0.12f, pole_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.06f, 0.35f, p.z - 0.06f, p.x + 0.06f, h, p.z + 0.06f, pole_col, meta_arch);

        XMFLOAT3 arm_c = Add3({p.x, h - 0.05f, p.z}, Mul3(right, 0.55f * p.scale));
        AppendOrientedBox(verts, indices, arm_c, right, up, fwd, 0.55f * p.scale, 0.05f, 0.05f, pole_col, meta_arch);
        XMFLOAT3 head_c = Add3({p.x, h - 0.12f, p.z}, Mul3(right, 0.95f * p.scale));
        AppendOrientedBox(verts, indices, head_c, right, up, fwd, 0.22f, 0.05f, 0.12f, lamp_col, meta_em);
    } else if (p.kind == CityProp::Kind::Tree) {
        float sc = p.scale;
        XMFLOAT4 planter_col{0.18f, 0.22f, 0.28f, 1.0f};
        XMFLOAT4 trunk_col{0.24f, 0.16f, 0.11f, 1.0f};
        XMFLOAT4 leaf1_col = (p.seed & 1u) ? XMFLOAT4{0.10f, 0.38f, 0.26f, 1.0f} : XMFLOAT4{0.12f, 0.44f, 0.30f, 1.0f};
        XMFLOAT4 leaf2_col = {0.14f, 0.52f, 0.34f, 1.0f};

        AppendBox(verts, indices, p.x - 0.65f * sc, 0.0f, p.z - 0.65f * sc, p.x + 0.65f * sc, 0.26f, p.z + 0.65f * sc, planter_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.14f * sc, 0.26f, p.z - 0.14f * sc, p.x + 0.14f * sc, 1.35f * sc, p.z + 0.14f * sc, trunk_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.95f * sc, 1.25f * sc, p.z - 0.95f * sc, p.x + 0.95f * sc, 2.35f * sc, p.z + 0.95f * sc, leaf1_col, meta_leaf);
        AppendBox(verts, indices, p.x - 0.62f * sc, 2.35f * sc, p.z - 0.62f * sc, p.x + 0.62f * sc, 3.15f * sc, p.z + 0.62f * sc, leaf2_col, meta_leaf);
    } else if (p.kind == CityProp::Kind::DataNode) {
        XMFLOAT4 base_col{0.15f, 0.20f, 0.28f, 1.0f};
        XMFLOAT4 glow_col{0.20f, 0.82f, 1.00f, 1.0f};
        AppendBox(verts, indices, p.x - 0.55f, 0.0f, p.z - 0.55f, p.x + 0.55f, 0.5f, p.z + 0.55f, base_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.24f, 0.5f, p.z - 0.24f, p.x + 0.24f, 4.2f, p.z + 0.24f, base_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.28f, 1.6f, p.z - 0.28f, p.x + 0.28f, 3.6f, p.z + 0.28f, glow_col, meta_em);
    } else if (p.kind == CityProp::Kind::PerimeterPylon) {
        XMFLOAT4 base_col{0.12f, 0.16f, 0.24f, 1.0f};
        XMFLOAT4 glow_col{0.18f, 0.68f, 0.98f, 1.0f};
        AppendBox(verts, indices, p.x - 0.6f, 0.0f, p.z - 0.6f, p.x + 0.6f, 6.2f, p.z + 0.6f, base_col, meta_arch);
        AppendBox(verts, indices, p.x - 0.64f, 5.4f, p.z - 0.64f, p.x + 0.64f, 5.85f, p.z + 0.64f, glow_col, meta_em);
    }
}

void AppendPlane(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices, const PlaneStrike& s) {
    // Build orthonormal aircraft basis from yaw, pitch, roll
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(s.pitch, s.yaw, s.roll);
    XMFLOAT3 right{}, up{}, fwd{};
    XMStoreFloat3(&right, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rot));
    XMStoreFloat3(&up,    XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rot));
    XMStoreFloat3(&fwd,   XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rot));

    float sc = s.scale;
    XMFLOAT3 r = Mul3(right, sc);
    XMFLOAT3 u = Mul3(up, sc);
    XMFLOAT3 f = Mul3(fwd, sc);

    XMFLOAT4 hull_col{0.90f, 0.92f, 0.96f, 1.0f};
    XMFLOAT4 belly_col{0.52f, 0.58f, 0.66f, 1.0f};
    XMFLOAT4 stripe_col{0.12f, 0.34f, 0.78f, 1.0f};
    XMFLOAT4 cockpit_col{0.06f, 0.09f, 0.14f, 1.0f};
    XMFLOAT4 wing_col{0.78f, 0.82f, 0.88f, 1.0f};
    XMFLOAT4 eng_col{0.28f, 0.32f, 0.38f, 1.0f};
    XMFLOAT4 red_nav{1.0f, 0.14f, 0.10f, 1.0f};
    XMFLOAT4 green_nav{0.14f, 1.0f, 0.28f, 1.0f};
    XMFLOAT4 exhaust_glow{1.0f, 0.62f, 0.20f, 1.0f};

    XMFLOAT4 meta_plane{0.0f, 3.0f, 1.0f, 0.0f};
    XMFLOAT4 meta_em{0.0f, 2.0f, 1.0f, 0.0f};

    // Main fuselage (8-sided cylinder)
    AppendOctaPrismLocal(verts, indices, s.pos, r, u, f, -4.4f, 4.2f, 0.88f, 0.94f, 0.88f, 0.94f, 0.0f, 0.0f, hull_col, meta_plane, false, false);
    // Tapered nose cone
    AppendOctaPrismLocal(verts, indices, s.pos, r, u, f, 4.2f, 6.2f, 0.88f, 0.94f, 0.32f, 0.36f, 0.0f, -0.18f, hull_col, meta_plane, false, false);
    AppendOctaPrismLocal(verts, indices, s.pos, r, u, f, 6.2f, 7.1f, 0.32f, 0.36f, 0.06f, 0.06f, -0.18f, -0.28f, cockpit_col, meta_plane, false, true);
    // Tapered tail cone
    AppendOctaPrismLocal(verts, indices, s.pos, r, u, f, -7.2f, -4.4f, 0.18f, 0.22f, 0.88f, 0.94f, 0.25f, 0.0f, belly_col, meta_plane, true, false);

    // Airline cheatline stripe + cockpit windshield wrap
    AppendOrientedBox(verts, indices, Add3(s.pos, Mul3(f, -0.1f)), right, up, fwd, 0.90f * sc, 0.14f * sc, 4.3f * sc, stripe_col, meta_plane);
    AppendOrientedBox(verts, indices, Add3(s.pos, Add3(Mul3(f, 4.9f), Mul3(u, 0.38f))), right, up, fwd, 0.56f * sc, 0.22f * sc, 0.65f * sc, cockpit_col, meta_plane);

    // Swept main wings (left & right built from 2 chordwise/spanwise oriented segments each for clean sweep + dihedral)
    for (float side : {-1.0f, 1.0f}) {
        XMFLOAT3 wing_fwd = Normalize3(Add3(fwd, Mul3(right, -side * 0.48f)));
        XMFLOAT3 wing_up = Normalize3(Add3(up, Mul3(right, side * 0.08f)));
        XMFLOAT3 wing_right = Normalize3(Cross3(wing_up, wing_fwd));

        // Inner wing
        XMFLOAT3 w1_c = Add3(s.pos, Add3(Mul3(r, side * 2.5f), Add3(Mul3(u, -0.12f), Mul3(f, -0.55f))));
        AppendOrientedBox(verts, indices, w1_c, wing_right, wing_up, wing_fwd, 1.95f * sc, 0.13f * sc, 1.35f * sc, wing_col, meta_plane);

        // Outer wing
        XMFLOAT3 w2_c = Add3(s.pos, Add3(Mul3(r, side * 5.2f), Add3(Mul3(u, 0.10f), Mul3(f, -1.85f))));
        AppendOrientedBox(verts, indices, w2_c, wing_right, wing_up, wing_fwd, 1.75f * sc, 0.09f * sc, 0.85f * sc, wing_col, meta_plane);

        // Winglet + nav light
        XMFLOAT3 tip_c = Add3(s.pos, Add3(Mul3(r, side * 6.95f), Add3(Mul3(u, 0.45f), Mul3(f, -2.65f))));
        AppendOrientedBox(verts, indices, tip_c, right, up, fwd, 0.08f * sc, 0.42f * sc, 0.45f * sc, stripe_col, meta_plane);
        XMFLOAT3 nav_c = Add3(s.pos, Add3(Mul3(r, side * 7.02f), Add3(Mul3(u, 0.22f), Mul3(f, -2.65f))));
        AppendOrientedBox(verts, indices, nav_c, right, up, fwd, 0.12f * sc, 0.12f * sc, 0.18f * sc,
                          (side < 0.0f) ? red_nav : green_nav, meta_em);

        // Underwing turbofan engine + pylon + exhaust glow
        XMFLOAT3 eng_c = Add3(s.pos, Add3(Mul3(r, side * 2.55f), Add3(Mul3(u, -0.78f), Mul3(f, 0.35f))));
        AppendOrientedBox(verts, indices, Add3(eng_c, Mul3(u, 0.42f)), right, up, fwd, 0.12f * sc, 0.32f * sc, 0.65f * sc, wing_col, meta_plane);
        AppendOctaPrismLocal(verts, indices, eng_c, r, u, f, -1.05f, 1.15f, 0.44f, 0.44f, 0.46f, 0.46f, 0.0f, 0.0f, eng_col, meta_plane, true, true);
        AppendOrientedBox(verts, indices, Add3(eng_c, Mul3(f, -1.12f)), right, up, fwd, 0.24f * sc, 0.24f * sc, 0.12f * sc, exhaust_glow, meta_em);
    }

    // Tail Vertical Stabilizer (swept fin)
    XMFLOAT3 fin_up = Normalize3(Add3(up, Mul3(fwd, -0.45f)));
    XMFLOAT3 fin_fwd = Normalize3(Cross3(right, fin_up));
    XMFLOAT3 fin_c = Add3(s.pos, Add3(Mul3(u, 1.75f), Mul3(f, -5.85f)));
    AppendOrientedBox(verts, indices, fin_c, right, fin_up, fin_fwd, 0.10f * sc, 1.45f * sc, 1.15f * sc, stripe_col, meta_plane);
    // White strobe on tail tip
    XMFLOAT3 strobe_c = Add3(s.pos, Add3(Mul3(u, 3.15f), Mul3(f, -6.55f)));
    AppendOrientedBox(verts, indices, strobe_c, right, up, fwd, 0.12f * sc, 0.12f * sc, 0.18f * sc, {1.0f, 0.96f, 0.90f, 1.0f}, meta_em);

    // Horizontal Stabilizers
    for (float side : {-1.0f, 1.0f}) {
        XMFLOAT3 h_fwd = Normalize3(Add3(fwd, Mul3(right, -side * 0.42f)));
        XMFLOAT3 h_right = Normalize3(Cross3(up, h_fwd));
        XMFLOAT3 h_c = Add3(s.pos, Add3(Mul3(r, side * 1.45f), Add3(Mul3(u, 0.35f), Mul3(f, -5.95f))));
        AppendOrientedBox(verts, indices, h_c, h_right, up, h_fwd, 1.25f * sc, 0.07f * sc, 0.65f * sc, wing_col, meta_plane);
    }
}

void AppendTrafficCar(std::vector<CityVertex>& verts, std::vector<uint32_t>& indices, const TrafficCar& car) {
    XMFLOAT3 pos = car.along_z ? XMFLOAT3{car.lane_coord, 0.24f, car.axis_pos}
                               : XMFLOAT3{car.axis_pos, 0.24f, car.lane_coord};
    XMFLOAT3 fwd = car.along_z ? XMFLOAT3{0.0f, 0.0f, static_cast<float>(car.dir)}
                               : XMFLOAT3{static_cast<float>(car.dir), 0.0f, 0.0f};
    XMFLOAT3 up{0.0f, 1.0f, 0.0f};
    XMFLOAT3 right = Cross3(up, fwd);

    XMFLOAT4 meta_car{0.0f, 1.0f, 1.0f, 0.0f};
    XMFLOAT4 meta_em{0.0f, 2.0f, 1.0f, 0.0f};

    AppendOrientedBox(verts, indices, pos, right, up, fwd, 0.36f, 0.18f, 0.85f, car.color, meta_car);
    AppendOrientedBox(verts, indices, Add3(pos, {0.0f, 0.22f, 0.0f}), right, up, fwd, 0.30f, 0.13f, 0.42f, {0.10f, 0.14f, 0.20f, 1.0f}, meta_car);
    // Headlights & Taillights
    AppendOrientedBox(verts, indices, Add3(pos, Add3(Mul3(fwd, 0.86f), Mul3(right, -0.22f))), right, up, fwd, 0.08f, 0.06f, 0.05f, {1.0f, 0.95f, 0.80f, 1.0f}, meta_em);
    AppendOrientedBox(verts, indices, Add3(pos, Add3(Mul3(fwd, 0.86f), Mul3(right,  0.22f))), right, up, fwd, 0.08f, 0.06f, 0.05f, {1.0f, 0.95f, 0.80f, 1.0f}, meta_em);
    AppendOrientedBox(verts, indices, Add3(pos, Add3(Mul3(fwd, -0.86f), Mul3(right, -0.22f))), right, up, fwd, 0.09f, 0.06f, 0.05f, {1.0f, 0.12f, 0.10f, 1.0f}, meta_em);
    AppendOrientedBox(verts, indices, Add3(pos, Add3(Mul3(fwd, -0.86f), Mul3(right,  0.22f))), right, up, fwd, 0.09f, 0.06f, 0.05f, {1.0f, 0.12f, 0.10f, 1.0f}, meta_em);
}

void AppendParticleBox(std::vector<ParticleVertex>& verts, std::vector<uint32_t>& indices,
                       const XMFLOAT3& center, const XMFLOAT3& right, const XMFLOAT3& up, const XMFLOAT3& fwd,
                       float hx, float hy, float hz,
                       const XMFLOAT4& col, float mode, float boost) {
    auto pt = [&](float sx, float sy, float sz) -> XMFLOAT3 {
        return Add3(center, Add3(Mul3(right, sx * hx), Add3(Mul3(up, sy * hy), Mul3(fwd, sz * hz))));
    };
    XMFLOAT3 p000 = pt(-1, -1, -1);
    XMFLOAT3 p001 = pt(-1, -1,  1);
    XMFLOAT3 p010 = pt(-1,  1, -1);
    XMFLOAT3 p011 = pt(-1,  1,  1);
    XMFLOAT3 p100 = pt( 1, -1, -1);
    XMFLOAT3 p101 = pt( 1, -1,  1);
    XMFLOAT3 p110 = pt( 1,  1, -1);
    XMFLOAT3 p111 = pt( 1,  1,  1);

    XMFLOAT4 meta{mode, boost, 0.0f, 0.0f};
    auto quad = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, XMFLOAT3 n) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        verts.push_back({a, n, {0, 0}, col, meta});
        verts.push_back({b, n, {0, 1}, col, meta});
        verts.push_back({c, n, {1, 1}, col, meta});
        verts.push_back({d, n, {1, 0}, col, meta});
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    };

    quad(p000, p010, p110, p100, Mul3(fwd, -1.0f));
    quad(p101, p111, p011, p001, fwd);
    quad(p001, p011, p010, p000, Mul3(right, -1.0f));
    quad(p100, p110, p111, p101, right);
    quad(p010, p011, p111, p110, up);
    quad(p001, p000, p100, p101, Mul3(up, -1.0f));
}

void AppendGroundRingQuad(std::vector<ParticleVertex>& verts, std::vector<uint32_t>& indices,
                          float cx, float y, float cz, float radius, const XMFLOAT4& col) {
    uint32_t base = static_cast<uint32_t>(verts.size());
    XMFLOAT4 meta{1.0f, 1.5f, 0.0f, 0.0f};
    verts.push_back({{cx - radius, y, cz - radius}, {0, 1, 0}, {0, 0}, col, meta});
    verts.push_back({{cx - radius, y, cz + radius}, {0, 1, 0}, {0, 1}, col, meta});
    verts.push_back({{cx + radius, y, cz + radius}, {0, 1, 0}, {1, 1}, col, meta});
    verts.push_back({{cx + radius, y, cz - radius}, {0, 1, 0}, {1, 0}, col, meta});
    indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
    indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
}

template <typename TVertex>
void UploadDynamicPair(ID3D11Device* device, ID3D11DeviceContext* ctx,
                       const std::vector<TVertex>& verts, const std::vector<uint32_t>& indices,
                       Microsoft::WRL::ComPtr<ID3D11Buffer>& vb, Microsoft::WRL::ComPtr<ID3D11Buffer>& ib,
                       UINT& vb_cap, UINT& ib_cap, UINT& index_count) {
    index_count = static_cast<UINT>(indices.size());
    if (verts.empty() || indices.empty()) return;

    UINT need_vb = static_cast<UINT>(verts.size() * sizeof(TVertex));
    UINT need_ib = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    if (need_vb > vb_cap) {
        vb.Reset();
        vb_cap = std::max(need_vb, 65536u);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = vb_cap;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &vb);
    }
    if (need_ib > ib_cap) {
        ib.Reset();
        ib_cap = std::max(need_ib, 65536u);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = ib_cap;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &ib);
    }

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (vb && SUCCEEDED(ctx->Map(vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, verts.data(), need_vb);
        ctx->Unmap(vb.Get(), 0);
    }
    if (ib && SUCCEEDED(ctx->Map(ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, indices.data(), need_ib);
        ctx->Unmap(ib.Get(), 0);
    }
}

bool RayIntersectsAABB(const XMVECTOR& orig, const XMVECTOR& dir,
                       const XMFLOAT3& bmin, const XMFLOAT3& bmax, float& out_t) {
    XMFLOAT3 o{}, d{};
    XMStoreFloat3(&o, orig);
    XMStoreFloat3(&d, dir);

    float tmin = 0.0f;
    float tmax = 1e9f;

    auto slab = [&](float o_c, float d_c, float min_c, float max_c) -> bool {
        if (std::abs(d_c) < 1e-6f) {
            return (o_c >= min_c && o_c <= max_c);
        }
        float inv = 1.0f / d_c;
        float t1 = (min_c - o_c) * inv;
        float t2 = (max_c - o_c) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        return tmin <= tmax;
    };

    if (!slab(o.x, d.x, bmin.x, bmax.x)) return false;
    if (!slab(o.y, d.y, bmin.y, bmax.y)) return false;
    if (!slab(o.z, d.z, bmin.z, bmax.z)) return false;
    out_t = tmin;
    return true;
}

} // namespace

XMFLOAT3 Camera::GetForward() const {
    float cp = std::cos(pitch);
    return {
        std::sin(yaw) * cp,
        -std::sin(pitch),
        std::cos(yaw) * cp
    };
}

XMFLOAT3 Camera::GetRight() const {
    return {
        std::cos(yaw),
        0.0f,
        -std::sin(yaw)
    };
}

XMMATRIX Camera::GetViewMatrix() const {
    XMFLOAT3 fwd = GetForward();
    XMFLOAT3 eye_pos{pos.x + shake_offset.x, pos.y + shake_offset.y, pos.z + shake_offset.z};
    XMVECTOR eye = XMLoadFloat3(&eye_pos);
    XMVECTOR dir = XMLoadFloat3(&fwd);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookToLH(eye, dir, up);
}

CityRenderer::~CityRenderer() {
    Shutdown();
}

bool CityRenderer::Init(HWND hwnd, int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION, &device_, &fl, &ctx_
    );
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, 2, D3D11_SDK_VERSION, &device_, &fl, &ctx_
        );
        if (FAILED(hr)) return false;
    }

    UINT q = 0;
    if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q)) && q > 0) {
        msaa_count_ = 4;
        msaa_quality_ = 0;
    } else {
        msaa_count_ = 1;
        msaa_quality_ = 0;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_dev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(device_.As(&dxgi_dev))) return false;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = static_cast<UINT>(width_);
    sd.BufferDesc.Height = static_cast<UINT>(height_);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = msaa_count_;
    sd.SampleDesc.Quality = msaa_quality_;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    if (FAILED(factory->CreateSwapChain(device_.Get(), &sd, &swap_chain_))) return false;

    if (!CreateRenderTargets()) return false;
    if (!CreateShadersAndStates()) return false;
    return true;
}

bool CityRenderer::CreateRenderTargets() {
    rtv_.Reset();
    dsv_.Reset();
    depth_tex_.Reset();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buf;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buf)))) return false;
    if (FAILED(device_->CreateRenderTargetView(back_buf.Get(), nullptr, &rtv_))) return false;

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = static_cast<UINT>(width_);
    dd.Height = static_cast<UINT>(height_);
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = msaa_count_;
    dd.SampleDesc.Quality = msaa_quality_;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&dd, nullptr, &depth_tex_))) return false;
    if (FAILED(device_->CreateDepthStencilView(depth_tex_.Get(), nullptr, &dsv_))) return false;

    return true;
}

bool CityRenderer::CreateShadersAndStates() {
    auto compile = [](const char* src, const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> bool {
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        return SUCCEEDED(hr);
    };

    // Sky shader
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vs_blob, ps_blob;
        if (!compile(kSkyShaderSrc, "VSMain", "vs_5_0", vs_blob)) return false;
        if (!compile(kSkyShaderSrc, "PSMain", "ps_5_0", ps_blob)) return false;
        if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &sky_vs_))) return false;
        if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &sky_ps_))) return false;
        D3D11_INPUT_ELEMENT_DESC sky_ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SkyVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device_->CreateInputLayout(sky_ied, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &sky_layout_))) return false;
    }

    // Building shader
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vs_blob, ps_blob;
        if (!compile(kBuildingShaderSrc, "VSMain", "vs_5_0", vs_blob)) return false;
        if (!compile(kBuildingShaderSrc, "PSMain", "ps_5_0", ps_blob)) return false;
        if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &bld_vs_))) return false;
        if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &bld_ps_))) return false;
        D3D11_INPUT_ELEMENT_DESC bld_ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(CityVertex, pos),    D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(CityVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(CityVertex, uv),     D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CityVertex, color),  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CityVertex, meta),   D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device_->CreateInputLayout(bld_ied, 5, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &bld_layout_))) return false;
    }

    // Ground shader
    {
        Microsoft::WRL::ComPtr<ID3DBlob> gvs_blob, gps_blob;
        if (!compile(kGroundShaderSrc, "VSMain", "vs_5_0", gvs_blob)) return false;
        if (!compile(kGroundShaderSrc, "PSMain", "ps_5_0", gps_blob)) return false;
        if (FAILED(device_->CreateVertexShader(gvs_blob->GetBufferPointer(), gvs_blob->GetBufferSize(), nullptr, &gnd_vs_))) return false;
        if (FAILED(device_->CreatePixelShader(gps_blob->GetBufferPointer(), gps_blob->GetBufferSize(), nullptr, &gnd_ps_))) return false;
        D3D11_INPUT_ELEMENT_DESC gnd_ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GroundVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(GroundVertex, uv),  D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device_->CreateInputLayout(gnd_ied, 2, gvs_blob->GetBufferPointer(), gvs_blob->GetBufferSize(), &gnd_layout_))) return false;
    }

    // Particle shader
    {
        Microsoft::WRL::ComPtr<ID3DBlob> pvs_blob, pps_blob;
        if (!compile(kParticleShaderSrc, "VSMain", "vs_5_0", pvs_blob)) return false;
        if (!compile(kParticleShaderSrc, "PSMain", "ps_5_0", pps_blob)) return false;
        if (FAILED(device_->CreateVertexShader(pvs_blob->GetBufferPointer(), pvs_blob->GetBufferSize(), nullptr, &pt_vs_))) return false;
        if (FAILED(device_->CreatePixelShader(pps_blob->GetBufferPointer(), pps_blob->GetBufferSize(), nullptr, &pt_ps_))) return false;
        D3D11_INPUT_ELEMENT_DESC pt_ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ParticleVertex, pos),    D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ParticleVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(ParticleVertex, uv),     D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ParticleVertex, color),  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ParticleVertex, meta),   D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device_->CreateInputLayout(pt_ied, 5, pvs_blob->GetBufferPointer(), pvs_blob->GetBufferSize(), &pt_layout_))) return false;
    }

    // Constant buffer
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(FrameCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &cb_frame_))) return false;

    // Sky unit cube (drawn around camera with CULL_NONE)
    SkyVertex sverts[8] = {
        {{-1, -1, -1}}, {{-1,  1, -1}}, {{ 1,  1, -1}}, {{ 1, -1, -1}},
        {{-1, -1,  1}}, {{-1,  1,  1}}, {{ 1,  1,  1}}, {{ 1, -1,  1}},
    };
    uint32_t sidx[36] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,
        0,4,5, 0,5,1,  3,2,6, 3,6,7,
        1,5,6, 1,6,2,  0,3,7, 0,7,4
    };
    sky_index_count_ = 36;
    {
        D3D11_BUFFER_DESC vbd{sizeof(sverts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA vsd{sverts, 0, 0};
        if (FAILED(device_->CreateBuffer(&vbd, &vsd, &sky_vb_))) return false;
        D3D11_BUFFER_DESC ibd{sizeof(sidx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA isd{sidx, 0, 0};
        if (FAILED(device_->CreateBuffer(&ibd, &isd, &sky_ib_))) return false;
    }

    // Ground quad [-340, 340]
    GroundVertex gverts[4] = {
        {{-340.0f, 0.0f, -340.0f}, {0.0f, 0.0f}},
        {{-340.0f, 0.0f,  340.0f}, {0.0f, 1.0f}},
        {{ 340.0f, 0.0f,  340.0f}, {1.0f, 1.0f}},
        {{ 340.0f, 0.0f, -340.0f}, {1.0f, 0.0f}},
    };
    uint32_t gidx[6] = {0, 1, 2, 0, 2, 3};
    {
        D3D11_BUFFER_DESC vbd{sizeof(gverts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA vsd{gverts, 0, 0};
        if (FAILED(device_->CreateBuffer(&vbd, &vsd, &gnd_vb_))) return false;
        D3D11_BUFFER_DESC ibd{sizeof(gidx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA isd{gidx, 0, 0};
        if (FAILED(device_->CreateBuffer(&ibd, &isd, &gnd_ib_))) return false;
    }

    // Rasterizer states
    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_BACK;
    rsd.DepthClipEnable = TRUE;
    rsd.MultisampleEnable = (msaa_count_ > 1) ? TRUE : FALSE;
    if (FAILED(device_->CreateRasterizerState(&rsd, &rs_solid_))) return false;

    rsd.CullMode = D3D11_CULL_NONE;
    if (FAILED(device_->CreateRasterizerState(&rsd, &rs_nocull_))) return false;

    // Depth stencil states
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device_->CreateDepthStencilState(&dsd, &dss_default_))) return false;

    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device_->CreateDepthStencilState(&dsd, &dss_sky_))) return false;

    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device_->CreateDepthStencilState(&dsd, &dss_readonly_))) return false;

    // Blend states
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &bs_opaque_))) return false;

    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(device_->CreateBlendState(&bd, &bs_alpha_))) return false;

    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    if (FAILED(device_->CreateBlendState(&bd, &bs_additive_))) return false;

    return true;
}

void CityRenderer::Resize(int width, int height) {
    if (!swap_chain_ || width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    rtv_.Reset();
    dsv_.Reset();
    depth_tex_.Reset();
    swap_chain_->ResizeBuffers(0, static_cast<UINT>(width_), static_cast<UINT>(height_), DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTargets();
}

void CityRenderer::Shutdown() {
    static_vb_.Reset();
    static_ib_.Reset();
    dyn_vb_.Reset();
    dyn_ib_.Reset();
    alpha_vb_.Reset();
    alpha_ib_.Reset();
    add_vb_.Reset();
    add_ib_.Reset();
    sky_vb_.Reset();
    sky_ib_.Reset();
    gnd_vb_.Reset();
    gnd_ib_.Reset();
    cb_frame_.Reset();
    sky_layout_.Reset();
    sky_vs_.Reset();
    sky_ps_.Reset();
    bld_layout_.Reset();
    bld_vs_.Reset();
    bld_ps_.Reset();
    gnd_layout_.Reset();
    gnd_vs_.Reset();
    gnd_ps_.Reset();
    pt_layout_.Reset();
    pt_vs_.Reset();
    pt_ps_.Reset();
    rs_solid_.Reset();
    rs_nocull_.Reset();
    dss_default_.Reset();
    dss_sky_.Reset();
    dss_readonly_.Reset();
    bs_opaque_.Reset();
    bs_alpha_.Reset();
    bs_additive_.Reset();
    dsv_.Reset();
    depth_tex_.Reset();
    rtv_.Reset();
    swap_chain_.Reset();
    ctx_.Reset();
    device_.Reset();
}

void CityRenderer::RebuildStaticMesh(const std::vector<Building>& buildings, const std::vector<CityProp>& props) {
    std::vector<CityVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(buildings.size() * 600 + props.size() * 96);
    indices.reserve(buildings.size() * 900 + props.size() * 144);

    for (const auto& b : buildings) {
        if (!b.IsStaticSettled()) continue;
        AppendDetailedBuilding(verts, indices, b);
    }

    for (const auto& p : props) {
        AppendCityProp(verts, indices, p);
    }

    UploadDynamicPair(device_.Get(), ctx_.Get(), verts, indices,
                      static_vb_, static_ib_, static_vb_cap_, static_ib_cap_, static_index_count_);
}

void CityRenderer::UpdateDynamicMeshes(const std::vector<Building>& buildings,
                                       const std::vector<PlaneStrike>& strikes,
                                       const std::vector<TrafficCar>& cars,
                                       const std::vector<Particle3D>& particles,
                                       int hovered_idx,
                                       int selected_idx,
                                       float time_sec) {
    // 1. Opaque dynamic mesh: non-settled buildings, aircraft, cars, solid debris
    {
        std::vector<CityVertex> verts;
        std::vector<uint32_t> indices;
        verts.reserve(4096);
        indices.reserve(6144);

        for (const auto& b : buildings) {
            if (b.IsStaticSettled() || b.demolished) continue;
            AppendDetailedBuilding(verts, indices, b);
        }

        for (const auto& s : strikes) {
            if (!s.impacted && !s.finished) {
                AppendPlane(verts, indices, s);
            }
        }

        for (const auto& car : cars) {
            AppendTrafficCar(verts, indices, car);
        }

        for (const auto& pt : particles) {
            if (pt.kind != Particle3D::Kind::Debris) continue;
            float t = 1.0f - std::clamp(pt.life / std::max(0.01f, pt.max_life), 0.0f, 1.0f);
            float sz = pt.size + (pt.end_size - pt.size) * t;
            XMMATRIX rmat = XMMatrixRotationRollPitchYaw(pt.rot.x, pt.rot.y, pt.rot.z);
            XMFLOAT3 r{}, u{}, f{};
            XMStoreFloat3(&r, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rmat));
            XMStoreFloat3(&u, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rmat));
            XMStoreFloat3(&f, XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rmat));
            AppendOrientedBox(verts, indices, pt.pos, r, u, f, sz * 0.65f, sz * 0.42f, sz * 0.55f,
                              pt.color, {0.0f, 5.0f, sz, 0.0f});
        }

        UploadDynamicPair(device_.Get(), ctx_.Get(), verts, indices,
                          dyn_vb_, dyn_ib_, dyn_vb_cap_, dyn_ib_cap_, dyn_index_count_);
    }

    // 2. Alpha-blended mesh: smoke plumes, shockwaves, selection rings
    {
        std::vector<ParticleVertex> verts;
        std::vector<uint32_t> indices;
        verts.reserve(2048);
        indices.reserve(3072);

        // Ground selection / hover / strike ring
        auto draw_ring = [&](int idx, const XMFLOAT4& col, float pad) {
            if (idx < 0 || idx >= static_cast<int>(buildings.size())) return;
            const auto& b = buildings[static_cast<size_t>(idx)];
            if (b.demolished) return;
            float cx = (b.x0 + b.x1) * 0.5f;
            float cz = (b.z0 + b.z1) * 0.5f;
            float r = std::max(b.x1 - b.x0, b.z1 - b.z0) * 0.72f + pad;
            AppendGroundRingQuad(verts, indices, cx, 0.08f, cz, r, col);
        };
        if (hovered_idx >= 0 && hovered_idx != selected_idx) {
            draw_ring(hovered_idx, {0.35f, 0.88f, 1.0f, 0.65f}, 1.4f);
        }
        if (selected_idx >= 0) {
            float pulse = 0.75f + 0.25f * std::sin(time_sec * 5.0f);
            draw_ring(selected_idx, {1.0f, 0.82f, 0.22f, 0.85f * pulse}, 1.8f + 0.35f * pulse);
        }
        for (const auto& s : strikes) {
            if (!s.impacted && s.building_idx >= 0) {
                float pulse = 0.6f + 0.4f * std::sin(time_sec * 12.0f);
                draw_ring(s.building_idx, {1.0f, 0.22f, 0.16f, 0.85f * pulse}, 2.4f);
            }
        }

        for (const auto& pt : particles) {
            if (pt.kind != Particle3D::Kind::Smoke && pt.kind != Particle3D::Kind::Shockwave) continue;
            float u = 1.0f - std::clamp(pt.life / std::max(0.01f, pt.max_life), 0.0f, 1.0f);
            float sz = pt.size + (pt.end_size - pt.size) * u;
            float fade = (1.0f - u) * (1.0f - u);
            XMFLOAT4 col = pt.color;
            col.w *= fade;

            if (pt.kind == Particle3D::Kind::Shockwave) {
                AppendGroundRingQuad(verts, indices, pt.pos.x, pt.pos.y, pt.pos.z, sz, col);
            } else {
                XMMATRIX rmat = XMMatrixRotationRollPitchYaw(pt.rot.x, pt.rot.y, pt.rot.z);
                XMFLOAT3 r{}, up{}, f{};
                XMStoreFloat3(&r,  XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rmat));
                XMStoreFloat3(&up, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rmat));
                XMStoreFloat3(&f,  XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rmat));
                AppendParticleBox(verts, indices, pt.pos, r, up, f, sz * 0.5f, sz * 0.5f, sz * 0.5f, col, 0.0f, 1.0f);
            }
        }

        UploadDynamicPair(device_.Get(), ctx_.Get(), verts, indices,
                          alpha_vb_, alpha_ib_, alpha_vb_cap_, alpha_ib_cap_, alpha_index_count_);
    }

    // 3. Additive mesh: fireballs, sparks, impact flashes
    {
        std::vector<ParticleVertex> verts;
        std::vector<uint32_t> indices;
        verts.reserve(1536);
        indices.reserve(2304);

        for (const auto& pt : particles) {
            if (pt.kind != Particle3D::Kind::Fire && pt.kind != Particle3D::Kind::Spark) continue;
            float u = 1.0f - std::clamp(pt.life / std::max(0.01f, pt.max_life), 0.0f, 1.0f);
            float sz = pt.size + (pt.end_size - pt.size) * u;
            float fade = 1.0f - u;
            XMFLOAT4 col = pt.color;
            col.w *= fade;

            XMMATRIX rmat = XMMatrixRotationRollPitchYaw(pt.rot.x, pt.rot.y, pt.rot.z);
            XMFLOAT3 r{}, up{}, f{};
            XMStoreFloat3(&r,  XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rmat));
            XMStoreFloat3(&up, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rmat));
            XMStoreFloat3(&f,  XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rmat));
            float mode = (pt.kind == Particle3D::Kind::Spark) ? 2.0f : 0.0f;
            float boost = (pt.kind == Particle3D::Kind::Spark) ? 2.4f : 1.8f;
            AppendParticleBox(verts, indices, pt.pos, r, up, f, sz * 0.5f, sz * 0.5f, sz * 0.5f, col, mode, boost);
        }

        UploadDynamicPair(device_.Get(), ctx_.Get(), verts, indices,
                          add_vb_, add_ib_, add_vb_cap_, add_ib_cap_, add_index_count_);
    }
}

void CityRenderer::BeginFrame(float /*time_sec*/) {
    const float clear_col[4] = {0.015f, 0.024f, 0.052f, 1.0f};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
    ctx_->ClearRenderTargetView(rtv_.Get(), clear_col);
    ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
}

void CityRenderer::RenderCity(const std::vector<Building>& buildings,
                              const std::vector<CityProp>& props,
                              const std::vector<PlaneStrike>& strikes,
                              const std::vector<TrafficCar>& cars,
                              const std::vector<Particle3D>& particles,
                              int hovered_idx,
                              int selected_idx,
                              const Camera& cam,
                              float time_sec,
                              uint64_t static_mesh_epoch) {
    float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    XMMATRIX view = cam.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(54.0f), aspect, 0.4f, 900.0f);
    XMMATRIX vp = XMMatrixTranspose(view * proj);

    float hov_id = (hovered_idx >= 0 && hovered_idx < static_cast<int>(buildings.size()))
        ? static_cast<float>(buildings[static_cast<size_t>(hovered_idx)].id) : 0.0f;
    float sel_id = (selected_idx >= 0 && selected_idx < static_cast<int>(buildings.size()))
        ? static_cast<float>(buildings[static_cast<size_t>(selected_idx)].id) : 0.0f;

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(ctx_->Map(cb_frame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        auto* cb = static_cast<FrameCB*>(ms.pData);
        XMStoreFloat4x4(&cb->view_proj, vp);
        cb->cam_pos = {cam.pos.x + cam.shake_offset.x, cam.pos.y + cam.shake_offset.y, cam.pos.z + cam.shake_offset.z, 1.0f};
        cb->time_and_fog = {time_sec, 135.0f, 340.0f, 0.0f};
        cb->highlight_ids = {hov_id, sel_id, 0.0f, 0.0f};
        for (int k = 0; k < 4; ++k) cb->strikes[k] = {0.0f, 0.0f, 0.0f, 0.0f};

        int sk = 0;
        for (const auto& b : buildings) {
            if (b.burn_intensity > 0.01f && !b.demolished && sk < 4) {
                cb->strikes[sk++] = {
                    static_cast<float>(b.id),
                    b.impact_y,
                    b.burn_intensity,
                    b.collapse_timer
                };
            }
        }
        ctx_->Unmap(cb_frame_.Get(), 0);
    }

    ctx_->VSSetConstantBuffers(0, 1, cb_frame_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cb_frame_.GetAddressOf());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float blend_factor[4] = {0, 0, 0, 0};

    // 1. Sky Dome
    ctx_->RSSetState(rs_nocull_.Get());
    ctx_->OMSetDepthStencilState(dss_sky_.Get(), 0);
    ctx_->OMSetBlendState(bs_opaque_.Get(), blend_factor, 0xFFFFFFFF);
    {
        UINT stride = sizeof(SkyVertex);
        UINT offset = 0;
        ctx_->IASetInputLayout(sky_layout_.Get());
        ctx_->IASetVertexBuffers(0, 1, sky_vb_.GetAddressOf(), &stride, &offset);
        ctx_->IASetIndexBuffer(sky_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->VSSetShader(sky_vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(sky_ps_.Get(), nullptr, 0);
        ctx_->DrawIndexed(sky_index_count_, 0, 0);
    }

    // 2. Ground + Road Network
    ctx_->RSSetState(rs_solid_.Get());
    ctx_->OMSetDepthStencilState(dss_default_.Get(), 0);
    {
        UINT g_stride = sizeof(GroundVertex);
        UINT g_offset = 0;
        ctx_->IASetInputLayout(gnd_layout_.Get());
        ctx_->IASetVertexBuffers(0, 1, gnd_vb_.GetAddressOf(), &g_stride, &g_offset);
        ctx_->IASetIndexBuffer(gnd_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->VSSetShader(gnd_vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(gnd_ps_.Get(), nullptr, 0);
        ctx_->DrawIndexed(6, 0, 0);
    }

    // 3. Static Cached City Geometry
    if (cached_epoch_ != static_mesh_epoch) {
        cached_epoch_ = static_mesh_epoch;
        RebuildStaticMesh(buildings, props);
    }

    ctx_->IASetInputLayout(bld_layout_.Get());
    ctx_->VSSetShader(bld_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(bld_ps_.Get(), nullptr, 0);

    if (static_index_count_ > 0 && static_vb_ && static_ib_) {
        UINT b_stride = sizeof(CityVertex);
        UINT b_offset = 0;
        ctx_->IASetVertexBuffers(0, 1, static_vb_.GetAddressOf(), &b_stride, &b_offset);
        ctx_->IASetIndexBuffer(static_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(static_index_count_, 0, 0);
    }

    // 4. Dynamic Opaque Geometry (rising/collapsing buildings, planes, traffic, debris)
    UpdateDynamicMeshes(buildings, strikes, cars, particles, hovered_idx, selected_idx, time_sec);
    if (dyn_index_count_ > 0 && dyn_vb_ && dyn_ib_) {
        UINT b_stride = sizeof(CityVertex);
        UINT b_offset = 0;
        ctx_->IASetVertexBuffers(0, 1, dyn_vb_.GetAddressOf(), &b_stride, &b_offset);
        ctx_->IASetIndexBuffer(dyn_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(dyn_index_count_, 0, 0);
    }

    // 5. Transparent & Additive FX Passes
    ctx_->OMSetDepthStencilState(dss_readonly_.Get(), 0);
    ctx_->RSSetState(rs_nocull_.Get());
    ctx_->IASetInputLayout(pt_layout_.Get());
    ctx_->VSSetShader(pt_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(pt_ps_.Get(), nullptr, 0);

    if (alpha_index_count_ > 0 && alpha_vb_ && alpha_ib_) {
        ctx_->OMSetBlendState(bs_alpha_.Get(), blend_factor, 0xFFFFFFFF);
        UINT p_stride = sizeof(ParticleVertex);
        UINT p_offset = 0;
        ctx_->IASetVertexBuffers(0, 1, alpha_vb_.GetAddressOf(), &p_stride, &p_offset);
        ctx_->IASetIndexBuffer(alpha_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(alpha_index_count_, 0, 0);
    }

    if (add_index_count_ > 0 && add_vb_ && add_ib_) {
        ctx_->OMSetBlendState(bs_additive_.Get(), blend_factor, 0xFFFFFFFF);
        UINT p_stride = sizeof(ParticleVertex);
        UINT p_offset = 0;
        ctx_->IASetVertexBuffers(0, 1, add_vb_.GetAddressOf(), &p_stride, &p_offset);
        ctx_->IASetIndexBuffer(add_ib_.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(add_index_count_, 0, 0);
    }

    // Restore default state for ImGui
    ctx_->OMSetBlendState(bs_opaque_.Get(), blend_factor, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(dss_default_.Get(), 0);
    ctx_->RSSetState(rs_solid_.Get());
}

void CityRenderer::EndFrame() {
    swap_chain_->Present(1, 0);
}

int CityRenderer::PickBuilding(const std::vector<Building>& buildings, const Camera& cam, float mouse_x, float mouse_y) const {
    if (width_ <= 0 || height_ <= 0) return -1;

    float ndc_x = (2.0f * mouse_x) / static_cast<float>(width_) - 1.0f;
    float ndc_y = 1.0f - (2.0f * mouse_y) / static_cast<float>(height_);

    float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    XMMATRIX view = cam.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(54.0f), aspect, 0.4f, 900.0f);
    XMMATRIX inv_vp = XMMatrixInverse(nullptr, view * proj);

    XMVECTOR p_near = XMVector3TransformCoord(XMVectorSet(ndc_x, ndc_y, 0.0f, 1.0f), inv_vp);
    XMVECTOR p_far  = XMVector3TransformCoord(XMVectorSet(ndc_x, ndc_y, 1.0f, 1.0f), inv_vp);
    XMVECTOR dir = XMVector3Normalize(p_far - p_near);

    int best_idx = -1;
    float best_t = 1e9f;
    for (int i = 0; i < static_cast<int>(buildings.size()); ++i) {
        const auto& b = buildings[static_cast<size_t>(i)];
        if (b.collapsing || b.demolished || b.strike_incoming || b.height <= 0.1f) continue;
        float t = 0.0f;
        if (RayIntersectsAABB(p_near, dir, {b.x0, 0.0f, b.z0}, {b.x1, b.height + 1.5f, b.z1}, t)) {
            if (t < best_t) {
                best_t = t;
                best_idx = i;
            }
        }
    }
    return best_idx;
}

bool CityRenderer::ProjectToScreen(const XMFLOAT3& world_pos, const Camera& cam, float& out_x, float& out_y) const {
    if (width_ <= 0 || height_ <= 0) return false;
    float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    XMMATRIX view = cam.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(54.0f), aspect, 0.4f, 900.0f);
    XMMATRIX vp = view * proj;

    XMVECTOR p = XMVectorSet(world_pos.x, world_pos.y, world_pos.z, 1.0f);
    XMVECTOR clip = XMVector4Transform(p, vp);
    XMFLOAT4 c{};
    XMStoreFloat4(&c, clip);
    if (c.w <= 0.1f) return false;

    float ndc_x = c.x / c.w;
    float ndc_y = c.y / c.w;
    if (ndc_x < -1.15f || ndc_x > 1.15f || ndc_y < -1.15f || ndc_y > 1.15f) return false;

    out_x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(width_);
    out_y = (0.5f - ndc_y * 0.5f) * static_cast<float>(height_);
    return true;
}

} // namespace city
