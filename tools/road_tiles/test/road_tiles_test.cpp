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

    // S7.4: exhaustive kernel sweep support.
    //   - tile_word: reads a decoded ROM tile word straight out of the private
    //     `tiles[]` array (0x10000 entries), for the "real ROM tiles[]" p0 set.
    //   - set_synth_tile_row0: overwrites tile index 0's 8 rows with {p0, 0,
    //     0, 0, 0, 0, 0, 0} -- a synthetic one-row tile, rows 1-7 == the
    //     nMaskColour used below (0) so render8x8_tile_mask_hires_ref's own
    //     8-row loop skips them, leaving only row 0's write for the memcmp.
    //   - hires_ref: the 8-row scalar reference kernel, called on the
    //     synthetic tile above.
    //   - new_row: the new kernel (tile_row_hires), called directly, one row,
    //     via the hwtiles::test_tile_row_hires forwarder.
    static uint32_t tile_word(hwtiles& t, uint32_t idx) { return t.tiles[idx]; }
    static uint32_t tile_length() { return (uint32_t)hwtiles::TILES_LENGTH; }
    static void set_synth_tile_row0(hwtiles& t, uint32_t p0)
    {
        t.tiles[0] = p0;
        for (int i = 1; i < 8; i++) t.tiles[i] = 0;
    }
    static void hires_ref(hwtiles& t, uint16_t* buf, uint16_t nTileNumber,
                           uint16_t StartX, uint16_t StartY, uint16_t nTilePalette,
                           uint16_t nColourDepth, uint16_t nMaskColour, uint16_t nPaletteOffset)
    {
        t.render8x8_tile_mask_hires_ref(buf, nTileNumber, StartX, StartY, nTilePalette, nColourDepth, nMaskColour, nPaletteOffset);
    }
    static void new_row(uint16_t* buf, uint32_t p0, uint32_t nPalette, uint16_t width)
    {
        hwtiles::test_tile_row_hires(buf, p0, nPalette, width);
    }

    // S6: diagnostic peek at the decoded road bytes, used only to report
    // texel-value coverage (e.g. confirming value 7 shows up) for the s6d
    // synthetic ROM patterns; not used by any pass/fail comparison.
    static const uint8_t* roads(HWRoad& r) { return r.roads; }

    // S5.4 negative control (driver-only). hwtiles::render_text_layer is
    // NEVER changed to accept a dead_rows mask (IMPLEMENTATION_SPEC.md
    // S5.3): this is a from-scratch re-implementation of its body, with the
    // exact S5.2 dead-row skip spliced into its my-loop, dispatching through
    // the REAL private kernel pointers -- used only to prove that unit s5's
    // comparison methodology (mask-based skip, then stamp masked rows with a
    // constant in both buffers, then memcmp) would catch it if the mask were
    // ever wired into a layer that draws after the road foreground (so
    // nothing later actually overwrites the rows a wrong skip would leave
    // blank). See run_unit_s5's negative-control block: confirmed by hand to
    // report a mismatch, then left disabled so a normal `unit s5` run
    // (expected: 0 failures) doesn't trip over an intentionally-failing
    // check.
    static void text_layer_masked_BUG(hwtiles& t, uint16_t* buf, uint8_t priority_draw, const uint8_t* dead_rows)
    {
        uint16_t mx, my, Code, Colour, x, y, Priority, TileIndex = 0;

        int    xs[64]; uint8_t mxl[64]; int nmx = 0;
        for (int mx = 0; mx < 64; mx++) {
            int x = 8 * mx - 192;
            if (x > -8 && x < t.s16_width_noscale) { xs[nmx] = x; mxl[nmx++] = (uint8_t)mx; }
        }
        int    ys[32]; uint8_t myl[32]; int nmy = 0;
        for (int my = 0; my < 32; my++) {
            int y = 8 * my;
            if (y >= 0 && y < S16_HEIGHT) {
                // S5.2's dead-row skip, verbatim, applied here to the TEXT
                // layer's my-loop (the bug under test).
                if (dead_rows) {
                    const int r0 = y < 0 ? 0 : y;
                    const int r1 = (y + 8 > S16_HEIGHT) ? S16_HEIGHT : y + 8;
                    bool dead = true;
                    for (int r = r0; r < r1; r++) if (!dead_rows[r]) { dead = false; break; }
                    if (dead) continue;
                }
                ys[nmy] = y; myl[nmy++] = (uint8_t)my;
            }
        }

        for (int j = 0; j < nmy; j++)
        {
            my = myl[j];
            y = ys[j];
            for (int i = 0; i < nmx; i++)
            {
                mx = mxl[i];
                x = xs[i];
                TileIndex = (my * 64 + mx) * 2;
                Code = (t.text_ram[TileIndex + 0] << 8) | t.text_ram[TileIndex + 1];
                Priority = (Code >> 15) & 1;

                if (Priority == priority_draw)
                {
                    Colour = (Code >> 9) & 0x07;
                    Code &= 0x1ff;
                    Code += (t.tile_banks[0] << 12);
                    Code &= (hwtiles::NUM_TILES - 1);

                    if (Code != 0)
                    {
                        if (x > 7 && x < (t.s16_width_noscale - 8) && y > 7 && y <= (S16_HEIGHT - 8))
                            (t.*t.render8x8_tile_mask)(buf, Code, x + config.s16_x_off, y, Colour, 3, 0, hwtiles::TILEMAP_COLOUR_OFFSET);
                        else if (x > -8 && x < t.s16_width_noscale && y >= 0 && y < S16_HEIGHT)
                            (t.*t.render8x8_tile_mask_clip)(buf, Code, x + config.s16_x_off, y, Colour, 3, 0, hwtiles::TILEMAP_COLOUR_OFFSET);
                    }
                }
            }
        }
    }
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
// unit s3: road-foreground overwrite mask + invariant check
// (IMPLEMENTATION_SPEC.md S3.4). road_row_written/road_row_sky are free
// functions declared in hwroad.hpp, so the closed-form assertions below call
// them (indirectly, via the closed-form re-derivation) with no friend access
// needed; road_ram_buffer() is a public S0.4 accessor.
// ---------------------------------------------------------------------------
static int run_unit_s3(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x53332121u); // "S3!!"

    int failures = 0;
    long long rows_both_total = 0;

    // S3.4 asks for "hi-res and lo-res"; the row predicates and the mask
    // don't depend on width or widescreen, so the narrow variant of each
    // resolution is representative (S5's own unit test covers the
    // x_clamp/W combinations that matter to the tile-layer clip).
    static const Mode kResModes[2] = { kModes[0], kModes[2] }; // lores-narrow, hires-narrow

    for (const Mode& m : kResModes) {
        set_mode(m);
        HWRoad road;
        road.init(road_rom.data(), m.hires);

        PixelBuffer scratch;
        scratch.allocate(config.s16_width, config.s16_height);

        for (int direct = 0; direct < 2; direct++) {
            for (unsigned mode = 0; mode < 4; mode++) {
                // Random line-select bits, h-scroll and colour tables across
                // the whole 0x800-halfword RAM...
                for (uint32_t i = 0; i < 0x800; i++) road.write16(i * 2, (uint16_t)rng());

                // ...then force just the 0x800 (solid) bit of each line's two
                // line-select words to cycle through all four (s0, s1)
                // combinations across the 224 lines, keeping every other bit
                // (line-select index, unrelated flags) freshly random.
                for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
                    const int combo = yy & 3;
                    const bool s0 = (combo & 2) != 0;
                    const bool s1 = (combo & 1) != 0;
                    uint16_t w0 = (uint16_t)rng();
                    uint16_t w1 = (uint16_t)rng();
                    w0 = (uint16_t)((w0 & ~0x0800u) | (s0 ? 0x0800u : 0u));
                    w1 = (uint16_t)((w1 & ~0x0800u) | (s1 ? 0x0800u : 0u));
                    road.write16((uint32_t)(0x000 + yy) * 2, w0);
                    road.write16((uint32_t)(0x100 + yy) * 2, w1);
                }
                road.read_road_control();
                road.write_road_control((uint8_t)((direct ? 4 : 0) | mode));

                uint8_t mask_true[S16_HEIGHT], mask_false[S16_HEIGHT];
                road.foreground_row_mask(mask_true, true);
                road.foreground_row_mask(mask_false, false);

                for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
                    if (mask_false[yy] != 0) {
                        std::printf("  UNIT S3 FAIL: mode=%u direct=%d yy=%d mask_false nonzero (fg_enabled=false must give an all-zero mask)\n",
                                    mode, direct, yy);
                        failures++;
                    }
                }

                int rb1 = 0;
                int v1 = road.check_foreground_rows(mask_true, true, scratch.pixels, &rb1);
                if (v1 != 0) {
                    std::printf("  UNIT S3 FAIL: mode=%u direct=%d fg_enabled=true check_foreground_rows returned %d\n", mode, direct, v1);
                    failures += v1;
                }

                int rb2 = 0;
                int v2 = road.check_foreground_rows(mask_false, false, scratch.pixels, &rb2);
                if (v2 != 0) {
                    std::printf("  UNIT S3 FAIL: mode=%u direct=%d fg_enabled=false check_foreground_rows returned %d\n", mode, direct, v2);
                    failures += v2;
                }
                rows_both_total += rb1;

                // Closed-form assertion (S3.4): mode 0 written iff !s0, mode
                // 3 iff !s1, modes 1/2 iff !(s0 && s1) -- re-derived here
                // independently of road_row_written's own implementation,
                // read back via the public road_ram_buffer() accessor.
                const uint16_t* ramBuff = road.road_ram_buffer();
                for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
                    const unsigned d0 = ramBuff[yy];
                    const unsigned d1 = ramBuff[0x100 + yy];
                    const bool s0 = (d0 & 0x800u) != 0, s1 = (d1 & 0x800u) != 0;
                    bool expected;
                    if (mode == 0)      expected = !s0;
                    else if (mode == 3) expected = !s1;
                    else                expected = !(s0 && s1);
                    const bool actual = mask_true[yy] != 0;
                    if (expected != actual) {
                        std::printf("  UNIT S3 FAIL: closed-form mismatch mode=%u direct=%d yy=%d s0=%d s1=%d expected=%d actual=%d\n",
                                    mode, direct, yy, (int)s0, (int)s1, (int)expected, (int)actual);
                        failures++;
                    }
                }
            }
        }
    }

    std::printf("  s3: %s (%d failure(s)); rows_both total (background also fills, across all cases) = %lld\n",
                failures ? "FAIL" : "ok", failures, rows_both_total);
    return failures;
}

