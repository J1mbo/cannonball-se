/* SNES NTSC video filter */

/* Based on snes_ntsc 0.2.2

Updates for CannonBall-SE are Copyright (c) 2025, James Pearce:
- to provide S16 emulation at 24-bit output depth.
*/

#ifndef SNES_NTSC_H
#define SNES_NTSC_H

#include "snes_ntsc_config.h"


#ifdef __cplusplus
extern "C" {
#endif

#define SNES_NTSC_HAVE_SIMD 0


// Portable 'restrict' for C and C++. Note - moved from snes_ntsc_impl.h so that the blitters
// can be defined with 'restrict' parameters
#ifndef restrict
  #if defined(__GNUC__) || defined(__clang__)
    #define restrict __restrict__
  #elif defined(_MSC_VER) && _MSC_VER > 1300
    #define restrict __restrict
  #else
    /* no support: leave empty */
    #define restrict
  #endif
#endif

// Portable byteswap primitive
#if defined(_MSC_VER)  // MSVC (Windows)
    #include <stdlib.h>
    #define bswap32(x) _byteswap_ulong(x)
#else                  // glibc / Linux / other GCC-like
    #include <byteswap.h>
    #define bswap32(x) bswap_32(x)
#endif

	/* Image parameters, ranging from -1.0 to 1.0. Actual internal values shown
	in parenthesis and should remain fairly stable in future versions. */
	typedef struct snes_ntsc_setup_t
	{
		/* Basic parameters */
		double hue;        /* -1 = -180 degrees     +1 = +180 degrees */
		double saturation; /* -1 = grayscale (0.0)  +1 = oversaturated colors (2.0) */
		double contrast;   /* -1 = dark (0.5)       +1 = light (1.5) */
		double brightness; /* -1 = dark (0.5)       +1 = light (1.5) */
		double sharpness;  /* edge contrast enhancement/blurring */

		/* Advanced parameters */
		double gamma;      /* -1 = dark (1.5)       +1 = light (0.5) */
		double resolution; /* image resolution */
		double artifacts;  /* artifacts caused by color changes */
		double fringing;   /* color artifacts caused by brightness changes */
		double bleed;      /* color bleed (color resolution reduction) */
		int merge_fields;  /* if 1, merges even and odd fields together to reduce flicker */
		float const* decoder_matrix; /* optional RGB decoder matrix, 6 elements */

		unsigned long const* bsnes_colortbl; /* undocumented; set to 0 */
	} snes_ntsc_setup_t;

	/* Video format presets */
	extern snes_ntsc_setup_t const snes_ntsc_composite; /* color bleeding + artifacts */
	extern snes_ntsc_setup_t const snes_ntsc_svideo;    /* color bleeding only */
	extern snes_ntsc_setup_t const snes_ntsc_rgb;       /* crisp image */
	extern snes_ntsc_setup_t const snes_ntsc_monochrome;/* desaturated + artifacts */

	/* Initializes and adjusts parameters. Can be called multiple times on the same
	snes_ntsc_t object. Can pass NULL for either parameter. */
	typedef struct snes_ntsc_t snes_ntsc_t;
	void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup);

	/* Filters one or more rows of pixels. Input pixel format is set by SNES_NTSC_IN_FORMAT
	and output RGB depth is set by SNES_NTSC_OUT_DEPTH. Both default to 16-bit RGB.
	In_row_width is the number of pixels to get to the next input row. Out_pitch
	is the number of *bytes* to get to the next output row. */
	void snes_ntsc_blit(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input,
		long in_row_width, int burst_phase, int in_width, int in_height,
		void* rgb_out, long out_pitch, long Alevel);

	void snes_ntsc_blit_hires(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input,
		long in_row_width, int burst_phase, int in_width, int in_height,
		void* rgb_out, long out_pitch, long Alevel);

	/* Number of output pixels written by low-res blitter for given input width. Width
	might be rounded down slightly; use SNES_NTSC_IN_WIDTH() on result to find rounded
	value. Guaranteed not to round 256 down at all. */
#define SNES_NTSC_OUT_WIDTH( in_width ) \
	((((in_width) - 1) / snes_ntsc_in_chunk + 1) * snes_ntsc_out_chunk)

	/* Number of low-res input pixels that will fit within given output width. Might be
	rounded down slightly; use SNES_NTSC_OUT_WIDTH() on result to find rounded
	value. */
