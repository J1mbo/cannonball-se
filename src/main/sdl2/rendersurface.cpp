/***************************************************************************
    SDL2 Hardware Surface Video Rendering
    Copyright (c) 2012,2020 Manuel Alfayate, Chris White.

    Copyright (c) 2020,2025 James Pearce:
    - Blargg CRT filter integration enhanced for 24-bit support
    - Screen edge mask
    - Shader via SDL_gpu to support GLSL shader based on crt-consumer

    See license.txt for more details.
***************************************************************************/

// Aligned Memory Allocation
#include <boost/align/aligned_alloc.hpp>
#include <boost/align/aligned_delete.hpp>

#include <iostream>
#include <mutex>
#include "rendersurface.hpp"
#include "frontend/config.hpp"
#include <omp.h>
#include <math.h>

// for crt shader
#ifdef __linux__
#include <SDL_gpu.h>
#else
#include <c:/Libraries/sdl-gpu/include/SDL_gpu.h> // for shader support
#endif

//#define VERTEX_SHADER   "res/crt-consumer-vertex.glsl"
//#define FRAGMENT_SHADER "res/crt-consumer-pixel.glsl"
#define VERTEX_SHADER   "res/Cannonball-Shader-Vertex.glsl"
#define FRAGMENT_SHADER "res/Cannonball-Shader-Fragment.glsl"

RenderSurface::RenderSurface()
{
    ntsc = (snes_ntsc_t*) malloc( sizeof(snes_ntsc_t) );
}

RenderSurface::~RenderSurface()
{
    free(ntsc);
}

bool RenderSurface::init(int source_width, int source_height,
                         int source_scale, int video_mode_requested, int scanlines_requested)
{
    src_width  = source_width;
    src_height = source_height;
    scale      = source_scale;
    video_mode = video_mode_requested;

    // Capture current settings
    blargg = config.video.blargg;
    setup.saturation = double(config.video.saturation) / 100;
    setup.contrast = double(config.video.contrast) / 100;
    setup.brightness = double(config.video.brightness) / 100;
    setup.sharpness = double(config.video.sharpness) / 100;
    setup.resolution = double(config.video.resolution) / 100;
    setup.gamma = double(config.video.gamma) / 10;
    setup.hue = double(config.video.hue) / 100;

    // stop rendering threads
    //drawFrameMutex.lock();
    //finalizeFrameMutex.lock();

    // Initialise Blargg. Comes first as determins working image dimensions.
    init_blargg_filter(); // NTSC filter (CPU based)
    
    // Initialise SDL
    if (!init_sdl(video_mode)) return false;

    // Get SDL Pixel Format Information
    Rshift = GameSurface[0]->format->Rshift;
    Gshift = GameSurface[0]->format->Gshift;
    Bshift = GameSurface[0]->format->Bshift;
    Ashift = GameSurface[0]->format->Ashift;
    Rmask = GameSurface[0]->format->Rmask;
    Gmask = GameSurface[0]->format->Gmask;
    Bmask = GameSurface[0]->format->Bmask;
    Amask = GameSurface[0]->format->Amask;

    // call other initialisation routines
    init_overlay();       // CRT curved edge mask (applied as a mask by GPU rendering)
    create_buffers();     // used for working space processing image from [game output]->[blargg-filtered]->[renderer input]
    FrameCounter = 0;

    // restart rendering threads
    //drawFrameMutex.unlock();
    //finalizeFrameMutex.unlock();

    return true;
}

void RenderSurface::swap_buffers()
{
    // swap the pixel buffers
    drawFrameMutex.lock();
    current_game_surface ^= 1;
    drawFrameMutex.unlock();
    GameSurfacePixels = (uint32_t*)GameSurface[current_game_surface]->pixels;

}


void RenderSurface::disable()
{
    // Free GPU-managed images and shader program.
    if (overlayImage) {
        GPU_FreeImage(overlayImage);
        overlayImage = nullptr;
    }
    if (pass1Target) {
        GPU_FreeImage(pass1Target);
        pass1Target = nullptr;
    }
    if (gpushader) {
        GPU_FreeShaderProgram(gpushader);
        gpushader = 0;
    }

    // Shutdown SDL_gpu.
    GPU_Quit(); 
    GPU_CloseCurrentRenderer();
    GPU_FreeTarget(renderer);
    SDL_DestroyWindow(window);

    // Free the CPU surfaces.
    if (GameSurface[0]) {
        SDL_FreeSurface(GameSurface[0]);
        GameSurface[0] = nullptr;
    }
    if (GameSurface[1]) {
        SDL_FreeSurface(GameSurface[1]);
        GameSurface[1] = nullptr;
    }
    if (overlaySurface) {
        SDL_FreeSurface(overlaySurface);
        overlaySurface = nullptr;
    }

    // Release any additional buffers.
    destroy_buffers();
}


