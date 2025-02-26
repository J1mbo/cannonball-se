#version 100

/* ----------------------------------------------------------------------------- */
/*                                                                               */
/* Lightweight Vertex shader for Cannonball, by James Pearce, Copyright (c) 2025 */
/*                                                                               */
/* Provides processing to provide a game image that looks broadly like it's on   */
/* on real CRT, when combined with Blargg filtering which is done CPU side.      */
/* Pi Zero 2W can run this at 60fps under Raspbian command-line installation.    */
/*                                                                               */
/* ----------------------------------------------------------------------------- */


/* Version independence defines */
#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying 
#define COMPAT_ATTRIBUTE attribute 
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec2 TexCoord;

uniform mat4 MVPMatrix; // Assumed to be set to identity by SDL_gpu
uniform COMPAT_PRECISION vec2 OutputSize;

// Outputs passed to the fragment shader.
// Note - gl_Position implicitly defined

COMPAT_VARYING vec2 v_texCoord;
COMPAT_VARYING vec2 maskpos;

void main() {
    // Pass the vertex through.
    gl_Position = MVPMatrix * VertexCoord;
    
    // Pass through the texture coordinate.
    v_texCoord = TexCoord;

    // Pass calculated mask position
    maskpos = v_texCoord.xy*OutputSize.xy;
}