#define SNES_NTSC_IN_WIDTH( out_width ) \
	(((out_width) / snes_ntsc_out_chunk - 1) * snes_ntsc_in_chunk + 1)


	/* Interface for user-defined custom blitters */

	enum { snes_ntsc_in_chunk = 3 }; /* number of input pixels read per chunk */
	enum { snes_ntsc_out_chunk = 7 }; /* number of output pixels generated per chunk */
	enum { snes_ntsc_black = 0 }; /* palette index for black */
	enum { snes_ntsc_burst_count = 3 }; /* burst phase cycles through 0, 1, and 2 */

	/* Begins outputting row and starts three pixels. First pixel will be cut off a bit.
	Use snes_ntsc_black for unused pixels. Declares variables, so must be before first
	statement in a block (unless you're using C++). */
#define SNES_NTSC_BEGIN_ROW( ntsc, burst, pixel0, pixel1, pixel2 ) \
	char const* ktable = \
		(char const*) (ntsc)->table + burst * (snes_ntsc_burst_size * sizeof (snes_ntsc_rgb_t));\
	SNES_NTSC_BEGIN_ROW_6_( pixel0, pixel1, pixel2, SNES_NTSC_IN_FORMAT, ktable )

	/* Begins input pixel */
#define SNES_NTSC_COLOR_IN( index, color ) \
	SNES_NTSC_COLOR_IN_( index, color, SNES_NTSC_IN_FORMAT, ktable )

/* Generates output pixel. Bits can be 24, 16, 15, 14, 32 (treated as 24), or 0:
24:          RRRRRRRR GGGGGGGG BBBBBBBB (8-8-8 RGB)
16:                   RRRRRGGG GGGBBBBB (5-6-5 RGB)
15:                    RRRRRGG GGGBBBBB (5-5-5 RGB)
14:                    BBBBBGG GGGRRRRR (5-5-5 BGR, native SNES format)
 0: xxxRRRRR RRRxxGGG GGGGGxxB BBBBBBBx (native internal format; x = junk bits) */
 // JJP - x either side of RGB values used for signed math overflow/underlow in clamp
 // operation?
#define SNES_NTSC_RGB_OUT( index, rgb_out, bits, Alevel ) \
	SNES_NTSC_RGB_OUT_14_( index, rgb_out, bits, 1, Alevel )

/* Hires equivalents */
#define SNES_NTSC_HIRES_ROW( ntsc, burst, pixel1, pixel2, pixel3, pixel4, pixel5 ) \
	char const* ktable = \
		(char const*) (ntsc)->table + burst * (snes_ntsc_burst_size * sizeof (snes_ntsc_rgb_t));\
	unsigned const snes_ntsc_pixel1_ = (pixel1);\
	snes_ntsc_rgb_t const* kernel1  = SNES_NTSC_IN_FORMAT( ktable, snes_ntsc_pixel1_ );\
	unsigned const snes_ntsc_pixel2_ = (pixel2);\
	snes_ntsc_rgb_t const* kernel2  = SNES_NTSC_IN_FORMAT( ktable, snes_ntsc_pixel2_ );\
	unsigned const snes_ntsc_pixel3_ = (pixel3);\
	snes_ntsc_rgb_t const* kernel3  = SNES_NTSC_IN_FORMAT( ktable, snes_ntsc_pixel3_ );\
	unsigned const snes_ntsc_pixel4_ = (pixel4);\
	snes_ntsc_rgb_t const* kernel4  = SNES_NTSC_IN_FORMAT( ktable, snes_ntsc_pixel4_ );\
	unsigned const snes_ntsc_pixel5_ = (pixel5);\
	snes_ntsc_rgb_t const* kernel5  = SNES_NTSC_IN_FORMAT( ktable, snes_ntsc_pixel5_ );\
	snes_ntsc_rgb_t const* kernel0 = kernel1;\
	snes_ntsc_rgb_t const* kernelx0;\
	snes_ntsc_rgb_t const* kernelx1 = kernel1;\
	snes_ntsc_rgb_t const* kernelx2 = kernel1;\
	snes_ntsc_rgb_t const* kernelx3 = kernel1;\
	snes_ntsc_rgb_t const* kernelx4 = kernel1;\
	snes_ntsc_rgb_t const* kernelx5 = kernel1


