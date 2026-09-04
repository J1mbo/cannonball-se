/***************************************************************************
    road_tiles_test - standalone test driver for the road/tile renderers.

    IMPLEMENTATION_SPEC.md S0.6. Compiled ONLY by
    tools/road_tiles/test/CMakeLists.txt (CB_RENDER_TEST=1, CB_RENDER_CHECKS=1),
    against the REAL src/main/hwvideo/hwroad.cpp and hwtiles.cpp and the stub
    frontend/config.hpp in tools/road_tiles/test/stubs (placed first on the
    include path, so it shadows the real config.hpp and no SDL2/TinyXML2
    dependency is needed). Never linked into the shipping game.

    Sub-commands:
      selftest  (this S0 drop) - proves every _ref reference twin added by S0.3
                is a faithful, behaviour-preserving copy of the currently-
                shipping function it mirrors: for many random RAM/ROM fills and
                every screen mode, render with the shipping function into one
                identically-prefilled pixel buffer and with the _ref twin into
                another, then memcmp the *entire* allocation (front slack, the
                visible rows and the two spare rows), exactly as S0.6 step 4
                specifies for `replay`. Since no optimised kernel exists yet in
                this drop, the twins ARE what render_tile_layer/render_text_layer
                and hwroad's four render_*() functions call, so this is also a
                simple correctness smoke test of the harness plumbing itself.

      unit, replay, bench, golden - not implemented in this (S0) drop; each
                prints a message and exits 0 so the sub-command dispatch and
                the two build targets are already exercised end to end. They
                land with S1-S7 (S0.6/S0.7):
                  unit   - per-section property tests (e.g. S1.4's 3.1M-case
                           scan-equivalence sweep).
                  replay - captured-RAM replay comparing the shipping pipeline
                           to the _ref pipeline across real gameplay frames.
                  golden - prints per-frame FNV-1a hashes of the ref render for
                           checking into tools/road_tiles/test/golden/.
                  bench  - S0.8 isolated per-function ns/row timing.
                Also deferred, per the task that produced this file: the
                -dumpram/CB_RAM_DUMP capture (S0.5) and RENDER_CHECKS game-side
                assertions beyond defining the macro (S3) -- both are additions
                to main.cpp/video.cpp that later sections make, not this driver.

    ROM loading (--roms <dir>):
      Loads the real road ROM (opr-10185.11, 32 KB; decode_road only reads the
      first 0x8000 bytes, hwroad.cpp) and the six tile ROMs concatenated in
      roms.cpp's order into a 0x30000-byte image (opr-10268.99 @0,
      opr-10232.102 @0x8000, opr-10267.100 @0x10000, opr-10231.103 @0x18000,
      opr-10266.101 @0x20000, opr-10230.104 @0x28000), exactly as S0.6 specifies.

      When --roms is omitted (or a file is missing), a deterministically-seeded
      random image of the same size is used instead: decode_road() and the
      tile-plane bit-interleave in hwtiles::init() are pure, lossless
      reformatting of whatever ROM bytes they are given (there is no
      "invalid" ROM content from the code's point of view), so a random image
      exercises exactly the same code paths as a real one for this section's
      purpose -- proving the _ref twins match the shipping renderers on random
      RAM. This also lets `selftest` run with no ROM set at hand. See the task
      note in IMPLEMENTATION_SPEC.md S0.6 ("If exact concatenation is fiddly,
      the selftest can instead synthesise random ROM contents...").
***************************************************************************/

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <new>
#include <chrono>
#include <cmath>

#include "hwvideo/hwroad.hpp"
#include "hwvideo/hwtiles.hpp"
#include "hwvideo/hwarch.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

// The stub Config (tools/road_tiles/test/stubs/frontend/config.hpp) declares
// this extern; the real game defines it in frontend/config.cpp, which this
// driver never links.
Config config;

// ---------------------------------------------------------------------------
// S0.4: the friend struct. HWRoad and hwtiles each declare
// "friend struct RoadTilesTestAccess;" under CB_RENDER_TEST. Most state this
// driver needs (road_control, ramBuff, x_clamp, tile_banks, tile_ram,
// text_ram) is already reachable through public API (write16/write_road_control/
// the public render_background/render_foreground function pointers,
// set_x_clamp, the public tile/text RAM arrays) or through the unconditional
// S0.4 accessors, so this struct is kept to exactly what has no public path:
// the four HWRoad _ref twins and the two hwtiles _ref layer twins (all
// private, by design -- nothing outside the class should call them except a
// test that is proving they still agree with the shipping code).
// ---------------------------------------------------------------------------
struct RoadTilesTestAccess
{
    static void bg_lores_ref(HWRoad& r, uint16_t* p) { r.render_background_lores_ref(p); }
    static void fg_lores_ref(HWRoad& r, uint16_t* p) { r.render_foreground_lores_ref(p); }
    static void bg_hires_ref(HWRoad& r, uint16_t* p) { r.render_background_hires_ref(p); }
    static void fg_hires_ref(HWRoad& r, uint16_t* p) { r.render_foreground_hires_ref(p); }