void RenderSurface::create_buffers() {
    uint32_t pixels;
	uint32_t* t32p;

    constexpr std::size_t alignment = 64;

    // allocates pixel buffers
    pixels = src_width * src_height;
    std::size_t size = pixels *sizeof(uint32_t);
    t32p = rgb_pixels = (uint32_t*)boost::alignment::aligned_alloc(alignment, size);

	while (pixels--) *(t32p++) = 0x00000000; // fill with black
}

void RenderSurface::destroy_buffers() {
    // deallocates the pixel buffers
    boost::alignment::aligned_free(rgb_pixels);
    rgb_pixels = nullptr;
}

bool RenderSurface::start_frame()
{
    return true;
}

void RenderSurface::init_blargg_filter()
{
    // Initialises the Blargg NTSC filter effects. This configures the output (s-video/rgb etc) and
    // also pre-calculates the pixel mapping (which is slow), so the shader then runs on a lookup basis
    // in the game (fast).
    if (blargg) {
        // first calculate the resultant image size.
        if (config.video.hires) snes_src_width = SNES_NTSC_OUT_WIDTH_SIMD(src_width); // for 640px input = 752;
        else snes_src_width = SNES_NTSC_OUT_WIDTH(src_width);

        // configure selcted filtering type
        switch (config.video.blargg) {
        case video_settings_t::BLARGG_COMPOSITE:
            setup = snes_ntsc_composite;
            break;
        case video_settings_t::BLARGG_SVIDEO:
            setup = snes_ntsc_svideo;
            break;
        case video_settings_t::BLARGG_RGB:
            setup = snes_ntsc_rgb;
            break;
        }
        setup.merge_fields = 0;         // mimic interlacing
        phase = 0;                      // initial frame will be phase 0
        phaseframe = 0;                 // used to track phase at 60 fps
        Alevel = 255;                   // blackpoint
        setup.hue = double(config.video.hue) / 100;
        setup.saturation = double(config.video.saturation) / 100;
        setup.contrast = double(config.video.contrast) / 100;
        setup.brightness = double(config.video.brightness) / 100;
        setup.sharpness = double(config.video.sharpness) / 100;
        setup.gamma = double(config.video.gamma) / 10;
        setup.resolution = double(config.video.resolution) / 100;
        
        snes_ntsc_init(ntsc, &setup); // configure the library
    }
    else snes_src_width = src_width; // provides constraint to buffer allocation
}


// ----------------------------------------------------------------------------------
// SDL Initialisation
// ----------------------------------------------------------------------------------

