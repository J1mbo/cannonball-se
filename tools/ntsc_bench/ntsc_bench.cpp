/*****************************************************************************
 * ntsc_bench.cpp - minimal throughput/correctness harness for the Blargg
 *                  SNES NTSC hires blitters. See DESIGN.md in this directory.
 *
 * One binary tests exactly one blitter, selected at compile time so the same
 * source builds unmodified against any revision of snes_ntsc.{cpp,h} - the
 * current working tree (default: snes_ntsc_blit_hires_vec, Stage 4's sole
 * hires blitter), or an older revision still carrying the retired
 * snes_ntsc_blit_hires_fast()/scalar snes_ntsc_blit_hires() for reference:
 *
 *   g++ -O3 -mcpu=cortex-a53 -std=c++17 -DBENCH_FN=snes_ntsc_blit_hires_fast \
 *       -DBENCH_REV=086591c ntsc_bench.cpp snes_ntsc.cpp -o bench_fast
 *
 * -DBENCH_FN=<function>   function under test (default snes_ntsc_blit_hires_vec).
 *                         run_host.sh/run_pi.sh also accept <function>_vec_scalar
 *                         as shorthand for "snes_ntsc_blit_hires_vec with
 *                         -DSNES_NTSC_FORCE_SCALAR_SIMD" (see snes_ntsc_simd.h).
 * -DBENCH_SCALAR          building against a pre-Stage-4 revision's scalar
 *                         snes_ntsc_blit_hires reference; also switches the
 *                         output width formula to match the non-SIMD game
 *                         build. run_host.sh/run_pi.sh add this automatically
 *                         whenever <function> is exactly snes_ntsc_blit_hires.
 * -DBENCH_REV=<token>     free-form revision tag, printed in the RESULT line.
 * -DBENCH_SLOT_API        build against the slot-indexed snes_ntsc_init(3-arg)/
 *                         snes_ntsc_update_slot(s)() API (Part A of the NTSC filter
 *                         re-architecture) instead of the classic 65536-row,
 *                         colour-indexed table. Pixel buffers hold palette *slots*
 *                         (0..8191) rather than looked-up colour values; a synthetic
 *                         colours[8192] array (or, under --input, each captured
 *                         frame's own table) is generated to snes_ntsc_init()/
 *                         snes_ntsc_update_slots() instead. run_host.sh/run_pi.sh add
 *                         this automatically when the staged snes_ntsc.h defines
 *                         snes_ntsc_update_slot. Without it, this file builds and
 *                         behaves exactly as it did before the re-architecture
 *                         (e.g. against revision 086591c).
 *
 * Links only against snes_ntsc.cpp: no SDL, no CMake, no game headers.
 *
 * Runtime flags (see also README.md):
 *   --pattern random|mixed|runs  pixel pattern used to fill the input frame
 *                                 (default random). "runs" is a deterministic
 *                                 (fixed-seed) sequence of runs of random
 *                                 length 1-48 pixels and random colour; half
 *                                 the (non-single) runs are pair-doubled
 *                                 (each successive pair of pixels gets its
 *                                 own fresh colour, so the run is never
 *                                 chunk-uniform but every pair is identical)
 *                                 and every 7th run is forced to one pixel -
 *                                 see README.md for why this is the reference
 *                                 pattern for Part C's run-length ladder
 *                                 (snes_ntsc_blit_hires_vec's uniform/pair/
 *                                 general classification). Ignored when
 *                                 --input is given.
 *   --colours N / --colors N     size of the fixed-seed palette, 1-65536
 *                                 (default 256). Ignored when --input is given.
 *   --palette random|contiguous  how palette *values* map onto ntsc table
 *                                 rows (default random). "random" keeps the
 *                                 original pseudo-random 16-bit values, bit-
 *                                 identical to pre-existing runs/checksums.
 *                                 "contiguous" instead uses values 0..N-1, so
 *                                 the table rows touched are the first N of
 *                                 them (512 B/row, N/8 4 KB pages) instead of
 *                                 scattered across the full 32 MiB table.
 *                                 Pixel *placement* (which palette slot lands
 *                                 on which pixel) uses the same RNG sequence
 *                                 in both modes. Ignored when --input is given.
 *   --width W / --height H       input frame size (default 640x448). Ignored
 *                                 (overridden by the loaded frames' own
 *                                 dimensions) when --input is given, and
 *                                 forced to 320x224 by --lores.
 *   --seconds S                  length of the timed loop (default 5).
 *   --threads T                  horizontal bands run concurrently (default 1).
 *   --hugepages                  allocate the 32 MiB ntsc table 2 MiB-aligned
 *                                 and madvise(MADV_HUGEPAGE) it before init
 *                                 (default off). Linux only; prints
 *                                 "unsupported" and continues elsewhere.
 *   --input <path>                replace the synthetic input with real
 *                                 captured game frames: <path> may be one
 *                                 frame_*.bin file, a directory (all
 *                                 frame_*.bin files in it, sorted), or a
 *                                 comma-separated list of either. All loaded
 *                                 frames must share one width/height (mixed
 *                                 geometries are rejected). The timed loop
 *                                 cycles through the loaded frames in order,
 *                                 one frame per iteration, at *that frame's
 *                                 own recorded* burst phase (mirrors the
 *                                 game); the checksum instead runs every
 *                                 loaded frame at phases 0, 1 and 2 (frames
 *                                 outer, phases inner), one FNV-1a over all
 *                                 of it, as the cross-build bit-exactness
 *                                 gate for the whole captured-frame corpus.
 *                                 Mutually exclusive with --lores.
 *   --lores                       test the low-res blitter (snes_ntsc_blit)
 *                                 against a synthetic 320x224 input instead
 *                                 of the hires blitter under test; BENCH_FN
 *                                 is ignored (fn=snes_ntsc_blit in the
 *                                 output). Regression-only for the lores
 *                                 path in later stages, not for performance
 *                                 comparison against the hires numbers
 *                                 elsewhere in this file. Mutually exclusive
 *                                 with --input.
 *   --dump-output <file>          append every output frame produced during
 *                                 the correctness-checksum pass (the same
 *                                 frames/phases folded into `checksum` -
 *                                 every loaded frame at phases 0,1,2 for
 *                                 --input, or the three synthetic phases
 *                                 otherwise) to <file>, tightly packed
 *                                 (height rows x out_width x 4 bytes, pitch
 *                                 == out_width*4), preceded by one 32-byte
 *                                 header: magic "CBSEOUT1", uint32 out_width,
 *                                 uint32 height, uint32 frame_count, uint32
 *                                 phases_per_frame, uint32 reserved[2]. Pure
 *                                 addition - does not alter timing, the
 *                                 checksum, or any RESULT field.
 *****************************************************************************/
