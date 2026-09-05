/***************************************************************************
    sprite_test - standalone test driver for the sprite renderer.

    SPRITE_IMPLEMENTATION_SPEC.md S0.7. Compiled ONLY by
    tools/sprite_bench/test/CMakeLists.txt (CB_RENDER_TEST=1,
    CB_SPRITE_STATS=1), against the REAL src/main/hwvideo/hwsprites.cpp and
    the stub frontend/config.hpp in tools/sprite_bench/test/stubs (placed
    first on the include path, so it shadows the real config.hpp and no
    SDL2/TinyXML2 dependency is needed). Never linked into the shipping game.

    Sub-commands:
      selftest - proves S0's two reference twins are correct (S0.7):
                 (a) render_ref(fix=0) vs the shipping render(): many random
                     RAM fills, driven in lockstep through both functions'
                     independent lazy-table histories (each keeps its own
                     copy, hwsprites.hpp S0.3), memcmp the whole pixel
                     allocation each trial. Zero mismatches required --
                     proves the twin is a faithful copy of the shipped code,
                     history-dependence included.
                 (b) render_ref(fix=1) vs render_px(glow=false): fresh
                     lazy-table state every trial (fix=1 makes the twin
                     history-independent by construction, S0.3), so it must
                     equal the pixel-serial oracle everywhere. Zero
                     mismatches required. Separately tallies how many of the
                     random records are *F1-eligible* (the destination row
                     the draw loop last visits reads back exactly
                     sprite_height, SPRITE_IMPLEMENTATION_SPEC.md fact 1) --
                     this is the "F1 occurrence tally" the task asks for: how
                     often the fix=0/fix=1 distinction would actually have
                     mattered, not a pass/fail count (fix=1 must always
                     match the oracle regardless).

      replay   - loads a CB_SPR_DUMP capture directory (S0.6) and classifies
                 every shipped-vs-oracle pixel difference as F1 or F2
                 (SPRITE_IMPLEMENTATION_SPEC.md S0.7). Reports "no frames
                 found" and exits 0 if --frames names an empty/missing/
                 non-matching directory (no capture was made available to
                 this run) -- see the task report for how to produce one.

      unit [s1a|s1b|s1c] - S1.4's schedule/nibrev/record-partition proofs
                 against the LINEBUF core (hwsprites::sched/decode_row); no
                 subcmd runs all three. bench is not yet implemented (S1.5
                 lands with the D/D' measurement pass; use in-game
                 prof_ns_sprites/CB_SPRITE_PMU meanwhile).

      fuzz [n]  - n (default 200000) random sprite lists of 1-122 records
                 per trial, edge-biased (S0.7), against render_px(false);
                 whole-allocation memcmp; all four modes; both replay_mode
                 values in one run. --negctrl additionally disables
                 replay()'s clip window (S1.4's negative control) and
                 inverts the pass/fail sense -- it is expected to find
                 mismatches, proving the memcmp actually catches overruns.

      golden   - prints FNV-1a 64 hashes (video.cpp's rolling-hash form) of
                 render_px(false) and render_px(true) over a captured frame
                 set, for checking into golden/<set>.txt. Prints "no frames"
                 and exits 0 without --frames.

    ROM loading (--roms <dir>):
      Loads the eight sprite ROMs interleaved 4-way exactly as roms.cpp:65-73
      (mpr-10371.9/10373.10/10375.11/10377.12 @0, mpr-10372.13/10374.14/
      10376.15/10378.16 @0x80000, RomLoader::INTERLEAVE4 i.e.
      rom[i*4+lane]=file[i]) into the 1 MB image hwsprites::init() expects.
      Without --roms (or if a file is missing), a deterministically-seeded
      random image of the same size is used instead -- hwsprites::init() is
      a pure, lossless endian/order conversion of whatever bytes it is
      given, so a random image exercises exactly the same code paths as a
      real one for selftest's purpose (twin fidelity is a property of the
      *algorithm*, not the ROM content).
***************************************************************************/

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <new>
#include <tuple>

#include "hwvideo/hwsprites.hpp"
#include "hwvideo/hwarch.hpp"
#include "globals.hpp"
#include "frontend/config.hpp"

// The stub Config (tools/sprite_bench/test/stubs/frontend/config.hpp)
// declares this extern; the real game defines it in frontend/config.cpp,
// which this driver never links.
Config config;

// ---------------------------------------------------------------------------
// S0.5: the friend struct. hwsprites declares "friend struct
// SpriteTestAccess;" under CB_RENDER_TEST. Most state this driver needs
// (ramBuff, x1/x2, glow) is already reachable through the unconditional S0.5
// accessors (sprite_ram_buffer, get_x1/x2, set_glow/get_glow,
// load_sprite_ram) or the public render()/render_px(), so this struct is
// kept to exactly what has no public path: render_ref, ref_prepass_fix,
// reset_ref_tables (all CB_RENDER_TEST-only), and the always-compiled but
// private parse_record/draw_sprite_px/SpriteRec (needed for the F1 tally and
// the F1/F2 classifier). set_clip is a direct-field poke (private data,
// not a private function) used by `replay` to install a captured frame's
// exact x1/x2 and by the F2 probe to temporarily widen the clip window.
// ---------------------------------------------------------------------------
struct SpriteTestAccess
{
    static void render_ref(hwsprites& hw, uint16_t* pixels, uint8_t priority) { hw.render_ref(pixels, priority); }
    static void set_prepass_fix(hwsprites& hw, bool v) { hw.ref_prepass_fix = v; }
    static void reset_ref_tables(hwsprites& hw) { hw.reset_ref_tables(); }

    using SpriteRec = hwsprites::SpriteRec;
    static bool parse_record(hwsprites& hw, uint16_t d, uint8_t priority, SpriteRec& r) { return hw.parse_record(d, priority, r); }
    static void draw_sprite_px(hwsprites& hw, const SpriteRec& r, uint16_t* pixels, bool glow) { hw.draw_sprite_px(r, pixels, glow); }

    static void set_clip(hwsprites& hw, uint16_t x1, uint16_t x2) { hw.x1 = x1; hw.x2 = x2; }
    static uint16_t raw_x1(hwsprites& hw) { return hw.x1; }
    static uint16_t raw_x2(hwsprites& hw) { return hw.x2; }

    // -----------------------------------------------------------------------
    // SPRITE_IMPLEMENTATION_SPEC.md S1.4/S0.5: hooks onto the S1 LINEBUF core
    // (decode_row/replay/sched/emit's rec[]/line[]/nrec scratch, the tunable
    // constants, nibrev, the raw sprites[] ROM image, and the negative-
    // control/fallback-count test hooks) that have no other public path.
    // -----------------------------------------------------------------------
    using SpanRec = hwsprites::SpanRec;

    static const uint8_t* sched(hwsprites& hw, uint32_t zoom) { return hw.sched(zoom); }
    static void decode_row(hwsprites& hw, const SpriteRec& r, int32_t j, int32_t pend) { hw.decode_row(r, j, pend); }
    static void replay(hwsprites& hw, const SpriteRec& r, uint16_t* pixels, int32_t da, int32_t db, int32_t cx0, int32_t cx1) {
        hw.replay(r, pixels, da, db, cx0, cx1);
    }
    static int32_t nrec(hwsprites& hw) { return hw.nrec; }
    static SpanRec rec_at(hwsprites& hw, int32_t i) { return hw.rec[i]; }
    static uint16_t line_at(hwsprites& hw, int32_t i) { return hw.line[i]; }
    static uint32_t nibrev(uint32_t w) { return hwsprites::nibrev(w); }
    static uint32_t* sprites_ptr(hwsprites& hw) { return hw.sprites; }
    static uint32_t sprites_length() { return hwsprites::SPRITES_LENGTH; }

    static int32_t sched_len()     { return hwsprites::SPR_SCHED_LEN; }
    static int32_t wmax()          { return hwsprites::SPR_WMAX; }
    static int32_t lb_len()        { return hwsprites::SPR_LB_LEN; }
    static int32_t fast_zoom_min() { return hwsprites::SPR_FAST_ZOOM_MIN; }
    static int32_t pend_max()      { return hwsprites::SPR_PEND_MAX; }
    static uint8_t type_gap()      { return hwsprites::SPR_GAP; }
    static uint8_t type_colour()   { return hwsprites::SPR_COLOUR; }
    static uint8_t type_shadow()   { return hwsprites::SPR_SHADOW; }

    static void set_debug_disable_clip(hwsprites& hw, bool v) { hw.debug_disable_clip = v; }

    static long long stat_fallback(hwsprites& hw) {
#ifdef CB_SPRITE_STATS
        return hw.stat_fallback;
#else
        (void)hw; return -1;
#endif
    }
    static void reset_stat_fallback(hwsprites& hw) {
#ifdef CB_SPRITE_STATS
        hw.stat_fallback = 0;
#else
        (void)hw;
#endif
    }
};

// ---------------------------------------------------------------------------
// Pixel buffer, allocated exactly as Video::init() does (video.cpp:66-73):
// size = (width * (height+2) + alignment) halfwords, 64-byte aligned, pixels
// points 64 halfwords (128 bytes, still 64-byte aligned) into the
// allocation. Comparisons in this driver cover the *entire* allocation
// (front pad and the two spare rows included), because that is exactly the
// region an F2-style unclipped overrun (fact 6) can reach.
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

