#pragma once
/***************************************************************************
    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
***************************************************************************/

#include "stdint.hpp"

class RomLoader;

class hwtiles
{
public:
    enum
    {
        LEFT,
        RIGHT,
        CENTRE,
    };

    alignas(64) uint8_t text_ram[0x1000+4];  // Text RAM; +4 to eliminate need to check addresses on 16/32 bit writes
    alignas(64) uint8_t tile_ram[0x10000+4]; // Tile RAM; +4 to eliminate need to check addresses on 16/32 bit writes
    alignas(16) uint32_t PL0[256], PL1[256], PL2[256];

    hwtiles(void);
    ~hwtiles(void);

    void init(uint8_t* src_tiles, const bool hires);
    void patch_tiles(RomLoader* patch);
    void restore_tiles();
    void set_x_clamp(const uint16_t);
    void update_tile_values();
    // S5.2: dead_rows is Video::road_fg_rows (S16_HEIGHT lo-res-line entries,
    // 1 iff the road foreground will overwrite that line in full this frame)
    // or nullptr for no clipping. Defaulted so existing 3-arg callers (the
    // driver's selftest, the _ref twin's own dispatch) are unaffected.
    void render_tile_layer(uint16_t*, uint8_t, uint8_t, const uint8_t* dead_rows = nullptr);
    void render_text_layer(uint16_t*, uint8_t);
    void render_all_tiles(uint16_t*);

    // S0.4: unconditional accessors (needed by the game's future -dumpram, S0.5,
    // not yet wired up here). Harmless in the shipping game either way.
    int16_t get_x_clamp() const { return x_clamp; }
    uint8_t get_tile_bank(int i) const { return tile_banks[i]; }

#ifdef CB_RENDER_TEST
    // S0.4: lets the standalone test driver (tools/road_tiles/test) reach
    // private state and the _ref reference twins below without
    // "#define private public". Never defined by the game build.
    friend struct RoadTilesTestAccess;

    // S7.4: test-only entry point for tile_row_hires (a file-scope function in
    // hwtiles.cpp, not a class member) so the driver's exhaustive kernel sweep
    // can call it directly, one row at a time, bypassing the 8-row dispatch
    // and the tile/RAM plumbing entirely. Defined in hwtiles.cpp next to
    // tile_row_hires. Not compiled into the game.
    static void test_tile_row_hires(uint16_t* buf, uint32_t p0, uint32_t nPalette, uint16_t width);
#endif

private:
    int16_t x_clamp;

    // S16 Width, ignoring widescreen related scaling.
    uint16_t s16_width_noscale;

    static const int TILES_LENGTH = 0x10000;
    alignas(64) uint32_t tiles[TILES_LENGTH];        // Converted tiles
    alignas(64) uint32_t tiles_backup[TILES_LENGTH]; // Converted tiles (backup without patch)

    uint16_t page[4];
    uint16_t scroll_x[4];
    uint16_t scroll_y[4];

    uint8_t tile_banks[2];

    static const uint16_t NUM_TILES = 0x2000; // Length of graphic rom / 24
    static const uint16_t TILEMAP_COLOUR_OFFSET = 0x1c00;
    
    void (hwtiles::*render8x8_tile_mask)(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void (hwtiles::*render8x8_tile_mask_clip)(
        uint16_t *buf,
        uint16_t nTileNumber,
        int16_t StartX,
        int16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);

#ifdef CB_RENDER_TEST
    // S0.3: kernel pointer pair the _ref layer twins dispatch through, set in
    // hwtiles::init next to render8x8_tile_mask/render8x8_tile_mask_clip above,
    // so a ref scan never calls a new (post-S0) kernel.
    void (hwtiles::*render8x8_tile_mask_ref)(
        uint16_t *buf,
        uint16_t nTileNumber,
        uint16_t StartX,
        uint16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);

    void (hwtiles::*render8x8_tile_mask_clip_ref)(
        uint16_t *buf,
        uint16_t nTileNumber,
        int16_t StartX,
        int16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);
#endif

    void render8x8_tile_mask_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 

    void render8x8_tile_mask_clip_lores(
        uint16_t *buf,
        uint16_t nTileNumber, 
        int16_t StartX, 
        int16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset);
        
    void render8x8_tile_mask_hires(
        uint16_t *buf,
        uint16_t nTileNumber, 
        uint16_t StartX, 
        uint16_t StartY, 
        uint16_t nTilePalette, 
        uint16_t nColourDepth, 
        uint16_t nMaskColour, 
        uint16_t nPaletteOffset); 
        
    void render8x8_tile_mask_clip_hires(
        uint16_t *buf,
        uint16_t nTileNumber,
        int16_t StartX,
        int16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);

#ifdef CB_RENDER_TEST
    // S0.3: verbatim reference twins, as these six functions stood on this
    // branch before any optimisation section was applied (render_tile_layer and
    // render_text_layer are already post-S1, which is correct: the twin is the
    // reference for sections AFTER S1, not before it). Not compiled into the
    // game; every future section is checked against these.
    void render_tile_layer_ref(uint16_t*, uint8_t, uint8_t);
    void render_text_layer_ref(uint16_t*, uint8_t);

    void render8x8_tile_mask_hires_ref(
        uint16_t *buf,
        uint16_t nTileNumber,
        uint16_t StartX,
        uint16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);

    void render8x8_tile_mask_clip_hires_ref(
        uint16_t *buf,
        uint16_t nTileNumber,
        int16_t StartX,
        int16_t StartY,
        uint16_t nTilePalette,
        uint16_t nColourDepth,
        uint16_t nMaskColour,
        uint16_t nPaletteOffset);
#endif

};