// ---------------------------------------------------------------------------
// unit s5: clip the tile layers to rows the road foreground will overwrite
// (IMPLEMENTATION_SPEC.md S5.4).
// ---------------------------------------------------------------------------

// Mask with runs (a handful of contiguous dead/alive stretches), like a real
// road foreground's row coverage, rather than independently-random per line.
static void make_run_mask(uint8_t mask[S16_HEIGHT], std::mt19937& rng)
{
    uint8_t cur = (uint8_t)(rng() & 1u);
    for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
        if ((rng() % 100u) < 15u) cur ^= 1u; // ~15%/line flip chance -> runs a few lines long
        mask[yy] = cur;
    }
}

// Random tile_ram with ~30% of the 16-bit tile entries carrying a non-zero
// Code (bits 0-12); the rest carry Code 0 (transparent -- skipped by both the
// ref and masked renderer regardless of the mask). Priority/colour bits stay
// random either way. S5.4: "random tile RAM with 30% non-zero codes".
static void fill_tile_ram_30pct(uint8_t* tile_ram, std::size_t bytes, std::mt19937& rng)
{
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t data = (uint16_t)rng();
        const bool nonzero = (rng() % 100u) < 30u;
        uint16_t code = data & 0x1fffu;
        if (!nonzero)       code = 0;
        else if (code == 0) code = 1; // force genuinely non-zero
        data = (uint16_t)((data & ~0x1fffu) | code);
        tile_ram[i]     = (uint8_t)(data >> 8);
        tile_ram[i + 1] = (uint8_t)(data & 0xffu);
    }
}

