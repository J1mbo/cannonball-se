/* SNES NTSC video filter */

/* Based on snes_ntsc 0.2.2

Updates for CannonBall-SE are Copyright (c) 2025, James Pearce:
- to provide S16 emulation at 24-bit output depth,
- to provide SIMD optimised processing for clamp and RGB conversion.
Updates Sep-26: slot-indexed, phase-major kernel table (Part A of the NTSC filter
                re-architecture). The table is now addressed by palette *slot*
                (0..4095 standard, 4096..8191 shadow, plus one reserved black slot),
                not by a 16-bit packed colour value, and is generated/regenerated one
                slot at a time via snes_ntsc_update_slot()/snes_ntsc_update_slots()
                so a palette RAM write no longer requires a full table rebuild.
                Stage 2: added snes_ntsc_blit_hires_vec(), a portable (NEON/SSE2/RVV/
                scalar) adjacent-outputs vector kernel; widened each alignment block
                from 16 to 20 words (3 leading + 14 real + 3 trailing zero words, see
                snes_ntsc_block_lead) so every compile-time-constant 16-byte window the
                vector kernel needs - including ones that start before the first real
                tap or end after the last - stays inside one block without a bounds
                check.
                Stage 3: added the run-length ladder (Part C, see
                tools/ntsc_bench/analysis/ARCHITECTURE.md section 6) inside
                snes_ntsc_blit_hires_vec(): three more derived tables per slot per phase
                (plane_pk, plane_ck, plane_flat - exact integer sums of plane's base
                blocks, kept coherent by gen_slot()/snes_ntsc_update_slot(s)()) let a
                row classify each 6-pixel chunk once and skip most of the general
                per-pixel kernel on real content (uniform chunks -> one chunk-kernel
                load, identical pairs -> one pair-kernel load, steady runs of 3+
                uniform same-slot chunks -> a precomputed constant, clamped and
                stored). The general path (Part B) remains exactly as Stage 2 left it
                and is still what handles the row's prologue/tail edge groups and any
                pixel the ladder can't shortcut.
                Stage 4: retired the per-ISA snes_ntsc_blit_hires_fast() and the
                separate scalar snes_ntsc_blit_hires() - snes_ntsc_blit_hires_vec() is
                now the only hires blitter, on every platform (its own portable v4u32
                layer, snes_ntsc_simd.h, supplies the SIMD-or-scalar backend), and the
                run-length ladder's classifier is now fused into the group loop (no
                row-wide pre-pass, no heap allocation - see snes_ntsc_blit_hires_vec()'s
                banner comment in snes_ntsc.cpp). The ladder's pair level (PK) is off by
                default (SNES_NTSC_LADDER_MASK); it measured as a net loss on
                transition-heavy content and no measurable gain on real frames (see
                README.md's "Ladder-level experiment").

Note: the per-slot kernel blocks inside snes_ntsc_t are 4-byte-word aligned within a
      64-byte-aligned plane (see snes_ntsc_block_words below); with 20-word (80-byte)
      blocks a block no longer always sits inside one 64-byte cache line, which only
      matters for raw load-latency, not correctness - every reader (the lores blitter,
      the hires vector kernel, and table generation) addresses words via SNES_NTSC_KW /
      explicit offsets, so the block size change is transparent to all of them.
*/

#ifndef SNES_NTSC_H
#define SNES_NTSC_H