#include "snes_ntsc.h"
#include "snes_ntsc_simd.h" // for SNES_NTSC_SIMD_BACKEND_NAME - see the simd= override below

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#ifndef BENCH_FN
#define BENCH_FN snes_ntsc_blit_hires_vec
#endif
#ifndef BENCH_REV
#define BENCH_REV unknown
#endif
#define STR_(x) #x
#define STR(x) STR_(x)

#if !SNES_NTSC_HAVE_SIMD && !defined(BENCH_SCALAR)
int main() {
    std::printf("SNES_NTSC_HAVE_SIMD is 0 and BENCH_SCALAR is not defined - "
                 "no SIMD blitter to test on this target.\n");
    return 0;
}
#else

// ---- 64-byte aligned allocation (required by the SIMD blitters) -----------
static void* aligned_alloc64(size_t bytes) {
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, 64);
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, bytes) != 0) p = nullptr;
    return p;
#endif
}
static void aligned_free64(void* p) {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    free(p);
#endif
}

// ---- --hugepages support (Linux only) --------------------------------------
// Allocates `bytes` 2 MiB-aligned, rounded up to a 2 MiB multiple, and asks
// the kernel via madvise(MADV_HUGEPAGE) to back it with transparent huge
// pages. THP is opportunistic even in "madvise" mode: this only requests it,
// it does not guarantee it - see read_anon_hugepages_kb() below for how the
// caller confirms whether the kernel actually granted any. On non-Linux
// builds this degrades to a plain, non-huge allocation and the caller prints
// "unsupported".
#if defined(__linux__)
static const size_t HUGEPAGE_BYTES = 2u * 1024 * 1024;

static void* alloc_hugepage(size_t bytes, bool& madvise_ok) {
    madvise_ok = false;
    size_t rounded = ((bytes + HUGEPAGE_BYTES - 1) / HUGEPAGE_BYTES) * HUGEPAGE_BYTES;
    void* p = nullptr;
    if (posix_memalign(&p, HUGEPAGE_BYTES, rounded) != 0) return nullptr;
    madvise_ok = (madvise(p, rounded, MADV_HUGEPAGE) == 0);
    return p;
}

// Reports the process-wide "AnonHugePages" total (kB) from smaps_rollup,
// falling back to summing per-mapping entries in smaps if the rollup file
// isn't available. The bench has exactly one allocation large enough to be
// THP-eligible (the 32 MiB ntsc table) at the point this is called, so the
// process-wide total is a good proxy for "was the table's memory granted
// huge pages". Returns -1 if neither file yields a value.
static long read_anon_hugepages_kb() {
    long kb;
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (f) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "AnonHugePages: %ld kB", &kb) == 1) {
                std::fclose(f);
                return kb;
            }
        }
        std::fclose(f);
    }
    f = std::fopen("/proc/self/smaps", "r");
    if (f) {
        long total = 0;
        bool found = false;
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "AnonHugePages: %ld kB", &kb) == 1) {
                total += kb;
                found = true;
            }
        }
        std::fclose(f);
        if (found) return total;
    }
    return -1;
}
#endif // __linux__

// ---- deterministic xorshift32 ----------------------------------------------
static inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

enum class Palette { Random, Contiguous };

// Fixed-seed palette of `n` distinct-ish 15-bit RGB + shadow-bit values
// (0x0000-0xFFFF is the full valid SNES_NTSC_S16 index range).
//
// Palette::Random (default) is bit-identical to the original hard-coded
// behaviour - do not change this branch, it is a checksum-compatibility
// requirement. Palette::Contiguous instead uses values 0..n-1 directly, so
// the ntsc table rows touched by the blit (snes_ntsc_entry_size==128 entries
// = 512 B per row, indexed directly by pixel value) are the first n rows -
// contiguous in memory, spanning n/8 4 KB pages - instead of scattered
// pseudo-randomly across the full 32 MiB / 65536-row table.
static void build_palette(std::vector<uint16_t>& pal, int n, Palette mode) {
    pal.resize((size_t)n);
    if (mode == Palette::Contiguous) {
        for (int i = 0; i < n; ++i) pal[(size_t)i] = (uint16_t)i;
        return;
    }
    uint32_t rng = 0xC0FFEEu;
    for (int i = 0; i < n; ++i) {
        uint32_t r = xorshift32(rng);
        pal[(size_t)i] = (uint16_t)(r & 0xFFFFu);
    }
}

enum class Pattern { Random, Mixed, Runs };

