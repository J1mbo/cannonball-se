/* SNES NTSC video filter */

/* snes_ntsc 0.2.2 */

// Updated by James Pearce for S16 emulation at 24-bit output depth and to provide
// SIMD optimised processing for clamp and RGB conversion.

#ifndef SNES_NTSC_H
#define SNES_NTSC_H

#include "snes_ntsc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(_M_X64)
#include <intrin.h> 
#include <tmmintrin.h> // SSE4
#include <emmintrin.h> // SSE2
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#else
#error "Unsupported architecture: This implementation requires SSE4 or NEON."
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

	void snes_ntsc_blit_hires_fast(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input, long in_row_width,
		int burst_phase, int in_width, int in_height, void* rgb_out, long out_pitch, long Alevel);

	/* Number of output pixels written by low-res blitter for given input width. Width
	might be rounded down slightly; use SNES_NTSC_IN_WIDTH() on result to find rounded
	value. Guaranteed not to round 256 down at all. */
#define SNES_NTSC_OUT_WIDTH( in_width ) \
	((((in_width) - 1) / snes_ntsc_in_chunk + 1) * snes_ntsc_out_chunk)
	
	/* SIMD version of the above (note - SIMD only works for hires inputs */
#define SNES_NTSC_OUT_WIDTH_SIMD( in_width ) \
	(((in_width - 16) * 7 / 6) + 24)

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
	SNES_NTSC_CLAMP_( raw_, 0 );\
	SNES_NTSC_RGB_OUT_( rgb_out, (bits), 0, Alevel );\
}

// this version ONLY does the maths on each pixel. Clamp and RGB_OUT are skipped,
// since this can be done in parallel across all outputs later
#define SNES_NTSC_HIRES_OUT_SIMD( x ) (\
		kernel0  [ x       ] + kernel2  [(x+5)%7+14] + kernel4  [(x+3)%7+28] +\
		kernelx0 [(x+7)%7+7] + kernelx2 [(x+5)%7+21] + kernelx4 [(x+3)%7+35] +\
		kernel1  [(x+6)%7  ] + kernel3  [(x+4)%7+14] + kernel5  [(x+2)%7+28] +\
		kernelx1 [(x+6)%7+7] + kernelx3 [(x+4)%7+21] + kernelx5 [(x+2)%7+35]\
)

/* private */
	enum { snes_ntsc_entry_size = 128 };
	enum { snes_ntsc_palette_size = 0x18000 }; // JJP - S16 is 5 bits per channel and three table (std/shadow/hilite)
	typedef unsigned long snes_ntsc_rgb_t;
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
	SNES_NTSC_CLAMP_( raw_, shift );\
	SNES_NTSC_RGB_OUT_( rgb_out, bits, shift, Alevel );\
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

/* JJP - SIMD macro predefined mask vectors for performance */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	// ARM Architecture with NEON support
#define SIMD_REGISTER_t uint32x4_t
#define SIMD_ZERO SIMD_ZERO_NEON
#define INSERT_SIMD_REGISTER_VALUE INSERT_SIMD_REGISTER_VALUE_NEON
#define EXTRACT_SIMD_REGISTER_VALUE EXTRACT_SIMD_REGISTER_VALUE_NEON
#define LOAD_SIMD_REGISTER LOAD_SIMD_REGISTER_NEON
#define SET_SIMD_REGISTER vdupq_n_u32
#define ZERO_SIMD_REGISTER vdupq_n_u32(0)
#define ROTATE_OUT ROTATE_OUT_NEON
#define CRC32_ __crc32w
#define SET_SNES_MASK_VECTORS SET_SNES_MASK_VECTORS_NEON
#define SNES_NTSC_CLAMP SNES_NTSC_CLAMP_NEON
#define SNES_NTSC_CLAMP_AND_CONVERT SNES_NTSC_CLAMP_AND_CONVERT_NEON
#define SNES_NTSC_RGB_OUT_STORE SNES_NTSC_RGB_OUT_STORE_NEON
#define SNES_NTSC_RGB_STORE SNES_NTSC_RGB_STORE_NEON
#elif defined(_M_X64) || defined(__x86_64__) || defined(__i386__)
	// x86 Architecture with SSE4.1 support
#define SIMD_REGISTER_t __m128i
#define SIMD_ZERO SIMD_ZERO_SSE4
#define INSERT_SIMD_REGISTER_VALUE INSERT_SIMD_REGISTER_VALUE_SSE4
#define EXTRACT_SIMD_REGISTER_VALUE EXTRACT_SIMD_REGISTER_VALUE_SSE4
#define LOAD_SIMD_REGISTER LOAD_SIMD_REGISTER_SSE4
#define SET_SIMD_REGISTER _mm_set_epi32
#define ZERO_SIMD_REGISTER _mm_setzero_si128()
#define ROTATE_OUT ROTATE_OUT_SSE4
#define CRC32_ _mm_crc32_u32
#define SET_SNES_MASK_VECTORS SET_SNES_MASK_VECTORS_SSE4
#define SNES_NTSC_CLAMP SNES_NTSC_CLAMP_SSE4
#define SNES_NTSC_CLAMP_AND_CONVERT SNES_NTSC_CLAMP_AND_CONVERT_SSE4
#define SNES_NTSC_RGB_OUT_STORE SNES_NTSC_RGB_OUT_STORE_SSE4
#define SNES_NTSC_RGB_STORE SNES_NTSC_RGB_STORE_SSE4
#else
#error "Unsupported architecture: This implementation requires SSE4 or NEON."
#endif