    static void tile_layer_ref(hwtiles& t, uint16_t* buf, uint8_t page_index, uint8_t priority_draw)
    { t.render_tile_layer_ref(buf, page_index, priority_draw); }
    static void text_layer_ref(hwtiles& t, uint16_t* buf, uint8_t priority_draw)
    { t.render_text_layer_ref(buf, priority_draw); }

    // S6: diagnostic peek at the decoded road bytes, used only to report
    // texel-value coverage (e.g. confirming value 7 shows up) for the s6d
    // synthetic ROM patterns; not used by any pass/fail comparison.
    static const uint8_t* roads(HWRoad& r) { return r.roads; }
};

// ---------------------------------------------------------------------------
// Pixel buffer, allocated exactly as Video::init() does (video.cpp:66-72):
// size = (width * (height+2) + alignment) halfwords, 64-byte aligned, pixels
// points 64 halfwords (128 bytes, still 64-byte aligned) into the allocation.
// S0.6 step 4 requires comparing the *entire* allocation, front slack and
// spare rows included, so callers memcmp base..base+halfwords, not just the
// visible pixels.
// ---------------------------------------------------------------------------
struct PixelBuffer
{
    static constexpr int ALIGNMENT = 64; // halfwords, matching video.cpp's `alignment`

    uint16_t*   base = nullptr;
    uint16_t*   pixels = nullptr;
    std::size_t halfwords = 0;

    void allocate(uint16_t width, uint16_t height)
    {
        release();
        halfwords = (std::size_t)width * (std::size_t)(height + 2) + ALIGNMENT;
        base = static_cast<uint16_t*>(::operator new(halfwords * sizeof(uint16_t), std::align_val_t(64)));
        pixels = base + ALIGNMENT;
    }
    void release()
    {
        if (base) { ::operator delete(base, std::align_val_t(64)); base = nullptr; pixels = nullptr; }
        halfwords = 0;
    }
    ~PixelBuffer() { release(); }
};

static void fill_random(uint16_t* buf, std::size_t n, std::mt19937& rng)
{
    for (std::size_t i = 0; i < n; i++)
        buf[i] = (uint16_t)rng();
}

// Prints the first differing halfword (decoded to a (row,x) when it falls
// inside the visible width*height region, else labelled as front slack or one
// of the two spare rows) plus both values, mirroring what S0.6's `replay`
// asks for on a mismatch.
static void report_mismatch(const char* what, const char* mode, int variant, int trial,
                             const uint16_t* a, const uint16_t* b, std::size_t n,
                             uint16_t width, uint16_t height)
{
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) i++;
    std::printf("  MISMATCH %-16s mode=%-13s variant=%d trial=%d halfword_index=%zu",
                what, mode, variant, trial, i);
    std::size_t pix_off = (i >= (std::size_t)PixelBuffer::ALIGNMENT) ? i - PixelBuffer::ALIGNMENT : 0;
    if (i < (std::size_t)PixelBuffer::ALIGNMENT) {
        std::printf(" (front slack)");
    } else if (pix_off < (std::size_t)width * height) {
        std::printf(" (row=%zu, x=%zu)", pix_off / width, pix_off % width);
    } else {
        std::printf(" (spare rows)");
    }
    std::printf(" shipping=0x%04x ref=0x%04x\n", a[i], b[i]);
}

struct Mode { bool hires; bool wide; const char* name; };
static const Mode kModes[] = {
    { false, false, "lores-narrow" },
    { false, true,  "lores-wide"   },
    { true,  false, "hires-narrow" },
    { true,  true,  "hires-wide"   },
};

static void set_mode(const Mode& m)
{
    // Mirrors Video::set_video_mode (video.cpp:119-139), renderer bits omitted.
    if (m.wide) { config.s16_width = S16_WIDTH_WIDE; config.s16_x_off = (uint16_t)((S16_WIDTH_WIDE - S16_WIDTH) / 2); }
    else        { config.s16_width = S16_WIDTH;      config.s16_x_off = 0; }
    config.s16_height = S16_HEIGHT;
    if (m.hires) { config.s16_width = (uint16_t)(config.s16_width << 1); config.s16_height = (uint16_t)(config.s16_height << 1); }
}

