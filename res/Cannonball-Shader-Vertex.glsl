precision highp float;

/* ---------------------------------------------------------------------------------------- */
/*                                                                                          */
/* Lightweight Vertex shader for Cannonball, by James Pearce, Copyright (c) 2025            */
/*                                                                                          */
/* Provides processing to provide a game image that looks broadly like it's on              */
/* on real CRT, when combined with Blargg filtering which is done CPU side.                 */
/* Pi Zero 2W can run this at 60fps under Raspbian command-line installation.               */
/*                                                                                          */
/* Can be used with either:                                                                 */
/* - Cannonball-Shader-Fragment.glsl      - CRT curvature, noise, shadow mask, vignette, or */
/* - Cannonball-Shader-Fragment-Fast.gsls - CRT curvature and noise only                    */
/*                                                                                          */
/* ---------------------------------------------------------------------------------------- */


attribute vec4 VertexCoord;
attribute vec2 TexCoord;

uniform vec2 OutputSize;

// Outputs passed to the fragment shader.
// Note - gl_Position implicitly defined

varying vec2 v_texCoord;
varying vec2 maskpos;

void main() {
    // Pass the vertex through.
    gl_Position = vec4(VertexCoord.xy, 0.0, 1.0);
    
    // Pass through the texture coordinate.
    v_texCoord = TexCoord;

    // Pass calculated mask position
    maskpos       = TexCoord * OutputSize;
}