#define SIMD_ZERO_SSE4 _mm_setzero_si128()

#define SIMD_ZERO_NEON vdupq_n_u32(0)

#define SET_SNES_MASK_VECTORS_SSE4 \
	const __m128i alevel_vec = _mm_set1_epi32(0x000000FF);\
	const __m128i RED_MASK = _mm_set1_epi32(0xFF000000);\
	const __m128i GREEN_MASK = _mm_set1_epi32(0x00FF0000);\
	const __m128i BLUE_MASK = _mm_set1_epi32(0x0000FF00);\
	const __m128i clamp_mask_vec = _mm_set1_epi32(snes_ntsc_clamp_mask);\
	const __m128i clamp_add_vec = _mm_set1_epi32(snes_ntsc_clamp_add)

#define SET_SNES_MASK_VECTORS_NEON \
    const uint32x4_t alevel_vec = vdupq_n_u32(0x000000FF); \
    const uint32x4_t RED_MASK = vdupq_n_u32(0xFF000000); \
    const uint32x4_t GREEN_MASK = vdupq_n_u32(0x00FF0000); \
    const uint32x4_t BLUE_MASK = vdupq_n_u32(0x0000FF00);\
	const uint32x4_t clamp_mask_vec = vdupq_n_u32(snes_ntsc_clamp_mask);\
	const uint32x4_t clamp_add_vec = vdupq_n_u32(snes_ntsc_clamp_add)

#define INSERT_SIMD_REGISTER_VALUE_SSE4(reg, value, pos) \
	_mm_insert_epi32(reg, value, pos)

#define INSERT_SIMD_REGISTER_VALUE_NEON(reg, value, pos) \
	vsetq_lane_u32(value, reg, pos)

#define EXTRACT_SIMD_REGISTER_VALUE_SSE4(reg, pos) \
	_mm_extract_epi32(reg, pos)

#define EXTRACT_SIMD_REGISTER_VALUE_NEON(reg, pos) \
	vgetq_lane_u32(reg, pos)

#define LOAD_SIMD_REGISTER_SSE4(ptr) \
	_mm_load_si128((__m128i*)ptr)

#define LOAD_SIMD_REGISTER_NEON(ptr) \
	vld1q_u32(ptr)

#define ROTATE_OUT_SSE4(A, B, elements) \
	_mm_alignr_epi8(B, A, elements*4)

#define ROTATE_OUT_NEON(A, B, elements) \
	vextq_u32(A, B, elements)


/* JJP - SIMD versions of clamp macros for S16, where shift = 0 */

// SSE4 (Intel/AMD) Macro...
// SSE4 version of Blargg's clamp:
#define SNES_NTSC_CLAMP_SSE4(io) do { \
    __m128i sub = _mm_and_si128(_mm_srli_epi32((io), 9), clamp_mask_vec); \
    __m128i clamp = _mm_sub_epi32(clamp_add_vec, sub); \
    (io) = _mm_or_si128((io), clamp); \
    clamp = _mm_sub_epi32(clamp, sub); \
    (io) = _mm_and_si128((io), clamp); \
} while(0)


// SSE4 version of Blargg's clamp which converts to RGB also
#define SNES_NTSC_CLAMP_AND_CONVERT_SSE4(io) do { \
    /* Clamping Operations */ \
    __m128i sub = _mm_and_si128(_mm_srli_epi32(io, 9), clamp_mask_vec); \
    __m128i clamp = _mm_sub_epi32(clamp_add_vec, sub); \
    io = _mm_or_si128(io, clamp); \
    clamp = _mm_sub_epi32(clamp, sub); \
    io = _mm_and_si128(io, clamp); \
    \
    /* Color Conversion */ \
    __m128i red_shifted = _mm_slli_epi32(io, 3); \
    __m128i green_shifted = _mm_slli_epi32(io, 5); /* 3 + 2 */ \
    __m128i blue_shifted = _mm_slli_epi32(io, 7); /* 5 + 2 */ \
    \
    __m128i red   = _mm_and_si128(red_shifted, RED_MASK); \
    __m128i green = _mm_and_si128(green_shifted, GREEN_MASK); \
    __m128i blue  = _mm_and_si128(blue_shifted, BLUE_MASK); \
    \
    __m128i combined = _mm_or_si128(red, green); \
    combined = _mm_or_si128(combined, blue); \
    io = _mm_or_si128(combined, alevel_vec); \
} while(0)