// ---------------------------------------------------------------------------
// ROM loading
// ---------------------------------------------------------------------------
static bool read_exact(const std::string& path, uint8_t* dst, std::size_t n)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(dst), (std::streamsize)n);
    return f.good() || (std::size_t)f.gcount() == n;
}

// Road ROM: 32 KB (opr-10185.11). decode_road() only reads the first 0x8000
// bytes for both roads (hwroad.cpp), so that is all this driver loads.
static const std::size_t ROAD_ROM_SIZE = 0x8000;

// Tile ROM image: six 0x8000-byte files concatenated into 0x30000 bytes, in
// the exact order and at the exact offsets roms.cpp loads them at
// (roms.cpp:52-57), because hwtiles::init() slices the buffer into three
// 0x10000 planes (p0/p1/p2) at those same offsets.
static const std::size_t TILE_ROM_SIZE = 0x30000;
struct TileRomFile { const char* name; std::size_t offset; };
static const TileRomFile kTileRomFiles[6] = {
    { "opr-10268.99",  0x00000 },
    { "opr-10232.102", 0x08000 },
    { "opr-10267.100", 0x10000 },
    { "opr-10231.103", 0x18000 },
    { "opr-10266.101", 0x20000 },
    { "opr-10230.104", 0x28000 },
};

// Returns true iff both ROM images were loaded from real files.
static bool load_roms(const std::string& roms_dir, std::vector<uint8_t>& road_rom,
                       std::vector<uint8_t>& tile_rom, unsigned seed)
{
    road_rom.resize(ROAD_ROM_SIZE);
    tile_rom.resize(TILE_ROM_SIZE);

    bool ok = !roms_dir.empty();
    if (ok) {
        ok = read_exact(roms_dir + "/opr-10185.11", road_rom.data(), ROAD_ROM_SIZE);
        for (const TileRomFile& f : kTileRomFiles) {
            if (!ok) break;
            ok = read_exact(roms_dir + "/" + f.name, tile_rom.data() + f.offset, 0x8000);
        }
    }

    if (!ok) {
        // Synthetic fallback (documented above and in the task report): a
        // deterministic PRNG fill of the same size, so init()'s decode/format
        // conversion has real, reproducible bytes to work with.
        std::mt19937 rng(seed ^ 0x524f4d21u); // "ROM!" - distinct stream from the RAM fills
        fill_random(reinterpret_cast<uint16_t*>(road_rom.data()), road_rom.size() / 2, rng);
        fill_random(reinterpret_cast<uint16_t*>(tile_rom.data()), tile_rom.size() / 2, rng);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// selftest: road (background/foreground, both resolutions, all 8 control values)
// ---------------------------------------------------------------------------
static int road_selftest(HWRoad& road, const Mode& m, std::mt19937& rng)
{
    int fails = 0;
    PixelBuffer A, B;
    A.allocate(config.s16_width, config.s16_height);
    B.allocate(config.s16_width, config.s16_height);
    const std::size_t n = A.halfwords;

    for (int control = 0; control < 8; control++) {
        for (int trial = 0; trial < 3; trial++) {
            // Randomise road RAM through the same public path the game uses:
            // writes go to `ram` (write16), and read_road_control() performs
            // the hardware buffer swap into `ramBuff` (hwroad.cpp), which is
            // what every render_* function actually reads.
            for (uint32_t i = 0; i < 0x800; i++)
                road.write16(i * 2, (uint16_t)rng());
            road.read_road_control();
            road.write_road_control((uint8_t)control);

            fill_random(A.base, n, rng);
            std::memcpy(B.base, A.base, n * sizeof(uint16_t));
            (road.*road.render_background)(A.pixels); // shipping dispatch, as Video::prepare_frame uses it
            if (m.hires) RoadTilesTestAccess::bg_hires_ref(road, B.pixels);
            else         RoadTilesTestAccess::bg_lores_ref(road, B.pixels);
            if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
                report_mismatch("road background", m.name, control, trial, A.base, B.base, n, config.s16_width, config.s16_height);
                fails++;
            }

            fill_random(A.base, n, rng);
            std::memcpy(B.base, A.base, n * sizeof(uint16_t));
            (road.*road.render_foreground)(A.pixels);
            if (m.hires) RoadTilesTestAccess::fg_hires_ref(road, B.pixels);
            else         RoadTilesTestAccess::fg_lores_ref(road, B.pixels);
            if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
                report_mismatch("road foreground", m.name, control, trial, A.base, B.base, n, config.s16_width, config.s16_height);
                fails++;
            }
        }
    }
    return fails;
}