// The real engine (engine/osprites.cpp) always emits addr/pitch/bank/height
// combinations whose lazy-table walk (hwsprites.cpp:511-598, both render()
// and render_ref) stays within the sprite's own 0x10000-entry bank, because
// real ROM data is laid out that way. A *fully* unconstrained random
// ramBuff has no such guarantee -- addr is a free 16-bit field and pitch is
// derived from another -- so the pre-pass loop's shadowaddr walk (which has
// no bounds check at all, by design: it is the shipped, unmodified code,
// SPRITE_IMPLEMENTATION_SPEC.md S0.3) can walk out of the 0x40000-entry
// sprites[]/sprites_flipped[]/sprites_shadowinfo[] arrays and read
// unrelated memory. This is a latent property of the shipped renderer given
// adversarial input, not something S0 may fix (render()/render_ref must
// stay byte-identical); the fix belongs in the fuzz generator, which must
// mimic what a real engine record looks like. Clamp addr and pitch here, on
// every record, so shadowaddr's forward walk (addr + pitch*sprite_height,
// sprite_height bounded ~2048 by height<=256 and zoom<=0xfff) and backward
// walk (a flipped record's addr-(pitch-1)) both stay inside one bank's
// [0, 0x10000) range with a wide safety margin, regardless of bank, height,
// zoom or the flip bit -- exercising the real draw logic (pitch up to 15,
// non-zero, so the ROM-word loop is meaningful) without the unrelated OOB
// hazard.
// Two further pre-existing shipped-code hazards this same sanitizer also
// avoids (documented in SPRITE_IMPLEMENTATION_SPEC.md's own preamble, not
// introduced by S0, and not fixable there -- render()/render_ref must stay
// byte-identical to the shipped code):
//   - zoom == 0: draw_pixel_1row_ns()'s "while (xacc < 0x200) { ... xacc +=
//     zoom; }" never terminates when zoom == 0 (the spec's own words: "the
//     shipped renderer would loop forever"). parse_record/render_px skip
//     zoom == 0 records for exactly this reason; render()/render_ref have
//     no such guard, so a record must never reach them with zoom == 0.
//   - ydelta == -1 (bit 15 of word 4 clear): sprite_height's "(uint64_t)
//     steps * zoom >> 9" casts a *negative* steps (== -height for
//     ydelta==-1) to uint64_t, i.e. sign-extends then reinterprets as a
//     huge unsigned value, then truncates the product to sprite_height's
//     uint32_t -- effectively an unpredictable, potentially huge pre-pass
//     bound for a direction real ROM data apparently never exercises this
//     way. Forcing ydelta==1 keeps the fuzz domain inside what the shipped
//     renderer can process at all; genuine bidirectional coverage is S1+'s
//     `fuzz`/`unit` job once a bounded implementation exists to hold it to.
static void report_mismatch(const char* what, const char* mode, int variant, int trial,
                             const uint16_t* a, const uint16_t* b, std::size_t n,
                             uint16_t width, uint16_t height)
{
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) i++;
    std::printf("  MISMATCH %-24s mode=%-13s variant=%d trial=%d halfword_index=%zu",
                what, mode, variant, trial, i);
    std::size_t pix_off = (i >= (std::size_t)PixelBuffer::ALIGNMENT) ? i - PixelBuffer::ALIGNMENT : 0;
    if (i < (std::size_t)PixelBuffer::ALIGNMENT) {
        std::printf(" (front slack)");
    } else if (pix_off < (std::size_t)width * height) {
        std::printf(" (row=%zu, x=%zu)", pix_off / width, pix_off % width);
    } else {
        std::printf(" (spare rows)");
    }
    std::printf(" a=0x%04x b=0x%04x\n", a[i], b[i]);
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
    // Mirrors Video::set_video_mode, renderer bits omitted (matches
    // tools/road_tiles/test's set_mode).
    if (m.wide) { config.s16_width = S16_WIDTH_WIDE; config.s16_x_off = (uint16_t)((S16_WIDTH_WIDE - S16_WIDTH) / 2); }
    else        { config.s16_width = S16_WIDTH;      config.s16_x_off = 0; }
    config.s16_height = S16_HEIGHT;
    if (m.hires) { config.s16_width = (uint16_t)(config.s16_width << 1); config.s16_height = (uint16_t)(config.s16_height << 1); }
    config.video.hires = m.hires ? 1 : 0;
    config.video.hiresprites = 0;
}

// ---------------------------------------------------------------------------
// ROM loading (S0.7): eight sprite ROMs, 4-way interleaved exactly as
// roms.cpp:65-73 (RomLoader::INTERLEAVE4: rom[i*4+lane] = file[i]), into the
// 1 MB image hwsprites::init() expects.
// ---------------------------------------------------------------------------
static bool read_exact(const std::string& path, uint8_t* dst, std::size_t n)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(dst), (std::streamsize)n);
    return f.good() || (std::size_t)f.gcount() == n;
}

static const std::size_t SPR_ROM_FILE_SIZE = 0x20000;
static const std::size_t SPR_ROM_TOTAL     = 0x100000;
struct SpriteRomFile { const char* name; std::size_t base; int lane; };
static const SpriteRomFile kSpriteRomFiles[8] = {
    { "mpr-10371.9",  0x000000, 0 },
    { "mpr-10373.10", 0x000000, 1 },
    { "mpr-10375.11", 0x000000, 2 },
    { "mpr-10377.12", 0x000000, 3 },
    { "mpr-10372.13", 0x080000, 0 },
    { "mpr-10374.14", 0x080000, 1 },
    { "mpr-10376.15", 0x080000, 2 },
    { "mpr-10378.16", 0x080000, 3 },
};

static bool load_sprite_roms(const std::string& roms_dir, std::vector<uint8_t>& image, unsigned seed)
{
    image.assign(SPR_ROM_TOTAL, 0);
    bool ok = !roms_dir.empty();
    if (ok) {
        std::vector<uint8_t> buf(SPR_ROM_FILE_SIZE);
        for (const SpriteRomFile& f : kSpriteRomFiles) {
            if (!read_exact(roms_dir + "/" + f.name, buf.data(), SPR_ROM_FILE_SIZE)) { ok = false; break; }
            for (std::size_t i = 0; i < SPR_ROM_FILE_SIZE; i++)
                image[f.base + i * 4 + (std::size_t)f.lane] = buf[i];
        }
    }
    if (!ok) {
        // S0.7: "a seeded random image where nibble 6 of every 6th word is
        // forced to 0xF so rows terminate." Nibble 6 (bits 4-7 of the final
        // packed sprites[] word) is the exact bit range render()'s
        // terminator test reads ("(pixels & 0xf0) == 0xf0", hwsprites.cpp);
        // hwsprites::init() packs source bytes d3,d2,d1,d0 (read in that
        // order) as (d0<<24)|(d1<<16)|(d2<<8)|(d3<<0), so nibble 6 is the
        // high nibble of d3 -- the FIRST byte of each 4-byte source group.
        // Without a real ROM's artwork (which always has transparent
        // padding to terminate a row), fully random content has no bound on
        // how far the shipped renderer's unclipped ("_nc") fast path can
        // walk before it happens to see this pattern; forcing it at least
        // every 6 words bounds that walk to a handful of pixels, matching
        // what real ROM content does structurally.
        std::mt19937 rng(seed ^ 0x53505221u); // "SPR!" -- distinct stream from the RAM fills
        fill_random(reinterpret_cast<uint16_t*>(image.data()), image.size() / 2, rng);
        // Also force nibble 1 (bits 24-27, the low nibble of the byte at
        // source offset+3) at the same forced positions: a flipped row's
        // table is built by fully reversing a word's nibble order
        // (hwsprites.cpp's flip pre-pass, == nibrev), so the terminator-
        // relevant bits[4:7] of a *flipped* read of a forced word come from
        // nibble 1 of the original, not nibble 6. Forcing both means a
        // forced word terminates the scan whichever table (forward or
        // flipped) happens to be read.
        for (std::size_t word = 0; word * 4 < image.size(); word += 6) {
            image[word * 4 + 0] = (uint8_t)((image[word * 4 + 0] & 0x0Fu) | 0xF0u);
            image[word * 4 + 3] = (uint8_t)((image[word * 4 + 3] & 0xF0u) | 0x0Fu);
        }
    }
    return ok;
}

