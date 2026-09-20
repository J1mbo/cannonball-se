#pragma once
// Copyright (c) 2025, 2026 James Pearce.  Part of Cannonball-SE.
// CRT shadow-mask weight bake. Reproduces, per output pixel, the {0,1} weight
// w = condA * (1.0 - extraMask) that fastmask() in res/Cannonball-Shader-Fragment*.glsl used to
// compute per fragment. Design and proof of equivalence: tools/GLSL_SHADER_PERFORMANCE_ANALYSIS.md section 3.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow_mask {

// Effective pitch: identical to the uniform derivation in rendersurface.cpp
// (int this_mask_size = std::max(3, config.video.mask_size)).
inline int effective_pitch(int cfg_mask_size) { return cfg_mask_size < 3 ? 3 : cfg_mask_size; }

// Reference oracle: w for column x (from the left) and row y (from the top), pitch P >= 3.
// Equals the GLSL for 3 <= P <= 26 (LLD section 3.1). Used by bake() and by the check program.
inline bool weight(int x, int y, int P)
{
    const int  P2    = 2 * P;            // period of mx.y  (inv2MaskPitch = 1/(2P))
    const int  Q     = 2 * (P - 2);      // vertical period (inv2Height   = 1/(2(P-2)))
    const bool condA = (x % P)  != 0;    // step(0.02, fract(x * invMaskPitch))
    const bool condB = (x % P2) >= P;    // step(0.5,  fract(x * inv2MaskPitch))
    const int  yq    = y % Q;
    const bool v0    = (yq == 0);        // 1.0 - step(0.02, fract(y * inv2Height))
    const bool vhalf = (yq == Q / 2);    // 1.0 - step(0.02, abs(fract(y * inv2Height) - 0.5))
    const bool extraMask = condB ? vhalf : v0;   // mix(v0, vhalf, condB)
    return condA && !extraMask;                  // condA * (1.0 - extraMask)
}

// Fills dst (tightly packed, W bytes per row, row 0 = top of the screen) with 0xFF where w == 1
// and 0x00 where w == 0. Separable: O(W + H) integer divisions, then a lookup per pixel.
inline void bake(uint8_t* dst, int W, int H, int P)
{
    const int P2 = 2 * P;
    const int Q  = 2 * (P - 2);
    std::vector<uint8_t> colA((size_t)W), colB((size_t)W);
    for (int x = 0; x < W; ++x) {
        colA[(size_t)x] = (uint8_t)((x % P)  != 0);
        colB[(size_t)x] = (uint8_t)((x % P2) >= P);
    }
    for (int y = 0; y < H; ++y) {
        const int     yq    = y % Q;
        const uint8_t v0    = (uint8_t)(yq == 0);
        const uint8_t vhalf = (uint8_t)(yq == Q / 2);
        uint8_t* row = dst + (size_t)y * (size_t)W;
        for (int x = 0; x < W; ++x) {
            const uint8_t extra = colB[(size_t)x] ? vhalf : v0;
            row[x] = (colA[(size_t)x] && !extra) ? 0xFF : 0x00;
        }
    }
}

} // namespace shadow_mask