// Fills the input frame once; reused unchanged every timed/checksum frame.
// `palette` selects the *values* stored in the palette (see build_palette);
// the RNG sequence driving *where* each palette slot is placed is unchanged
// by it, so spatial placement stays exactly as random as before either way.
static void fill_input(uint16_t* in, int w, int h, Pattern pattern, int colours, Palette palette) {
    std::vector<uint16_t> pal;
    build_palette(pal, colours, palette);
    uint32_t rng = 0xA5A5A5A5u;
    for (int y = 0; y < h; ++y) {
        uint16_t* row = in + (size_t)y * w;
        bool noisy_third = (pattern == Pattern::Random) ||
                            (y >= 2 * h / 3);          // mixed: bottom third
        bool gradient_third = (pattern == Pattern::Mixed) &&
                               (y >= h / 3) && (y < 2 * h / 3);
        for (int x = 0; x < w; ++x) {
            if (noisy_third) {
                uint32_t r = xorshift32(rng);
                row[x] = pal[r % (uint32_t)colours];
            } else if (gradient_third) {
                int idx = (x * colours / w) % colours;
                row[x] = pal[(size_t)idx];
            } else {
                row[x] = pal[0]; // flat "sky" band
            }
        }
    }
}

// --pattern runs: deterministic fixed-seed sequence of runs (see README.md and the
// Stage 3 design brief - Part C's run-length ladder in snes_ntsc_blit_hires_vec). Each
// row is built as a sequence of runs of random length 1-48 pixels: every 7th run is
// forced to a single pixel (a lone differing pixel breaks both chunk-uniformity and
// pair-equality right there, exercising the general per-pixel path and a boundary
// transition); of the rest, half are "pair-doubled" - instead of one flat run of a
// single colour (which would just be a longer uniform/steady-run chunk), each
// successive *pair* of pixels within the run gets its own fresh random colour
// (c0 c0 c1 c1 c2 c2 ...), so the run is never chunk-uniform but every adjacent pair is
// identical, exercising the PK pair path specifically rather than letting a cheaper
// uniform-chunk/steady-run shortcut swallow it. The other half of non-single runs are
// flat (one colour for the whole run), exercising uniform chunks and steady runs. Run
// boundaries land at essentially every phase relative to both the 6-pixel chunk grid
// and the 2-pixel pair grid over many runs/rows, so every alignment and residue the
// ladder distinguishes gets exercised. Independent fixed-seed RNG stream (not shared
// with Random/Mixed's 0xA5A5A5A5u) since this is a wholly different generation scheme.
static void fill_input_runs(uint16_t* in, int w, int h, int colours, Palette palette) {
    std::vector<uint16_t> pal;
    build_palette(pal, colours, palette);
    uint32_t rng = 0x52554e53u; // "RUNS"
    for (int y = 0; y < h; ++y) {
        uint16_t* row = in + (size_t)y * w;
        int x = 0;
        long run_index = 0;
        while (x < w) {
            ++run_index;
            int run_len = 1 + (int)(xorshift32(rng) % 48u); // 1..48
            bool single = (run_index % 7) == 0;
            bool pair_doubled = !single && ((xorshift32(rng) & 1u) == 0u);
            if (single) {
                row[x++] = pal[xorshift32(rng) % (uint32_t)colours];
            } else if (pair_doubled) {
                for (int i = 0; i < run_len && x < w; ++i) {
                    uint16_t c = pal[xorshift32(rng) % (uint32_t)colours];
                    row[x++] = c;
                    if (x < w) row[x++] = c;
                }
            } else {
                uint16_t c = pal[xorshift32(rng) % (uint32_t)colours];
                for (int i = 0; i < run_len && x < w; ++i) row[x++] = c;
            }
        }
    }
}

#ifdef BENCH_SLOT_API
// Slot-mode counterpart of fill_input_runs(): same run-length algorithm and the same
// RNG stream/decisions, writing slot indices (0..colours-1) instead of colour values,
// with colours_out[s] = pal[s] for s<colours so slot s renders the same colour
// fill_input_runs() would have written directly (mirrors fill_input_slots() above).
static void fill_input_slots_runs(uint16_t* in, int w, int h, int colours, Palette palette,
                                   std::vector<uint16_t>& colours_out) {
    std::vector<uint16_t> pal;
    build_palette(pal, colours, palette);
    colours_out.assign((size_t)snes_ntsc_slot_count, (uint16_t)0);
    for (int s = 0; s < colours; ++s) colours_out[(size_t)s] = pal[(size_t)s];

    uint32_t rng = 0x52554e53u; // "RUNS"
    for (int y = 0; y < h; ++y) {
        uint16_t* row = in + (size_t)y * w;
        int x = 0;
        long run_index = 0;
        while (x < w) {
            ++run_index;
            int run_len = 1 + (int)(xorshift32(rng) % 48u);
            bool single = (run_index % 7) == 0;
            bool pair_doubled = !single && ((xorshift32(rng) & 1u) == 0u);
            if (single) {
                row[x++] = (uint16_t)(xorshift32(rng) % (uint32_t)colours);
            } else if (pair_doubled) {
                for (int i = 0; i < run_len && x < w; ++i) {
                    uint16_t s = (uint16_t)(xorshift32(rng) % (uint32_t)colours);
                    row[x++] = s;
                    if (x < w) row[x++] = s;
                }
            } else {
                uint16_t s = (uint16_t)(xorshift32(rng) % (uint32_t)colours);
                for (int i = 0; i < run_len && x < w; ++i) row[x++] = s;
            }
        }
    }
}
#endif