// ---------------------------------------------------------------------------
// selftest: tiles (background/foreground tile layer, text layer)
// ---------------------------------------------------------------------------
static int tile_selftest(hwtiles& tiles, const Mode& m, std::mt19937& rng)
{
    int fails = 0;
    PixelBuffer A, B;
    A.allocate(config.s16_width, config.s16_height);
    B.allocate(config.s16_width, config.s16_height);
    const std::size_t n = A.halfwords;

    for (int trial = 0; trial < 6; trial++) {
        for (std::size_t i = 0; i < sizeof(tiles.text_ram); i++) tiles.text_ram[i] = (uint8_t)rng();
        for (std::size_t i = 0; i < sizeof(tiles.tile_ram); i++) tiles.tile_ram[i] = (uint8_t)rng();
        tiles.update_tile_values();

        for (uint8_t page_index = 0; page_index < 2; page_index++) {
            for (uint8_t pri = 0; pri < 2; pri++) {
                fill_random(A.base, n, rng);
                std::memcpy(B.base, A.base, n * sizeof(uint16_t));
                tiles.render_tile_layer(A.pixels, page_index, pri);
                RoadTilesTestAccess::tile_layer_ref(tiles, B.pixels, page_index, pri);
                if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
                    report_mismatch("tile_layer", m.name, page_index * 10 + pri, trial, A.base, B.base, n, config.s16_width, config.s16_height);
                    fails++;
                }
            }
        }

        for (uint8_t pri = 0; pri < 2; pri++) {
            fill_random(A.base, n, rng);
            std::memcpy(B.base, A.base, n * sizeof(uint16_t));
            tiles.render_text_layer(A.pixels, pri);
            RoadTilesTestAccess::text_layer_ref(tiles, B.pixels, pri);
            if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
                report_mismatch("text_layer", m.name, pri, trial, A.base, B.base, n, config.s16_width, config.s16_height);
                fails++;
            }
        }
    }
    return fails;
}

static bool run_selftest(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    bool used_real_roms = load_roms(roms_dir, road_rom, tile_rom, seed);

    std::printf("road_tiles_test selftest: arch=%s roms=%s seed=%u\n",
                CB_ARCH_NAME, used_real_roms ? "real (--roms)" : "synthetic (deterministic random)", seed);

    int failures = 0;
    std::mt19937 rng(seed);

    for (const Mode& m : kModes) {
        set_mode(m);

        HWRoad road;
        hwtiles tiles;
        road.init(road_rom.data(), m.hires);
        tiles.init(tile_rom.data(), m.hires);
        tiles.set_x_clamp(hwtiles::CENTRE);

        int road_fails = road_selftest(road, m, rng);
        int tile_fails = tile_selftest(tiles, m, rng);
        failures += road_fails + tile_fails;

        std::printf("  mode=%-13s width=%4u height=%4u x_off=%3u  road:%s  tiles:%s\n",
                    m.name, config.s16_width, config.s16_height, config.s16_x_off,
                    road_fails ? "FAIL" : "ok", tile_fails ? "FAIL" : "ok");
    }

    if (failures == 0) {
        std::printf("selftest PASS: every _ref twin matches its shipping function (arch=%s)\n", CB_ARCH_NAME);
        return true;
    }
    std::printf("selftest FAIL: %d mismatch(es)\n", failures);
    return false;
}

// ---------------------------------------------------------------------------
// unit s6: render_foreground_hires (S6.2's run-length kernel) vs
// render_foreground_hires_ref, hires modes only (S6 does not touch lo-res).
// IMPLEMENTATION_SPEC.md S6.4. All comparisons memcmp the *whole* pixel
// allocation (front slack + visible rows + spare rows) of two buffers seeded
// from the same random content, exactly as S0.6/road_selftest does.
// ---------------------------------------------------------------------------

// s6a's line-select sweep uses one extra value beyond the 256 real ROM lines:
// the "solid" state (data word bit 0x800 set), which forces that road's
// source to decode_road's dummy all-3 line regardless of the line-select
// bits. set_row0/set_row1 below translate it into the 0x800 bit.
static const int SOLID_L = 256;
static const int L_COUNT = 257;   // 0..255 real lines + SOLID_L
static const int H_COUNT = 4096;  // full 12-bit h-scroll range