// force_clip: SPRITE_IMPLEMENTATION_SPEC.md fact 6 (F2) is exactly about
// the shipped renderer's *unclipped* ("_nc") fast path (selected when the
// record's clip bit, ramBuff[d+0]&0x2000, is CLEAR): it has no x bound at
// all, relying entirely on the ROM data's terminator nibble to ever stop.
// Real ROM artwork reliably terminates within a sprite's own row; fully
// random addr/pitch (even over *real* ROM bytes, at an offset with no
// relation to any actual sprite boundary) has no such guarantee and can
// walk arbitrarily far -- observed here as ASan stack/heap corruption
// before this fix. The CLIPPED path (bit set) is safe regardless of ROM
// content: its outer word loop is bounded by the screen-edge test
// (`x < scrn_width`) independently of whether a terminator ever appears.
// selftest's bulk trials therefore force clip=1 (still exercising the
// pre-pass, both lazy tables, flip, shadow and the count=1/2/3 row-batching
// -- everything F1 depends on); a separate, small unclipped-path sub-phase
// (run_selftest's phase C) forces clip=0 paired with a *densely* forced
// synthetic ROM (build_dense_synthetic_rom) that terminates every row after
// exactly one word, which is safe by construction regardless of pitch or
// flip, to also cover the twin's copy of the twelve "_nc" macros.
static void sanitize_ram_for_render(std::vector<uint16_t>& ram, std::mt19937& rng, bool force_clip = true)
{
    for (std::size_t d = 0; d + 16 <= ram.size(); d += 16) {
        if (ram[d + 0] & 0x8000) break; // end-of-list marker; leave the rest untouched
        // pitch is forced to >= 6 (not just <= 15): sprites_shadowinfo's
        // per-row summary only scans a row's own `pitch`-many words
        // (hwsprites.cpp's prepass), but the shipped draw loop's own word
        // scan is bounded only by the screen edge (clipped path) or nothing
        // at all (unclipped path) -- NOT by pitch -- so if no terminator
        // appears within those pitch words it keeps reading past the row's
        // "own" data. Real ROM artwork always terminates within a row's
        // authored pitch; the synthetic ROM's every-6th-word forcing
        // (load_sprite_roms) guarantees a terminator within any 6
        // consecutive words, so pitch >= 6 guarantees the shadow summary
        // and the actual scan agree on where the row ends. Without this,
        // an under-6 pitch could let the scan run past its own
        // (correctly-summarised) row into content the summary never
        // examined, misclassifying a shadow nibble found there as colour --
        // observed as a real, if rare (1/20000 trials), phase-b mismatch
        // before this fix; not a bug in render_px/parse_record/draw_sprite_px,
        // which check every nibble directly and were unaffected.
        const uint16_t pitch = (uint16_t)(6 + (rng() % 10)); // 6..15
        ram[d + 2] = (uint16_t)((ram[d + 2] & 0x01FFu) | (pitch << 9));
        ram[d + 4] = (uint16_t)(ram[d + 4] & ~0x1000u);
        ram[d + 1] = (uint16_t)(0x2000 + (rng() % 0x2000)); // addr, safely mid-bank
        if ((ram[d + 3] & 0x0fff) == 0) ram[d + 3] |= 0x0001; // zoom != 0
        ram[d + 4] |= 0x8000; // ydelta == 1 (top-to-bottom)
        if (force_clip) {
            ram[d + 0] = (uint16_t)(ram[d + 0] | 0x2000);
        } else {
            // The unclipped ("_nc") macros apply *no* x bound at all --
            // not even the screen edge -- so unlike the clipped path
            // (safe for any xpos: out-of-range pixels are simply never
            // drawn) an unconstrained random xpos/xdelta here would start
            // the very first write far outside the buffer. Force xpos to
            // the raw value that decodes to 0 pre-widescreen-offset (see
            // parse_record: xpos -= 0xbe with no wraparound since
            // 0xbe >= 0x80) and xdelta to the non-rtl direction, so the
            // draw genuinely starts inside the allocated buffer; the
            // dense ROM above still bounds how far the unclipped scan can
            // run once it does.
            ram[d + 0] = (uint16_t)(ram[d + 0] & ~0x2000u);
            ram[d + 6] = 0x00be;
            ram[d + 4] = (uint16_t)(ram[d + 4] | 0x2000u); // xdelta == 1 (not rtl)
        }
    }
}

// Every word forced on both nibble 6 and nibble 1 (see load_sprite_roms'
// comment on why both): guarantees every row -- forward or flipped, any
// pitch -- terminates after exactly one word, so the unclipped path can
// never walk further than a single ROM read regardless of what garbage
// pitch/addr/flip combination the sanitizer above hands it.
static std::vector<uint8_t> build_dense_synthetic_rom(unsigned seed)
{
    std::vector<uint8_t> image(SPR_ROM_TOTAL);
    std::mt19937 rng(seed ^ 0x44454e53u); // "DENS"
    fill_random(reinterpret_cast<uint16_t*>(image.data()), image.size() / 2, rng);
    for (std::size_t word = 0; word * 4 < image.size(); word++) {
        image[word * 4 + 0] = (uint8_t)((image[word * 4 + 0] & 0x0Fu) | 0xF0u);
        image[word * 4 + 3] = (uint8_t)((image[word * 4 + 3] & 0xF0u) | 0x0Fu);
    }
    return image;
}


// Sprite RAM size, halfwords (128 records x 16 halfwords each = 0x800),
// matching hwsprites' private SPRITE_RAM_SIZE and the -dumpspr format's
// 4096-byte ramBuff field.
static const uint16_t SPR_RAM_SIZE = 0x800;

// ---------------------------------------------------------------------------
// selftest (S0.7)
// ---------------------------------------------------------------------------

// Phase (a): render_ref(fix=0) vs the shipping render(), both driven through
// their own independent lazy-table history by the SAME sequence of random
// RAM fills (SpriteTestAccess exposes render_ref/reset_ref_tables; the
// shipped tables are reset once up front by hw.init() and never again,
// exactly mirroring a real session across many frames).
static int selftest_phase_a(hwsprites& hw, const Mode& m, std::mt19937& rng, int trials)
{
    int fails = 0;
    PixelBuffer A, B;
    A.allocate(config.s16_width, config.s16_height);
    B.allocate(config.s16_width, config.s16_height);
    const std::size_t n = A.halfwords;
    std::vector<uint16_t> ram(SPR_RAM_SIZE);

    for (int trial = 0; trial < trials; trial++) {
        fill_random(ram.data(), ram.size(), rng);
        sanitize_ram_for_render(ram, rng);
        hw.load_sprite_ram(ram.data());

        fill_random(A.base, n, rng);
        std::memcpy(B.base, A.base, n * sizeof(uint16_t));

        hw.render(A.pixels, 8);
        SpriteTestAccess::render_ref(hw, B.pixels, 8);

        if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
            report_mismatch("render vs render_ref(fix=0)", m.name, 0, trial, A.base, B.base, n, config.s16_width, config.s16_height);
            fails++;
        }
    }
    return fails;
}

// Phase (b): render_ref(fix=1) [fresh table state every trial] vs
// render_px(glow=false). Must always match (fix=1 is history-independent by
// construction). Separately tallies F1-eligible records: for each record
// actually rendered at priority 8, the shipped sprite_height formula
// (bit-for-bit, including its signed-steps quirk for ydelta==-1) versus
// whether the draw loop's last visited row reads exactly that row back --
// SPRITE_IMPLEMENTATION_SPEC.md fact 1's condition, "(H*zoom) mod 512 >=
// zoom" evaluated on the row COUNT (nsteps = |H|) so it is well-defined for
// both ydelta values; this is a diagnostic count of how often the fix=0/
// fix=1 distinction would matter, not a pass/fail gate.
static int selftest_phase_b(hwsprites& hw, const Mode& m, std::mt19937& rng, int trials, long long& f1_records, long long& f1_frames)
{
    int fails = 0;
    PixelBuffer A, B;
    A.allocate(config.s16_width, config.s16_height);
    B.allocate(config.s16_width, config.s16_height);
    const std::size_t n = A.halfwords;
    std::vector<uint16_t> ram(SPR_RAM_SIZE);

    SpriteTestAccess::set_prepass_fix(hw, true);

    for (int trial = 0; trial < trials; trial++) {
        SpriteTestAccess::reset_ref_tables(hw);
        fill_random(ram.data(), ram.size(), rng);
        sanitize_ram_for_render(ram, rng);
        hw.load_sprite_ram(ram.data());

        fill_random(A.base, n, rng);
        std::memcpy(B.base, A.base, n * sizeof(uint16_t));

        SpriteTestAccess::render_ref(hw, A.pixels, 8);
        hw.render_px(B.pixels, 8, false);

        if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
            report_mismatch("render_ref(fix=1) vs render_px", m.name, 1, trial, A.base, B.base, n, config.s16_width, config.s16_height);
            fails++;
#ifdef SPRITE_TEST_DEBUG_DUMP
            for (uint16_t d = 0; d < SPR_RAM_SIZE; d += 16) {
                if ((ram[d + 0] & 0x8000) != 0) break;
                SpriteTestAccess::SpriteRec rec;
                if (!SpriteTestAccess::parse_record(hw, d, 8, rec)) continue;
                std::printf("    rec d=%u bank=%d top=%d clip=%d addr=%d pitch=%d shadow=%u zoom=%d ydelta=%d flip=%d xdelta=%d width=%d color=%d xpos=%d offset=%d ytarget=%d rtl=%d H=%d\n",
                    d, rec.bank, rec.top, (int)rec.clip, rec.addr, rec.pitch, rec.shadow, rec.zoom, rec.ydelta, rec.flip, rec.xdelta, rec.width, rec.color, rec.xpos, rec.offset, rec.ytarget, (int)rec.rtl, rec.H);
            }
#endif
        }

        // F1-eligibility tally over this trial's record list.
        bool any_this_frame = false;
        for (uint16_t d = 0; d < SPR_RAM_SIZE; d += 16) {
            if ((ram[d + 0] & 0x8000) != 0) break;
            SpriteTestAccess::SpriteRec rec;
            if (!SpriteTestAccess::parse_record(hw, d, 8, rec)) continue;
            const long long nsteps = (rec.ydelta > 0) ? rec.H : -rec.H;
            if (nsteps <= 0) continue;
            const unsigned long long prod = (unsigned long long)nsteps * (unsigned long long)rec.zoom;
            if ((prod % 512ULL) >= (unsigned long long)rec.zoom) {
                f1_records++;
                any_this_frame = true;
            }
        }
        if (any_this_frame) f1_frames++;
    }

    SpriteTestAccess::set_prepass_fix(hw, false);
    return fails;
}

