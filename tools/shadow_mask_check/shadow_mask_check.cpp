// Copyright (c) 2026 James Pearce.  Part of Cannonball-SE.
// Checks shadow_mask::weight()/bake() against a literal fp32 transliteration of fastmask().
#include "shadow_mask_bake.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static float fract32(float v) { return v - std::floor(v); }
static float step32(float edge, float x) { return x < edge ? 0.0f : 1.0f; }

// Exactly fastmask() with mpos = (x, y) and the uniforms as rendersurface.cpp:1003-1006 derives them.
static float glsl_w(int x, int y, int P)
{
    const float invMaskPitch  = 1.0f /    float(P);
    const float inv2MaskPitch = 1.0f / (2*float(P));
    const float inv2Height    = 1.0f / (2*float(P-2));
    const float mx0 = fract32(float(x) * invMaskPitch);
    const float mx1 = fract32(float(x) * inv2MaskPitch);
    const float my  = fract32(float(y) * inv2Height);
    const float condA = step32(0.02f, mx0);
    const float condB = step32(0.5f,  mx1);
    const float v0    = 1.0f - step32(0.02f, my);
    const float vhalf = 1.0f - step32(0.02f, std::fabs(my - 0.5f));
    const float extra = v0 * (1.0f - condB) + vhalf * condB;   // mix(v0, vhalf, condB)
    return condA * (1.0f - extra);
}

static bool same(float g, bool s) { return (g == 0.0f || g == 1.0f) && ((g == 1.0f) == s); }

int main(int argc, char** argv)
{
    // Optional: --dump W H P out.raw  writes the baked buffer for the GPU harness (A2).
    if (argc == 6 && std::string(argv[1]) == "--dump") {
        const int W = atoi(argv[2]), H = atoi(argv[3]), P = atoi(argv[4]);
        std::vector<uint8_t> buf((size_t)W * H);
        shadow_mask::bake(buf.data(), W, H, P);
        FILE* f = fopen(argv[5], "wb"); fwrite(buf.data(), 1, buf.size(), f); fclose(f);
        return 0;
    }
    long failures = 0;
    // 1) Oracle vs GLSL transliteration along each axis (the pattern is separable), P = 3..26,
    //    coordinates 0..8191. Rows/columns chosen to exercise every branch of the other axis.
    for (int P = 3; P <= 26; ++P) {
        const int Q = 2 * (P - 2);
        for (int t = 0; t < 8192; ++t) {
            for (int y : {0, Q / 2, 1})            // v0 row, vhalf row, neither
                if (!same(glsl_w(t, y, P), shadow_mask::weight(t, y, P))) ++failures;
            for (int x : {0, 1, P, P + 1})         // condA 0/1 x condB 0/1
                if (!same(glsl_w(x, t, P), shadow_mask::weight(x, t, P))) ++failures;
        }
    }
    // 2) Full frame: bake() == weight() == GLSL at the real resolutions for the menu range.
    const int sizes[][2] = {{1280, 1024}, {1920, 1080}, {1024, 768}, {800, 600}};
    for (auto& s : sizes) for (int P = 3; P <= 6; ++P) {
        const int W = s[0], H = s[1];
        std::vector<uint8_t> buf((size_t)W * H);
        shadow_mask::bake(buf.data(), W, H, P);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t b = buf[(size_t)y * W + x];
            if (b != 0x00 && b != 0xFF) ++failures;
            if ((b == 0xFF) != shadow_mask::weight(x, y, P)) ++failures;
            if (!same(glsl_w(x, y, P), b == 0xFF)) ++failures;
        }
    }
    std::printf("shadow_mask_check: %ld mismatches\n", failures);
    return failures ? 1 : 0;
}