// Direct-mode (road_control bit 0x4) per-row setters. In direct mode the
// h-scroll/colour-table index is forced to yy (road 0) / 0x100+yy (road 1),
// so writing roadram[0x200+yy]/[0x400+0x100+yy] controls that row's h-scroll
// in isolation; the line-select word roadram[0x000+yy]/[0x100+yy] is read
// unconditionally by both direct and indirect mode (hwroad.cpp), so it always
// controls src0/src1 regardless of road_control&4.
static void set_row0(HWRoad& road, int yy, int L, uint16_t h)
{
    uint16_t data0 = (L == SOLID_L) ? (uint16_t)0x0800 : (uint16_t)((L & 0xff) << 1);
    road.write16((uint32_t)(0x000 + yy) * 2, data0);
    road.write16((uint32_t)(0x200 + yy) * 2, h);
}
static void set_row1(HWRoad& road, int yy, int L, uint16_t h)
{
    uint16_t data1 = (L == SOLID_L) ? (uint16_t)0x0800 : (uint16_t)((L & 0xff) << 1);
    road.write16((uint32_t)(0x100 + yy) * 2, data1);
    road.write16((uint32_t)(0x400 + 0x100 + yy) * 2, h);
}

// Shared per-mode test state: the road device, a "clean" randomised template
// pixel buffer and two working buffers rendered into and compared each call.
// Keeping one allocation per width/height avoids re-allocating (or re-running
// mt19937 across the whole buffer) on every one of the tens of thousands of
// calls s6b/s6a make; restore() reseeds both working buffers from the same
// template so every call starts from identical, if not freshly-random, state
// (the template itself is refreshed periodically by the callers below).
struct S6Ctx
{
    HWRoad road;
    PixelBuffer A, B, C;
    std::size_t n = 0;
    uint16_t width = 0, height = 0;

    void allocate(uint16_t w, uint16_t h)
    {
        width = w; height = h;
        A.allocate(w, h); B.allocate(w, h); C.allocate(w, h);
        n = A.halfwords;
    }
    void restore()
    {
        std::memcpy(A.base, C.base, n * sizeof(uint16_t));
        std::memcpy(B.base, C.base, n * sizeof(uint16_t));
    }
};

// Renders shipping vs _ref into ctx.A/ctx.B (already restored from ctx.C by
// the caller) and memcmp's the whole allocation; reports and counts a
// mismatch exactly as road_selftest's report_mismatch does.
static bool s6_render_compare(S6Ctx& ctx, const char* what, const std::string& mode, int variant, long long trial)
{
    (ctx.road.*ctx.road.render_foreground)(ctx.A.pixels);
    RoadTilesTestAccess::fg_hires_ref(ctx.road, ctx.B.pixels);
    if (std::memcmp(ctx.A.base, ctx.B.base, ctx.n * sizeof(uint16_t)) != 0) {
        report_mismatch(what, mode.c_str(), variant, (int)trial, ctx.A.base, ctx.B.base, ctx.n, ctx.width, ctx.height);
        return false;
    }
    return true;
}

// s6a: exhaustive single-road direct-mode sweep (S6.4). road_control = 4 |
// mode puts each of the 224 hi-res rows (yy = 0..S16_HEIGHT-1) on its own
// h-scroll/colour-table index, so one call exercises 224 independent
// (line, hscroll) pairs of the swept road; the other road is held at a
// random (line, hscroll) for the whole call.
static int s6a_run(S6Ctx& ctx, bool sweep_road0, unsigned mode, std::mt19937& rng, unsigned refresh_every)
{
    int fails = 0;
    HWRoad& road = ctx.road;

    // Fresh random colour tables / RAM slack for this (road, mode) sweep;
    // set_row0/set_row1 below only ever touch the line-select and h-scroll
    // words, so everything else (colour tables, the unused indirect-index
    // bits) keeps whatever this fill gave it for the whole sweep.
    for (uint32_t i = 0; i < 0x800; i++) road.write16(i * 2, (uint16_t)rng());

    const long long total = (long long)L_COUNT * H_COUNT;
    long long combo = 0;
    long long call = 0;
    std::string modestr = std::string("s6a-") + (sweep_road0 ? "road0" : "road1") + "-m" + std::to_string(mode);

    while (combo < total) {
        int Lo = (int)(rng() % 256);
        uint16_t ho = (uint16_t)(rng() % 4096);

        for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
            long long c = combo + yy;
            if (c >= total) c = total - 1; // tail rows of the last call: reuse an already-tested combo
            int Ls = (int)(c / H_COUNT);
            uint16_t hs = (uint16_t)(c % H_COUNT);
            if (sweep_road0) { set_row0(road, yy, Ls, hs); set_row1(road, yy, Lo, ho); }
            else             { set_row1(road, yy, Ls, hs); set_row0(road, yy, Lo, ho); }
        }
        road.read_road_control();
        road.write_road_control((uint8_t)(4 | mode));

        if ((call % refresh_every) == 0) fill_random(ctx.C.base, ctx.n, rng);
        ctx.restore();
        if (!s6_render_compare(ctx, "s6a", modestr, (int)mode, call)) fails++;

        combo += S16_HEIGHT;
        call++;
    }
    return fails;
}