// Phase (c): render_ref(fix=0) vs render() again, this time with clip
// forced to 0 (the twelve "_nc" unclipped macros, cases 4-7/12-15) over the
// densely-forced synthetic ROM, so this twin-fidelity coverage does not
// depend on the ROM content's row-boundary hazard at all (see
// sanitize_ram_for_render's comment).
static int selftest_phase_c(hwsprites& hw, const Mode& m, std::mt19937& rng, int trials)
{
    int fails = 0;
    PixelBuffer A, B;
    A.allocate(config.s16_width, config.s16_height);
    B.allocate(config.s16_width, config.s16_height);
    const std::size_t n = A.halfwords;
    std::vector<uint16_t> ram(SPR_RAM_SIZE);

    for (int trial = 0; trial < trials; trial++) {
        fill_random(ram.data(), ram.size(), rng);
        sanitize_ram_for_render(ram, rng, /*force_clip=*/false);
        hw.load_sprite_ram(ram.data());

        fill_random(A.base, n, rng);
        std::memcpy(B.base, A.base, n * sizeof(uint16_t));

        hw.render(A.pixels, 8);
        SpriteTestAccess::render_ref(hw, B.pixels, 8);

        if (std::memcmp(A.base, B.base, n * sizeof(uint16_t)) != 0) {
            report_mismatch("render vs render_ref(fix=0) [unclipped]", m.name, 2, trial, A.base, B.base, n, config.s16_width, config.s16_height);
            fails++;
        }
    }
    return fails;
}

static bool run_selftest(const std::string& roms_dir, unsigned seed)
{
    // selftest deliberately does NOT use --roms (real or the sparsely-forced
    // synthetic fallback) for its own rendering: with addr/pitch/bank picked
    // independently of any real sprite boundary (there being no real engine
    // here to guarantee that relationship), a row can go many words without
    // a terminator regardless of ROM source, in which case
    // sprites_shadowinfo's per-row summary (which only scans a row's own
    // `pitch`-many words) and the actual draw scan (bounded only by the
    // screen edge or nothing at all, not by pitch) disagree about where the
    // row ends -- observed here as a genuine, rare (roughly 1/20000 trials)
    // phase-b mismatch with both real ROM bytes and the sparse synthetic
    // ROM before this fix, in neither case a bug in render_px/parse_record/
    // draw_sprite_px (which examine every nibble directly and are
    // unaffected). The densely-forced synthetic ROM (every word, not just
    // every 6th) guarantees a terminator inside word 1 always, for any
    // pitch, eliminating this class of disagreement outright, so all three
    // phases below use it. --roms is still accepted (used by `replay`/
    // `golden`, which load real captured frames from real gameplay where
    // this hazard cannot arise) and reported for the record.
    std::vector<uint8_t> rom;
    bool used_real_roms = load_sprite_roms(roms_dir, rom, seed);
    std::vector<uint8_t> dense_rom = build_dense_synthetic_rom(seed);

    std::printf("sprite_test selftest: arch=%s seed=%u (uses a densely-forced synthetic "
                "ROM regardless of --roms=%s; see run_selftest's comment)\n",
                CB_ARCH_NAME, seed, used_real_roms ? "real" : "synthetic");

    int failures = 0;
    long long f1_records = 0, f1_frames = 0;
    const int trials_a = 300;
    const int trials_b = 5000; // x4 modes = 20,000 total, matching S0.7
    const int trials_c = 200;

    std::mt19937 rng(seed);

    for (const Mode& m : kModes) {
        set_mode(m);

        hwsprites hw;
        hw.init(dense_rom.data());
        SpriteTestAccess::reset_ref_tables(hw);
        hw.set_x_clip(false); // full width -- driver default clip window

        int fails_a = selftest_phase_a(hw, m, rng, trials_a);
        long long mode_f1_records = 0, mode_f1_frames = 0;
        int fails_b = selftest_phase_b(hw, m, rng, trials_b, mode_f1_records, mode_f1_frames);
        f1_records += mode_f1_records;
        f1_frames  += mode_f1_frames;

        hwsprites hw_dense;
        hw_dense.init(dense_rom.data());
        SpriteTestAccess::reset_ref_tables(hw_dense);
        hw_dense.set_x_clip(false);
        int fails_c = selftest_phase_c(hw_dense, m, rng, trials_c);

        failures += fails_a + fails_b + fails_c;

        std::printf("  mode=%-13s width=%4u height=%4u  twin(fix=0):%s [%d/%d]  oracle(fix=1):%s [%d/%d]  unclipped-twin:%s [%d/%d]  F1-eligible records=%lld/%d frames\n",
                    m.name, config.s16_width, config.s16_height,
                    fails_a ? "FAIL" : "ok", fails_a, trials_a,
                    fails_b ? "FAIL" : "ok", fails_b, trials_b,
                    fails_c ? "FAIL" : "ok", fails_c, trials_c,
                    mode_f1_records, trials_b);
        (void)mode_f1_frames;
    }

    std::printf("selftest: %lld F1-eligible records seen across %lld/%d frames (all modes, %d trials/mode phase-b)\n",
                f1_records, f1_frames, trials_b * 4, trials_b);

    if (failures == 0) {
        std::printf("selftest PASS (arch=%s)\n", CB_ARCH_NAME);
        return true;
    }
    std::printf("selftest FAIL: %d mismatch(es)\n", failures);
    return false;
}

// ---------------------------------------------------------------------------
// -dumpspr capture format (S0.6), read-side. Written by Video::prepare_frame
// under CB_SPR_DUMP; see video.cpp for the write side and the authoritative
// field-by-field format comment.
// ---------------------------------------------------------------------------
struct SprDumpFrame
{
    uint16_t s16_width = 0, s16_height = 0, s16_x_off = 0;
    uint8_t  hires = 0, widescreen = 0, hiresprites = 0;
    uint8_t  fg_enabled = 0, glow = 0;
    uint16_t x1 = 0, x2 = 0;
    uint32_t frame_no = 0;
    std::vector<uint16_t> ram;   // 0x800 halfwords
    std::vector<uint16_t> pre;   // s16_width*s16_height halfwords
    std::vector<uint16_t> post;  // s16_width*s16_height halfwords
};

static bool load_sprdump_frame(const std::string& path, SprDumpFrame& f)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    unsigned char hdr[28];
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!in || in.gcount() != (std::streamsize)sizeof(hdr)) return false;
    if (std::memcmp(hdr, "CBSPRD01", 8) != 0) return false;

    auto rd16 = [&](int off) { return (uint16_t)(hdr[off] | (hdr[off + 1] << 8)); };
    auto rd32 = [&](int off) {
        return (uint32_t)hdr[off] | ((uint32_t)hdr[off + 1] << 8) |
               ((uint32_t)hdr[off + 2] << 16) | ((uint32_t)hdr[off + 3] << 24);
    };

    f.s16_width    = rd16(8);
    f.s16_height   = rd16(10);
    f.s16_x_off    = rd16(12);
    f.hires        = hdr[14];
    f.widescreen   = hdr[15];
    f.hiresprites  = hdr[16];
    f.fg_enabled   = hdr[17];
    f.glow         = hdr[18];
    f.x1           = rd16(20);
    f.x2           = rd16(22);
    f.frame_no     = rd32(24);

    f.ram.resize(0x800);
    in.read(reinterpret_cast<char*>(f.ram.data()), 0x800 * 2);
    if (!in) return false;

    const std::size_t px = (std::size_t)f.s16_width * f.s16_height;
    f.pre.resize(px);
    f.post.resize(px);
    in.read(reinterpret_cast<char*>(f.pre.data()), px * 2);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(f.post.data()), px * 2);
    if (!in) return false;

    return true;
}

