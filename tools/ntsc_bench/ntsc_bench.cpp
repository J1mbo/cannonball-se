/*****************************************************************************
 * ntsc_bench.cpp - minimal throughput/correctness harness for the Blargg
 *                  SNES NTSC hires blitters. See DESIGN.md in this directory.
 *
 * One binary tests exactly one blitter, selected at compile time so the same
 * source builds unmodified against any revision of snes_ntsc.{cpp,h}:
 *
 *   g++ -O3 -mcpu=cortex-a53 -std=c++17 -DBENCH_FN=snes_ntsc_blit_hires_fast2 \
 *       -DBENCH_REV=WT ntsc_bench.cpp snes_ntsc.cpp -o bench_fast2
 *
 * -DBENCH_FN=<function>   function under test (default snes_ntsc_blit_hires_fast)
 * -DBENCH_SCALAR          building against the scalar snes_ntsc_blit_hires
 *                         reference; also switches the output width formula
 *                         to match the non-SIMD game build.
 * -DBENCH_REV=<token>     free-form revision tag, printed in the RESULT line.
 *
 * Links only against snes_ntsc.cpp: no SDL, no CMake, no game headers.
 *
 * Runtime flags (see also README.md):
 *   --pattern random|mixed       pixel pattern used to fill the input frame
 *                                 (default random).
 *   --colours N / --colors N     size of the fixed-seed palette, 1-65536
 *                                 (default 256).
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
 *                                 in both modes.
 *   --width W / --height H       input frame size (default 640x448).
 *   --seconds S                  length of the timed loop (default 5).
 *   --threads T                  horizontal bands run concurrently (default 1).
 *   --hugepages                  allocate the 32 MiB ntsc table 2 MiB-aligned
 *                                 and madvise(MADV_HUGEPAGE) it before init
 *                                 (default off). Linux only; prints
 *                                 "unsupported" and continues elsewhere.
 *****************************************************************************/
#include "snes_ntsc.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#ifndef BENCH_FN
#define BENCH_FN snes_ntsc_blit_hires_fast
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

enum class Pattern { Random, Mixed };

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

// 64-bit FNV-1a, run cumulatively across the three checksum frames.
static uint64_t fnv1a(const void* data, size_t len, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
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
    }
    if (a.colours < 1) a.colours = 1;
    if (a.colours > 65536) a.colours = 65536;
    if (a.threads < 1) a.threads = 1;
    if (a.palette != "random" && a.palette != "contiguous") a.palette = "random";
    return a;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    const int width  = args.width;
    const int height = args.height;
    const int threads = args.threads;
    Pattern pattern = (args.pattern == "mixed") ? Pattern::Mixed : Pattern::Random;
    Palette palette = (args.palette == "contiguous") ? Palette::Contiguous : Palette::Random;

#ifdef BENCH_SCALAR
    const int out_width = SNES_NTSC_OUT_WIDTH(width >> 1); // matches non-SIMD game build
#else
    const int out_width = SNES_NTSC_OUT_WIDTH_SIMD(width);
#endif
    const long out_pitch = (long)out_width * 4;
    const long Alevel = 255;

    const char* simd =
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

    std::printf("ntsc_bench fn=%s rev=%s simd=%s\n", STR(BENCH_FN), STR(BENCH_REV), simd);
    std::printf("geometry: in %dx%d -> out_width=%d out_pitch=%ld  pattern=%s colours=%d threads=%d\n",
                width, height, out_width, out_pitch, args.pattern.c_str(), args.colours, threads);

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
    snes_ntsc_init(ntsc, &setup);

#if defined(__linux__)
    if (args.hugepages) {
        long kb = read_anon_hugepages_kb();
        if (kb >= 0) std::printf("hugepages: AnonHugePages=%ld kB (after init)\n", kb);
        else         std::printf("hugepages: AnonHugePages unavailable\n");
    }
#endif

    uint16_t* input = (uint16_t*)aligned_alloc64((size_t)width * height * sizeof(uint16_t));
    uint32_t* out   = (uint32_t*)aligned_alloc64((size_t)out_width * height * sizeof(uint32_t));
    if (!input || !out) { std::printf("alloc buffers failed\n"); return 1; }

    fill_input(input, width, height, pattern, args.colours, palette);
    std::memset(out, 0, (size_t)out_width * height * sizeof(uint32_t));

    // Runs one full frame at the given base burst phase, split into `threads`
    // horizontal bands. Each band's starting phase is offset by its absolute
    // row count so the result is bit-identical to a single un-split call
    // (the blitter advances burst_phase by one, mod 3, per row it processes).
    auto run_frame = [&](int phase) {
        int base = height / threads, extra = height % threads;
        int rows0 = base + (0 < extra ? 1 : 0);
        std::vector<std::thread> pool;
        int row = rows0;
        for (int t = 1; t < threads; ++t) {
            int rows = base + (t < extra ? 1 : 0);
            const uint16_t* in_band = input + (size_t)row * width;
            uint32_t* out_band      = out + (size_t)row * out_width;
            int band_phase = (phase + row) % 3;
            pool.emplace_back([=]() {
                BENCH_FN(ntsc, in_band, width, band_phase, width, rows, out_band, out_pitch, Alevel);
            });
            row += rows;
        }
        BENCH_FN(ntsc, input, width, phase % 3, width, rows0, out, out_pitch, Alevel); // band 0
        for (auto& th : pool) th.join();
    };

    // Warm-up
    for (int i = 0; i < 10; ++i) run_frame(i % 3);

    // Timed loop
    using clk = std::chrono::steady_clock;
    double min_ms = 1e18, max_ms = 0.0, total_ms = 0.0;
    long frames = 0;
    auto run_start = clk::now();
    while (std::chrono::duration<double>(clk::now() - run_start).count() < args.seconds) {
        auto t0 = clk::now();
        run_frame((int)(frames % 3));
        auto t1 = clk::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        total_ms += ms;
        ++frames;
    }
    double mean_ms = (frames > 0) ? total_ms / (double)frames : 0.0;
    double fps = (mean_ms > 0.0) ? 1000.0 / mean_ms : 0.0;

    // Correctness checksum: 3 more frames at phases 0,1,2, FNV-1a accumulated
    // cumulatively across all three output buffers.
    uint64_t checksum = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    size_t out_bytes = (size_t)out_width * height * sizeof(uint32_t);
    for (int phase = 0; phase < 3; ++phase) {
        run_frame(phase);
        checksum = fnv1a(out, out_bytes, checksum);
    }

    std::printf("RESULT fn=%s rev=%s simd=%s w=%d h=%d pattern=%s colours=%d threads=%d "
                "frames=%ld ms=%.2f fps=%.1f min=%.1f max=%.1f checksum=0x%016llx "
                "palette=%s hugepages=%d\n",
                STR(BENCH_FN), STR(BENCH_REV), simd, width, height, args.pattern.c_str(),
                args.colours, threads, frames, mean_ms, fps, min_ms, max_ms,
                (unsigned long long)checksum, args.palette.c_str(), args.hugepages ? 1 : 0);

    aligned_free64(input);
    aligned_free64(out);
    std::free(ntsc);
    return 0;
}
#endif