bool RenderSurface::init_sdl(int video_mode)
{
    // First, determine our source and destination dimensions.
    // RenderBase::sdl_screen_size() should set orig_width and orig_height.
    if (!RenderBase::sdl_screen_size())
        return false;

    int fullscreen = 0;

    // Compute source and destination rectangles.
    // These values are computed differently for fullscreen vs. windowed mode.
    if (video_mode == video_settings_t::MODE_FULL ||
        video_mode == video_settings_t::MODE_STRETCH)
    {
        fullscreen = 1;
        // For fullscreen:
        scn_width = orig_width;
        scn_height = orig_height;

        // Set src_rect dimensions (accounting for a potential “blargg” mode)
        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.w = (blargg) ? snes_src_width : src_width;
        src_rect.h = src_height;

        // Determine destination rectangle:
        dst_rect.x = 0;
        dst_rect.y = 0;
        dst_rect.w = scn_width;
        if (video_mode == video_settings_t::MODE_FULL) {
            // For “stretched” vertically to maintain aspect ratio:
            dst_rect.h = int(uint32_t(src_height) * uint32_t(scn_width) / uint32_t(src_width));
        }
        else { // MODE_STRETCH: fill the screen vertically
            dst_rect.h = scn_height;
        }
        // If the calculated height is too large, scale the other way and center horizontally:
        if (dst_rect.h > scn_height) {
            dst_rect.w = int(uint32_t(src_width) * uint32_t(scn_height) / uint32_t(src_height));
            dst_rect.h = scn_height;
            dst_rect.x = (scn_width - dst_rect.w) >> 1;
        }
        SDL_ShowCursor(SDL_DISABLE);
    }
    else  // Windowed mode
    {
        video_mode = video_settings_t::MODE_WINDOW;
        // Start with a desired scale (e.g. 4× the native resolution)
        scn_width = src_width * scale;
        scn_height = src_height * scale;
        // *** BUG FIX: Also scale the height (not only the width) ***
        //while (scn_width > orig_width) {
        //    scn_width /= 2;
        //    scn_height /= 2;
        //}
        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.w = (blargg == 0) ? src_width : snes_src_width;
        src_rect.h = src_height;

        dst_rect.x = 0;
        dst_rect.y = 0;
        dst_rect.w = scn_width;
        dst_rect.h = scn_height;
        SDL_ShowCursor(SDL_ENABLE);
    }

    //--------------------------------------------------------
    // Use SDL_gpu to create the window and renderer
    //--------------------------------------------------------

    // Create a window manually so that it can be closed on video restart (e.g. Blargg on/off)
    window = SDL_CreateWindow("Cannonball",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        scn_width, scn_height, SDL_WINDOW_OPENGL);

    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
        return false;
    }

    GPU_SetInitWindow(SDL_GetWindowID(window));
    renderer = GPU_Init((Uint16)scn_width, (Uint16)scn_height, GPU_DEFAULT_INIT_FLAGS);
    if (!renderer) {
        std::cerr << "GPU initialization failed: " << SDL_GetError() << std::endl;
        return false;
    }
    screen = renderer;

    if (fullscreen)
        GPU_SetFullscreen(GPU_TRUE, GPU_TRUE);

    Uint32 window_format = SDL_GetWindowPixelFormat(window);
    printf("Window Pixel Format: %s (0x%08X)\n", SDL_GetPixelFormatName(window_format), window_format);

    //--------------------------------------------------------
    // Set up the GLSL shaders using SDL_gpu
    //--------------------------------------------------------
    uint32_t vertexShader = GPU_LoadShader(GPU_VERTEX_SHADER, VERTEX_SHADER);
    if (!vertexShader) {
        std::cerr << "Failed to load vertex shader: " << GPU_GetShaderMessage() << std::endl;
        GPU_Quit();
        return false;
    }
    uint32_t pixelShader = GPU_LoadShader(GPU_PIXEL_SHADER, FRAGMENT_SHADER);
    if (!pixelShader) {
        std::cerr << "Failed to load pixel shader: " << GPU_GetShaderMessage() << std::endl;
        GPU_Quit();
        return false;
    }
    if (gpushader)
        GPU_FreeShaderProgram(gpushader);
    gpushader = GPU_LinkShaders(vertexShader, pixelShader);
    if (!gpushader) {
        std::cerr << "Failed to link shader program: " << GPU_GetShaderMessage() << std::endl;
        GPU_Quit();
        return false;
    }
    block = GPU_LoadShaderBlock(gpushader, "VertexCoord", "TexCoord", "COLOR", "MVPMatrix");

    //--------------------------------------------------------
    // Create an off-screen render target for post-processing.
    //--------------------------------------------------------
    pass1Target = GPU_CreateImage((Uint16)scn_width, (Uint16)scn_height, GPU_FORMAT_RGBA);
    if (!pass1Target) {
        std::cerr << "Failed to create off-screen image." << std::endl;
        GPU_Quit();
        return false;
    }
    GPU_LoadTarget(pass1Target);
    if (!pass1Target->target) {
        std::cerr << "Failed to create off-screen target." << std::endl;
        GPU_Quit();
        return false;
    }
    pass1 = pass1Target->target;

    //--------------------------------------------------------
    // Create CPU surfaces for the game image and an overlay.
    //--------------------------------------------------------


    // Double-buffered game surfaces
    GameSurface[0] = SDL_CreateRGBSurfaceWithFormat(0, src_rect.w, src_rect.h, BPP, SDL_PIXELFORMAT_RGBA8888);
    GameSurface[1] = SDL_CreateRGBSurfaceWithFormat(0, src_rect.w, src_rect.h, BPP, SDL_PIXELFORMAT_RGBA8888);
    if (!GameSurface[0] || !GameSurface[1]) {
        std::cerr << "GameSurface creation failed: " << SDL_GetError() << std::endl;
        GPU_Quit();
        return false;
    }
    current_game_surface = 0;
    GameSurfacePixels = static_cast<uint32_t*>(GameSurface[current_game_surface]->pixels);
    
    Uint32 black_color = SDL_MapRGBA(GameSurface[0]->format, 0, 0, 0, 255);
    SDL_FillRect(GameSurface[0], NULL, black_color);
    SDL_FillRect(GameSurface[1], NULL, black_color);

    loc_alloff = GPU_GetUniformLocation(gpushader, "alloff");
    loc_warpX = GPU_GetUniformLocation(gpushader, "warpX");
    loc_warpY = GPU_GetUniformLocation(gpushader, "warpY");
    loc_expandX = GPU_GetUniformLocation(gpushader, "expandX");
    loc_expandY = GPU_GetUniformLocation(gpushader, "expandY");
    loc_brightboost = GPU_GetUniformLocation(gpushader, "brightboost");
    loc_noiseIntensity = GPU_GetUniformLocation(gpushader, "noiseIntensity");
    loc_vignette = GPU_GetUniformLocation(gpushader, "vignette");
    loc_desaturate = GPU_GetUniformLocation(gpushader, "desaturate");
    loc_desaturateEdges = GPU_GetUniformLocation(gpushader, "desaturateEdges");
    loc_Shadowmask = GPU_GetUniformLocation(gpushader, "Shadowmask");
    //loc_framecount = GPU_GetUniformLocation(gpushader, "FrameCount");
    loc_u_Time = GPU_GetUniformLocation(gpushader, "u_Time");
    loc_OutputSize = GPU_GetUniformLocation(gpushader, "OutputSize");

    // Create the GPU GameImage buffer
    GameImage = GPU_CopyImageFromSurface(GameSurface[0]);

    // textures (i.e., game image)
    GPU_SetShaderImage(GameImage, GPU_GetUniformLocation(gpushader, "Texture"), 0);
    GPU_SetAnchor(GameImage, 0, 0);

    // screen_pixels = static_cast<uint32_t*>(surface->pixels);

    // Overlay surface for additional effects
    overlaySurface = SDL_CreateRGBSurfaceWithFormat(0, dst_rect.w, dst_rect.h, BPP, SDL_PIXELFORMAT_RGBA8888);
    if (!overlaySurface) {
        std::cerr << "Overlay Surface creation failed: " << SDL_GetError() << std::endl;
        GPU_Quit();
        return false;
    }
    overlaySurfacePixels = static_cast<uint32_t*>(overlaySurface->pixels);

    return true;
}