static std::vector<std::string> list_sprdump_files(const std::string& dir)
{
    std::vector<std::string> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return files;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind("spr_", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".bin")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// replay (S0.7): the F1/F2 classifier.
// ---------------------------------------------------------------------------

// F1: this (y,x) lies on a destination row d of `rec` such that the row
// loop's accumulator reads back exactly sprite_height (fact 1) -- i.e. d is
// among the *last* rows the shipped pre-pass would have left unbuilt. We
// recompute, for the given record, every d whose closed-form source row
// src(d) = (d*zoom)>>9 equals the shipped sprite_height (bit-for-bit,
// including the signed-steps cast the shipped formula performs), and check
// whether y == top + ydelta*d for one of them.
static bool f1_row_matches(const SpriteTestAccess::SpriteRec& rec, int y)
{
    const int32_t steps = rec.H; // == ytarget - top, signed, exactly render()'s "steps"
    const uint32_t sprite_height = (uint32_t)(((uint64_t)(int64_t)steps * (uint64_t)rec.zoom) >> 9);
    const int32_t nsteps = (rec.ydelta > 0) ? rec.H : -rec.H;
    if (nsteps <= 0) return false;
    for (int32_t d = 0; d < nsteps; d++) {
        const uint32_t src_d = (uint32_t)(((uint64_t)d * (uint64_t)rec.zoom) >> 9);
        if (src_d == sprite_height) {
            const int32_t yy = rec.top + rec.ydelta * d;
            if (yy == y) return true;
        } else if (src_d > sprite_height) {
            break; // src(d) is non-decreasing; no further d can match
        }
    }
    return false;
}

// F2 (fact 6): a clip==0 sprite's unclipped fast path can write a few
// pixels past its row's edge; because the frame buffer is one flat
// row-major array, those pixels land in the *adjacent* row's opposite end.
// Per SPRITE_IMPLEMENTATION_SPEC.md S0.7, re-render `rec` alone with
// render_px and a clip window widened by 16 pixels on each side, and check
// whether the "unwrapped" position -- (y-1, x+width) for x in the last 16
// columns, (y+1, x-width) for x in the first 16 -- carries exactly the
// shipped value once written at its *wrapped* (observed) position.
//
// Implementation note: the widened window is realised by temporarily
// setting hw's x1/x2 (SpriteTestAccess::set_clip); since those are
// uint16_t, a widened left edge cannot go below 0, so x1 is clamped there
// rather than allowed to go negative -- a minor loss of reach right at
// x1==0 that does not affect the ordinary case. The row/column pairing
// below follows the spec text verbatim; SPRITE_IMPLEMENTATION_SPEC.md's own
// derivation was not independently re-derivable from flat-index arithmetic
// during this implementation (flagged in the task report as a low-
// confidence area -- unexercised by any of this task's required
// verification, since it needs a real capture of a sprite actively
// crossing a screen edge).
static bool f2_check(hwsprites& hw, const SpriteTestAccess::SpriteRec& rec,
                      int y, int x, uint16_t shipped_value,
                      uint16_t s16_width, uint16_t s16_height, uint16_t x1, uint16_t x2)
{
    if (!(x >= (int)s16_width - 16 || x < 16)) return false;

    int uy, ux;
    if (x >= (int)s16_width - 16) { uy = y - 1; ux = x + (int)s16_width; }
    else                          { uy = y + 1; ux = x - (int)s16_width; }
    if (uy < 0 || uy >= (int)s16_height) return false;

    PixelBuffer scratch;
    scratch.allocate(s16_width, s16_height);
    std::memset(scratch.base, 0, scratch.halfwords * sizeof(uint16_t));

    const uint16_t wx1 = (x1 >= 16) ? (uint16_t)(x1 - 16) : (uint16_t)0;
    const uint16_t wx2 = (uint16_t)std::min<int>(65535, (int)x2 + 16);
    SpriteTestAccess::set_clip(hw, wx1, wx2);
    SpriteTestAccess::draw_sprite_px(hw, rec, scratch.pixels, false);
    SpriteTestAccess::set_clip(hw, x1, x2); // restore

    if (ux < 0 || ux >= (int)s16_width) return false; // outside what the widened window can reach with a uint16_t x1
    const uint16_t got = scratch.pixels[(std::size_t)uy * s16_width + ux];
    return got == shipped_value;
}

static bool run_replay(const std::string& roms_dir, const std::string& frames_dir, unsigned seed, int replay_mode)
{
    std::vector<std::string> files = list_sprdump_files(frames_dir);
    if (files.empty()) {
        std::printf("replay: no capture files found under '%s' (expected spr_NNNNNN.bin, "
                     "CB_SPR_DUMP/-dumpspr format -- see video.cpp). Nothing to replay.\n",
                     frames_dir.c_str());
        return true;
    }

    std::vector<uint8_t> rom;
    bool used_real_roms = load_sprite_roms(roms_dir, rom, seed);
    std::printf("replay: %zu capture file(s), roms=%s, replay_mode=%d\n", files.size(),
                used_real_roms ? "real (--roms)" : "synthetic (WARNING: captures made with real ROMs will not replay correctly)",
                replay_mode);

    long long total_f1 = 0, total_f2 = 0, total_unclassified = 0, total_diff = 0;
    long long f1_sprites = 0, f2_sprites = 0;
    bool any_fail = false;

    for (const std::string& path : files) {
        SprDumpFrame cap;
        if (!load_sprdump_frame(path, cap)) {
            std::printf("replay: '%s': failed to read/parse -- skipped\n", path.c_str());
            continue;
        }

        config.s16_width  = cap.s16_width;
        config.s16_height = cap.s16_height;
        config.s16_x_off  = cap.s16_x_off;
        config.video.hires = cap.hires;
        config.video.hiresprites = cap.hiresprites;

        hwsprites hw;
        hw.init(rom.data());
        SpriteTestAccess::set_clip(hw, cap.x1, cap.x2);
        hw.load_sprite_ram(cap.ram.data());
        hw.set_replay_mode(replay_mode);

        const std::size_t px = (std::size_t)cap.s16_width * cap.s16_height;

        // A = function under test (S1's render()) on a copy of the captured pre-frame.
        std::vector<uint16_t> A(cap.pre);
        hw.render(A.data(), 8);

        if (std::memcmp(A.data(), cap.post.data(), px * 2) == 0) {
            std::printf("  %-40s exact match (no F1/F2)\n", path.c_str());
            continue;
        }

        // E = the oracle, glow-off, on the same starting frame. Note: unlike
        // an earlier draft of this classifier, A (the function under test,
        // run fresh) disagreeing with E here is *not* itself treated as a
        // failure -- F1 (SPRITE_IMPLEMENTATION_SPEC.md fact 1) is exactly
        // "a destination row reads its source row back before that row's
        // lazy build ever ran," which is the DEFAULT outcome for any record
        // whose own sprite_height boundary condition is met, unless some
        // earlier sprite in the session happened to already touch that ROM
        // address; a single fresh render() call (no replayable prior-frame
        // history) hits this on its own merits whenever a record's own
        // condition is met, not only across frames. Confirmed empirically
        // here: render()'s value at a given F1-affected pixel varies frame
        // to frame (depends on what else the real session touched before
        // capture), while render_px's value at the same pixel is constant --
        // exactly F1's signature, and exactly why every A/captured_post
        // difference must go through the classifier below rather than being
        // gated on A==E first. The A-vs-E diff count is still reported, as
        // a coarse indicator of how much of a frame is F1-affected.
        std::vector<uint16_t> E(cap.pre);
        hw.render_px(E.data(), 8, false);
        long long a_vs_e_diff = 0;
        for (std::size_t i = 0; i < px; i++) if (A[i] != E[i]) a_vs_e_diff++;

        // Parse every priority-8 record once for the F1/F2 checks below.
        std::vector<SpriteTestAccess::SpriteRec> recs;
        for (uint16_t d = 0; d < 0x800; d += 16) {
            if ((cap.ram[d + 0] & 0x8000) != 0) break;
            SpriteTestAccess::SpriteRec r;
            if (SpriteTestAccess::parse_record(hw, d, 8, r))
                recs.push_back(r);
        }

        long long frame_f1 = 0, frame_f2 = 0, frame_unclassified = 0;
        bool sprite_hit_f1[1] = { false }; (void)sprite_hit_f1;
        std::vector<char> rec_is_f1(recs.size(), 0), rec_is_f2(recs.size(), 0);

        for (std::size_t i = 0; i < px; i++) {
            if (A[i] == cap.post[i]) continue;
            total_diff++;
            const int y = (int)(i / cap.s16_width);
            const int x = (int)(i % cap.s16_width);
            const uint16_t shipped_value = cap.post[i];

            bool classified = false;
            for (std::size_t ri = 0; ri < recs.size() && !classified; ri++) {
                if (f1_row_matches(recs[ri], y)) {
                    frame_f1++; rec_is_f1[ri] = 1; classified = true;
                }
            }
            for (std::size_t ri = 0; ri < recs.size() && !classified; ri++) {
                if (!recs[ri].clip &&
                    f2_check(hw, recs[ri], y, x, shipped_value, cap.s16_width, cap.s16_height, cap.x1, cap.x2)) {
                    frame_f2++; rec_is_f2[ri] = 1; classified = true;
                }
            }
            if (!classified) frame_unclassified++;
        }

        for (char c : rec_is_f1) if (c) f1_sprites++;
        for (char c : rec_is_f2) if (c) f2_sprites++;
        total_f1 += frame_f1;
        total_f2 += frame_f2;
        total_unclassified += frame_unclassified;

        std::printf("  %-40s diff=%lld F1_px=%lld F2_px=%lld unclassified=%lld (vs fresh oracle: %lld px)\n",
                    path.c_str(), frame_f1 + frame_f2 + frame_unclassified, frame_f1, frame_f2, frame_unclassified,
                    a_vs_e_diff);
        if (frame_unclassified != 0) any_fail = true;
    }

    std::printf("replay: totals diff=%lld F1_px=%lld F2_px=%lld unclassified=%lld "
                "F1_sprites=%lld F2_sprites=%lld\n",
                total_diff, total_f1, total_f2, total_unclassified, f1_sprites, f2_sprites);
    if (any_fail) {
        std::printf("replay FAIL: at least one unclassified pixel\n");
        return false;
    }
    std::printf("replay PASS: every shipped-vs-oracle difference attributed to F1 or F2\n");
    return true;
}

// ---------------------------------------------------------------------------
// golden (S0.7): FNV-1a 64 (video.cpp's rolling-hash form) of render_px
// output per captured frame, both glow values.
// ---------------------------------------------------------------------------
static uint64_t fnv1a64(const uint16_t* p, std::size_t n, uint64_t h = 1469598103934665603ULL)
{
    for (std::size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static bool run_golden(const std::string& roms_dir, const std::string& frames_dir, unsigned seed)
{
    std::vector<std::string> files = list_sprdump_files(frames_dir);
    if (files.empty()) {
        std::printf("golden: no capture files found under '%s' -- nothing to hash. "
                     "See video.cpp / -dumpspr for how to produce a capture set, then "
                     "check the printed hashes into golden/<set>.txt.\n", frames_dir.c_str());
        return true;
    }

    std::vector<uint8_t> rom;
    load_sprite_roms(roms_dir, rom, seed);

    for (const std::string& path : files) {
        SprDumpFrame cap;
        if (!load_sprdump_frame(path, cap)) continue;

        config.s16_width  = cap.s16_width;
        config.s16_height = cap.s16_height;
        config.s16_x_off  = cap.s16_x_off;
        config.video.hires = cap.hires;
        config.video.hiresprites = cap.hiresprites;

        hwsprites hw;
        hw.init(rom.data());
        SpriteTestAccess::set_clip(hw, cap.x1, cap.x2);
        hw.load_sprite_ram(cap.ram.data());

        const std::size_t px = (std::size_t)cap.s16_width * cap.s16_height;
        std::vector<uint16_t> off(cap.pre), on(cap.pre);
        hw.render_px(off.data(), 8, false);
        hw.render_px(on.data(), 8, true);

        std::printf("%s  glow_off=0x%016llx  glow_on=0x%016llx\n", path.c_str(),
                    (unsigned long long)fnv1a64(off.data(), px),
                    (unsigned long long)fnv1a64(on.data(), px));
    }
    return true;
}

// ---------------------------------------------------------------------------
// unit s1a (S1.4): the schedule table equals the closed form, and the
// closed-form vertical-loop source row equals the shipped accumulator's
// row sequence, for every zoom 1..0xfff.
// ---------------------------------------------------------------------------
static bool run_unit_s1a()
{
    hwsprites hw;
    hw.init(nullptr);
    int fails = 0;
    const int32_t sched_len = SpriteTestAccess::sched_len();
    const int32_t fast_zoom_min = SpriteTestAccess::fast_zoom_min();

    // Spec-ambiguity note (S1.4): sched_tab is uint8_t (S1.2's own words),
    // so a run value above 255 -- unavoidable for zoom < 2 (run up to 512)
    // -- silently truncates mod 256. sched() is never called below
    // SPR_FAST_ZOOM_MIN in production (render()'s dispatch routes those
    // records to render_px instead, S1.2), and S1.2 itself only asserts
    // "r <= 4 for zoom >= 0x80" -- i.e. the table's documented valid domain
    // already excludes zoom < 0x80. Read literally, S1.4's "for every zoom
    // 1..0xfff" would require testing run[] against the ceil formula in a
    // domain sched()'s own chosen storage type cannot represent; resolved
    // by scoping the run[]-vs-formula check to sched()'s actual domain
    // [SPR_FAST_ZOOM_MIN, 0xfff] (confirmed: every value fails below that
    // domain, purely from the uint8_t truncation, before this fix). The
    // closed-form/accumulator j-sequence check below does not touch
    // sched_tab at all, so it stays exhaustive over the full 1..0xfff.
    for (uint32_t zoom = (uint32_t)fast_zoom_min; zoom <= 0xfff; zoom++) {
        // (1) run[i] == ceil(512*(i+1)/zoom) - ceil(512*i/zoom).
        const uint8_t* run = SpriteTestAccess::sched(hw, zoom);
        for (int32_t i = 0; i < sched_len && fails < 20; i++) {
            const uint64_t ceil_hi = (512ULL * (uint64_t)(i + 1) + zoom - 1) / zoom;
            const uint64_t ceil_lo = (512ULL * (uint64_t)i + zoom - 1) / zoom;
            const uint64_t want = ceil_hi - ceil_lo;
            if ((uint64_t)run[i] != want) {
                std::printf("  s1a FAIL sched: zoom=0x%03x i=%d run=%u want=%llu\n",
                            zoom, i, run[i], (unsigned long long)want);
                fails++;
            }
        }
    }

    for (uint32_t zoom = 1; zoom <= 0xfff; zoom++) {
        // (2) closed-form src(d) == the shipped accumulator's row sequence,
        // for d < 1024 (hwsprites.cpp's yacc/addr row-stepping recurrence).
        uint32_t yacc = 0, jrow = 0;
        for (uint32_t d = 0; d < 1024 && fails < 20; d++) {
            const uint32_t j_closed = (uint32_t)(((uint64_t)d * zoom) >> 9);
            if (jrow != j_closed) {
                std::printf("  s1a FAIL j-seq: zoom=0x%03x d=%u accumulator=%u closed=%u\n",
                            zoom, d, jrow, j_closed);
                fails++;
            }
            yacc += zoom;
            jrow += (yacc >> 9);
            yacc &= 0x1ff;
        }
    }

    if (fails == 0) { std::printf("unit s1a PASS (zoom 1..0xfff exhaustive)\n"); return true; }
    std::printf("unit s1a FAIL: %d mismatch(es) (further reports suppressed)\n", fails);
    return false;
}

// ---------------------------------------------------------------------------
// unit s1b (S1.4): nibrev(w) nibble k == w nibble 7-k, for all 2^32 words.
// ---------------------------------------------------------------------------
static inline uint32_t reference_nibrev(uint32_t w)
{
    // Independent (non-bswap-based) per-nibble reversal, deliberately not
    // sharing hwsprites::nibrev's bit-trick implementation.
    uint32_t r = 0;
    for (int k = 0; k < 8; k++) {
        const uint32_t nib = (w >> (4 * (7 - k))) & 0xFu;
        r |= nib << (4 * k);
    }
    return r;
}

static bool run_unit_s1b()
{
    std::printf("unit s1b: exhaustive nibrev over 2^32 words (this takes a while)...\n");
    std::fflush(stdout);
    uint64_t w = 0;
    int fails = 0;
    do {
        const uint32_t got = SpriteTestAccess::nibrev((uint32_t)w);
        const uint32_t want = reference_nibrev((uint32_t)w);
        if (got != want) {
            std::printf("  s1b FAIL: w=0x%08x got=0x%08x want=0x%08x\n", (uint32_t)w, got, want);
            if (++fails >= 20) break;
        }
        w++;
    } while (w <= 0xFFFFFFFFull);

    if (fails == 0) { std::printf("unit s1b PASS (2^32 words exhaustive)\n"); return true; }
    std::printf("unit s1b FAIL: %d mismatch(es) (further reports suppressed)\n", fails);
    return false;
}

// ---------------------------------------------------------------------------
// unit s1c (S1.4): decode_row's rec[]/nrec equal the maximal-run partition
// of the row it actually expanded, over 100,000 random (bank/addr/pitch/
// flip/shadow/zoom) rows. The reference partition is built independently:
// it re-walks the same ROM words (same rules: terminator, SPR_WMAX bound,
// bank-range check, nibrev mirroring) but writes each expanded position's
// type directly into a flat array and then scans for maximal runs, instead
// of decode_row's incremental emit()-with-lookback bookkeeping -- so this
// specifically exercises whether emit()'s drop/merge logic reproduces the
// obviously-correct batch answer.
// ---------------------------------------------------------------------------
struct RefRecord { int32_t pos; uint8_t type; };

static std::vector<RefRecord> reference_row_records(hwsprites& hw, const SpriteTestAccess::SpriteRec& r,
                                                      int32_t j, int32_t pend)
{
    const uint8_t* run = SpriteTestAccess::sched(hw, (uint32_t)r.zoom);
    uint32_t* sprites = SpriteTestAccess::sprites_ptr(hw);
    const int64_t slen = (int64_t)SpriteTestAccess::sprites_length();
    const int32_t wmax = SpriteTestAccess::wmax();
    const int32_t lb_len = SpriteTestAccess::lb_len();

    std::vector<uint8_t> types(lb_len, SpriteTestAccess::type_gap());
    std::vector<uint8_t> painted(lb_len, 0);

    const int64_t rowbase = (int64_t)r.bank * 0x10000 + r.addr + (int64_t)r.pitch * j;
    int32_t pos = 0, words = 0;
    for (;;) {
        if (pos >= pend || words == wmax) break;
        const int64_t widx = r.flip ? (rowbase + r.pitch - 1 - words) : (rowbase + words);
        if (widx < 0 || widx >= slen) break;
        uint32_t w = sprites[widx];
        if (r.flip) w = SpriteTestAccess::nibrev(w);

        for (int k = 0; k < 8; k++) {
            const unsigned n = (w >> (28 - 4 * k)) & 0xF;
            const uint8_t t = (n == 0 || n == 15) ? SpriteTestAccess::type_gap()
                             : ((r.shadow && n == 10) ? SpriteTestAccess::type_shadow() : SpriteTestAccess::type_colour());
            const int32_t rl = run[words * 8 + k];
            for (int32_t q = 0; q < rl && pos + q < lb_len; q++) {
                types[pos + q] = t;
                painted[pos + q] = 1;
            }
            pos += rl;
        }
        words++;
        if ((w & 0xF0) == 0xF0) break;
    }

    const int32_t final_pos = std::min(pos, lb_len);
    std::vector<RefRecord> recs;
    // decode_row's emit() starts implicitly in the GAP state (prev = GAP,
    // no record actually stored for it) and only appends a record on a
    // transition -- so a row that *begins* with a gap gets no leading {0,
    // GAP} record either. Match that here (prev starts at GAP, not an
    // out-of-band sentinel), or every gap-first row would show a spurious
    // extra record that decode_row correctly omits.
    uint8_t prev = SpriteTestAccess::type_gap();
    for (int32_t p = 0; p < final_pos; p++) {
        const uint8_t t = painted[p] ? types[p] : SpriteTestAccess::type_gap();
        if (t != prev) { recs.push_back({p, t}); prev = t; }
    }
    // Trailing sentinel: decode_row's own emit(GAP, pos) call after the word
    // loop applies the same merge rule as every other emit() call -- a
    // no-op if the last real record is already GAP (including "no records
    // at all", an all-gap row: nrec stays 0, not 1).
    if (recs.empty() || recs.back().type != SpriteTestAccess::type_gap())
        recs.push_back({final_pos, SpriteTestAccess::type_gap()});
    return recs;
}

static bool run_unit_s1c(unsigned seed)
{
    std::mt19937 rom_rng(seed ^ 0x53314330u); // "S1C0" -- distinct stream from the record RNG below
    std::vector<uint8_t> rom(SPR_ROM_TOTAL);
    fill_random(reinterpret_cast<uint16_t*>(rom.data()), rom.size() / 2, rom_rng);

    hwsprites hw;
    hw.init(rom.data());
    std::mt19937 rng(seed ^ 0x53314331u); // "S1C1"

    const int trials = 100000;
    int fails = 0;

    for (int trial = 0; trial < trials && fails < 20; trial++) {
        SpriteTestAccess::SpriteRec r{};
        r.bank   = (int32_t)(rng() % 4);
        r.addr   = (int32_t)(rng() % 0x8000);           // stays in-range across all reads below
        r.pitch  = (int32_t)(1 + rng() % 40);
        r.flip   = (int32_t)(rng() % 2);
        r.shadow = (uint8_t)(rng() % 2);
        // Zoom spans both regimes runs 1-4 and runs 0-2 (S1.4's "runs 0-4"),
        // always inside decode_row's real envelope (>= SPR_FAST_ZOOM_MIN) --
        // below that, line[]'s per-nibble run can exceed decode_row's fixed
        // margin, which is only ever exercised via draw_sprite_px, not
        // decode_row directly.
        const unsigned band = rng() % 5;
        uint32_t zoom;
        if      (band == 0) zoom = 0x80  + rng() % (0x1F8 - 0x80 + 1);
        else if (band == 1) zoom = 0x100 + rng() % (0x3F0 - 0x100 + 1);
        else if (band == 2) zoom = 0x200;
        else if (band == 3) zoom = 0x401 + rng() % (0x7ff - 0x401 + 1);
        else                zoom = 0x80  + rng() % (0xfff - 0x80 + 1);
        r.zoom  = (int32_t)zoom;
        r.color = 0x800;

        const int32_t pend = (int32_t)(20 + rng() % 3000); // well under SPR_PEND_MAX

        SpriteTestAccess::decode_row(hw, r, /*j=*/0, pend);
        const int32_t got_n = SpriteTestAccess::nrec(hw);

        std::vector<RefRecord> ref = reference_row_records(hw, r, /*j=*/0, pend);

        // Compare the SPANS the two record lists would replay (consecutive
        // non-GAP (pos_k, pos_k+1) pairs), not the raw record arrays
        // byte-for-byte. Spec-ambiguity note (S1.4): emit()'s own drop rule
        // (verbatim from S1.2) can leave a harmless single trailing record
        // with no partner to pair with -- e.g. a row whose only painted
        // pixels are GAP-type throughout, where the final emit(GAP, pos)
        // call's drop cancels the last real transition but nrec lands on 1
        // instead of 0 (confirmed by tracing decode_row's actual emit
        // sequence nibble-by-nibble against this same reference walk: both
        // computed the identical (pos, type) trajectory throughout; only
        // the bookkeeping's dangling entry differed). replay()'s own loop
        // (`for (k = 0; k + 1 < nrec; k++)`) never pairs that lone entry
        // with anything, so it produces zero spans -- functionally
        // identical to nrec == 0. "Records equal the maximal runs" is
        // therefore verified at the level that actually matters for S2's
        // glow (which spans replay() emits), not the literal record count.
        auto spans_of = [](auto get_pos, auto get_type, int32_t n) {
            std::vector<std::tuple<int32_t,int32_t,uint8_t>> out;
            for (int32_t k = 0; k + 1 < n; k++) {
                const uint8_t t = get_type(k);
                const int32_t lo = get_pos(k), hi = get_pos(k + 1);
                if (t != SpriteTestAccess::type_gap() && hi > lo) out.push_back({lo, hi, t});
            }
            return out;
        };
        auto got_spans = spans_of(
            [&](int32_t i) { return (int32_t)SpriteTestAccess::rec_at(hw, i).pos; },
            [&](int32_t i) { return SpriteTestAccess::rec_at(hw, i).type; },
            got_n);
        auto ref_spans = spans_of(
            [&](int32_t i) { return ref[(size_t)i].pos; },
            [&](int32_t i) { return ref[(size_t)i].type; },
            (int32_t)ref.size());

        if (got_spans != ref_spans) {
            fails++;
            std::printf("  s1c FAIL trial=%d bank=%d addr=%d pitch=%d flip=%d shadow=%u zoom=0x%x pend=%d "
                        "nrec=%d/%zu spans=%zu/%zu\n",
                        trial, r.bank, r.addr, r.pitch, r.flip, r.shadow, r.zoom, pend, got_n, ref.size(),
                        got_spans.size(), ref_spans.size());
        }
    }

    if (fails == 0) { std::printf("unit s1c PASS (%d trials)\n", trials); return true; }
    std::printf("unit s1c FAIL: %d mismatch(es)\n", fails);
    return false;
}

static bool run_unit(const std::string& subcmd, unsigned seed)
{
    bool ok = true;
    if (subcmd.empty() || subcmd == "s1a") ok &= run_unit_s1a();
    if (subcmd.empty() || subcmd == "s1b") ok &= run_unit_s1b();
    if (subcmd.empty() || subcmd == "s1c") ok &= run_unit_s1c(seed);
    return ok;
}

// ---------------------------------------------------------------------------
// fuzz (S1.4): n random sprite lists (1-122 records), edge-biased, run()
// (the function under test) vs render_px(glow=false), whole-allocation
// memcmp, across all four modes and both replay_mode values. Backgrounds:
// seeded random halfwords < 0x2000 with bit 12 set in stripes.
// ---------------------------------------------------------------------------
static uint32_t fuzz_rand_zoom(std::mt19937& rng)
{
    const unsigned r = rng() % 100;
    if (r < 20) return 0x80  + rng() % (0x1F8 - 0x80 + 1);   // hi-res shipping, runs 1-4
    if (r < 40) return 0x100 + rng() % (0x3F0 - 0x100 + 1);  // lo-res shipping
    if (r < 55) return 0x100 + rng() % (0x7E0 - 0x100 + 1);  // hiresprites shipping, up to 0x7E0
    if (r < 65) return 0x200;                                // all-run-1 boundary
    if (r < 80) return 0x401 + rng() % (0x7ff - 0x401 + 1);  // shrink band
    return 1 + rng() % 0xfff;                                 // broad uniform, incl. < 0x80 (render_px envelope)
}

static int32_t fuzz_rand_xpos(std::mt19937& rng, uint16_t s16_width)
{
    switch (rng() % 6) {
        case 0: return -(int32_t)(rng() % 200);
        case 1: return (int32_t)s16_width + (int32_t)(rng() % 200);
        case 2: return (int32_t)(rng() % 16);
        case 3: return (int32_t)s16_width - (int32_t)(rng() % 16) - 1;
        default: return (int32_t)(rng() % (s16_width ? s16_width : 1));
    }
}

// Builds one raw 16-halfword record at ram[d..d+15] (S0.4's field layout,
// reverse-engineered bit-for-bit from parse_record so every target field
// value -- including pitch's split representation across ram[d+2]/ram[d+4]
// -- lands exactly where intended). priority is always encoded as 8 (the
// game's only render() call site, video.cpp:350) so every generated record
// is live.
static void fuzz_build_record(std::vector<uint16_t>& ram, uint16_t d, std::mt19937& rng,
                               uint16_t s16_width, uint16_t s16_height)
{
    const uint32_t zoom      = fuzz_rand_zoom(rng);
    unsigned pitch_r         = rng() % 20;
    const uint32_t pitch     = (pitch_r == 0) ? 0u : (pitch_r == 1) ? 255u : (rng() % 256);
    const bool flip          = (rng() % 2) != 0;
    const bool xdelta_pos    = (rng() % 2) != 0; // true => xdelta == +1 (not rtl)
    const bool shadow        = (rng() % 2) != 0;
    const bool ydelta_pos    = (rng() % 100) >= 5; // 5% ydelta == -1
    const bool clip_bit      = (rng() % 2) != 0;   // stat only (S1.3); must not affect output
    const uint32_t width_val = rng() % 512;

    const int32_t top_raw = (int32_t)(rng() % ((uint32_t)s16_height + 256)) - 128; // straddles [0, s16_height)
    const uint16_t top_field = (uint16_t)(((top_raw + 0x100) & 0x1ff));
    const uint16_t height_m1 = (uint16_t)(rng() % 256);
    const uint32_t bank = rng() % 8;

    const int32_t xpos_raw = fuzz_rand_xpos(rng, s16_width);
    const uint16_t xpos_field = (uint16_t)((uint32_t)xpos_raw & 0xffffu);
    const uint16_t addr_field = (uint16_t)(rng() & 0xffffu); // uniform 16-bit: covers safe mid-bank through near-0/negative-after-flip

    const uint16_t pitch_lo7 = (uint16_t)(pitch & 0x7Fu);
    const bool pitch_hibit   = (pitch & 0x80u) != 0;

    ram[d + 0] = (uint16_t)(((bank & 7u) << 9) | top_field | (clip_bit ? 0x2000u : 0u));
    ram[d + 1] = addr_field;
    ram[d + 2] = (uint16_t)(pitch_lo7 << 9);
    ram[d + 3] = (uint16_t)((3u << 12) | (shadow ? 0x4000u : 0u) | (zoom & 0x0fffu)); // priority select == 8
    ram[d + 4] = (uint16_t)((ydelta_pos ? 0x8000u : 0u) | (flip ? 0u : 0x4000u) |
                             (xdelta_pos ? 0x2000u : 0u) | (pitch_hibit ? 0x1000u : 0u) |
                             (width_val & 0x7ffu));
    ram[d + 5] = (uint16_t)((height_m1 << 8) | (rng() % 0x80));
    ram[d + 6] = xpos_field;
    ram[d + 7] = 0;
    for (int e = 8; e < 15; e++) ram[d + e] = 0;
    ram[d + 15] = (uint16_t)(rng() & 0xffffu); // offset (hiresprites only)
}

// Backgrounds: seeded random halfwords < 0x2000, bit 12 set in stripes
// (S0.7's fuzz description).
static void fuzz_fill_background(uint16_t* buf, std::size_t n, uint16_t width, std::mt19937& rng)
{
    for (std::size_t i = 0; i < n; i++) {
        uint16_t v = (uint16_t)(rng() % 0x1000);
        const std::size_t row = width ? (i / width) : 0;
        if ((row & 7) < 3) v |= 0x1000; // stripe every few rows
        buf[i] = v;
    }
}

static bool run_fuzz_pass(int n, int replay_mode, const std::string& roms_dir, unsigned seed, bool negctrl)
{
    std::vector<uint8_t> rom;
    load_sprite_roms(roms_dir, rom, seed);
    std::mt19937 rng(seed ^ (unsigned)(0x465A5A31u + replay_mode));

    // hwsprites::init() copies the whole 1 MB ROM image into sprites[] (a
    // pure, session-invariant conversion -- render()/render_px() never
    // mutate it), and the object itself carries ~1 MB of storage (sprites[]
    // dominates); constructing/init-ing fresh per trial was the actual
    // bottleneck (not the pixel buffers below), at fuzz's target trial
    // counts. One instance, reused for every trial and every mode.
    hwsprites hw;
    hw.init(rom.data());
    hw.set_replay_mode(replay_mode);
    hw.set_glow(false);
#ifdef CB_RENDER_TEST
    SpriteTestAccess::set_debug_disable_clip(hw, negctrl);
#endif

    long long fails = 0, tested = 0;
    std::vector<uint16_t> ram(SPR_RAM_SIZE, 0);

    for (const Mode& m : kModes) {
        set_mode(m);
        hw.set_x_clip(false);

        PixelBuffer A, B;
        A.allocate(config.s16_width, config.s16_height);
        B.allocate(config.s16_width, config.s16_height);
        const std::size_t npx = A.halfwords;

        for (int trial = 0; trial < n; trial++) {
            config.video.hiresprites = (rng() % 4 == 0) ? 1 : 0; // S0.7: all (..., hiresprites) combos

            // 20% of trials additionally exercise an x1 > 0 clip window
            // (S0.7: "including x1 > 0 windows"), driver-only per S1.3.
            if (rng() % 5 == 0) {
                uint16_t half = (uint16_t)(config.s16_width / 4);
                SpriteTestAccess::set_clip(hw, half, (uint16_t)(config.s16_width - half));
            } else {
                hw.set_x_clip(false);
            }

            std::fill(ram.begin(), ram.end(), (uint16_t)0);
            const int nrecs = 1 + (int)(rng() % 122);
            for (int i = 0; i < nrecs; i++)
                fuzz_build_record(ram, (uint16_t)(i * 16), rng, config.s16_width, config.s16_height);
            if (nrecs * 16 < (int)SPR_RAM_SIZE)
                ram[(std::size_t)nrecs * 16] = 0x8000; // end-of-list marker
            hw.load_sprite_ram(ram.data());

            fuzz_fill_background(A.base, npx, config.s16_width, rng);
            std::memcpy(B.base, A.base, npx * sizeof(uint16_t));

            hw.render(A.pixels, 8);
            hw.render_px(B.pixels, 8, false);

            tested++;
            if (std::memcmp(A.base, B.base, npx * sizeof(uint16_t)) != 0) {
                fails++;
                if (fails <= 20) {
                    report_mismatch("render vs render_px", m.name, replay_mode, trial, A.base, B.base, npx,
                                     config.s16_width, config.s16_height);
                }
            }
        }
    }

    std::printf("fuzz: replay_mode=%d negctrl=%s trials=%lld(x4 modes) fails=%lld\n",
                replay_mode, negctrl ? "on" : "off", tested, fails);
    return fails == 0;
}

static bool run_fuzz(int n, const std::string& roms_dir, unsigned seed, bool negctrl)
{
    bool ok = true;
    for (int mode = 0; mode <= 1; mode++) {
        ok &= run_fuzz_pass(n, mode, roms_dir, seed, negctrl);
    }
    if (negctrl) {
        // The negative control is expected to FAIL (that is what proves the
        // harness catches overruns); invert the sense for the process's
        // own PASS/FAIL reporting.
        if (!ok) { std::printf("fuzz negative control: mismatches observed as expected -- harness OK\n"); return true; }
        std::printf("fuzz negative control FAIL: expected mismatches with clipping disabled, saw none\n");
        return false;
    }
    return ok;
}

static void print_usage(const char* argv0)
{
    std::printf(
        "usage: %s [selftest|unit|fuzz|replay|golden|bench] [subcmd] [--roms <dir>] "
        "[--frames <dir>] [--seed N] [--negctrl]\n"
        "  selftest (default)  - S0.7 twin-fidelity + oracle-fidelity check\n"
        "  unit [s1a|s1b|s1c]  - S1.4 schedule/nibrev/record-partition proofs (all three if omitted)\n"
        "  fuzz [n]            - n random sprite lists vs render_px (S1.4); --negctrl for the negative control\n"
        "  replay              - F1/F2 classifier over a -dumpspr capture set\n"
        "  golden              - print render_px FNV-1a hashes over a capture set\n"
        "  bench               - not yet implemented (S1.5)\n",
        argv0);
}

int main(int argc, char** argv)
{
    std::string roms_dir, frames_dir, cmd, subcmd;
    unsigned seed = 12345u;
    bool negctrl = false;
    int replay_mode_arg = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--roms" && i + 1 < argc)          roms_dir = argv[++i];
        else if (a == "--frames" && i + 1 < argc)   frames_dir = argv[++i];
        else if (a == "--seed" && i + 1 < argc)     seed = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--negctrl")                  negctrl = true;
        else if (a == "--replay-mode" && i + 1 < argc) replay_mode_arg = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help")        { print_usage(argv[0]); return 0; }
        else if (cmd.empty() && !a.empty() && a[0] != '-') cmd = a;
        else if (subcmd.empty() && !a.empty() && a[0] != '-') subcmd = a;
        else { std::fprintf(stderr, "unrecognised argument '%s'\n", a.c_str()); print_usage(argv[0]); return 2; }
    }

    if (cmd.empty()) cmd = "selftest";

    if (cmd == "unit") {
        return run_unit(subcmd, seed) ? 0 : 1;
    }
    if (cmd == "fuzz") {
        const int n = subcmd.empty() ? 200000 : std::atoi(subcmd.c_str());
        return run_fuzz(n, roms_dir, seed, negctrl) ? 0 : 1;
    }
    if (cmd == "bench") {
        std::printf("%s: not yet implemented (lands with S1.5's measurement pass; "
                     "in-game prof_ns_sprites/CB_SPRITE_PMU cover S1.5 in the meantime).\n", cmd.c_str());
        return 0;
    }
    if (cmd == "replay") {
        return run_replay(roms_dir, frames_dir, seed, replay_mode_arg) ? 0 : 1;
    }
    if (cmd == "golden") {
        return run_golden(roms_dir, frames_dir, seed) ? 0 : 1;
    }
    if (cmd != "selftest") {
        std::fprintf(stderr, "unknown sub-command '%s'\n", cmd.c_str());
        print_usage(argv[0]);
        return 2;
    }

    return run_selftest(roms_dir, seed) ? 0 : 1;
}