static int run_unit_s5(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x53352121u); // "S5!!"

    int failures = 0;
    const int trials_per_mode = 500; // kModes has 4 entries: 4 x 500 = 2000 (S5.4)

    for (const Mode& m : kModes) { // all four x_clamp/W combinations, hi-res and lo-res
        set_mode(m);
        hwtiles tiles;
        tiles.init(tile_rom.data(), m.hires);
        tiles.set_x_clamp(hwtiles::CENTRE);

        PixelBuffer A, B;
        A.allocate(config.s16_width, config.s16_height);
        B.allocate(config.s16_width, config.s16_height);
        const std::size_t n = A.halfwords;
        const int rows_per_line = m.hires ? 2 : 1;

        int mode_fails = 0;
        for (int trial = 0; trial < trials_per_mode; trial++) {
            fill_tile_ram_30pct(tiles.tile_ram, sizeof(tiles.tile_ram), rng);
            for (std::size_t i = 0; i < sizeof(tiles.text_ram); i++) tiles.text_ram[i] = (uint8_t)rng(); // random page/scroll registers
            tiles.update_tile_values();

            uint8_t mask[S16_HEIGHT];
            make_run_mask(mask, rng);

            const uint8_t page_index = (uint8_t)(rng() % 2u);
            const uint8_t pri        = (uint8_t)(rng() % 2u);

            fill_random(A.base, n, rng);
            std::memcpy(B.base, A.base, n * sizeof(uint16_t));

            RoadTilesTestAccess::tile_layer_ref(tiles, A.pixels, page_index, pri);   // A: no mask (ref)
            tiles.render_tile_layer(B.pixels, page_index, pri, mask);               // B: masked (shipping)

            // Synthetic "foreground": overwrite every masked lo-res line, in
            // full (all rows_per_line hi-res rows, full width), with the
            // same constant in BOTH buffers -- S3 proves the real road
            // foreground always does exactly this to these rows.
            const uint16_t FG_CONST = 0x7bcd;
            for (int yy = 0; yy < (int)S16_HEIGHT; yy++) {
                if (!mask[yy]) continue;
                for (int r = 0; r < rows_per_line; r++) {
                    const int y = yy * rows_per_line + r;
                    uint16_t* rowA = A.pixels + (std::size_t)y * config.s16_width;
                    uint16_t* rowB = B.pixels + (std::size_t)y * config.s16_width;
                    for (int x = 0; x < config.s16_width; x++) { rowA[x] = FG_CONST; rowB[x] = FG_CONST; }
                }
            }

            if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
                report_mismatch("s5 tile_layer", m.name, page_index * 10 + pri, trial, A.base, B.base, n, config.s16_width, config.s16_height);
                mode_fails++;
            }
        }

        std::printf("  s5 mode=%-13s: %s (%d/%d mismatch(es))\n", m.name, mode_fails ? "FAIL" : "ok", mode_fails, trials_per_mode);
        failures += mode_fails;

        // ------------------------------------------------------------------
        // S5.4 negative control: proves this comparison methodology (mask-
        // based skip, then stamp masked rows with a constant in both
        // buffers, then memcmp) DOES detect a masking bug -- specifically,
        // masking a layer (text) that draws AFTER the road foreground, so
        // nothing later actually overwrites the rows a wrong implementation
        // would skip. Confirmed by hand (see the task report) to report a
        // mismatch on every mode; left disabled here so a normal `unit s5`
        // run, which expects 0 failures, doesn't trip over an
        // intentionally-failing check. hwtiles::render_text_layer itself is
        // NEVER changed to accept a mask (S5.3) --
        // RoadTilesTestAccess::text_layer_masked_BUG is a driver-only
        // re-implementation for this one purpose. To re-run by hand,
        // uncomment this block.
        // ------------------------------------------------------------------
        // {
        //     uint8_t nc_mask[S16_HEIGHT];
        //     make_run_mask(nc_mask, rng);
        //     for (std::size_t i = 0; i < sizeof(tiles.text_ram); i++) tiles.text_ram[i] = (uint8_t)rng();
        //     tiles.update_tile_values();
        //     fill_random(A.base, n, rng);
        //     std::memcpy(B.base, A.base, n * sizeof(uint16_t));
        //     RoadTilesTestAccess::text_layer_ref(tiles, A.pixels, 1);
        //     RoadTilesTestAccess::text_layer_masked_BUG(tiles, B.pixels, 1, nc_mask);
        //     bool mismatch = std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0;
        //     std::printf("  s5 NEGATIVE CONTROL mode=%-13s: %s\n", m.name,
        //                 mismatch ? "correctly FAILED (mask-on-text-layer bug detected)" : "DID NOT FAIL -- test is blind!");
        // }
    }

    return failures;
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

