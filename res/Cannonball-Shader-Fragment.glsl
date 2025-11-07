precision mediump float;

/* ---------------------------------------------------------------------------- */
/*                                                                              */
/* Lightweight Pixel shader for Cannonball, by James Pearce, Copyright (c) 2025 */
/*                                                                              */
/* Provides processing to provide a game image that looks broadly like it's on  */
/* on real CRT, when combined with Blargg filtering, which is done CPU side.    */
/*                                                                              */
/* Pi Zero 2W and Pi3 can run this at 60fps under Raspbian command-line         */
/* installation based on: 1280x1024@60fps with vSync, and clocks:               */
/*                                                                              */
/* gpu_freq=450                                                                 */
/*                                                                              */
/* Can also be used on Pi1 at 30fps at stock clocks.                            */
/*                                                                              */
/* Specifically, provides curvature, noise, vignette, desaturation, shadow mask,*/
/* and brightness boost. Use -Fast variant on Pi2, which skips noise,           */
/* desaturation and vignette (vignette can be done via the overlay though).     */
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
uniform float vignette;

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


// -------------------------------------------------------------------
// Random noise generator. Creates the illusion of analogue signal processing.
// -------------------------------------------------------------------

// Fast approximate sine function.


// This function wraps the input to [-pi, pi] and approximates sin(x)
// using a simple linear/quadratic combination.

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


// ------------------------------------------------------------------------
// DesaturateAndVignette. This calculates the distance from the centre of
// the image and brings up the black-point and down the overall luminance
// according to the configured desaturateEdges and vignette values.
// An overall desaturation is also applied.
// ------------------------------------------------------------------------

vec3 DesaturateAndVignette(vec2 pos, vec3 colour)
{
    // n in [0,1] from center to corners, without a temp vec2
    float n = (dot(pos, pos) - (pos.x + pos.y) + 0.5) * 2.0;

    // Desaturate - approximate reciprocal with mix
    float invApprox = mix(desat_inv0, desat_inv1, n);

    // (colour + offset)*inv  ==  mix(1.0, colour, inv)
    vec3 pCol = mix(vec3(1.0), colour, invApprox);

    // Apply vignette (darken edges)
    pCol *= (1.0 - n * vignette);

    return pCol;
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

    // Apply desaturation and vignette
    pCol = DesaturateAndVignette(warpedUV, pCol);

    // Apply mask effect
    pCol *= mix(fastmask(), vec3(1.0), lum);

    // Apply brighening
    pCol *= brightboost;

    // Apply overlay
    pCol *= texture2D(Overlay, v_texCoord).rgb;
    
    gl_FragColor = vec4(pCol, 1.0);
}
