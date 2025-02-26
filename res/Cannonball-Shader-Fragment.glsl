#version 100

/* ----------------------------------------------------------------------------- */
/*                                                                               */
/* Lightweight Pixel shader for Cannonball, by James Pearce, Copyright (c) 2025  */
/*                                                                               */
/* Provides processing to provide a game image that looks broadly like it's on   */
/* on real CRT, when combined with Blargg filtering, which is done CPU side.     */
/*                                                                               */
/* Pi Zero 2W can run this at 60fps under Raspbian command-line installation     */
/* based on: 1280x1024@60fps with vSync, and clocks:                             */
/*                                                                               */
/* arm_freq=1200                                                                 */
/* core_freq=450                                                                 */
/* gpu_freq=450                                                                  */
/* sdram_freq=450                                                                */
/*                                                                               */
/* ----------------------------------------------------------------------------- */


/* Version independence defines */
#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

#if __VERSION__ >= 130
out COMPAT_PRECISION vec4 FragColor;
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

/* Inputs from Vertex Shader */
uniform sampler2D Texture;
COMPAT_VARYING vec2 v_texCoord;
COMPAT_VARYING vec2 maskpos;


/* Inputs from Application */
/* User configurable settings than can be adjusted in the UI */
uniform COMPAT_PRECISION float warpX;
uniform COMPAT_PRECISION float warpY;
uniform COMPAT_PRECISION float expandX;
uniform COMPAT_PRECISION float expandY;
uniform COMPAT_PRECISION float brightboost;
uniform COMPAT_PRECISION float noiseIntensity;
uniform COMPAT_PRECISION float vignette;
uniform COMPAT_PRECISION float desaturate;
uniform COMPAT_PRECISION float desaturateEdges;
uniform COMPAT_PRECISION float Shadowmask;
uniform COMPAT_PRECISION vec2 u_Time; // set both elements to FrameCount / 60.0

/* Constants */
#define masksize 1.0 


// -------------------------------------------------------------------
// CRT Texture Warp function. Creates the illusion of curved glass.
// -------------------------------------------------------------------

vec2 Warp(vec2 pos)
{
    // Expand the input texture coordinates about the center (0.5, 0.5)
    // This remaps pos so that if expandX or expandY is not 1.0, the image is zoomed in or out.
    pos = (pos - 0.5) / vec2(expandX, expandY) + 0.5;

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
    // Remove the unused computation:
    // vec2 dummy = floor(pos / masksize);
    vec2 mpos = floor(maskpos);
    float tmpvar_1 = fract(mpos.x / 3.0);
    float tmpvar_2 = fract(mpos.x / 6.0);
    float tmpvar_3 = fract(mpos.y / 2.0);
    
    // Step 1: choose base value without an if statement.
    float base = mix(0.7498125, 1.333, step(0.001, tmpvar_1));
    
    // Step 2: choose the multiplier conditionally.
    float condA = step(0.001, tmpvar_1);
    float condBranchA = 1.0 - step(0.5, tmpvar_2);
    float condBranchB = step(0.5, tmpvar_2);
    float branchSelector = step(tmpvar_3, 0.001);
    float maskCondition = branchSelector * condBranchA + (1.0 - branchSelector) * condBranchB;
    float multiplier = mix(1.0, 0.75, condA * maskCondition);
    
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
    vec3 pCol = (colour + vec3(totalOffset)) / (1.0 + totalOffset);
    
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

    // Uncomment the if statement to black-out areas beyond the texture to calibrate warp
    // and stretch in your configuration combined with CRT shape mask.
    // Conditional branches are very expensive on VideoCore IV GPU so this line will
    // reduce framerate on Pi Zero 2W and Pi3.
    // if (warpedUV.x < 0.0 || warpedUV.x > 1.0 || warpedUV.y < 0.0 || warpedUV.y > 1.0)
    //    pCol = vec3(0.0);
        
    //vec3 lumWeighting = vec3(0.299,0.587,0.114);
    //float lum=dot(pCol, lumWeighting);

    // Add noise
    pCol = addNoise(pCol, warpedUV, noiseIntensity);

    // Apply desaturation and vignette
    pCol = DesaturateAndVignette(warpedUV, pCol);

    // Apply mask effect
    pCol *= mix(fastmask(), vec3(1.0), lum);

    // Apply brighening
    pCol *= brightboost;

    FragColor = vec4(pCol, 1.0);
}