// ---------------------------------------------------------------------------
// unit s7: NEON tile-row kernel (tile_row_hires) vs render8x8_tile_mask_hires_ref
// (IMPLEMENTATION_SPEC.md S7.4).
//
// Scratch: 4 rows x 32 halfwords (S7_N = 128), width = 32, kernel target at
// scratch + 32 + 8 -- one guard row above and below the two written rows,
// 8 guard columns to the left of the 16-pixel row and 8 to the right
// (32 - 8 - 16 = 8). config.s16_width is set to 32 for the whole unit so
// render8x8_tile_mask_hires_ref's internal row stride (it reads
// config.s16_width, not a caller-supplied width) matches the scratch.
//
// Each trial: fill the scratch with fresh seeded random halfwords, copy it,
// run the reference 8-row kernel on a synthetic one-row tile (tile index 0:
// row 0 = p0, rows 1-7 = 0 = nMaskColour, so the reference's own loop skips
// them) into A, run the new kernel directly (hwtiles::test_tile_row_hires,
// one row, no 8-row dispatch at all) into B, memcmp all 128 halfwords.
//
// p0 set (union, per the task):
//   - all TILES_LENGTH (65536) decoded ROM tile words, only when --roms
//     loaded real files (used_real_roms) -- else this set is skipped
//     entirely, since a synthetic/random ROM image carries no more
//     information than the "100,000 random words" set already does.
//   - all 65536 words where each of the 8 nibbles is independently drawn
//     from {0, 1, 8, 15} (4^8 = 65536 combinations) -- transparent, minimum,
//     mid and maximum nibble values in every position.
//   - 100,000 seeded random 32-bit words.
//
// Palette subsampling (30M invocations at 65536+65536+100000 candidates x
// 128 palettes each is not needed for "seconds" runtime, so full coverage is
// used instead of subsampling): the ROM set and the random set each sweep a
// single fixed representative palette (91: nPalette = 91<<3 = 728, mid-range
// and non-trivial-bit-pattern); the {0,1,8,15}^8 edge set sweeps all 128
// palettes, since it is the deliberately-constructed "representative subset"
// that already exercises every transparent/opaque and min/mid/max nibble
// combination the palette add could interact with. Total invocations:
// 65536 (ROM, if present) + 65536*128 (edge, all palettes) + 100000 (random)
// = ~8.55M -- a few seconds on x86-64 scalar.
// ---------------------------------------------------------------------------
static const uint16_t S7_WIDTH  = 32;
static const int      S7_ROWS   = 4;
static const int      S7_N      = S7_WIDTH * S7_ROWS; // 128
static const int      S7_TARGET = S7_WIDTH + 8;        // row 1, col 8
static const uint32_t S7_FIXED_PALETTE = 91;            // 0..127, arbitrary mid-range