#ifdef BENCH_SLOT_API
// Slot-mode counterpart of fill_input(): the RNG streams (build_palette()'s and this
// function's own xorshift32 sequence) and every spatial decision are byte-for-byte
// identical to fill_input() above, so the *placement* of each palette entry is
// unchanged - only what gets written differs. Where fill_input() would write pal[k]
// (a colour value, looked up directly in the old 65536-row table), this instead writes
// k itself (a slot index, 0..colours-1) and returns colours_out[8192] with
// colours_out[s] = pal[s] for s<colours and 0 elsewhere, so that slot k's kernel -
// generated from colours_out[k] - renders exactly the colour pal[k] the old mode used,
// keeping the two modes' rendered output (and hence checksums) identical.
static void fill_input_slots(uint16_t* in, int w, int h, Pattern pattern, int colours,
                              Palette palette, std::vector<uint16_t>& colours_out) {
    std::vector<uint16_t> pal;
    build_palette(pal, colours, palette);

    colours_out.assign((size_t)snes_ntsc_slot_count, (uint16_t)0);
    for (int s = 0; s < colours; ++s) colours_out[(size_t)s] = pal[(size_t)s];

    uint32_t rng = 0xA5A5A5A5u;
    for (int y = 0; y < h; ++y) {
        uint16_t* row = in + (size_t)y * w;
        bool noisy_third = (pattern == Pattern::Random) ||
                            (y >= 2 * h / 3);          // mixed: bottom third
        bool gradient_third = (pattern == Pattern::Mixed) &&
                               (y >= h / 3) && (y < 2 * h / 3);
        for (int x = 0; x < w; ++x) {
            if (noisy_third) {
                uint32_t r = xorshift32(rng);
                row[x] = (uint16_t)(r % (uint32_t)colours); // slot index, not pal[]
            } else if (gradient_third) {
                int idx = (x * colours / w) % colours;
                row[x] = (uint16_t)idx;
            } else {
                row[x] = 0; // flat "sky" band -> slot 0 (colours_out[0] == pal[0])
            }
        }
    }
}
#endif

// 64-bit FNV-1a, run cumulatively across the three checksum frames.
static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ---- --input: captured game frames (see write_dump_frame() in --------------
// src/main/sdl2/rendersurface.cpp for the authoritative format doc) ---------
//
// A frame-dump file holds a 64-byte header, then three uint16_t arrays:
// raw[w*h] (S16 slot indices 0..8191), table[palette_entries] (rgb_blargg,
// exactly as it stood for this frame) and colour[w*h] = table[raw[]] -
// exactly what the blitter is handed today. `raw` and `table` are kept (not
// just `colour`) so a future slot-indexed blitter API can be benched from
// the same loaded frames without changing this loader.
struct Frame {
    uint32_t width = 0, height = 0, frame_number = 0, phase = 0;
    uint32_t hires = 0, wide = 0, palette_entries = 0;
    std::vector<uint16_t> raw;    // slot indices, for a future slot-indexed API
    std::vector<uint16_t> table;  // per-frame colour table, ditto
    std::vector<uint16_t> colour; // raw[] through table[]: today's blit input
    std::string path;
};

// Loads and validates one CBSEFRM1 frame-dump file: magic, and file size
// against the size implied by the header's own width/height/palette_entries
// (mirrors frame_stats.py's read_frame()). Returns false and fills `err` on
// any problem; never partially fills `fr` on failure.
static bool load_frame(const std::string& path, Frame& fr, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = path + ": cannot open"; return false; }

    std::fseek(f, 0, SEEK_END);
    long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 64) { err = path + ": file too small for a header"; std::fclose(f); return false; }

    uint32_t hdr[16];
    if (std::fread(hdr, sizeof(uint32_t), 16, f) != 16) {
        err = path + ": short read on header"; std::fclose(f); return false;
    }
    if (std::memcmp(hdr, "CBSEFRM1", 8) != 0) {
        err = path + ": bad magic (not a CBSEFRM1 frame dump)"; std::fclose(f); return false;
    }

    Frame tmp;
    tmp.width = hdr[2]; tmp.height = hdr[3]; tmp.frame_number = hdr[4];
    tmp.phase = hdr[5]; tmp.hires = hdr[6]; tmp.wide = hdr[7]; tmp.palette_entries = hdr[8];
    tmp.path = path;

    const size_t pixel_count = (size_t)tmp.width * (size_t)tmp.height;
    const long expected_size = 64 + (long)(pixel_count * 2) + (long)(tmp.palette_entries * 2)
                                   + (long)(pixel_count * 2);
    if (file_size != expected_size) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      ": size mismatch: file is %ld bytes, expected %ld (width=%u height=%u palette_entries=%u)",
                      file_size, expected_size, tmp.width, tmp.height, tmp.palette_entries);
        err = path + msg;
        std::fclose(f);
        return false;
    }

    tmp.raw.resize(pixel_count);
    tmp.table.resize(tmp.palette_entries);
    tmp.colour.resize(pixel_count);
    bool ok = std::fread(tmp.raw.data(), sizeof(uint16_t), pixel_count, f) == pixel_count &&
              std::fread(tmp.table.data(), sizeof(uint16_t), tmp.palette_entries, f) == tmp.palette_entries &&
              std::fread(tmp.colour.data(), sizeof(uint16_t), pixel_count, f) == pixel_count;
    std::fclose(f);
    if (!ok) { err = path + ": short read on frame data"; return false; }

    fr = std::move(tmp);
    return true;
}