#include "snes_ntsc_config.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Whether this target has *some* native SIMD ISA available - consulted only by
   tools/ntsc_bench/ntsc_bench.cpp (to decide whether an explicitly-requested
   pre-Stage-4 reference function like snes_ntsc_blit_hires_fast can exist at
   all when built against an older revision's header) and by RenderSurface's
   invExpandX shader-uniform choice. The engine's own hires blitter,
   snes_ntsc_blit_hires_vec(), does not consult this: its portable v4u32 layer
   (snes_ntsc_simd.h) resolves its own backend independently, all the way down
   to a plain-C fallback, so it is available and correct regardless of this
   value. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define SNES_NTSC_HAVE_SIMD 1
#elif defined(_M_X64) || defined(__x86_64__)
    /* 64-bit x86: SSE2 is guaranteed by the x86-64 ABI. */
    #define SNES_NTSC_HAVE_SIMD 1
#elif defined(__i386__) || defined(_M_IX86)
    /* 32-bit x86: only counts as SIMD-capable when SSE2 is explicitly available. */
    #if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        #define SNES_NTSC_HAVE_SIMD 1
    #else
        #define SNES_NTSC_HAVE_SIMD 0
    #endif
#elif defined(_M_ARM64) || defined(__aarch64__)
    /* AArch64 without NEON is unusual; if absent we fall back to scalar. */
    #define SNES_NTSC_HAVE_SIMD 0
#elif defined(__riscv) && defined(__riscv_v)
    /* RISC-V with the V extension (RVV 1.0). Set by -march=rv64gcv or similar. */
    #define SNES_NTSC_HAVE_SIMD 1
#else
    /* No native SIMD ISA (e.g., ARMv6 / Raspberry Pi Zero W). */
    #define SNES_NTSC_HAVE_SIMD 0
#endif


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

	/* Initializes and adjusts parameters, generating every slot's kernel from
	slot_colours[0..snes_ntsc_slot_count-1] (packed S16 colour values, same encoding as
	SNES_NTSC_S16 below). Can be called multiple times on the same snes_ntsc_t object.
	setup NULL means composite video; slot_colours NULL means every slot is black. */
	typedef struct snes_ntsc_t snes_ntsc_t;
	void snes_ntsc_init(snes_ntsc_t* ntsc, snes_ntsc_setup_t const* setup,
		unsigned short const* slot_colours);

	/* Regenerates the kernel for one slot (0..snes_ntsc_slot_count-1) from a new packed
	S16 colour value, without touching any other slot. Must be called with the same
	setup that was last passed to snes_ntsc_init() (the setup/decoder state is cached
	inside *ntsc). Safe to call repeatedly as individual palette RAM entries change. */
	void snes_ntsc_update_slot(snes_ntsc_t* ntsc, int slot, unsigned short colour);

	/* Bulk form of snes_ntsc_update_slot(): regenerates 'count' slots, given as parallel
	arrays of slot indices and their new colour values. Parallelised with OpenMP when
	count is large (see snes_ntsc.cpp) and the build has OpenMP available. */
	void snes_ntsc_update_slots(snes_ntsc_t* ntsc, int const* slots, int count,
		unsigned short const* slot_colours);

	/* Filters one or more rows of pixels. Input pixel format is set by SNES_NTSC_IN_FORMAT
	and output RGB depth is set by SNES_NTSC_OUT_DEPTH. Both default to 16-bit RGB.
	In_row_width is the number of pixels to get to the next input row. Out_pitch
	is the number of *bytes* to get to the next output row. */
	void snes_ntsc_blit(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* input,
		long in_row_width, int burst_phase, int in_width, int in_height,
		void* rgb_out, long out_pitch, long Alevel);

	/* Stage 4 of the NTSC filter re-architecture (see
	tools/ntsc_bench/analysis/ARCHITECTURE.md and README.md "Current state"): the only
	hires blitter now - snes_ntsc_blit_hires_fast() (the old per-ISA SSE4/NEON/RVV
	intrinsic version) and the separate scalar snes_ntsc_blit_hires() have both been
	retired. This one is an adjacent-outputs vector kernel with an inline run-length
	classifier (Part B/C of the re-architecture), built on the portable v4u32 SIMD layer
	(src/main/sdl2/snes_ntsc_simd.h), with NEON, SSE2 and RVV backends plus a plain-C
	scalar fallback - so, unlike the retired snes_ntsc_blit_hires_fast(), it is available
	and correct unconditionally on every build (SNES_NTSC_HAVE_SIMD 0 or 1). Same
	signature the two retired functions shared. */
	void snes_ntsc_blit_hires_vec(snes_ntsc_t const* ntsc, SNES_NTSC_IN_T const* restrict input, long in_row_width,
		int burst_phase, int in_width, int in_height, void* restrict rgb_out, long out_pitch, long Alevel);

	/* Number of output pixels written by low-res blitter for given input width. Width
	might be rounded down slightly; use SNES_NTSC_IN_WIDTH() on result to find rounded
	value. Guaranteed not to round 256 down at all. */
#define SNES_NTSC_OUT_WIDTH( in_width ) \
	((((in_width) - 1) / snes_ntsc_in_chunk + 1) * snes_ntsc_out_chunk)

	/* Output width for the hires blitter (snes_ntsc_blit_hires_vec), for a given hires
	input width - e.g. 752 for 640, 948 for 808, on every platform (its scalar backend
	produces the same width the old SIMD-only _fast did). Renamed from
	SNES_NTSC_OUT_WIDTH_SIMD in Stage 4; that name is kept below as an alias for one
	release. */
#define SNES_NTSC_OUT_WIDTH_HIRES( in_width ) \
	(((in_width - 16) * 7 / 6) + 24)

	/* Deprecated alias for SNES_NTSC_OUT_WIDTH_HIRES - kept for one release; use the new
	name in new code. */
#define SNES_NTSC_OUT_WIDTH_SIMD( in_width ) SNES_NTSC_OUT_WIDTH_HIRES( in_width )

	/* Number of low-res input pixels that will fit within given output width. Might be
	rounded down slightly; use SNES_NTSC_OUT_WIDTH() on result to find rounded
	value. */
#define SNES_NTSC_IN_WIDTH( out_width ) \
	(((out_width) / snes_ntsc_out_chunk - 1) * snes_ntsc_in_chunk + 1)


	/* Interface for user-defined custom blitters */

	enum { snes_ntsc_in_chunk = 3 }; /* number of input pixels read per chunk */
	enum { snes_ntsc_out_chunk = 7 }; /* number of output pixels generated per chunk */
	enum { snes_ntsc_black = 0 }; /* legacy colour value for black; unused by the slot-indexed
	                                  blitters below, which pad rows with snes_ntsc_black_slot
	                                  instead (see the slot-indexed table section further down) */
	enum { snes_ntsc_burst_count = 3 }; /* burst phase cycles through 0, 1, and 2 */

	/* Begins outputting row and starts three pixels. First pixel will be cut off a bit.
	Use snes_ntsc_black_slot for unused pixels. Declares variables, so must be before first
	statement in a block (unless you're using C++). */
#define SNES_NTSC_BEGIN_ROW( ntsc, burst, pixel0, pixel1, pixel2 ) \
	char const* ktable = (char const*) (ntsc)->plane[burst];\
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

/* private */
#include <stdint.h>
	enum { snes_ntsc_entry_size = 128 }; // JJP - size (words) of the temporary, flat per-colour
	                                     // kernel entry used only during slot generation (see
	                                     // gen_slot() in snes_ntsc.cpp); not a table dimension
	                                     // any more, kept purely as scratch-buffer sizing.

	// ---- Slot-indexed, phase-major kernel table (Part A of the NTSC re-architecture) ----
	// The table is addressed by *slot*, not by a 16-bit packed colour value: slots 0..4095
	// are standard palette entries, 4096..8191 are their shadow counterparts (exactly the
	// range of values already present in the emulator's S16 pixel buffer), slot 8192 is
	// reserved for black (used by the blitters to pad the start/end of each row) and slot
	// 8193 is a spare, also always black, kept only for layout symmetry/future use. Each
	// slot holds one burst-phase's kernel as three 20-word "alignment blocks": 3 leading
	// zero words, the 14 real kernel taps (see SNES_NTSC_KW below for how a classic 0..41
	// kernel-word index maps into this padded layout), then 3 trailing zero words. The
	// symmetric padding (added in Stage 2 of the re-architecture, see
	// tools/ntsc_bench/analysis/ARCHITECTURE.md) means every compile-time-constant 16-byte
	// (4-word) window that `snes_ntsc_blit_hires_vec` needs, starting anywhere from 3 words
	// before the first real tap to 3 words after the last, stays inside the padded block
	// without any bounds check; `_fast` and the scalar blitter are unaffected because they
	// only ever read via SNES_NTSC_KW, which folds to the same physical offsets regardless
	// of block size. This costs about 43% extra memory (60 words allocated per slot per
	// phase versus 42 actually used; table is about 5.9 MB total) in exchange for that.
	enum { snes_ntsc_slot_count  = 8192 };  // std (0..4095) + shadow (4096..8191)
	enum { snes_ntsc_black_slot  = 8192 };  // reserved slot for black; used as row padding
	enum { snes_ntsc_slot_total  = 8194 };  // slot_count + black_slot + one spare slot
	enum { snes_ntsc_block_words = 20 };    // 3 lead zeros + 14 real kernel words + 3 trail zeros
	enum { snes_ntsc_block_lead  = 3 };     // zero pad words before the first real tap of a block
	enum { snes_ntsc_slot_words  = 60 };    // 3 alignment blocks x 20 words, per slot per phase
	enum { snes_ntsc_alignment_count = 3 }; // number of alignment blocks per slot per phase

	// ---- Part C derived tables: the run-length ladder (Stage 3 of the re-architecture,
	// see tools/ntsc_bench/analysis/ARCHITECTURE.md section 6). All three are exact
	// integer sums of the base `plane` blocks above, recomputed for a slot every time
	// gen_slot() (re)generates it, so snes_ntsc_update_slot(s)() keep them coherent
	// automatically. Same 3-lead-zero-words convention as `plane`'s blocks so every
	// compile-time-constant 16-byte window the ladder needs lands inside the block
	// without a bounds check.
	//
	// Pair kernel: PK_a[n] = K_a[n] + K_a[n-1], n = 0..14 (15 real words), the exact
	// contribution of an identical even/odd pixel pair sharing alignment block a. 3 lead
	// + 15 real + 6 trail = 24 words (wider trailing pad than a K block: a residue-2
	// window load reaches word 20).
	enum { snes_ntsc_pk_block_words = 24 };
	enum { snes_ntsc_pk_slot_words  = (int)snes_ntsc_alignment_count * (int)snes_ntsc_pk_block_words }; // 72
	// Chunk kernel: CK[n] = PK_0[n] + PK_1[n-2] + PK_2[n-4], n = 0..18 (19 real words),
	// the exact contribution of a uniform 6-pixel chunk, combining all 3 alignments (one
	// block per slot per phase, not per-alignment). 3 lead + 19 real + 6 trail = 28 words
	// (a residue-2 window load reaches word 24).
	enum { snes_ntsc_ck_words = 28 };
	// Flat pattern: the RAW (pre-clamp/convert) 7 output words produced by a 12-pixel
	// window that is all one slot (steady run of 3+ uniform same-slot chunks), plus one
	// padding word (unused) so the block is 2 vectors' worth. Stored raw, not clamped,
	// because Alevel is a per-call blitter argument, not known at generation time; the
	// blitter clamps+converts these 2 vectors cheaply per steady-run chunk it emits.
	enum { snes_ntsc_flat_words = 8 };

	typedef uint32_t snes_ntsc_rgb_t;           // JJP - was unsigned long

	// Maps a classic kernel-word index (0..41, i.e. 3 alignment blocks of 14 words: block b
	// occupies words 14b..14b+13) into the padded, 20-word-block layout used by `plane`
	// below (block b now occupies words 20b..20b+19, with real taps at 20b+3..20b+16 and
	// words 20b..20b+2 / 20b+17..20b+19 always zero). Every caller passes a compile-time-
	// constant `w`, so this folds away entirely.
#define SNES_NTSC_KW( w ) (((w) / 14) * snes_ntsc_block_words + snes_ntsc_block_lead + ((w) % 14))

	struct snes_ntsc_t {
		// plane[burst phase][slot][word within slot's 3x16-word kernel]. One phase's plane
		// (plane[b]) is a contiguous, 64-byte-aligned block covering every slot, so a whole
		// row (which reads one phase throughout) touches one contiguous region of memory.
		alignas(64) snes_ntsc_rgb_t plane[snes_ntsc_burst_count][snes_ntsc_slot_total][snes_ntsc_slot_words];
		// Part C run-length ladder derived tables (see the enums above and
		// tools/ntsc_bench/analysis/ARCHITECTURE.md section 6): pair kernel, chunk kernel,
		// and flat (steady-run) pattern, phase-major like `plane` for the same row-locality
		// reason. About 10.6 MB combined, on top of `plane`'s ~5.9 MB.
		alignas(64) snes_ntsc_rgb_t plane_pk[snes_ntsc_burst_count][snes_ntsc_slot_total][snes_ntsc_alignment_count][snes_ntsc_pk_block_words];
		alignas(64) snes_ntsc_rgb_t plane_ck[snes_ntsc_burst_count][snes_ntsc_slot_total][snes_ntsc_ck_words];
		alignas(64) snes_ntsc_rgb_t plane_flat[snes_ntsc_burst_count][snes_ntsc_slot_total][snes_ntsc_flat_words];
		// Opaque copy of the generator state (decoder matrices, gamma table, rescale
		// kernels, etc. - see init_t in snes_ntsc_impl.h) plus the merge_fields flag,
		// captured by snes_ntsc_init() so that snes_ntsc_update_slot(s)() can regenerate
		// an individual slot's kernel later without needing the original snes_ntsc_setup_t.
		// Sized generously (init_t is about 3 KB); actual size is static_assert-checked
		// against snes_ntsc_gen_state_t in snes_ntsc.cpp.
		alignas(16) unsigned char gen_state[8192];
	};

#define SNES_NTSC_RGB16( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n & 0x001E) | (n >> 1 & 0x03E0) | (n >> 2 & 0x3C00)) * \
			(snes_ntsc_entry_size / 2 * sizeof (snes_ntsc_rgb_t)))

	// following input masks retain full 5-6-5 RGB values
