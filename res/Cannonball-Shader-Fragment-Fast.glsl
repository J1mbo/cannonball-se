precision mediump float;

/* ---------------------------------------------------------------------------- */
/*                                                                              */
/* Super-lightweight Pixel shader for Cannonball, by James Pearce,              */
/* Copyright (c) 2025                                                           */
/*                                                                              */
/* Provides processing to provide a game image that looks broadly like it's on  */
/* on real CRT, when combined with Blargg filtering (which is done CPU side)    */
/* and a suitable overlay providing edges, shadow mask, and vignette.           */
/*                                                                              */
/* Specifically, provides curvature, noise, and brightness boost.               */
/*                                                                              */
/* Note - mediump is sufficient for this shader, and may provide a performance  */
/*        advantage on some mobile GPUs.                                        */
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
uniform vec2  invExpand;       // (1/expandX, 1/expandY)
uniform float brightboost;
uniform float noiseIntensity;
uniform vec2 u_Time;           // set both elements to FrameCount / 60.0

/* The following are unused in this shader and are here to provide compatibility with the full shader environment */
uniform float vignette;
uniform float desat_inv0;
uniform float desat_inv1;
uniform float baseOff;
uniform float baseOn;
uniform float invMaskPitch;
uniform float inv2MaskPitch;
uniform float inv2Height;



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


// -------------------------------------------------------------------
// Random noise generator. Creates the illusion of analogue signal processing.
// -------------------------------------------------------------------


// Fast approximate sine function.
// This function wraps the input to [-pi, pi] and approximates sin(x)
// using a simple linear/quadratic combination.
/*
float fastSin(float x) {
    const float pi = 3.14159265;
    // Wrap x to the range [-pi, pi]
    x = mod(x + pi, 2.0 * pi) - pi;
    // Approximation: sin(x) ? 1.27323954*x - 0.405284735*x*abs(x)
    return 1.27323954 * x - 0.405284735 * x * abs(x);
}
*/

float fastSin(float x) {
    // Wrap x to [-pi, pi] using fract (cheaper than mod on VC4)
    // INV_TAU = 1/(2*pi), TAU = 2*pi
    const float INV_TAU = 0.15915494;  // 1/(2π)
    const float TAU     = 6.2831853;   // 2π
    const float PI      = 3.1415927;

    // Bring phase down to [0,1) then scale to [-pi,pi]
    x = fract(x * INV_TAU) * TAU - PI;

    // Same 2-term parabolic sine approximation you used
    // sin(x) ≈ 1.27323954*x - 0.405284735*x*abs(x)
    return 1.27323954 * x - 0.405284735 * x * abs(x);
}


// Generate a pseudo-random value in the range [0,1) based on a 2D coordinate,
// using the fastSin function instead of the standard sin() for better performance.
float fastRand(vec2 co) {
    // Compute a dot product and pass it through fastSin.
    return fract(fastSin(dot(co + u_Time, vec2(12.9898, 78.233))) * 43758.5453);
}

// Adds noise to the input colour.
// 'uv' is typically the texture coordinate, and 'intensity' scales the noise.
vec3 addNoise(vec3 colour, vec2 uv, float intensity) {
    // Generate noise centered around 0
    float noise = fastRand(uv) - 0.5;
    // Add the noise scaled by the intensity to the RGB channels.
    colour += noise * intensity;
    return colour;
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

    // Add noise
    pCol = addNoise(pCol, warpedUV, noiseIntensity);

    // Apply brighening
    pCol *= brightboost;

    // Apply overlay
    pCol *= texture2D(Overlay, v_texCoord).rgb;
    
    gl_FragColor = vec4(pCol, 1.0);
}