// Expands a --input spec into a list of frame_*.bin paths, in the order the
// bench will use them: a single .bin file, a directory (its frame_*.bin
// files, sorted - the zero-padded frame number makes lexicographic order
// equal numeric order), or a comma-separated list of either, concatenated in
// the order given.
static bool expand_input_paths(const std::string& spec, std::vector<std::string>& out, std::string& err) {
    namespace fs = std::filesystem;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string part = (comma == std::string::npos) ? spec.substr(start) : spec.substr(start, comma - start);
        start = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
        if (part.empty()) continue;

        std::error_code ec;
        fs::file_status st = fs::status(part, ec);
        if (ec || !fs::exists(st)) { err = part + ": not found"; return false; }

        if (fs::is_directory(st)) {
            std::vector<std::string> files;
            for (auto& entry : fs::directory_iterator(part, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                if (name.rfind("frame_", 0) == 0 && name.size() >= 4 &&
                    name.compare(name.size() - 4, 4, ".bin") == 0) {
                    files.push_back(entry.path().string());
                }
            }
            if (ec) { err = part + ": " + ec.message(); return false; }
            std::sort(files.begin(), files.end());
            out.insert(out.end(), files.begin(), files.end());
        } else if (fs::is_regular_file(st)) {
            out.push_back(part);
        } else {
            err = part + ": not a regular file or directory";
            return false;
        }
    }
    return true;
}

struct Args {
    std::string pattern = "random";
    int colours = 256;
    int width   = 640;
    int height  = 448;
    double seconds = 5.0;
    int threads = 1;
    std::string palette = "random";
    bool hugepages = false;
    std::string input; // empty = synthetic input (default)
    bool lores = false;
    std::string dump_output; // empty = no dump (default)
};

static Args parse_args(int argc, char** argv) {
    Args a;
    auto next = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--pattern") a.pattern = next(i);
        else if (s == "--colours" || s == "--colors") a.colours = std::atoi(next(i));
        else if (s == "--width")   a.width   = std::atoi(next(i));
        else if (s == "--height")  a.height  = std::atoi(next(i));
        else if (s == "--seconds") a.seconds = std::atof(next(i));
        else if (s == "--threads") a.threads = std::atoi(next(i));
        else if (s == "--palette") a.palette = next(i);
        else if (s == "--hugepages") a.hugepages = true;
        else if (s == "--input")  a.input   = next(i);
        else if (s == "--lores")  a.lores   = true;
        else if (s == "--dump-output") a.dump_output = next(i);
    }
    if (a.colours < 1) a.colours = 1;
    if (a.colours > 65536) a.colours = 65536;
    if (a.threads < 1) a.threads = 1;
    if (a.palette != "random" && a.palette != "contiguous") a.palette = "random";
    return a;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    if (!args.input.empty() && args.lores) {
        std::printf("error: --input and --lores may not be combined\n");
        return 1;
    }

    // ---- --input: load captured frames, if requested --------------------
    std::vector<Frame> in_frames;
    if (!args.input.empty()) {
        std::vector<std::string> paths;
        std::string err;
        if (!expand_input_paths(args.input, paths, err)) {
            std::printf("error: %s\n", err.c_str());
            return 1;
        }
        if (paths.empty()) {
            std::printf("error: --input '%s' matched no frame_*.bin files\n", args.input.c_str());
            return 1;
        }
        in_frames.reserve(paths.size());
        for (auto& p : paths) {
            Frame fr;
            if (!load_frame(p, fr, err)) {
                std::printf("error: %s\n", err.c_str());
                return 1;
            }
            if (!in_frames.empty() &&
                (fr.width != in_frames.front().width || fr.height != in_frames.front().height)) {
                std::printf("error: --input geometry mismatch: '%s' is %ux%u but '%s' is %ux%u "
                            "(mixed geometries in one run are not supported)\n",
                            in_frames.front().path.c_str(), in_frames.front().width, in_frames.front().height,
                            fr.path.c_str(), fr.width, fr.height);
                return 1;
            }
            in_frames.push_back(std::move(fr));
        }
    }
    const bool using_input = !in_frames.empty();
    if (using_input) {
        std::printf("input: %d frame(s) loaded, %ux%u; --pattern/--colours/--palette/--width/--height ignored\n",
                    (int)in_frames.size(), in_frames.front().width, in_frames.front().height);
    }

#ifdef BENCH_SLOT_API
    // The slot-indexed table has exactly snes_ntsc_slot_count addressable live slots
    // (standard + shadow), matching S16_PALETTE_ENTRIES*2 in the game - every captured
    // frame's table must be exactly that size for frame.table to be usable as the
    // colours array snes_ntsc_init()/snes_ntsc_update_slot(s)() expect.
    if (using_input && (int)in_frames.front().palette_entries != snes_ntsc_slot_count) {
        std::printf("error: --input frame(s) have palette_entries=%u, expected %d under BENCH_SLOT_API\n",
                    in_frames.front().palette_entries, (int)snes_ntsc_slot_count);
        return 1;
    }
#endif

    const int width  = args.lores ? 320 : (using_input ? (int)in_frames.front().width  : args.width);
    const int height = args.lores ? 224 : (using_input ? (int)in_frames.front().height : args.height);
    const int threads = args.threads;
    Pattern pattern = (args.pattern == "mixed") ? Pattern::Mixed
                     : (args.pattern == "runs") ? Pattern::Runs
                     : Pattern::Random;
    Palette palette = (args.palette == "contiguous") ? Palette::Contiguous : Palette::Random;

#ifdef BENCH_SLOT_API
    // The slot-indexed table only has snes_ntsc_slot_count addressable live slots (see
    // above), a hard cap the old 65536-row table did not have - clamp and warn rather
    // than silently misbehaving (colours beyond the cap would alias to slot 0 = black).
    if (!using_input && args.colours > (int)snes_ntsc_slot_count) {
        std::printf("warning: --colours %d exceeds the slot-indexed table's capacity (%d); clamping\n",
                    args.colours, (int)snes_ntsc_slot_count);
        args.colours = (int)snes_ntsc_slot_count;
    }
#endif