#define SNES_NTSC_RGB16_565( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n & 0x001F) | (n & 0x07E0) | (n & 0xF800)) * \
			(snes_ntsc_entry_size / 1 * sizeof (snes_ntsc_rgb_t)))

// Slot-indexed input: 'n' is a direct slot number (0..snes_ntsc_slot_count-1 for a live
// palette entry - standard in 0..4095, shadow in 4096..8191 - or snes_ntsc_black_slot /
// the spare slot for padding), exactly the value already carried by the emulator's S16
// pixel buffer. 'ktable' is the current burst phase's plane base, as set up by
// SNES_NTSC_BEGIN_ROW (ntsc->plane[burst]); no bit shuffling of 'n'
// is performed, unlike SNES_NTSC_RGB16 above. The 5-bit-per-channel packed colour value
// this slot corresponds to (R bit 14-10, G bit 9-5, B bit 0-4, bit 15 = shadow) is only
// consulted at *generation* time (snes_ntsc_init/snes_ntsc_update_slot(s)), not here.
#define SNES_NTSC_S16( ktable, n ) \
	(snes_ntsc_rgb_t const*) (ktable + ((n) * \
			(snes_ntsc_slot_words * sizeof (snes_ntsc_rgb_t))))

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
        kernel0  [SNES_NTSC_KW(x       )] + kernel1  [SNES_NTSC_KW((x+12)%7+14)] + kernel2  [SNES_NTSC_KW((x+10)%7+28)] +\
        kernelx0 [SNES_NTSC_KW((x+7)%14)] + kernelx1 [SNES_NTSC_KW((x+ 5)%7+21)] + kernelx2 [SNES_NTSC_KW((x+ 3)%7+35)];\
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

// Generic (without SIMD) producing ABGR:
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