// s6b: indirect-mode random (S6.4). road_control = mode (no direct bit), the
// entire 0x800-halfword road RAM randomised per call so the line-select,
// solid/centre bits and indirect 0x1ff index all vary together.
static int s6b_run(S6Ctx& ctx, unsigned mode, long long trials, std::mt19937& rng, unsigned refresh_every)
{
    int fails = 0;
    HWRoad& road = ctx.road;
    std::string modestr = std::string("s6b-m") + std::to_string(mode);

    for (long long t = 0; t < trials; t++) {
        for (uint32_t i = 0; i < 0x800; i++) road.write16(i * 2, (uint16_t)rng());
        road.read_road_control();
        road.write_road_control((uint8_t)mode);

        if ((t % refresh_every) == 0) fill_random(ctx.C.base, ctx.n, rng);
        ctx.restore();
        if (!s6_render_compare(ctx, "s6b", modestr, (int)mode, t)) fails++;
    }
    return fails;
}

// s6c: relative h-scroll offset sweep (S6.4). Modes 1 and 2 only; road 1's
// h-scroll is road 0's h-scroll + d for every d in -600..600 (wrapped to 12
// bits), random ROM lines per row, 3 calls per d -- walks every merge-
// boundary configuration render_row_runs's two-cursor merge can hit.
static int s6c_run(S6Ctx& ctx, unsigned mode, std::mt19937& rng, unsigned refresh_every)
{
    int fails = 0;
    HWRoad& road = ctx.road;
    std::string modestr = std::string("s6c-m") + std::to_string(mode);
    long long call = 0;

    for (int d = -600; d <= 600; d++) {
        for (int t = 0; t < 3; t++) {
            uint16_t h0 = (uint16_t)(rng() % 4096);
            uint16_t h1 = (uint16_t)(((int)h0 + d) & 0xfff);

            for (uint32_t i = 0; i < 0x800; i++) road.write16(i * 2, (uint16_t)rng()); // colour tables / slack
            for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
                int L0 = (int)(rng() % (unsigned)L_COUNT);
                int L1 = (int)(rng() % (unsigned)L_COUNT);
                set_row0(road, yy, L0, h0);
                set_row1(road, yy, L1, h1);
            }
            road.read_road_control();
            road.write_road_control((uint8_t)(4 | mode));

            if ((call % refresh_every) == 0) fill_random(ctx.C.base, ctx.n, rng);
            ctx.restore();
            if (!s6_render_compare(ctx, "s6c", modestr, d, t)) fails++;
            call++;
        }
    }
    return fails;
}

// s6d synthetic ROM images (S6.4). decode_road only reads the first 0x8000
// bytes, split into a bit-0 plane (0x0000-0x3FFF) and a bit-1 plane
// (0x4000-0x7FFF), 64 bytes (512 pixels) per line. The "alternating" pattern
// gives both planes the *same* alternating 0x55/0xAA bytes (not
// complementary), so bit0==bit1 at every pixel: decoded values alternate
// 0/3/0/3 (512 runs per line, the table's maximum size) and, unlike the
// all-zero pattern, actually reach value 3 so decode_road's stripe-area
// |= 4 (hwroad.cpp) turns some of those into 7 within the last 8 columns of
// the first 256 (the road-0 stripe region) -- see the printed histogram.
static std::vector<uint8_t> make_synthetic_rom(int which, std::mt19937& rng)
{
    std::vector<uint8_t> rom(0x8000, 0);
    switch (which) {
        case 0: // random
            for (auto& b : rom) b = (uint8_t)rng();
            break;
        case 1: // alternating 0x55/0xAA, identical pattern in both planes
            for (std::size_t i = 0; i < 0x4000; i++) {
                uint8_t b = (i & 1) ? (uint8_t)0xAA : (uint8_t)0x55;
                rom[i] = b;
                rom[i + 0x4000] = b;
            }
            break;
        case 2: // all-zero
            break; // already zero-filled
        case 3: // all-0xFF
            std::fill(rom.begin(), rom.end(), (uint8_t)0xFF);
            break;
    }
    return rom;
}

static const char* const kSyntheticRomNames[4] = { "random32k", "alt55aa", "allzero", "allff" };