// NEON Equivalents...
// 
// NEON version of Blargg's clamp with preloaded vectors
#define SNES_NTSC_CLAMP_NEON(io) do {       \
    uint32x4_t shifted = vshrq_n_u32(io, 9);                               \
    uint32x4_t sub = vandq_u32(shifted, clamp_mask_vec);                   \
    uint32x4_t clamp = vsubq_u32(clamp_add_vec, sub);                      \
    io = vorrq_u32(io, clamp);                                             \
    clamp = vsubq_u32(clamp, sub);                                         \
    io = vandq_u32(io, clamp);                                             \
} while(0)

// NEON version of Blargg's clamp with RGB conversion
#define SNES_NTSC_CLAMP_AND_CONVERT_NEON(io) do {                          \
    uint32x4_t sub = vandq_u32(vshrq_n_u32(io, 9), clamp_mask_vec);        \
    uint32x4_t clamp = vsubq_u32(clamp_add_vec, sub);                      \
    io = vorrq_u32(io, clamp);                                             \
    clamp = vsubq_u32(clamp, sub);                                         \
    io = vandq_u32(io, clamp);                                             \
    uint32x4_t red_shifted   = vshlq_n_u32(io, 3);                         \
    uint32x4_t green_shifted = vshlq_n_u32(io, 5);                         \
    uint32x4_t blue_shifted  = vshlq_n_u32(io, 7);                         \
    uint32x4_t red      = vandq_u32(red_shifted, RED_MASK);                \
    uint32x4_t green    = vandq_u32(green_shifted, GREEN_MASK);            \
    uint32x4_t blue     = vandq_u32(blue_shifted, BLUE_MASK);              \
    uint32x4_t combined = vorrq_u32(red, green);                           \
    combined = vorrq_u32(combined, blue);                                  \
    io = vorrq_u32(combined, alevel_vec);                                  \
} while(0)




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

// Generic...
#define SNES_NTSC_RGB_OUT_( rgb_out, bits, x, Alevel ) {\
	rgb_out = (raw_<<3&0xFF000000)|(raw_<<5&0x00FF0000)|(raw_<<7&0x0000FF00)|Alevel;\
}

// SSE4 (Intel/AMD) - convert to RGB and store to RAM
#define SNES_NTSC_RGB_OUT_STORE_SSE4(line_out, raw_vec, alevel_vec) do { \
    /* Shift raw_ left by 3 bits and mask with 0xFF000000 */ \
    __m128i shifted3 = _mm_slli_epi32(raw_vec, 3); \
    __m128i masked3  = _mm_and_si128(shifted3, RED_MASK); \
    \
    /* Shift raw_ left by 5 bits and mask with 0x00FF0000 */ \
    __m128i shifted5 = _mm_slli_epi32(raw_vec, 5); \
    __m128i masked5  = _mm_and_si128(shifted5, GREEN_MASK); \
    \
    /* Shift raw_ left by 7 bits and mask with 0x0000FF00 */ \
    __m128i shifted7 = _mm_slli_epi32(raw_vec, 7); \
    __m128i masked7  = _mm_and_si128(shifted7, BLUE_MASK); \
    \
    /* Combine the masked values into RGB */ \
    __m128i combined = _mm_or_si128(_mm_or_si128(masked3, masked5), masked7); \
    \
    /* Final RGBA Output: combined RGB | Alevel */ \
    __m128i rgba_out = _mm_or_si128(combined, alevel_vec); \
    \
    /* Store the RGBA values directly to memory */ \
    _mm_store_si128((__m128i*)line_out, rgba_out); \
} while(0)

// NEON Equivalent...
#define SNES_NTSC_RGB_OUT_STORE_NEON(line_out, raw_vec, alevel_vec) do { \
    uint32x4_t shifted3 = vshlq_n_u32(raw_vec, 3); \
    uint32x4_t masked3  = vandq_u32(shifted3, RED_MASK); \
    uint32x4_t shifted5 = vshlq_n_u32(raw_vec, 5); \
    uint32x4_t masked5  = vandq_u32(shifted5, GREEN_MASK); \
    uint32x4_t shifted7 = vshlq_n_u32(raw_vec, 7); \
    uint32x4_t masked7  = vandq_u32(shifted7, BLUE_MASK); \
    uint32x4_t combined = vorrq_u32(vorrq_u32(masked3, masked5), masked7); \
    uint32x4_t rgba_out = vorrq_u32(combined, alevel_vec); \
    vst1q_u32(line_out, rgba_out); \
} while(0)

// store four pre-computed RGB values to RAM
#define SNES_NTSC_RGB_STORE_SSE4(line_out, raw_vec) do { \
    _mm_store_si128((__m128i*)line_out, raw_vec); \
} while(0)

#define SNES_NTSC_RGB_STORE_NEON(line_out, raw_vec) do { \
    vst1q_u32((uint32_t*)line_out, raw_vec); \
} while(0)


#ifdef __cplusplus
}
#endif

#endif