#define SNES_NTSC_HIRES_OUT( x, rgb_out, bits, Alevel ) {\
	snes_ntsc_rgb_t raw_ =\
		kernel0  [ x       ] + kernel2  [(x+5)%7+14] + kernel4  [(x+3)%7+28] +\
		kernelx0 [(x+7)%7+7] + kernelx2 [(x+5)%7+21] + kernelx4 [(x+3)%7+35] +\
		kernel1  [(x+6)%7  ] + kernel3  [(x+4)%7+14] + kernel5  [(x+2)%7+28] +\
		kernelx1 [(x+6)%7+7] + kernelx3 [(x+4)%7+21] + kernelx5 [(x+2)%7+35];\
	SNES_NTSC_CLAMP_AND_CONVERT_SCALAR( raw_, Alevel );\
	rgb_out = raw_;\
}

/* private */
#include <stdint.h>
	enum { snes_ntsc_entry_size = 128 };
	enum { snes_ntsc_palette_size = 0x10000 };  // JJP - S16 is 5 bits per channel and two tables (std/shadow).
	                                            // Hilite not use in Outun so support removed.
	typedef uint32_t snes_ntsc_rgb_t;           // JJP - was unsigned long
	struct snes_ntsc_t {
		snes_ntsc_rgb_t table[snes_ntsc_palette_size][snes_ntsc_entry_size];
	};
	enum { snes_ntsc_burst_size = snes_ntsc_entry_size / snes_ntsc_burst_count };

#define SNES_NTSC_RGB16( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n & 0x001E) | (n >> 1 & 0x03E0) | (n >> 2 & 0x3C00)) * \
			(snes_ntsc_entry_size / 2 * sizeof (snes_ntsc_rgb_t)))

	// following input masks retain full 5-6-5 RGB values
#define SNES_NTSC_RGB16_565( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n & 0x001F) | (n & 0x07E0) | (n & 0xF800)) * \
			(snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))

// following input expects S16 palette entries (5 bits per channel),
// organised as
// R - bit 14-10, G - bit 9-5, B - bit 0-4
// Bits 15-16: 00b = standard, 01b = shadow, 10b = highlight
#define SNES_NTSC_S16( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + (n * \
			(snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t))))

#define SNES_NTSC_BGR15( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n << 9 & 0x3C00) | (n & 0x03E0) | (n >> 10 & 0x001E)) * \
			(snes_ntsc_entry_size / 2 * sizeof (snes_ntsc_rgb_t)))

/* common 3->7 ntsc macros */
#define SNES_NTSC_BEGIN_ROW_6_( pixel0, pixel1, pixel2, ENTRY, table ) \
	unsigned const snes_ntsc_pixel0_ = (pixel0);\
	snes_ntsc_rgb_t const* kernel0  = ENTRY( table, snes_ntsc_pixel0_ );\
	unsigned const snes_ntsc_pixel1_ = (pixel1);\
	snes_ntsc_rgb_t const* kernel1  = ENTRY( table, snes_ntsc_pixel1_ );\
	unsigned const snes_ntsc_pixel2_ = (pixel2);\
	snes_ntsc_rgb_t const* kernel2  = ENTRY( table, snes_ntsc_pixel2_ );\
	snes_ntsc_rgb_t const* kernelx0;\
	snes_ntsc_rgb_t const* kernelx1 = kernel0;\
	snes_ntsc_rgb_t const* kernelx2 = kernel0

#define SNES_NTSC_RGB_OUT_14_( x, rgb_out, bits, shift, Alevel ) {\
    snes_ntsc_rgb_t raw_ =\
        kernel0  [x       ] + kernel1  [(x+12)%7+14] + kernel2  [(x+10)%7+28] +\
        kernelx0 [(x+7)%14] + kernelx1 [(x+ 5)%7+21] + kernelx2 [(x+ 3)%7+35];\
        SNES_NTSC_CLAMP_( raw_, shift );                                      \
        SNES_NTSC_RGB_OUT_( rgb_out, bits, shift, Alevel );                   \
}


/* Colour Clamp Macros */
// Generic version
#define snes_ntsc_rgb_builder    ((1L << 21) | (1 << 11) | (1 << 1))
#define snes_ntsc_clamp_mask     (snes_ntsc_rgb_builder * 3 / 2)
#define snes_ntsc_clamp_add      (snes_ntsc_rgb_builder * 0x101)
#define SNES_NTSC_CLAMP_( io, shift ) {\
	snes_ntsc_rgb_t sub = (io) >> (9-(shift)) & snes_ntsc_clamp_mask;\
	snes_ntsc_rgb_t clamp = snes_ntsc_clamp_add - sub;\
	io |= clamp;\
	clamp -= sub;\
	io &= clamp;\
}