// Prints how many of the 513 x 512 decoded texels hold each of the 8
// possible values, for the diagnostic noted above.
static void print_roads_histogram(HWRoad& road, const char* label)
{
    const uint8_t* roads = RoadTilesTestAccess::roads(road);
    long long counts[8] = {0,0,0,0,0,0,0,0};
    for (std::size_t i = 0; i < 513u * 512u; i++) counts[roads[i] & 7] += 1;
    std::printf("    roads[] histogram (%s): 0=%lld 1=%lld 2=%lld 3=%lld 7=%lld (4/5/6=%lld)\n",
                label, counts[0], counts[1], counts[2], counts[3], counts[7],
                counts[4] + counts[5] + counts[6]);
}

static int run_unit_s6(const std::string& roms_dir, unsigned seed, const std::string& focus)
{
    std::vector<uint8_t> road_rom, tile_rom;
    load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x53365321u); // "S6!" - distinct stream from selftest/other subcommands

    const bool do_a = focus.empty() || focus == "s6" || focus == "s6a";
    const bool do_b = focus.empty() || focus == "s6" || focus == "s6b";
    const bool do_c = focus.empty() || focus == "s6" || focus == "s6c";
    const bool do_d = focus.empty() || focus == "s6" || focus == "s6d";

    int failures = 0;
    static const Mode kHiresModes[2] = { kModes[2], kModes[3] }; // hires-narrow (640), hires-wide (808)

    for (const Mode& m : kHiresModes) {
        set_mode(m);
        S6Ctx ctx;
        ctx.allocate(config.s16_width, config.s16_height);
        ctx.road.init(road_rom.data(), true);
        fill_random(ctx.C.base, ctx.n, rng);

        std::printf("unit s6: mode=%-13s width=%4u height=%4u\n", m.name, config.s16_width, config.s16_height);

        if (do_a) {
            int f = 0;
            for (unsigned mode : {0u, 1u, 2u}) f += s6a_run(ctx, /*sweep_road0=*/true,  mode, rng, 200);
            for (unsigned mode : {1u, 2u, 3u}) f += s6a_run(ctx, /*sweep_road0=*/false, mode, rng, 200);
            std::printf("  s6a exhaustive direct-mode sweep: %s (%d mismatch(es))\n", f ? "FAIL" : "ok", f);
            failures += f;
        }
        if (do_b) {
            int f = 0;
            for (unsigned mode : {0u, 1u, 2u, 3u}) f += s6b_run(ctx, mode, 20000, rng, 500);
            std::printf("  s6b indirect-mode random: %s (%d mismatch(es))\n", f ? "FAIL" : "ok", f);
            failures += f;
        }
        if (do_c) {
            int f = 0;
            for (unsigned mode : {1u, 2u}) f += s6c_run(ctx, mode, rng, 100);
            std::printf("  s6c relative-offset sweep: %s (%d mismatch(es))\n", f ? "FAIL" : "ok", f);
            failures += f;
        }
        if (do_d) {
            int f = 0;
            for (int which = 0; which < 4; which++) {
                std::vector<uint8_t> synth_rom = make_synthetic_rom(which, rng);
                ctx.road.init(synth_rom.data(), true);
                print_roads_histogram(ctx.road, kSyntheticRomNames[which]);
                for (unsigned mode : {0u, 1u, 2u, 3u}) {
                    std::string modestr = std::string("s6d-") + kSyntheticRomNames[which] + "-m" + std::to_string(mode);
                    for (long long t = 0; t < 500; t++) {
                        for (uint32_t i = 0; i < 0x800; i++) ctx.road.write16(i * 2, (uint16_t)rng());
                        ctx.road.read_road_control();
                        ctx.road.write_road_control((uint8_t)mode);
                        if ((t % 100) == 0) fill_random(ctx.C.base, ctx.n, rng);
                        ctx.restore();
                        if (!s6_render_compare(ctx, "s6d", modestr, (int)mode, t)) f++;
                    }
                }
            }
            std::printf("  s6d synthetic ROMs: %s (%d mismatch(es))\n", f ? "FAIL" : "ok", f);
            failures += f;
            ctx.road.init(road_rom.data(), true); // restore the real/random ROM for any later use of ctx
        }
    }

    return failures;
}

static bool run_unit(const std::string& roms_dir, unsigned seed, const std::string& focus)
{
    std::printf("road_tiles_test unit: arch=%s focus=%s\n", CB_ARCH_NAME, focus.empty() ? "(all)" : focus.c_str());
    int failures = run_unit_s6(roms_dir, seed, focus);
    if (failures == 0) {
        std::printf("unit PASS (arch=%s)\n", CB_ARCH_NAME);
        return true;
    }
    std::printf("unit FAIL: %d mismatch(es)\n", failures);
    return false;
}

