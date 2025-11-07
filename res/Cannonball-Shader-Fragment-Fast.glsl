precision mediump float;

/* ---------------------------------------------------------------------------- */
/*                                                                              */
/* Super-Lightweight Pixel shader for Cannonball, by James Pearce,              */
/* Copyright (c) 2025.                                                          */
/*                                                                              */
/* Provides processing to provide a game image that looks broadly like it's on  */
/* on real CRT, when combined with Blargg filtering, which is done CPU side.    */
/*                                                                              */
/* Provided for Pi2 (v1.1) and supports 60fps in hires mode based on:           */
/* 1280x1024@60fps with vSync, and clocks:                                      */
/*                                                                              */
/* cpu_freq=1050                                                                */
/* gpu_freq=450                                                                 */
/*                                                                              */
/* Specifically, provides curvature, shadow mask, and brightness boost.         */
/* Full shader also provides noise, vignette and desaturation.                  */
/* Vignette is provided with this shader via the overlay.                       */
/*                                                                              */
/* ---------------------------------------------------------------------------- */


/* Inputs from Vertex Shader */
uniform sampler2D Texture;
uniform sampler2D Overlay;
varying vec2 v_texCoord;
varying vec2 maskpos;


/* Inputs from Application */
/* User configurable settings than can be adjusted in the UI */
uniform float warpX;
uniform float warpY;
uniform vec2  invExpand;        // (1/expandX, 1/expandY)
uniform float brightboost;
uniform float noiseIntensity;   // not available in this shader
uniform float vignette;         // not available in this shader

uniform float desat_inv0;       // set to 1.0 / (1.0 + desaturate)
uniform float desat_inv1;       // set to 1.0 / (1.0 + desaturate + desaturateEdges)

uniform float baseOff;          // shadow mask dim value, e.g. 0.75f
uniform float baseOn;           // shadow mask boost value, usually 1/baseOff e.g. 1.333f

/* Shadow mask
 * maskPitch is between 3 and 6. Setting invMaskPitch saves division in the mask.
 * Use 3 for 1280x1024 screens, 4+ provide wider spacing e.g. high DPI screens
 * Hence, invMaskPitch is 1/3, 1/4, 1/5, or 1/6.
 * inv2MaskPitch is 1/(2*maskPitch) - i.e., 1/6 when invMaskPitch is 1/3.
 * inv2Height is 1/2,1/4,1/6,1/8... - i.e., the pattern height. Odd values won't work correctly.
 */
uniform float invMaskPitch;
uniform float inv2MaskPitch;
uniform float inv2Height;

uniform vec2 u_Time;           // set both elements to FrameCount / 60.0


// -------------------------------------------------------------------
// CRT Texture Warp function. Creates the illusion of curved glass.
// -------------------------------------------------------------------

vec2 Warp(vec2 pos)
{
    // Expand the input texture coordinates about the center (0.5, 0.5)
    // This remaps pos so that if expandX or expandY is not 1.0, the image is zoomed in or out.
    // Since divides are expense, we pass in invExpand as (1/expandX,1/expandY) so we multiply only
    // pos = (pos - 0.5) / vec2(expandX, expandY) + 0.5;
    vec2 centered = pos - 0.5;
    pos = centered * invExpand + 0.5;

    // Convert [0,1] texture coordinates to [-1,1]
    pos = pos * 2.0 - 1.0;    
    
    // Apply the warp distortion
    pos *= vec2(1.0 + (pos.y * pos.y) * warpX,
                1.0 + (pos.x * pos.x) * warpY);
                
    // Convert back to [0,1] texture coordinates
    return pos * 0.5 + 0.5;
}


// ------------------------------------------------------------------------
// CRT ShadowMask function. Creates an effect similar to an arcade monitor.
// ------------------------------------------------------------------------

mediump vec3 fastmask()
{
    mediump vec2 mpos = floor(maskpos);

    // Compute both x fracts together to limit temporaries
    mediump vec2 mx = fract(mpos.xx * vec2(invMaskPitch, inv2MaskPitch));
    mediump float my = fract(mpos.y * inv2Height);

    mediump float condA = step(0.02, mx.x);
    mediump float condB = step(0.5,  mx.y);

    // Vertical windows matching the original
    mediump float v0    = 1.0 - step(0.02, my);                 // near 0
    mediump float vhalf = 1.0 - step(0.02, abs(my - 0.5));      // near 0.5
    mediump float extraMask = mix(v0, vhalf, condB);

    mediump float base = mix(baseOff, baseOn, condA * (1.0 - extraMask));
    return vec3(base);
}


// -------------------------------------------------------------------
// Main function. Shader runs here.
// -------------------------------------------------------------------


void main()
{
    // Apply curved glass effect
    vec2 warpedUV = Warp(v_texCoord);

    // Sample texture colour and calculate luminance
    vec3 pCol = texture2D(Texture, warpedUV).rgb;

    float lum = (pCol.r + pCol.g + pCol.b) * 0.33333 * 0.9;

    // Apply mask effect
    pCol *= mix(fastmask(), vec3(1.0), lum);

    // Apply brighening
    pCol *= brightboost;

    // Apply overlay
    pCol *= texture2D(Overlay, v_texCoord).rgb;
    
    gl_FragColor = vec4(pCol, 1.0);
}