//-----------------------------------------------------------------
// Overlay mask. This pre-calculated mask combines CRT shape,
// vignette and CRT shadow mask effect in one pass, It is overlayed
// on each frame, reducing GPU demand compared to a pixel shader
// approach.
//-----------------------------------------------------------------

int find_circle_intersection(double x1, double y1, double r1,
    double x2, double y2, double r2,
    double* ix, double* iy) {
    // returns the top-left intersection between two circles
    // whose centres are Xn,Yn and having radii Rn
    // Distance between the centers
    double d = sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));

    // Check if there are no intersections
    if (d > r1 + r2 || d < fabs(r1 - r2) || d == 0) {
        *ix = 0;
        *iy = 0;
        return 0;  // No intersection
    }

    // Distance from the center of the first circle to the midpoint of the intersection line
    double a = (pow(r1, 2) - pow(r2, 2) + pow(d, 2)) / (2 * d);

    // Height of the intersection points from the midpoint
    double h = sqrt(pow(r1, 2) - pow(a, 2));

    // Midpoint on the line connecting the centers
    double px = x1 + a * (x2 - x1) / d;
    double py = y1 + a * (y2 - y1) / d;

    // Offset of the intersection points from the midpoint
    double offset_x = h * (y2 - y1) / d;
    double offset_y = h * (x2 - x1) / d;

    // Intersection points
    double ix1 = px + offset_x;
    double iy1 = py - offset_y;
    double ix2 = px - offset_x;
    double iy2 = py + offset_y;

    // Return smallest values
    if (ix1 < ix2) {
        *ix = ix1;
        *iy = iy1;
    }
    else {
        *ix = ix2;
        *iy = iy2;
    }

    return 1;  // Intersection found
}