static int run_unit_s7(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    bool used_real_roms = load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x53372121u); // "S7!!"

    hwtiles tiles;
    tiles.init(tile_rom.data(), /*hires=*/true);

    config.s16_width  = S7_WIDTH;
    config.s16_height = (uint16_t)S7_ROWS;
    config.s16_x_off  = 0;

    uint16_t A[S7_N], B[S7_N];
    int failures = 0;
    long long trials = 0;
    int printed = 0;

    auto trial = [&](uint32_t p0, uint32_t palette) {
        RoadTilesTestAccess::set_synth_tile_row0(tiles, p0);
        fill_random(A, S7_N, rng);
        std::memcpy(B, A, sizeof(A));
        RoadTilesTestAccess::hires_ref(tiles, A + S7_TARGET, 0, 0, 0, (uint16_t)palette, 3, 0, 0);
        RoadTilesTestAccess::new_row(B + S7_TARGET, p0, palette << 3, S7_WIDTH);
        trials++;
        if (std::memcmp(A, B, sizeof(A)) != 0) {
            failures++;
            if (printed < 20) {
                printed++;
                std::size_t i = 0;
                while (i < (std::size_t)S7_N && A[i] == B[i]) i++;
                std::printf("  UNIT S7 FAIL: p0=0x%08x palette=%u halfword_index=%zu ref=0x%04x new=0x%04x\n",
                            p0, palette, i, A[i], B[i]);
            }
        }
    };

    // Set 1: every decoded ROM tile word, fixed palette, only with real ROMs.
    if (used_real_roms) {
        const uint32_t n = RoadTilesTestAccess::tile_length();
        for (uint32_t i = 0; i < n; i++)
            trial(RoadTilesTestAccess::tile_word(tiles, i), S7_FIXED_PALETTE);
    }

    // Set 2: {0,1,8,15}^8 (65536 combinations), every palette 0..127.
    static const uint32_t EDGE_VALS[4] = { 0, 1, 8, 15 };
    for (uint32_t idx = 0; idx < 65536u; idx++) {
        uint32_t p0 = 0, t = idx;
        for (int k = 0; k < 8; k++) { p0 = (p0 << 4) | EDGE_VALS[t & 3u]; t >>= 2; }
        for (uint32_t pal = 0; pal < 128; pal++) trial(p0, pal);
    }

    // Set 3: 100,000 seeded random words, fixed palette.
    for (int i = 0; i < 100000; i++) trial((uint32_t)rng(), S7_FIXED_PALETTE);

    std::printf("  s7: %s (%d/%lld mismatch(es); roms=%s)\n",
                failures ? "FAIL" : "ok", failures, trials, used_real_roms ? "real" : "skipped ROM set (synthetic/random)");
    return failures;
}