#ifdef BENCH_SCALAR
    const int out_width = args.lores ? SNES_NTSC_OUT_WIDTH(320) : SNES_NTSC_OUT_WIDTH(width >> 1); // matches non-SIMD game build
#else
    // SNES_NTSC_OUT_WIDTH_SIMD: kept under its pre-Stage-4 name here (an alias for
    // SNES_NTSC_OUT_WIDTH_HIRES in the working tree's header - see snes_ntsc.h) so this
    // file keeps building unmodified against older revisions too (e.g. run_host.sh
    // a6dfa98 snes_ntsc_blit_hires_fast), whose headers only ever defined this name.
    const int out_width = args.lores ? SNES_NTSC_OUT_WIDTH(320) : SNES_NTSC_OUT_WIDTH_SIMD(width);
#endif
    const long out_pitch = (long)out_width * 4;
    const long Alevel = 255;

    // Function under test: normally BENCH_FN (selected at compile time), but
    // --lores always exercises the low-res blitter instead, regardless of
    // BENCH_FN. Both candidates share one call signature (see snes_ntsc.h);
    // the C-style casts below only strip 'restrict' qualifiers some of them
    // carry, which does not change calling convention or behaviour.
    using BlitFn = void (*)(snes_ntsc_t const*, SNES_NTSC_IN_T const*, long, int, int, int, void*, long, long);
    const BlitFn blit_fn = args.lores ? (BlitFn)&snes_ntsc_blit : (BlitFn)&BENCH_FN;
    const char* fn_name  = args.lores ? "snes_ntsc_blit" : STR(BENCH_FN);

    const char* simd_isa =
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        "NEON";
#elif defined(SNES_NTSC_HAVE_RVV)
        "RVV";
#elif defined(SNES_NTSC_X86_LEVEL)
    #if SNES_NTSC_X86_LEVEL >= 4
        "SSE4";
    #else
        "SSE2";
    #endif
#else
        "none";
#endif

    // snes_ntsc_blit_hires_vec() (Stage 2) is built on its own portable v4u32 SIMD
    // layer (snes_ntsc_simd.h), whose backend selection is independent of - and can
    // differ from - the compile-time ISA detected above: SNES_NTSC_FORCE_SCALAR_SIMD
    // (the "_vec_scalar" bench target mapped by run_host.sh/run_pi.sh) forces the
    // layer to its plain-C fallback even on a NEON/SSE2 host. Whenever the function
    // under test is the vec kernel (fn_name contains "_vec"), report what the layer
    // actually resolved to (NEON/SSE2/RVV/scalar) instead of simd_isa.
    const char* simd = (std::strstr(fn_name, "_vec") != nullptr) ? SNES_NTSC_SIMD_BACKEND_NAME : simd_isa;

    std::printf("ntsc_bench fn=%s rev=%s simd=%s sizeof_ntsc=%zu\n",
                fn_name, STR(BENCH_REV), simd, sizeof(snes_ntsc_t));
    if (args.lores) {
        std::printf("geometry: in %dx%d -> out_width=%d out_pitch=%ld  lores=1 pattern=%s colours=%d threads=%d\n",
                    width, height, out_width, out_pitch, args.pattern.c_str(), args.colours, threads);
    } else if (using_input) {
        std::printf("geometry: in %dx%d -> out_width=%d out_pitch=%ld  inputs=%d threads=%d\n",
                    width, height, out_width, out_pitch, (int)in_frames.size(), threads);
    } else {
        std::printf("geometry: in %dx%d -> out_width=%d out_pitch=%ld  pattern=%s colours=%d threads=%d\n",
                    width, height, out_width, out_pitch, args.pattern.c_str(), args.colours, threads);
    }

    bool hp_madvise_ok = false;
    snes_ntsc_t* ntsc = nullptr;
#if defined(__linux__)
    if (args.hugepages) {
        ntsc = (snes_ntsc_t*)alloc_hugepage(sizeof(snes_ntsc_t), hp_madvise_ok);
        std::printf("hugepages: madvise(MADV_HUGEPAGE) %s\n", hp_madvise_ok ? "succeeded" : "failed");
    } else {
        ntsc = (snes_ntsc_t*)std::malloc(sizeof(snes_ntsc_t));
    }
#else
    if (args.hugepages) std::printf("hugepages: unsupported on this platform\n");
    ntsc = (snes_ntsc_t*)std::malloc(sizeof(snes_ntsc_t));
#endif
    if (!ntsc) { std::printf("alloc ntsc failed\n"); return 1; }
    // Mimic RenderSurface::init_blargg_filter(): composite preset with merge_fields cleared.
    snes_ntsc_setup_t setup = snes_ntsc_composite;
    setup.merge_fields = 0;

    // ---- input buffers -----------------------------------------------------
    // Synthetic (default / --lores) modes fill one buffer once and reuse it every frame,
    // as before. --input instead needs one 64-byte-aligned copy of each loaded frame's
    // pixel data - the SIMD blitters require 64-byte alignment (see the note atop
    // snes_ntsc.h) and std::vector's allocator gives no such guarantee, so the
    // file-loaded data can't be handed to them directly.
    //
    // Under BENCH_SLOT_API the pixel buffers hold slot indices (not colour values), and
    // snes_ntsc_init() is fed a colours[] array separately: fill_input_slots()'s
    // colours_out for synthetic/--lores, or the first loaded frame's table for --input
    // (current_table tracks what the ntsc object's slots currently reflect, so later
    // frames with a different table can be applied incrementally - see sync_to_frame()
    // below).
    uint16_t* input = nullptr;
    std::vector<uint16_t*> input_aligned; // one per loaded frame, --input only
    const size_t pixel_bytes = (size_t)width * height * sizeof(uint16_t);