void RenderSurface::init_overlay()
{
    // This function builds out the mask.
    // This is called by init(), all also by draw_frame() if the user has changed a setting.
    // Texture dimensions must be previously defined (by init_textures)
    // This function is computationally expensive.

    uint32_t* texture_pixels;
    uint32_t* t32p;
    int pixels;

    // create CRT mask texture
	pixels = dst_rect.w * dst_rect.h;
    t32p = texture_pixels = overlaySurfacePixels; // new uint32_t[pixels];
    while (pixels--) *(t32p++) = 0xFFFFFFFF; // fill with white
    //if (config.video.crt_shape) {
    if (1) {
        uint32_t vignette_target = int((double(config.video.vignette) * 255.0 / 100.0));
        double midx = double(dst_rect.w >> 1);
        double midy = double(dst_rect.h >> 1);
        double dia = sqrt(((midx * midx) + (midy * midy)));
        double outer = dia * 1.00;
        double inner = dia * 0.30;
        double total_black = 0.0; double d;

        // Blacked-out corners and top/bottom fade and curved edges
        double corner_radius = 0.02 * dia;     // this is the radius of the rounded corner
        double edge_radius = 0.01 * dia;      // this amount will be faded to black, creating a smooth edge to the curve
        double crt_curve_radius_x = dia * 12;
        double crt_curve_radius_y = dia * 12;
        double corner_x = 0;
        double corner_y = 0;

        // calculate intersection of CRT curves at top left
        double x_intersection, y_intersection;
        find_circle_intersection(crt_curve_radius_x, midy, crt_curve_radius_x,
            midx, crt_curve_radius_y, crt_curve_radius_y, &x_intersection, &y_intersection);

        // calculate intersetion of curves at top left less edge and corner radius,
        // this will be the centre of the corner curve if configured
        find_circle_intersection(crt_curve_radius_x, midy, (crt_curve_radius_x - edge_radius - corner_radius),
            midx, crt_curve_radius_y, (crt_curve_radius_y - edge_radius - corner_radius), &corner_x, &corner_y);

        int x_intersect = int(x_intersection);
        int y_intersect = int(y_intersection);

        // calculate the mask values
#pragma omp parallel for
        for (int y = 0; y <= (dst_rect.h >> 1); y++) {
            uint32_t* scnlp1 = texture_pixels + (y * dst_rect.w);
            uint32_t* scnlp2 = scnlp1 + dst_rect.w - 1;
            uint32_t* scnlp3 = texture_pixels + ((dst_rect.h - y - 1) * dst_rect.w);
            uint32_t* scnlp4 = scnlp3 + dst_rect.w - 1;
            int y_pos = (y < midy) ? y : dst_rect.h - y;
            uint32_t shadeval, maskval;
            for (int x = 0; x <= (dst_rect.w >> 1); x++) {
                // mask is symetrical so we only need to calculate half

                shadeval = 0xff; // clear
                int x_pos = (x < midx) ? x : dst_rect.w - x;
                int value_set = 0;

                // Calculate the location of the current pixel relative to:
                // d1 - the screen centre (midx, midy)
                // d2 - the center of the CRT curve on x axis (crt_curve_radius_x, midy)
                // d3 - the center of the CRT curve on y axis (midx, crt_curve_radius_y)
                // d4 - the intersection of the CRT curves at the top left (x_intersect, y_intersect)
                // d5 -( the corner ra)dius centre (corner_x, corner_y)
                double d1 = sqrt(((midx - double(x_pos)) * (midx - double(x_pos))) + ((midy - double(y_pos)) * (midy - double(y_pos))));
                double d2 = sqrt(((crt_curve_radius_x - double(x_pos)) * (crt_curve_radius_x - double(x_pos))) + ((midy - double(y_pos)) * (midy - double(y_pos))));
                double d3 = sqrt(((midx - double(x_pos)) * (midx - double(x_pos))) + ((crt_curve_radius_y - double(y_pos)) * (crt_curve_radius_y - double(y_pos))));
                double d4 = sqrt((((x_intersect + edge_radius) - double(x_pos)) * ((x_intersect + edge_radius) - double(x_pos))) + (((y_intersect + edge_radius) - double(y_pos)) * (y_intersect + edge_radius - double(y_pos))));
                double d5 = sqrt(((corner_x - double(x_pos)) * (corner_x - double(x_pos))) + ((corner_y - double(y_pos)) * (corner_y - double(y_pos))));

                // first apply overall vignette effect based on distance from centre (d1)
                if (d1 >= outer) {
                    // black out beyond this region
                    shadeval = int(total_black);
                }
                else if ((d1 >= inner) && (config.video.shadow_mask < 2)) {
                    // vignette handled in GPU shader for all masks except 0 (off) and 1 (overlay based, code below)
                    // intermediate value; increase intensity with square of distance to avoid visible edge
                    shadeval = 255 - uint32_t(round(((vignette_target) * ((d1 - inner) * (d1 - inner)) /
                        ((outer - inner) * (outer - inner)))));
                }
                else shadeval = 0xff; // no dimming

                // create rounded corners about the intersection point adjusted for the corner radius
                // if that point could be determined
                if ((corner_x > 1.0) && (corner_y > 1.0)) {
                    if ((x_pos <= corner_x) &&
                        (y_pos <= corner_y)) {
                        // apply curve over existing vignette
                        shadeval = (shadeval *
                            (d5 >= (edge_radius + corner_radius) ? int(total_black) :
                                (d5 > corner_radius ? (uint32_t(round((255 * fabs(edge_radius - (d5 - corner_radius))) / edge_radius)))
                                    : 255))) >> 8;
                        value_set = 1;
                    }
                }
                else {
                    // follow the edge contour around the corners as the specified corner_radius
                    // did not intersect with both curves (i.e. was too small)
                    if ((x_pos <= (int(x_intersect + edge_radius))) &&
                        (y_pos <= (int(y_intersect + edge_radius)))) {
                        if (d4 < edge_radius) {
                            shadeval = (shadeval *
                                (uint32_t(round((255 * fabs(edge_radius - d4)) / edge_radius)))
                                ) >> 8; // apply curve over existing vignette
                            value_set = 1;
                        }
                    }
                }

                if (value_set == 0) {
                    // remove horizontal 'ears' at each corner
                    if ((y_pos <= int(y_intersect + edge_radius)) &&
                        (x_pos <= int(x_intersect + edge_radius))) shadeval = int(total_black);
  
                    // next apply the curved edge effect based on distance from the CRT curve (d2 and d3)
                    // first use d2 (x axis) as this will be larger
                    if (x_pos <= (x_intersect + edge_radius)) {
                        // somewhere on curve on left edge
                        if (d2 >= crt_curve_radius_x) {
                            // black out beyond this region
                            shadeval = int(total_black);
                        }
                        else if ((crt_curve_radius_x - d2) < edge_radius) {
                            // apply the curve, x255 then >> 8 saves on floating-point division
                            shadeval = (shadeval *
                                (uint32_t(round((255 * fabs(crt_curve_radius_x - d2)) / edge_radius)))
                                ) >> 8; // apply curve over existing vignette
                        } // else unaffected
                    }
                    else {
                        // next use d3 (y-axis curve)
                        if (y_pos <= (y_intersect + edge_radius)) {
                            // somewhere on curve on top edge
                            if ((d3 >= crt_curve_radius_y)) {
                                // black out beyond this region
                                shadeval = int(total_black);
                            }
                            else if ((crt_curve_radius_y - d3) < edge_radius) {
                                // apply the curve
                                shadeval = (shadeval *
                                    (uint32_t(round((255 * fabs(crt_curve_radius_y - d3)) / edge_radius)))
                                    ) >> 8; // apply curve over existing vignette
                            }
                        }
                    }
                }
                // store the calculated mask value in the texture
                maskval = (shadeval << Rshift) + (shadeval << Bshift) + (shadeval << Gshift);
                //maskval = shadeval;
                *(scnlp1++) = maskval; // top-left
                *(scnlp2--) = maskval; // top-right
                *(scnlp3++) = maskval; // bottom-left
                *(scnlp4--) = maskval; // bottom-right
            }
        }
    }

    // Overlay with CRT Mask, suitable for resource constrained targets as combines mask, vignette and shape in single
    // pass. However, the quality of the mask and vignette effects are reduced.
    // The entire image is processed in this loop
    if (config.video.shadow_mask == 1) {
        // small square mask effect but without rgb split, for standard screens like 1280x1024
        int current = 0;
        uint32_t* scnlp = texture_pixels;
        uint32_t dimval;
        uint32_t dimval_h = (100 - config.video.mask_intensity) * 255 / 100; // percent that shows through
        uint32_t dimval_v = dimval_h * dimval_h / 255; // verticals are darker
        
        for (int y = 0; y < dst_rect.h; y++) {
            current = 0;
            for (int x = 0; x < dst_rect.w; x++) {
                dimval = 0xFF; // reset to no further dimming
                if ((current == 2) || (current == 5))
                    dimval = dimval_v; // dark column
                else
                {
                    if ((y & 0x01) == 0) {
                        // dim alternate pixels on this whole row
                        if (current < 2) dimval = dimval_h;
                    }
                    if ((y & 0x01) == 1) {
                        // dim alternate pixels on this whole row
                        if ((current > 2) && (current < 5)) dimval = dimval_h;
                    }
                }
                if (++current == 6) current = 0;

                uint32_t maskval = (*scnlp >> Rshift) & 0xFF; // extract one value
                dimval = maskval * dimval / 255;
                *(scnlp++) = (dimval << Rshift) + (dimval << Gshift) + (dimval << Bshift);
            }
        }
    }

    // load the texture into the GPU for presentation each frame if configured
    overlayImage = GPU_CopyImageFromSurface(overlaySurface);
    GPU_SetBlendMode(overlayImage, GPU_BLEND_MULTIPLY);
    GPU_SetAnchor(overlayImage, 0, 0);
    GPU_SetImageVirtualResolution(overlayImage, dst_rect.w, dst_rect.h);
}





