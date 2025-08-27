precision highp float;

/* ---------------------------------------------------------------------------- */
/*                                                                              */
/* Lightweight Pixel shader for Cannonball, by James Pearce, Copyright (c) 2025 */
/*                                                                              */
/* Provides processing to provide a game image that looks broadly like it's on  */
/* on real CRT, when combined with Blargg filtering, which is done CPU side.    */
/*                                                                              */
/* Pi Zero 2W can run this at 60fps under Raspbian command-line installation    */
/* based on: 1280x1024@60fps with vSync, and clocks:                            */
/*                                                                              */
/* arm_freq=1100                                                                */
/* core_freq=450                                                                */
/* gpu_freq=450                                                                 */
/*                                                                              */
/* Specifically, provides curvature, noise, vignette, shadow mask, and          */
/* brightness boost. -Fast variant provides curvature and noise only.           */
/*                                                                              */
/* Note - requires highp for accurate shadow mask. Banding will be seen with    */
/*        mediump on GPUs that support fp16.                                    */
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
uniform float desaturate;
uniform float desaturateEdges;
uniform float baseOff;         // shadow mask dim value, e.g. 0.75f
uniform float baseOn;          // shadow mask boost value, usually 1/baseOff e.g. 1.333f
uniform float invMaskPos;      // mask size, 1/1 = normal (1280x1024 screens), 1/2 = high DPI screens
uniform vec2 u_Time; // set both elements to FrameCount / 60.0


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
    const float pi = 3.14159265;
    // Wrap x to the range [-pi, pi]
    x = mod(x + pi, 2.0 * pi) - pi;
    // Approximation: sin(x) ? 1.27323954*x - 0.405284735*x*abs(x)
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

vec3 fastmask()
{
    vec2 mpos = floor(maskpos * invMaskPos);
    //vec2 mpos = floor(maskpos);
    float tmpvar_1 = fract(mpos.x / 3.0);
    float tmpvar_2 = fract(mpos.x / 6.0);
    float tmpvar_3 = fract(mpos.y / 2.0);
    
    // Step 1: choose base value without an if statement.
    // float base = mix(0.7498125, 1.333, step(0.001, tmpvar_1));
    float base = mix(baseOff, baseOn, step(0.001, tmpvar_1));
    
    // Step 2: choose the multiplier conditionally.
    float condA = step(0.001, tmpvar_1);
    float condBranchA = 1.0 - step(0.5, tmpvar_2);
    float condBranchB = step(0.5, tmpvar_2);
    float branchSelector = step(tmpvar_3, 0.001);
    float maskCondition = branchSelector * condBranchA + (1.0 - branchSelector) * condBranchB;
    float multiplier = mix(1.0, baseOff, condA * maskCondition);
    
    float maskVal = base * multiplier;
    return vec3(maskVal);
}



// ------------------------------------------------------------------------
// DesaturateAndVignette. This calculates the distance from the centre of
// the image and brings up the black-point and down the overall luminance
// according to the configured desaturateEdges and vignette values.
// An overall desaturation is also applied.
// ------------------------------------------------------------------------
vec3 DesaturateAndVignette(vec2 pos, vec3 colour)
{
    // Compute a uniform base offset.
    float baseOffset = desaturate;

    // Compute an additional offset based on distance from the center.
    // First, compute a 2D vector from the center (0.5, 0.5) to the current uv.
    vec2 centeredUV = pos - vec2(0.5);

    // Compute the squared distance (avoiding sqrt for efficiency).
    float distSq = dot(centeredUV, centeredUV);

    // The maximum squared distance in a unit square centered at (0.5, 0.5)
    // is 0.5 (at the corners, where centeredUV = (0.5, 0.5)).
    // Normalize so that 0 is at the center and 1 at the corners.
    float normDist = distSq * 2.0;
    
    // The additional offset increases linearly with the normalized distance.
    float edgeOffset = normDist * desaturateEdges;
    
    // Total offset is the sum of the base and edge-dependent offsets.
    float totalOffset = baseOffset + edgeOffset;
    
    // Raise the black level but leave white unchanged - Moves black from 0 to
    // totalOffset/(1+totalOffset)
    //
    // the following is equivalent to but potentially 1 div instead of 3:
    // vec3 pCol = (colour + vec3(totalOffset)) / (1.0 + totalOffset);

    float invDenom   = 1.0 / (1.0 + totalOffset);
    vec3  pCol       = (colour + vec3(totalOffset)) * invDenom;    
    
    // Apply vignette using the same distance calculation
    float dimVal = normDist * vignette;
    pCol *= (1.0-dimVal);

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