#ifdef BENCH_SLOT_API
    std::vector<uint16_t> current_table; // --input only; snapshot of what ntsc holds now
#endif

    if (using_input) {
        input_aligned.assign(in_frames.size(), nullptr);
        for (size_t i = 0; i < in_frames.size(); ++i) {
            uint16_t* buf = (uint16_t*)aligned_alloc64(pixel_bytes);
            if (!buf) { std::printf("alloc input buffer failed\n"); return 1; }
#ifdef BENCH_SLOT_API
            std::memcpy(buf, in_frames[i].raw.data(), pixel_bytes);
#else
            std::memcpy(buf, in_frames[i].colour.data(), pixel_bytes);
#endif
            input_aligned[i] = buf;
        }
#ifdef BENCH_SLOT_API
        current_table = in_frames.front().table;
        snes_ntsc_init(ntsc, &setup, current_table.data());
#else
        snes_ntsc_init(ntsc, &setup);
#endif
    } else {
        input = (uint16_t*)aligned_alloc64(pixel_bytes);
        if (!input) { std::printf("alloc buffers failed\n"); return 1; }
#ifdef BENCH_SLOT_API
        std::vector<uint16_t> colours;
        if (pattern == Pattern::Runs)
            fill_input_slots_runs(input, width, height, args.colours, palette, colours);
        else
            fill_input_slots(input, width, height, pattern, args.colours, palette, colours);
        snes_ntsc_init(ntsc, &setup, colours.data());
#else
        if (pattern == Pattern::Runs)
            fill_input_runs(input, width, height, args.colours, palette);
        else
            fill_input(input, width, height, pattern, args.colours, palette);
        snes_ntsc_init(ntsc, &setup);
#endif
    }

#if defined(__linux__)
    if (args.hugepages) {
        long kb = read_anon_hugepages_kb();
        if (kb >= 0) std::printf("hugepages: AnonHugePages=%ld kB (after init)\n", kb);
        else         std::printf("hugepages: AnonHugePages unavailable\n");
    }
#endif

    // Regenerates whichever slots differ between in_frames[idx].table and the ntsc
    // object's current contents (tracked in current_table), applying only the diff via
    // snes_ntsc_update_slots(). No-op (returns 0) outside --input. See the call sites
    // in the timed and checksum loops below.
#ifdef BENCH_SLOT_API
    std::vector<int> upd_slots;
    std::vector<unsigned short> upd_colours;
    if (using_input) {
        upd_slots.reserve((size_t)snes_ntsc_slot_count);
        upd_colours.reserve((size_t)snes_ntsc_slot_count);
    }
    auto sync_to_frame = [&](size_t idx) -> int {
        if (!using_input) return 0;
        upd_slots.clear();
        upd_colours.clear();
        const std::vector<uint16_t>& tbl = in_frames[idx].table;
        for (int s = 0; s < (int)snes_ntsc_slot_count; ++s) {
            if (tbl[(size_t)s] != current_table[(size_t)s]) {
                upd_slots.push_back(s);
                upd_colours.push_back(tbl[(size_t)s]);
                current_table[(size_t)s] = tbl[(size_t)s];
            }
        }
        if (!upd_slots.empty())
            snes_ntsc_update_slots(ntsc, upd_slots.data(), (int)upd_slots.size(), upd_colours.data());
        return (int)upd_slots.size();
    };