// ---------------------------------------------------------------------------
// bench tiles: tile_row_hires vs render8x8_tile_mask_hires_ref timing
// (IMPLEMENTATION_SPEC.md S7.5), ns/row, opaque (all 8 nibbles non-zero, the
// fast vst1q path) and mixed (half the nibbles zero, the read-modify-write
// vbslq path) rows separately.
// ---------------------------------------------------------------------------
static void run_bench_tiles(const std::string& roms_dir, unsigned seed)
{
    std::vector<uint8_t> road_rom, tile_rom;
    load_roms(roms_dir, road_rom, tile_rom, seed);
    std::mt19937 rng(seed ^ 0x62656e74u); // "bent"

    hwtiles tiles;
    tiles.init(tile_rom.data(), true);

    config.s16_width  = S7_WIDTH;
    config.s16_height = (uint16_t)S7_ROWS;
    config.s16_x_off  = 0;

    uint16_t A[S7_N], B[S7_N];
    fill_random(A, S7_N, rng);
    std::memcpy(B, A, sizeof(A));

    const uint32_t opaque_p0 = 0x12345678u; // nibbles 1,2,3,4,5,6,7,8: all non-zero
    const uint32_t mixed_p0  = 0x10203040u; // nibbles 1,0,2,0,3,0,4,0: half zero

    const int iterations = 200000;
    using clk = std::chrono::steady_clock;

    std::printf("road_tiles_test bench tiles: arch=%s\n", CB_ARCH_NAME);

    for (const auto& kase : { std::pair<const char*, uint32_t>{"opaque", opaque_p0}, std::pair<const char*, uint32_t>{"mixed", mixed_p0} }) {
        const char* name = kase.first;
        const uint32_t p0 = kase.second;
        RoadTilesTestAccess::set_synth_tile_row0(tiles, p0);

        auto t0 = clk::now();
        for (int it = 0; it < iterations; it++)
            RoadTilesTestAccess::new_row(B + S7_TARGET, p0, S7_FIXED_PALETTE << 3, S7_WIDTH);
        auto t1 = clk::now();
        for (int it = 0; it < iterations; it++)
            RoadTilesTestAccess::hires_ref(tiles, A + S7_TARGET, 0, 0, 0, (uint16_t)S7_FIXED_PALETTE, 3, 0, 0);
        auto t2 = clk::now();

        double ns_new = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)iterations;
        double ns_ref = std::chrono::duration<double, std::nano>(t2 - t1).count() / (double)iterations;

        std::printf("  %-6s row: new=%7.1f ns/row  ref=%7.1f ns/row  speedup=%.2fx\n",
                    name, ns_new, ns_ref, ns_ref / ns_new);
    }
}

static bool run_unit(const std::string& roms_dir, unsigned seed, const std::string& focus)
{
    std::printf("road_tiles_test unit: arch=%s focus=%s\n", CB_ARCH_NAME, focus.empty() ? "(all)" : focus.c_str());
    int failures = 0;
    if (focus.empty() || focus == "s3") failures += run_unit_s3(roms_dir, seed);
    if (focus.empty() || focus == "s5") failures += run_unit_s5(roms_dir, seed);
    if (focus.empty() || focus == "s6" || focus.rfind("s6", 0) == 0) failures += run_unit_s6(roms_dir, seed, focus);
    if (focus.empty() || focus == "s7") failures += run_unit_s7(roms_dir, seed);
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
        "usage: %s [selftest|unit [s3|s5|s6|s6a|s6b|s6c|s6d|s7]|replay|bench fg|bench tiles|golden] "
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
        if (subcmd == "fg")    { run_bench_fg(roms_dir, seed); return 0; }
        if (subcmd == "tiles") { run_bench_tiles(roms_dir, seed); return 0; }
        std::fprintf(stderr, "bench: unknown or missing target '%s' (try 'bench fg' or 'bench tiles')\n", subcmd.c_str());
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