// ---------------------------------------------------------------------------
// bench fg: render_foreground_hires vs render_foreground_hires_ref timing
// (IMPLEMENTATION_SPEC.md S6.5), ns/row, over a synthetic driving frame
// (a gently curving road, road-0-priority indirect... direct-mode RAM layout
// so every row gets distinct, plausible line/h-scroll data).
// ---------------------------------------------------------------------------
static void run_bench_fg(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x62656e63u); // "benc"

    std::printf("road_tiles_test bench fg: arch=%s\n", CB_ARCH_NAME);
    static const Mode kHiresModes[2] = { kModes[2], kModes[3] };

    for (const Mode& m : kHiresModes) {
        set_mode(m);
        HWRoad road;
        road.init(road_rom.data(), true);

        // Synthetic driving frame: direct mode, road 0 priority (control bit
        // 0x4 | 1, the hot path), a gently curving centreline (sinusoidal
        // h-scroll) and cycling ROM lines, roughly like real OutRun road RAM.
        for (uint32_t i = 0; i < 0x800; i++) road.write16(i * 2, (uint16_t)rng());
        for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
            uint16_t h0 = (uint16_t)((2048 + (int)(400.0 * std::sin(yy * 0.05))) & 0xfff);
            uint16_t h1 = (uint16_t)((h0 + 24) & 0xfff);
            set_row0(road, yy, yy & 0xff, h0);
            set_row1(road, yy, yy & 0xff, h1);
        }
        road.read_road_control();
        road.write_road_control(4 | 1);

        PixelBuffer A;
        A.allocate(config.s16_width, config.s16_height);
        fill_random(A.base, A.halfwords, rng);

        const int iterations = 2000;
        using clk = std::chrono::steady_clock;

        auto t0 = clk::now();
        for (int it = 0; it < iterations; it++) (road.*road.render_foreground)(A.pixels);
        auto t1 = clk::now();
        for (int it = 0; it < iterations; it++) RoadTilesTestAccess::fg_hires_ref(road, A.pixels);
        auto t2 = clk::now();

        double ns_new = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)(iterations * config.s16_height);
        double ns_ref = std::chrono::duration<double, std::nano>(t2 - t1).count() / (double)(iterations * config.s16_height);

        std::printf("  mode=%-13s width=%4u  new=%7.1f ns/row  ref=%7.1f ns/row  speedup=%.2fx\n",
                    m.name, config.s16_width, ns_new, ns_ref, ns_ref / ns_new);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static void print_usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [selftest|unit [s6|s6a|s6b|s6c|s6d]|replay|bench fg|golden] "
        "[--roms <dir>] [--frames <dir>] [--seed N]\n"
        "  (default sub-command: selftest)\n", argv0);
}

int main(int argc, char** argv)
{
    std::string roms_dir, frames_dir, cmd, subcmd;
    unsigned seed = 12345u;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--roms" && i + 1 < argc)          roms_dir = argv[++i];
        else if (a == "--frames" && i + 1 < argc)   frames_dir = argv[++i];
        else if (a == "--seed" && i + 1 < argc)     seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "-h" || a == "--help")        { print_usage(argv[0]); return 0; }
        else if (cmd.empty() && !a.empty() && a[0] != '-') cmd = a;
        else if (subcmd.empty() && !a.empty() && a[0] != '-') subcmd = a;
        else { std::fprintf(stderr, "unrecognised argument '%s'\n", a.c_str()); print_usage(argv[0]); return 2; }
    }
    (void)frames_dir; // used by `replay`, not implemented in this (S0) drop

    if (cmd.empty()) cmd = "selftest";

    if (cmd == "unit") {
        return run_unit(roms_dir, seed, subcmd) ? 0 : 1;
    }
    if (cmd == "bench") {
        if (subcmd == "fg") { run_bench_fg(roms_dir, seed); return 0; }
        std::fprintf(stderr, "bench: unknown or missing target '%s' (try 'bench fg')\n", subcmd.c_str());
        return 2;
    }
    if (cmd == "replay" || cmd == "golden") {
        std::printf("%s: not yet implemented in this S0 drop -- lands with later "
                     "IMPLEMENTATION_SPEC.md sections (see S0.6/S0.7).\n", cmd.c_str());
        return 0;
    }
    if (cmd != "selftest") {
        std::fprintf(stderr, "unknown sub-command '%s'\n", cmd.c_str());
        print_usage(argv[0]);
        return 2;
    }

    return run_selftest(roms_dir, seed) ? 0 : 1;
}