bool RenderSurface::finalize_frame()
{
	// This function is called after the frame has been rendered, and is responsible for
	// updating the screen with the new frame. It also applies post-processing effects
	// via GPU shader and CRT edge overlay if enabled.

    int game_width = src_rect.w;
    int game_height = src_rect.h;
    
    // Whether to use off-screen target
    int offscreen_rendering = 0;// (config.video.crt_shape == 1);

    if (FrameCounter++ == 60) FrameCounter = 0;

    if (offscreen_rendering) {
        GPU_SetActiveTarget(pass1);
    }
    else {
        GPU_SetActiveTarget(screen);
    }

    // *** SHADER DRAW ***
    
    // get game image, which has been stored directly into surface->pixels by draw_frame
    // in one of two buffers. Use mutex in case user updates a config setting during rendering,
    // for example turning on/off the shader which would result in buffers being re-allocated.
    //finalizeFrameMutex.lock();

    /*
    drawFrameMutex.lock();
    int SurfaceIndex = current_game_surface ^ 1;
    drawFrameMutex.unlock();

    GPU_Rect fullRect = { 0, 0, (float)game_width, (float)game_height };
    GPU_UpdateImage(GameImage, &fullRect, GameSurface[SurfaceIndex], &fullRect );
    */

    int SurfaceIndex;
    {
        std::lock_guard<std::mutex> lock(drawFrameMutex);
        SurfaceIndex = current_game_surface ^ 1;
    }

    // Now we assume that only this thread accesses GameSurface[SurfaceIndex]
    const auto* const localGameSurface = GameSurface[SurfaceIndex];

    GPU_Rect fullRect = { 0, 0, static_cast<float>(game_width), static_cast<float>(game_height) };
    GPU_UpdateImage(GameImage, &fullRect, const_cast<SDL_Surface*>(localGameSurface), &fullRect);

    // enable the shader
    if (config.video.alloff == 0)
        GPU_ActivateShaderProgram(gpushader, &block);
    
    /* == Configure shader options ('uniforms') == */

    // check for any settings changes
    if (config.video.alloff == 0)
    {
        // Configure the various sizes
        float values[2];
        
        // OutputSize
        values[0] = (float)dst_rect.w;
        values[1] = (float)dst_rect.h;
        GPU_SetUniformfv(loc_OutputSize, 2, 1, values);

        // other settings
        GPU_SetUniformf(loc_warpX, float(config.video.warpX) / 100.0f);
        GPU_SetUniformf(loc_warpY, float(config.video.warpY) / 100.0f);
        GPU_SetUniformf(loc_expandX, 1 + (float(config.video.warpX) / 200.0f));
        GPU_SetUniformf(loc_expandY, 1 + (float(config.video.warpY) / 300.0f));
        GPU_SetUniformf(loc_brightboost, 1 + (float(config.video.brightboost) / 100.0f));
        GPU_SetUniformf(loc_noiseIntensity, float(config.video.noise) / 100.0f);
        float gpu_vignette = (config.video.shadow_mask < 2) ? 0.0f : float(config.video.vignette) / 100.0f;
        GPU_SetUniformf(loc_vignette, gpu_vignette);
        GPU_SetUniformf(loc_desaturate, float(config.video.desaturate) / 100.0f);
        GPU_SetUniformf(loc_desaturateEdges, float(config.video.desaturate_edges) / 100.0f);
        GPU_SetUniformf(loc_Shadowmask, float(config.video.shadow_mask));

        values[0] = float(FrameCounter) / 60.0;
        values[1] = values[0];
        GPU_SetUniformfv(loc_u_Time, 2, 1, values);
    }

    /* == blit the game image. This processes it with the shader into the configured target buffer == */

    float x_scale = ((float)dst_rect.w / (float)game_width);
    float y_scale = ((float)dst_rect.h / (float)game_height);

    if (offscreen_rendering) {
        GPU_BlitScale(GameImage, NULL, pass1, 0, 0, x_scale, y_scale);
        if (config.video.alloff == 0)
            GPU_DeactivateShaderProgram();
        GPU_Flip(pass1);
        GPU_SetActiveTarget(screen);
        //GPU_Clear(screen);
        GPU_SetAnchor(pass1Target, 0, 0);
        GPU_Blit(pass1Target, NULL, screen, 0, 0); // processed game image
        GPU_Blit(overlayImage, NULL, screen, 0, 0); //overlay
    }
    else {
        GPU_BlitScale(GameImage, NULL, screen, 0, 0, x_scale, y_scale);
        if (config.video.alloff == 0)
            GPU_DeactivateShaderProgram();
        if (config.video.crt_shape)
            GPU_Blit(overlayImage, NULL, screen, 0, 0); //overlay
    }

    // Present frame
    GPU_Flip(screen);

    return true;
}