// Scalar version: processes a single pixel into RGBA (little-endian)
#define SNES_NTSC_CLAMP_AND_CONVERT_SCALAR(io, Alevel) do {               \
    /* ---- Clamp ---- */                                                 \
    uint32_t sub   = ((io) >> 9) & snes_ntsc_clamp_mask;                  \
    uint32_t clamp = snes_ntsc_clamp_add - sub;                           \
    (io) |= clamp;                                                        \
    clamp -= sub;                                                         \
    (io) &= clamp;                                                        \
                                                                          \
    /* ---- Convert internal raw format directly to RGBA ---- */          \
    uint32_t rgba = (((io) << 3) & 0xFF000000u) | /*R*/                   \
                    (((io) << 5) & 0x00FF0000u) | /*G*/                   \
                    (((io) << 7) & 0x0000FF00u) | /*B*/                   \
                    (((uint32_t)(Alevel)));       /*A*/                   \
                                                                          \
    /* Final layout: RGBA little-endian (A in top byte, R in low byte) */ \
    (io) = bswap32(rgba);                                                 \
} while (0)


//    uint32_t r = ((io) << 3) & 0xFF000000u;                               \
//    uint32_t g = ((io) << 5) & 0x00FF0000u;                               \
//    uint32_t b = ((io) << 7) & 0x0000FF00u;                               \
//    uint32_t a = ((uint32_t)(Alevel));                                    \
//                                                                          \
//    /* Final layout: RGBA little-endian (A in top byte, R in low byte) */ \
//    (io) = (Alevel<<24) | (b<<8) | (g>>8) | (r>>24);                      \




/* Colour input macro. Shuffles kernel pointers */

#define SNES_NTSC_COLOR_IN_( index, color, ENTRY, table ) {\
	unsigned color_;\
	kernelx##index = kernel##index;\
	kernel##index = (color_ = (color), ENTRY( table, color_ ));\
}

/* RGB Output Macros. These take the internal format and present it for the renderer */

// Generalised RGB_out version...
// x is always zero except in snes_ntsc library
/*
#define SNES_NTSC_RGB_OUT_( rgb_out, bits, x, Alevel ) {\
	if ( bits == 16 )\
		rgb_out = (raw_>>(13-x)& 0xF800)|(raw_>>(8-x)&0x07E0)|(raw_>>(4-x)&0x001F);\
	if ( bits == 24 || bits == 32 )\
		rgb_out = (raw_>>(5-x)&0xFF0000)|(raw_>>(3-x)&0xFF00)|(raw_>>(1-x)&0xFF)|Alevel;\
	if ( bits == 15 )\
		rgb_out = (raw_>>(14-x)& 0x7C00)|(raw_>>(9-x)&0x03E0)|(raw_>>(4-x)&0x001F);\
	if ( bits == 14 )\
		rgb_out = (raw_>>(24-x)& 0x001F)|(raw_>>(9-x)&0x03E0)|(raw_<<(6+x)&0x7C00);\
	if ( bits == 0 )\
		rgb_out = raw_ << x;\
}*/

// JJP - The following versions are Cannonball/S16 specific and assumes RGBA 32-bit output

// Generic version producing ABGR:
#define SNES_NTSC_RGB_OUT_( rgb_out, bits, x, Alevel ) {                  \
    uint32_t rgba = (((raw_) << (3+x)) & 0xFF000000u) |  /*R*/            \
                    (((raw_) << (5+x)) & 0x00FF0000u) |  /*G*/            \
                    (((raw_) << (7+x)) & 0x0000FF00u) |  /*B*/            \
                    (((uint32_t)(Alevel)));              /*A*/            \
                                                                          \
    /* Final layout: RGBA little-endian (A in top byte, R in low byte) */ \
    (rgb_out) = bswap32(rgba);                                            \
}


#define SNES_NTSC_RGB_OUT_OLD( rgb_out, bits, x, Alevel ) {                  \
    uint32_t r = ((raw_) << (3+x)) & 0xFF000000u;                               \
    uint32_t g = ((raw_) << (5+x)) & 0x00FF0000u;                               \
    uint32_t b = ((raw_) << (7+x)) & 0x0000FF00u;                               \
    /* Final layout: RGBA little-endian (A in top byte, R in low byte) */ \
    rgb_out = (Alevel<<24) | (b<<8) | (g>>8) | (r>>24);                      \
}

#ifdef __cplusplus
}
#endif

#endif