#endif

    uint32_t* out = (uint32_t*)aligned_alloc64((size_t)out_width * height * sizeof(uint32_t));
    if (!out) { std::printf("alloc buffers failed\n"); return 1; }
    std::memset(out, 0, (size_t)out_width * height * sizeof(uint32_t));

    // Runs one full frame of `in_buf` at the given base burst phase, split
    // into `threads` horizontal bands. Each band's starting phase is offset
    // by its absolute row count so the result is bit-identical to a single
    // un-split call (the blitter advances burst_phase by one, mod 3, per row
    // it processes).
    auto run_frame = [&](const uint16_t* in_buf, int phase) {
        int base = height / threads, extra = height % threads;
        int rows0 = base + (0 < extra ? 1 : 0);
        std::vector<std::thread> pool;
        int row = rows0;
        for (int t = 1; t < threads; ++t) {
            int rows = base + (t < extra ? 1 : 0);
            const uint16_t* in_band = in_buf + (size_t)row * width;
            uint32_t* out_band      = out + (size_t)row * out_width;
            int band_phase = (phase + row) % 3;
            pool.emplace_back([=]() {
                blit_fn(ntsc, in_band, width, band_phase, width, rows, out_band, out_pitch, Alevel);
            });
            row += rows;
        }
        blit_fn(ntsc, in_buf, width, phase % 3, width, rows0, out, out_pitch, Alevel); // band 0
        for (auto& th : pool) th.join();
    };

    // Selects the input buffer and burst phase for iteration `i`. Default and
    // --lores: the single synthetic buffer, phase cycling 0,1,2 (unchanged
    // from before). --input: frame (i % N) in loaded order, at *that frame's
    // own recorded* phase - mirrors what the game hands the blitter frame to
    // frame, rather than a fixed 3-phase cycle.
    auto pick = [&](long i) -> std::tuple<const uint16_t*, int, size_t> {
        if (using_input) {
            size_t idx = (size_t)(i % (long)in_frames.size());
            return { input_aligned[idx], (int)in_frames[idx].phase, idx };
        }
        return { input, (int)(i % 3), (size_t)0 };
    };

    // Warm-up. Deliberately does not call sync_to_frame(): its output is discarded and
    // not timed, so a --input run may render a handful of warm-up frames against a
    // slightly stale table (harmless) rather than spend untimed work keeping it current.
    // The timed and checksum loops below each sync before their own use of a frame, so
    // correctness never depends on what warm-up leaves behind.
    for (long i = 0; i < 10; ++i) {
        auto [buf, ph, idx] = pick(i);
        (void)idx;
        run_frame(buf, ph);
    }

    // Timed loop. Each iteration's wall time (t0..t1, "ms" below) covers both the
    // per-frame slot-table sync (sync_to_frame(), which calls snes_ntsc_update_slots() in
    // bulk for whatever slots differ - never per-slot) and the blit itself; update_ms is
    // sync_to_frame()'s own share of that, timed separately with steady_clock so
    // filter-only time can be recovered as ms - update_ms. 0.00/0 outside --input +
    // BENCH_SLOT_API, where sync_to_frame() is a no-op.
    using clk = std::chrono::steady_clock;
    double min_ms = 1e18, max_ms = 0.0, total_ms = 0.0, total_update_ms = 0.0;
    long frames = 0;
    long slot_updates = 0; // --input + BENCH_SLOT_API only; 0 otherwise
    auto run_start = clk::now();
    while (std::chrono::duration<double>(clk::now() - run_start).count() < args.seconds) {
        auto [buf, ph, idx] = pick(frames);
        auto t0 = clk::now();
#ifdef BENCH_SLOT_API
        auto tu0 = clk::now();
        slot_updates += sync_to_frame(idx);
        auto tu1 = clk::now();
        total_update_ms += std::chrono::duration<double, std::milli>(tu1 - tu0).count();
#else
        (void)idx;
#endif
        run_frame(buf, ph);
        auto t1 = clk::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        total_ms += ms;
        ++frames;
    }
    double mean_ms = (frames > 0) ? total_ms / (double)frames : 0.0;
    double fps = (mean_ms > 0.0) ? 1000.0 / mean_ms : 0.0;
    double update_ms = (frames > 0) ? total_update_ms / (double)frames : 0.0;

    // Correctness checksum.
    // Default / --lores: 3 more frames at phases 0,1,2, FNV-1a accumulated
    // cumulatively across all three output buffers - unchanged from before.
    // --input: every loaded frame at phases 0, 1 and 2 (frames outer, phases
    // inner), one FNV-1a accumulated over all of it - the cross-build
    // bit-exactness gate for the whole captured-frame corpus.
    uint64_t checksum = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    size_t out_bytes = (size_t)out_width * height * sizeof(uint32_t);

    // ---- --dump-output: raw pixel dump of the checksum pass -------------
    // Same frames/phases as the checksum loop below, in the same order -
    // this is a pure addition (write-only observer of `out`), so it cannot
    // change `checksum` or any other RESULT field. See the header comment
    // above for the file format (32-byte header + tightly packed frames).
    FILE* dump_f = nullptr;
    if (!args.dump_output.empty()) {
        dump_f = std::fopen(args.dump_output.c_str(), "wb");
        if (!dump_f) {
            std::printf("error: --dump-output '%s': cannot open for writing\n", args.dump_output.c_str());
            return 1;
        }
        uint32_t hdr[8];
        std::memcpy(&hdr[0], "CBSEOUT1", 8); // occupies hdr[0] and hdr[1]
        hdr[2] = (uint32_t)out_width;
        hdr[3] = (uint32_t)height;
        hdr[4] = using_input ? (uint32_t)in_frames.size() : 1u;
        hdr[5] = 3u; // phases_per_frame
        hdr[6] = 0u; // reserved
        hdr[7] = 0u; // reserved
        std::fwrite(hdr, sizeof(uint32_t), 8, dump_f);
    }

    if (using_input) {
        for (size_t i = 0; i < in_frames.size(); ++i) {
            // Untimed: bring the ntsc object's slots to exactly this frame's table before
            // blitting it at all three phases, the same way the timed loop does per frame
            // (but not counted towards the printed slot_updates, which is a timed-loop
            // metric only) - see sync_to_frame() above. No-op under the old API.
#ifdef BENCH_SLOT_API
            sync_to_frame(i);
#endif
            for (int phase = 0; phase < 3; ++phase) {
                run_frame(input_aligned[i], phase);
                checksum = fnv1a(out, out_bytes, checksum);
                if (dump_f) std::fwrite(out, 1, out_bytes, dump_f);
            }
        }
    } else {
        for (int phase = 0; phase < 3; ++phase) {
            run_frame(input, phase);
            checksum = fnv1a(out, out_bytes, checksum);
            if (dump_f) std::fwrite(out, 1, out_bytes, dump_f);
        }
    }

    if (dump_f) {
        std::fclose(dump_f);
        std::printf("dump-output: wrote %s\n", args.dump_output.c_str());
    }

    std::printf("RESULT fn=%s rev=%s simd=%s w=%d h=%d pattern=%s colours=%d threads=%d "
                "frames=%ld ms=%.2f fps=%.1f min=%.1f max=%.1f checksum=0x%016llx "
                "palette=%s hugepages=%d inputs=%d lores=%d slot_updates=%ld update_ms=%.2f\n",
                fn_name, STR(BENCH_REV), simd, width, height,
                using_input ? "input" : args.pattern.c_str(),
                using_input ? 0 : args.colours, threads, frames, mean_ms, fps, min_ms, max_ms,
                (unsigned long long)checksum,
                using_input ? "n/a" : args.palette.c_str(),
                args.hugepages ? 1 : 0,
                using_input ? (int)in_frames.size() : 0,
                args.lores ? 1 : 0,
                slot_updates,
                update_ms);

    if (input) aligned_free64(input);
    for (auto* p : input_aligned) aligned_free64(p);
    aligned_free64(out);
    std::free(ntsc);
    return 0;
}
#endif