void RenderSurface::draw_frame(uint16_t* pixels)
{
    // grabs the S16 frame buffer ('pixels') and stores it, either
	// as straight SDL RGB or SNES RGB, then applies Blargg filter, if enabled, and colour mapping
    if (blargg) {
        // convert pixel data to format used by Blarrg filtering code
        int src_pixel_count = src_width * src_height;
        //int dst_pixel_count = snes_src_width * height;
        uint16_t* spix = pixels; // S16 Output
        uint32_t* bpix = rgb_pixels;
			
        // translate game image to lookup format that Blargg filter will use
        int pixel_count = (src_pixel_count) >> 2; // unroll 4:1
		while (pixel_count--) {
            // translate game image to lookup format that Blargg filter will use to
            // convert to RGB output levels in one step based on pre-defined S16-correct DAC output values
            *(bpix) = rgb_blargg[*(spix)];
            *(bpix+1) = rgb_blargg[*(spix+1)];
            *(bpix+2) = rgb_blargg[*(spix+2)];
            *(bpix+3) = rgb_blargg[*(spix+3)];
            bpix += 4;
            spix += 4;
        }
            
        long output_pitch = (snes_src_width << 2); // 4 bytes-per-pixel (8/8/8/8)

        // Set pointers
        bpix = rgb_pixels;
        uint32_t* tpix = GameSurfacePixels;

        // Calculated alpha mask
        uint32_t Ashifted = uint32_t(Alevel) << Ashift;
            
        // Now call the blargg code, to do the work of translating S16 output to RGB
        if (config.video.hires) {
            // hi-res
            snes_ntsc_blit_hires_fast(ntsc, bpix, long(src_width), phase,
                src_width, src_height, tpix, output_pitch, Ashifted);
        }
        else {
            // standard res processing
            snes_ntsc_blit(ntsc, bpix, long(src_width), phase,
                src_width, src_height, tpix, output_pitch, Ashifted);
        }

        if (config.fps == 60)
            phase = (phase + 1) % 3; // cycle through 0/1/2
        else
            phase = (phase + 2) % 3; // cycle through 0/1/2, but at twice the rate

    } else {
        // Standard image processing; direct RGB value lookup from rgb array for backbuffer
        int pixel_count = src_width * src_height;
        uint32_t Ashifted = uint32_t(Alevel) << Ashift; 

        uint16_t* spix = pixels;
        uint32_t* tpix = GameSurfacePixels;

        // translate game image to S16-correct RGB output levels
        int i = pixel_count >> 2; // unroll 4:1
        while (i--) {
            // Copy to surface->pixels, where finalize_frame will collect it from
            // translates game image to S16-correct RGB output levels
            *(tpix) = rgb[*(spix)] + Ashifted;
            *(tpix+1) = rgb[*(spix+1)] + Ashifted;
            *(tpix+2) = rgb[*(spix+2)] + Ashifted;
            *(tpix+3) = rgb[*(spix+3)] + Ashifted;
            tpix += 4;
            spix += 4;
        }
    }

    // Check for any changes to the configured video settings (that don't require full SDL restart)
    
    // Blargg filter settings. Changing these requires Blargg filter re-initialisation.
    // The filter uses doubles internally, so we need to convert the config values to doubles.
    if ((blargg           != config.video.blargg)                   ||
        (setup.saturation != double(config.video.saturation) / 100) ||
        (setup.contrast   != double(config.video.contrast) / 100)   ||
        (setup.brightness != double(config.video.brightness) / 100) ||
        (setup.sharpness  != double(config.video.sharpness) / 100)  ||
        (setup.resolution != double(config.video.resolution) / 100) ||
        (setup.gamma      != double(config.video.gamma) / 10)       ||
        (setup.hue        != double(config.video.hue) / 100)) {
        // re-initiatise the Blargg filter library after capturing new settings
        blargg            =  config.video.blargg;
        setup.saturation  =  double(config.video.saturation) / 100;
        setup.contrast    =  double(config.video.contrast) / 100;
        setup.brightness  =  double(config.video.brightness) / 100;
        setup.sharpness   =  double(config.video.sharpness) / 100;
        setup.resolution  =  double(config.video.resolution) / 100;
        setup.gamma       =  double(config.video.gamma) / 10;
        setup.hue         =  double(config.video.hue) / 100;
        
        // pause video whilst this is re-calculated
        drawFrameMutex.lock();
        //destroy_buffers();
        init_blargg_filter();
        //create_buffers();
        drawFrameMutex.unlock();
    }
}
