/***************************************************************************
    Sprite Handling Routines.
    
    - Initializing Sprites from level data.
    - Mapping palettes to sprites.
    - Ordering sprites by priority.
    - Adding shadows to sprites where appropriate.
    - Clipping sprites based on priority in relation to road hardware.
    - Conversion from internal format to output format required by hardware.
    
    Copyright Chris White.
    See license.txt for more details.

    Modifications for Cannonball-SE are Copyright (c) 2025, 2026 James Pearce.
***************************************************************************/

#include <cstdlib> // std::abs (integer overloads)

#include "../trackloader.hpp"

#include "engine/oanimseq.hpp"
#include "engine/ocrash.hpp"
#include "engine/oferrari.hpp"
#include "engine/olevelobjs.hpp"
#include "engine/osprites.hpp"
#include "engine/otraffic.hpp"
#include "engine/data/ozoom_lookup.hpp"

OSprites osprites;

OSprites::OSprites(void)
{
}

OSprites::~OSprites(void)
{
}

void OSprites::init()
{
    // Set activated number of sprites based on config
    no_sprites = config.engine.level_objects ? SPRITE_ENTRIES : 0x4F;

    // Also handled by clear_palette_data() now
    for (uint16_t i = 0; i < PAL_LOOKUP_LENGTH; i++)
        pal_lookup[i] = 0;

    for (uint16_t i = 0; i < 0x2000; i++)
    {
        sprite_order[i] = 0;
        sprite_order2[i] = 0;
    }

    // Reset hardware entries
    for (uint16_t i = 0; i < JUMP_ENTRIES_TOTAL; i++)
        sprite_entries[i].init();

    for (uint8_t i = 0; i < SPRITE_ENTRIES; i++)
        jump_table[i].init(i);

    // Ferrari + Passenger Sprites
    jump_table[SPRITE_FERRARI].init(SPRITE_FERRARI);        // Ferrari
    jump_table[SPRITE_PASS1].init(SPRITE_PASS1);
    jump_table[SPRITE_PASS2].init(SPRITE_PASS2);
    jump_table[SPRITE_SHADOW].init(SPRITE_SHADOW);          // Shadow Settings
    jump_table[SPRITE_SHADOW].shadow = 7;
    jump_table[SPRITE_SMOKE1].init(SPRITE_SMOKE1);
    jump_table[SPRITE_SMOKE2].init(SPRITE_SMOKE2);

    oferrari.init(&jump_table[SPRITE_FERRARI], &jump_table[SPRITE_PASS1], &jump_table[SPRITE_PASS2], &jump_table[SPRITE_SHADOW]);
    
    // ------------------------------------------------------------------------
    // Traffic in Right Hand Lane At Start of Stage 1
    // ------------------------------------------------------------------------

    for (uint8_t i = SPRITE_TRAFF1; i <= SPRITE_TRAFF8; i++)
    {
        jump_table[i].init(i);      
        jump_table[i].control |= SHADOW;
        jump_table[i].addr = outrun.adr.sprite_porsche; // Initial offset of traffic sprites. Will be changed.
    }

    // ------------------------------------------------------------------------
    // Crash Sprites
    // ------------------------------------------------------------------------

    for (uint8_t i = SPRITE_CRASH; i <= SPRITE_CRASH_PASS2_S; i++)
    {
        jump_table[i].init(i);
    }

    jump_table[SPRITE_CRASH_PASS1].draw_props = oentry::BOTTOM;
    jump_table[SPRITE_CRASH_PASS2].draw_props = oentry::BOTTOM;

    jump_table[SPRITE_CRASH_PASS1_S].shadow     = 7;
    jump_table[SPRITE_CRASH_PASS1_S].addr       = outrun.adr.shadow_data;
    jump_table[SPRITE_CRASH_PASS2_S].shadow     = 7;
    jump_table[SPRITE_CRASH_PASS2_S].draw_props = oentry::BOTTOM;
    jump_table[SPRITE_CRASH_PASS2_S].addr       = outrun.adr.shadow_data;
    
    jump_table[SPRITE_CRASH_SHADOW].shadow     = 7;
    jump_table[SPRITE_CRASH_SHADOW].zoom       = 0x80;
    jump_table[SPRITE_CRASH_SHADOW].draw_props = oentry::BOTTOM;
    jump_table[SPRITE_CRASH_SHADOW].addr       = outrun.adr.shadow_data;

    ocrash.init(
        &jump_table[SPRITE_CRASH], 
        &jump_table[SPRITE_CRASH_SHADOW], 
        &jump_table[SPRITE_CRASH_PASS1],
        &jump_table[SPRITE_CRASH_PASS1_S],
        &jump_table[SPRITE_CRASH_PASS2],
        &jump_table[SPRITE_CRASH_PASS2_S]
    );

    // ------------------------------------------------------------------------
    // Animation Sequence Sprites
    // ------------------------------------------------------------------------

    jump_table[SPRITE_FLAG].init(SPRITE_FLAG);
    oanimseq.init(jump_table);
    
    seg_pos             = 0;
    seg_total_sprites   = 0;
    seg_sprite_freq     = 0;
    seg_spr_offset2     = 0;
    seg_spr_offset1     = 0;
    seg_spr_addr        = 0;

    do_sprite_swap      = false;
    sprite_scroll_speed = 0;
    shadow_offset       = 0;
    sprite_count        = 0;
    spr_cnt_main        = 0;
    spr_cnt_shadow      = 0;

    spr_col_pal         = 0;
    pal_copy_count      = 0;    
}

// Swap Sprite RAM And Update Palette Data
void OSprites::update_sprites()
{
	if (do_sprite_swap)
	{
        do_sprite_swap = false;
        video.sprite_layer->swap();
        copy_palette_data();
	}
}

// Disable All Sprite Entries
// Source: 0x4A50
void OSprites::disable_sprites()
{
    for (uint8_t i = 0; i < SPRITE_ENTRIES; i++)
        jump_table[i].control &= ~OSprites::ENABLE;
}

void OSprites::tick()
{
    sprite_control();
}

// Sprite Control
//
// Source: 3BEE
//
// Notes:
// - Setup Jump Table Entry #2, the sprite control. This in turn is used to control and setup all the sprites.
// - Read 4 Byte Entry From Road_Seg_Adr1 which indicates the upcoming block of sprite data for the level
// - This first block of data specifies the position, total number of sprites in the block we want to try rendering 
//   and appropriate lookup for the sprite number/frequency info.
//
// - This second table in memory specifies the frequency and number of sprites in the sequence. 
//  
// - The second table also contains the actual sprite info (x,y,palette,type). This can be multipled sprites.
//
// ----------------------------------
//
// road_seg_addr1 Format: [4 byte boundaries]
//
// [+0] Road Position [Word]
// [+2] Number Of Sprites In Segment [Byte]
// [+3] Sprite Data Entry Number From Lookup Table * 4 [Byte]
//
// Sprite Master Table Format @ 0x1A43C:
//
// A Table Containing a series of longs. Each address in this table represents:
//
// [+0] Sprite Frequency Value Bitmask [Word]
// [+2] Reload Value For Sprite Info Offset [Word]
// [+4] Start of table with x,y,type,palette etc.
//      This table can appear as many times as desired in each block and follows the format below, starting +4:
//
// -------------------------------------------------
// [+0] [Byte] Bit 0 = H-Flip Sprite
//             Bit 1 = Enable Shadows
//            
//             Bits 4-7 = Routine Draw Number
// [+1] [Byte] Sprite X World Position
// [+2] [Word] Sprite Y World Position
// [+5] [Byte] Sprite Type
// [+7] [Byte] Sprite Palette
//-------------------------------------------------
//
// The Frequency bitmask, for example 111000111 00111000
// rotates left, and whenever a bit is set, a sprite from the sequence is rendered.
//
// When a bit is unset no draw occurs on that call.
//
// The entire sequence can repeat, until the max sprites counter expires.
//
// So the above example would draw 3 sprites in succession, then break for three attempts, then three again etc.

void OSprites::sprite_control()
{
    uint16_t pos = trackloader.read_scenery_pos();

    // Populate next road segment
    if (pos <= oroad.road_pos >> 16)
    {
        seg_pos = pos;                                                          // Position In Level Data [Word]
        seg_total_sprites = trackloader.read_total_sprites();                   // Number of Sprites In Segment
        uint8_t pattern_index = trackloader.read_sprite_pattern_index();        // Block Of Sprites
        trackloader.scenery_offset += 4;                                        // Advance to next scenery point
        
        uint32_t a0 = trackloader.read_scenerymap_table(pattern_index);         // Get Address of Scenery Pattern
        seg_sprite_freq = trackloader.read16(trackloader.scenerymap_data, &a0); // Scenery Frequency
        seg_spr_offset2 = trackloader.read16(trackloader.scenerymap_data, &a0); // Reload value for scenery pattern
        seg_spr_addr = a0;                                                      // Set ROM address for sprite info lookup (x, y, type)
                                                                                // NOTE: Sets to value of a0 itself, not memory location
        seg_spr_offset1 = 0;                                                    // And Clear the offset into the above table
    }

    // Process segment
    if (seg_total_sprites == 0) return;
    if (seg_pos > oroad.road_pos >> 16) return;

    // ------------------------------------------------------------------------
    // Sprite 1
    // ------------------------------------------------------------------------
    // Rotate 16 bit value left. Stick top bit in low bit.
    uint16_t carry = seg_sprite_freq & 0x8000;
    seg_sprite_freq = ((seg_sprite_freq << 1) | ((seg_sprite_freq & 0x8000) >> 15)) & 0xFFFF;

    if (carry)
    {
        seg_total_sprites--;
        seg_spr_offset1 -= 8; //  Decrement rom address offset to point at next sprite [8 byte boundary]
        if (seg_spr_offset1 < 0)
            seg_spr_offset1 = seg_spr_offset2; // Reload sprite info offset value
        olevelobjs.setup_sprites(0x10400);
    }
 
    if (seg_total_sprites == 0)
    {
        seg_pos++;
        return;
    }

    // ------------------------------------------------------------------------
    // Sprite 2 - Second Sprite is slightly set back from the first.
    // ------------------------------------------------------------------------
    carry = seg_sprite_freq & 0x8000;
    seg_sprite_freq = ((seg_sprite_freq << 1) | ((seg_sprite_freq & 0x8000) >> 15)) & 0xFFFF;

    if (carry)
    {
        seg_total_sprites--;
        seg_spr_offset1 -= 8; //  Decrement rom address offset to point at next sprite [8 byte boundary]
        if (seg_spr_offset1 < 0)
            seg_spr_offset1 = seg_spr_offset2; // Reload sprite info offset value
        olevelobjs.setup_sprites(0x10000);
    }

     seg_pos++;
}

// Source: 0x76F4
void OSprites::clear_palette_data()
{
    spr_col_pal = 0;
    for (int16_t i = 0; i < PAL_LOOKUP_LENGTH; i++)
        pal_lookup[i] = 0;
}


// Copy Sprite Palette Data To Palette RAM On Vertical Interrupt
// 
// Source Address: 0x858E
// Input:          Source address in rom of data format
// Output:         None
//

void OSprites::copy_palette_data()
{
    // Return if no palette entries to copy
    if (pal_copy_count <= 0) return;

    for (int16_t i = 0; i < pal_copy_count * 2;)
    {
        // Palette Data Source Offset (aligned to start of 32 byte boundry, * 32)
        uint32_t src_addr = pal_addresses[i++] << 3;
        uint32_t dst_addr = PAL_SPRITES + (pal_addresses[i++] << 5);
        for (uint16_t j = 0; j < 8; j++)
            video.write_pal32(&dst_addr, PALETTE_EXPANSION[src_addr++]);
    }
    pal_copy_count = 0; // All entries copied
}

// Map Palettes from ROM to Palette RAM for a particular sprite.
// 
// Source Address: 0x75EA
// Input:          Sprite
// Output:         None
//
// Prepares RAM for copy_palette_data routine on vint

// Notes:
// 1. Checks lookup table to determine whether relevant palette info is copied to ram. 
//    Return if already cached
// 2. Otherwise set the mapping between ROM and the HW Palette to be used
// 3. pal_copy_count contains the number of entries we need to copy
// 4. pal_addresses contains the address mapping

void OSprites::map_palette(oentry* spr)
{
    uint8_t pal = pal_lookup[spr->pal_src];

    // -----------------------------------
    // Entry is cached. Use entry.
    // -----------------------------------
    if (pal != 0)
    {
        spr->pal_dst = pal;
        return;
    }
    // -----------------------------------
    // Entry is not cached. Need to cache.
    // -----------------------------------

    // Increment hw colour palette entry to use
    if (++spr_col_pal > 0x7F) return;
    
    spr->pal_dst = spr_col_pal; // Set next available hw sprite colour palette
    pal_lookup[spr->pal_src] = spr_col_pal;

    pal_addresses[pal_copy_count * 2] = spr->pal_src;
    pal_addresses[(pal_copy_count * 2) + 1] = spr_col_pal;

    pal_copy_count++;
}

// Setup Sprite Ordering Table & Shadows
// 
// Source Address: 0x77A8
// Input:          Sprite To Copy
// Output:         None
//
// Notes:
// 1/ Reads Sprite-to-Sprite priority of individual sprite
// 2/ Creates ordered sprite table starting at 0x64000
// 3/ The format of this table is detailed in the comments at 0x78B0
// 4/ Optionally adds shadow to sprite if requires
//
// The end result is a table of sprite entries at 0x64000

void OSprites::do_spr_order_shadows(oentry* input)
{
    if (input->hidden) return;  // JJP ghost car related safety-net check

    // LayOut specific fix to avoid memory crash on over populated scenery segments
    if (spr_cnt_main + spr_cnt_shadow >= JUMP_ENTRIES_TOTAL)
        return;

    // Use priority as lookup into table. Assume we're on boundaries of 0x10
    uint16_t priority = (input->priority & 0x1FF) << 4;
    uint8_t bytes_to_copy = sprite_order[priority];

    // Maximum number of bytes we want to copy is 0x10
    if (bytes_to_copy < 0xE)
    {
        bytes_to_copy++;
        sprite_order[priority] = bytes_to_copy;
        sprite_order[priority + bytes_to_copy + 1] = input->jump_index; // put at offset +2
        spr_cnt_main++;
    }

    // Code to handle shadows under sprites
    // test_shadow: 
    if (!(input->control & SHADOW)) return;

    // LayOut specific fix to avoid memory crash on over populated scenery segments
    if (spr_cnt_main + spr_cnt_shadow >= JUMP_ENTRIES_TOTAL)
        return;

    input->dst_index = spr_cnt_shadow;
    spr_cnt_shadow++;                       // Increment total shadow count
    
    uint8_t pal_dst = input->pal_dst;       // Backup Sprite Colour Palette
    uint8_t shadow = input->shadow;         // and priority and shadow settings
    int16_t x = input->x;                   // and x position
    uint32_t addr = input->addr;            // and original sprite data address
    input->pal_dst = 0;                     // clear colour palette
    input->shadow = 7;                      // Set NEW priority & shadow settings
    
    input->x += (input->road_priority * shadow_offset) >> 9; // d0 = sprite z / distance into screen

    if (input->control & TRAFFIC_SPRITE)
    {
        input->addr = outrun.adr.sprite_shadow_small;
        input->x = x;
    }
    else
    {
        input->addr = roms.rom0p->read32(outrun.adr.shadow_frames + 0x3C);
    }

    do_sprite(input);           // Create Shadowed Version Of Sprite For Hardware
    
    input->pal_dst = pal_dst;   // Restore Sprite Colour Palette
    input->shadow = shadow;     // ...and other values
    input->x = x;
    input->addr = addr;
}

// Sprite Copying Routine
// 
// Source Address: 0x78B0
// Input:          None
// Output:         None
//

void OSprites::sprite_copy()
{
    if (spr_cnt_main == 0)
    {
        finalise_sprites();
        return;
    }

    if (spr_cnt_main + spr_cnt_shadow > 0x7F)
    {
        spr_cnt_main = spr_cnt_shadow = 0;
        finalise_sprites();
        return;
    }

    uint32_t spr_cnt_main_copy = spr_cnt_main;

    // look up in sprite_order
    int32_t src_addr = -0x10;

    // copy to sprite_entries
    uint32_t dst_index = 0;

    // Get next relevant entry (number of bytes to copy, followed by indexes of sprites)
    while (spr_cnt_main_copy != 0)
    {
        src_addr += 0x10;
        uint8_t bytes_to_copy = sprite_order[src_addr]; // warning: actually reads a word here normally, so this is wrong

        // Copy the required number of bytes
        if (bytes_to_copy != 0)
        {
            int32_t src_offset = src_addr + 2;

            do
            {
                // Sprite Index To Draw
                sprite_order2[dst_index++] = sprite_order[src_offset++];                
                if (--spr_cnt_main_copy == 0) break; // Loop continues until this is zero.
            }
            // Decrement number of bytes to copy
            while (--bytes_to_copy != 0);
        }

        // Clear number of bytes to copy
        sprite_order[src_addr] = 0; 
    }

    // cont2:
    uint16_t cnt_shadow_copy = spr_cnt_shadow;

    // next_sprite
    for (uint16_t i = 0; i < spr_cnt_main; i++)
    {
        uint16_t jump_index = sprite_order2[i];
        oentry *entry = &jump_table[jump_index];
        entry->dst_index = cnt_shadow_copy;
        cnt_shadow_copy++;
        do_sprite(entry);
    }

    finalise_sprites();
}

// Was originally labelled set_end_marker
// 
// Source Address: 0x7942
// Input:          None
// Output:         None
//

void OSprites::finalise_sprites()
{
    sprite_count = spr_cnt_main + spr_cnt_shadow;
    
    // Set end sprite marker
    sprite_entries[sprite_count].data[0] = 0xFFFF;
    sprite_entries[sprite_count].data[1] = 0xFFFF;

    // TODO: Code to wait for interrupt goes here
    blit_sprites();
    otraffic.traffic_logic();
    otraffic.traffic_sound();
    spr_cnt_main = spr_cnt_shadow = 0;

    // Ready to swap buffer and blit
    do_sprite_swap = true;
}

// Copy Sprite Data to Sprite RAM 
// 
// Source Address: 0x97E4
// Input:          None
// Output:         None
//

void OSprites::blit_sprites()
{
    uint32_t dst_addr = SPRITE_RAM;

    for (uint16_t i = 0; i <= sprite_count; i++)
    {
        uint16_t* data = sprite_entries[i].data;

        // Write sprite info
        for (int i=0; i<16; i++)
            video.write_sprite16(&dst_addr, data[i]);

        // Allign on correct boundary
//        dst_addr += 2;
    }
}

// Convert Sprite From Internal Software Format To Hardware Format
// 
// Source Address: 0x94EC
// Input:          Sprite To Copy
// Output:         None
//
// 1. Copies Sprite Information From Jump Table Area To RAM
// 2. Stores In Similar Format To Sprite Hardware, but with 4 extra bytes of scratch data on end
// 3. Note: Mostly responsible for setting x,y,width,height,zoom,pitch,priorities etc.
//
// 0x11ED2: Table of Sprite Addresses for Hardware. Contains:
//
// 5 x 10 bytes. One block for each sprite size lookup. 
// The exact sprite is selected using the ozoom_lookup.hpp table.
//
// + 0 : [Byte] Unused
// + 1 : [Byte] Width Helper Lookup  [Offsets into 0x20000 (the width and height table)]
// + 2 : [Byte] Line Data Width
// + 3 : [Byte] Height Helper Lookup [Offsets into 0x20000 (the width and height table)]
// + 4 : [Byte] Line Data Height
// + 5 : [Byte] Sprite Pitch
// + 7 : [Byte] Sprite Bank
// + 8 : [Word] Offset Within Sprite Bank

void OSprites::do_sprite(oentry* input)
{
    input->control |= DRAW_SPRITE; // Display input sprite

    // Get Correct Output Entry
    osprite* output = &sprite_entries[input->dst_index];

    // Copy address sprite was copied from.
    // todo: pass pointer?
    output->scratch = input->jump_index;
    // Per-frame memory-work Tier 1 #5: snapshot the TRAFFIC_SPRITE control
    // bit here too, so OTraffic::traffic_logic() can test it straight off
    // this record instead of re-dereferencing jump_table[scratch] for every
    // sprite. Nothing between this assignment and traffic_logic()'s read of
    // it (same tick, OSprites::finalise_sprites() calls blit_sprites() then
    // traffic_logic() with no intervening control-bit writes) can change
    // input->control, so the two stay in lockstep.
    output->is_traffic = (input->control & TRAFFIC_SPRITE) != 0;

    // Hide Sprite if zoom lookup not set
    if (input->zoom == 0)
    {
        hide_hwsprite(input, output);
        return;
    }

    // JJP - Ghost car fix. Hide the sprite if it's hidden count-down is >0
    // The 'hidden' field is set in OTraffic::move_spawned_sprite()
    if (input->hidden > 0) {
        hide_hwsprite(input, output);
        return;
    }
    // Sprite Width/Height Settings
    uint16_t width = 0;
    uint16_t height = 0;

    // Set real h/v zoom values
    uint32_t index = (input->zoom * 4); // x4 as table is 4-words per line

    // Hi-res-only sub-pixel x offset (sprite-RAM word 15), the zoom written to
    // the record, and the address of the frame the renderer will draw. The
    // 5-anchor width/height estimate that used to live here, with its per-SIZE
    // offset constants (+3/-1/+3/+4), is gone: both paths now use the arcade
    // WH-table geometry computed below, and the hires offset is derived from the
    // frame headers in the hires branch.
    int16_t  offset = 0;
    uint32_t zoom = 0;
    uint32_t src_offsets = 0;

    // determine output size (game logic)
    if (config.video.hiresprites == 0) {
        // original game resolution. Use (patched) original game sprite sizes.

        zoom = ZOOM_LOOKUP[index];
        output->set_vzoom(zoom);
        output->set_hzoom(zoom);

        // -------------------------------------------------------------------------
        // Set width & height values using lookup
        // -------------------------------------------------------------------------
        uint16_t lookup_mask = ZOOM_LOOKUP[index+1]; // Width/Height lookup helper

        // This is the address of the frame required for the level of zoom we're using
        // There are 5 unique frames that are typically used for zoomed sprites.
        // which correspond to different screen sizes
        src_offsets = input->addr + ZOOM_LOOKUP[index+2]; // sprite size e.g. SIZE1

        uint16_t d0 = input->draw_props | (input->zoom << 8);
        uint16_t top_bit = d0 & 0x8000; // set if zoom >= 0x80
        d0 &= 0x7FFF; // Clear top bit

        if (top_bit == 0) // zoom < 0x80
        {
            if (ZOOM_LOOKUP[index+2] != SIZE1) // Not largest sized sprite
            {
                lookup_mask += 0x4000;
                d0 = lookup_mask;
            }

            d0 = (d0 & 0xFF00) + roms.rom0p->read8(src_offsets + 1);
            width = roms.rom0p->read8(WH_TABLE + d0);
            d0 = (d0 & 0xFF00) + roms.rom0p->read8(src_offsets + 3);
            height = roms.rom0p->read8(WH_TABLE + d0);
        }
        // loc_9560:
        else
        {
            d0 &= 0x7C00;
            uint16_t h = d0;

            d0 = (d0 & 0xFF00) + roms.rom0p->read8(src_offsets + 1);
            width = roms.rom0p->read8(WH_TABLE + d0);
            d0 &= 0xFF;
            width += d0;

            h |= roms.rom0p->read8(src_offsets + 3);
            height = roms.rom0p->read8(WH_TABLE + h);
            h &= 0xFF;
            height += h;
        }
    } else {
        // hires path. Use larger sprites to improve image quality.
        zoom = ZOOM_LOOKUP_HIRES[index];
        output->set_vzoom(zoom);
        output->set_hzoom(zoom);

        uint16_t lookup_mask = ZOOM_LOOKUP_HIRES[index+1]; // Width/Height lookup helper

        src_offsets = input->addr + ZOOM_LOOKUP_HIRES[index+2]; // sprite size e.g. SIZE1
        // original sprite size entry from which rendered size will be determined.
        // this is different, because we are using "the next size up" sprites to improve hi-res fidelity
        // index+3 was previously an unused field
        uint32_t size_offset = input->addr + ZOOM_LOOKUP_HIRES[index+3];

        // Original frame's header bytes, kept for the substituted-frame guard
        // and offset below: +1 is the row length in nibbles, +3 the row count
        // minus one (the same convention as the hardware height field).
        int32_t n_orig    = 0;
        int32_t rows_orig = 0;

        uint16_t d0 = input->draw_props | (input->zoom << 8);
        uint16_t top_bit = d0 & 0x8000; // set if zoom >= 0x80
        d0 &= 0x7FFF; // Clear top bit

        if (top_bit == 0) {
            // zoom < 0x80
            if (ZOOM_LOOKUP_HIRES[index+3] != SIZE1) {
                // Not largest sized sprite
                lookup_mask += 0x4000;
                d0 = lookup_mask;
            }

            n_orig = roms.rom0p->read8(size_offset + 1);
            d0 = (d0 & 0xFF00) + n_orig;
            width = roms.rom0p->read8(WH_TABLE + d0);
            rows_orig = roms.rom0p->read8(size_offset + 3) + 1;
            d0 = (d0 & 0xFF00) + (rows_orig - 1);
            height = roms.rom0p->read8(WH_TABLE + d0);

        } else {

            d0 &= 0x7C00;
            uint16_t h = d0;

            n_orig = roms.rom0p->read8(size_offset + 1);
            d0 = (d0 & 0xFF00) + n_orig;
            width = roms.rom0p->read8(WH_TABLE + d0);
            d0 &= 0xFF;
            width += d0;

            rows_orig = roms.rom0p->read8(size_offset + 3) + 1;
            h |= (rows_orig - 1);
            height = roms.rom0p->read8(WH_TABLE + h);
            h &= 0xFF;
            height += h;
        }

        // Rows 0-62 of ZOOM_LOOKUP_HIRES substitute the next frame size up at
        // twice the zoom for extra detail. That is only valid when the slot
        // really holds a ~2x rendition of the original: for the 52
        // animation/strip tables it is a different sprite of similar size and
        // the doubled zoom would sweep 20-150 rows past the frame (the garbage
        // stripe under far strips); scenery types 564-576 keep a 248-nibble
        // strip there that the row count alone would pass. Reject on either
        // header byte and draw exactly what the default path draws.
        //
        // Otherwise the substituted frame is still not a pixel-exact 2x of the
        // original (53 % are 8-16 nibbles narrower, and the art lead-in is the
        // same 2-4 nibbles rather than doubled), so centre its row on the
        // original's row. Measured over every table and zoom row this leaves the
        // visible centre within 1 hi-res px in 99.4 % of cells, which is as good
        // as the headers allow (art bounds are not in ROM). It replaces the
        // per-SIZE +3/-1/+3/+4 constants, which were tuned before bd06b6e and
        // overshoot since.
        if (src_offsets != size_offset)
        {
            const int32_t n_big    = roms.rom0p->read8(src_offsets + 1);       // row length, nibbles
            const int32_t rows_big = roms.rom0p->read8(src_offsets + 3) + 1;

            if (std::abs(rows_big - 2 * rows_orig) > 2 || std::abs(n_big - 2 * n_orig) > 16)
            {
                zoom = ZOOM_LOOKUP[index];
                output->set_vzoom(zoom); output->set_hzoom(zoom);
                src_offsets = size_offset;
                offset = 0;
            }
            else
            {
                // zoom is still ZOOM_LOOKUP_HIRES[index] here (exactly 2x the
                // original zoom in rows 0-62). parse_record halves the record's
                // zoom for the hi-res canvas, so the renderer steps the
                // substituted frame's row at zoom>>1, and would step the
                // original's at (ZOOM_LOOKUP[index])>>1 == zoom>>2. Use those
                // divisors as integers: ZOOM_LOOKUP is odd in 34 of the 63
                // substituted rows, and using a fractional zoom/4 divisor
                // instead misplaces 39 table x zoom cells by 1 hi-res px.
                const int32_t z_big  = (int32_t)(zoom >> 1);
                const int32_t z_orig = (int32_t)(zoom >> 2);
                const int32_t w_big  = (512 * n_big  + z_big  - 1) / z_big;
                const int32_t w_orig = (512 * n_orig + z_orig - 1) / z_orig;
                offset = (int16_t)((w_orig - w_big) / 2);   // truncation toward zero, as modelled
            }
        }
    }

    // Exact arcade on-screen geometry for BOTH paths (extends bd06b6e, which
    // restored it for the default path only). The renderer draws every row and
    // nibble of the frame it is handed regardless of these values; width/height
    // only anchor the record (set_sprite_xy), size the game-logic collision box
    // (input->width) and place right-to-left records (set_width), and all three
    // must be the arcade's WH-table values or the sprite moves relative to the
    // default path. With hiresprites == 1 the former estimate made the height
    // field 1-3 rows too tall in 44 % of table x zoom cells, exposing the
    // frame's never-displayed last row (the black line under traffic tyres) or
    // sweeping past the frame entirely, and moved record x by 2-24 hi-res px.
    const int32_t calc_width  = width;
    const int32_t calc_height = height;
    if (config.video.hiresprites == 0)
        offset = 0;
    // Sprite-RAM word 15, read by the renderer only when hiresprites == 1, and
    // written here so that it carries the final value. NB: offset must stay
    // int16_t so this resolves to osprite::set_offset(int16_t) -> data[15], not
    // the uint16_t overload -> data[1] (the sprite-bank offset written below).
    output->set_offset(offset);

    // loc 9582:
//    input->width = width;
    input->width = calc_width;

    // JJP - pass width through to sprite renderer
//    output->set_width(width);
    output->set_width(calc_width);

    // -------------------------------------------------------------------------
    // Set Sprite X & Y Values
    // -------------------------------------------------------------------------
//    set_sprite_xy(input, output, width, height);
    set_sprite_xy(input, output, calc_width, calc_height);

    // Here we need the entire value set by above routine, not just top 0x1FF mask!
    // These two locals feed the off-screen cull and the clip flag below, both of
    // which work in lo-res units, so the hires offset (word 15, hi-res canvas
    // pixels) has to be halved on the way in. Note the offset *centres* the
    // substituted frame on the original rather than translating it: the drawn
    // left edge moves by +offset and the right edge by -offset. So x1 and x2
    // take it with opposite signs -- x2 is not x1 + calc_width. Measured against
    // the default path over every table x zoom cell, the right-hand term is out
    // by up to 40 hi-res px if the offset is added twice (as it was before
    // 0f1fe9a) and by up to 16 if it is modelled as a translation; with the
    // signs as below the two agree to within 1 px on all 39,680 cells.
    // Rounded away from zero by division rather than a shift, so the negative
    // case does not lean on >> semantics, and because away-from-zero is the
    // tighter bound in every substituted cell (never smaller than the shift's
    // magnitude). Either keeps the left-edge margin >= 3 hi-res px for any
    // offset; the measured offset range is -3..+8.
    //
    // Caveat on what the clip flag is worth: it was never an exact bound even on
    // the default path, because calc_width is the WH-table width and a frame's
    // rows can run wider (scenery type 8 at zoom row 219 draws 2W+20). Only
    // render_ref's no-clip variants trust it; draw_sprite_fast recomputes
    // cx0/cx1 regardless, so the shipped renderer is immune either way.
    const int16_t offset_lores = (int16_t)(offset >= 0 ? (offset + 1) / 2
                                                       : -((-offset + 1) / 2));
    int16_t sprite_x1 = output->get_x() + offset_lores;
//    int16_t sprite_x2 = sprite_x1 + width;
    int16_t sprite_x2 = output->get_x() + calc_width - offset_lores;
    int16_t sprite_y1 = output->get_y();
//    int16_t sprite_y2 = sprite_y1 + height;
    int16_t sprite_y2 = sprite_y1 + calc_height;

    const uint16_t x1_bounds = 512 + config.s16_x_off; // right edge
    const uint16_t x2_bounds = 192 - config.s16_x_off; // left edge

    // Hide Sprite if off screen (note bug fix to solve shadow wrapping issue on original game)
    // I think this bug might be permanently fixed with the introduction of widescreen mode
    // as I had to change the storage size of the x-coordinate.
    // Unsetting fix_bugs may no longer revert to the original behaviour.
    if (     sprite_y2 < 256            /* fully off the top */
          || sprite_y1 > 479            /* fully off the bottom */
          || sprite_x2 < x2_bounds      /* fully off LHS */
          || (config.engine.fix_bugs ?  /* fully off RHS */
                  sprite_x1 >= x1_bounds
                : sprite_x1 > x1_bounds)
       ) {
        hide_hwsprite(input, output);
        return;
    }

    // JJP - set a clip flag if the sprite is not entirely on-screen. This simplifies the sprite drawing by
    // eliminating the x-clip.
    bool fully_inside = (sprite_x1 >= x2_bounds) && (sprite_x2 < x1_bounds);
    bool clip = !fully_inside;
    output->set_clip(clip);

    // -------------------------------------------------------------------------
    // Set Palette & Sprite Bank Information
    // -------------------------------------------------------------------------
    output->set_pal(input->pal_dst); // Set Sprite Colour Palette
    output->set_offset(roms.rom0p->read16(src_offsets + 8)); // Set Offset within selected sprite bank
    output->set_bank(roms.rom0p->read8(src_offsets + 7) << 1); // Set Sprite Bank Value

    // -------------------------------------------------------------------------
    // Set Sprite Height
    // -------------------------------------------------------------------------
    if (sprite_y1 < 256)
    {
        int16_t y_adj = -(sprite_y1 - 256);
        y_adj *= roms.rom0p->read16(src_offsets + 2); // Width of line data (Unsigned multiply)
//        y_adj /= height; // Unsigned divide
        y_adj /= calc_height; // Unsigned divide
        y_adj *= roms.rom0p->read16(src_offsets + 4); // Length of line data (Unsigned multiply)
        output->inc_offset(y_adj);
        output->data[0x0] = (output->data[0x0] & 0xFF00) | 0x100; // Mask on negative y index
        output->set_height((uint8_t) sprite_y2);
    }
    else
    {
//        output->set_height((uint8_t) height);
        output->set_height((uint8_t) calc_height);
    }
    
    // -------------------------------------------------------------------------
    // Set Sprite Height Taking Elevation Of Road Into Account For Clipping Purposes
    //
    // Word 0: Priority of section
    // Word 1: Height of section
    //
    // Source: 0x9602
    // -------------------------------------------------------------------------

    // Start of priority elevation data in road ram
    uint16_t road_y_index = oroad.road_p0 + 0x280;
    
    // Priority List Not Populated (Flat Elevation)
    if (oroad.road_y[road_y_index + 0] == 0 && oroad.road_y[road_y_index + 1] == 0)
    {
        // set_spr_height:
        if (sprite_y2 > 0x1DF)
            output->sub_height(sprite_y2 - 0x1DF);
    }
    // Priority List Populated (Elevated Section of road)
    else
    {
        // Count number of height_entries
        int16_t height_entry = 0;
        do
        {
            height_entry++;
            road_y_index += 2;
        }
        while (oroad.road_y[road_y_index + 0] != 0 && oroad.road_y[road_y_index + 1] != 0);

        height_entry--;

        do
        {
            road_y_index -= 2;
        }
        while (--height_entry > 0 && input->road_priority > oroad.road_y[road_y_index + 0]);

        // Sprite has higher priority, draw sprite
        if (input->road_priority > oroad.road_y[road_y_index])
        {
            // set_spr_height:
            if (sprite_y2 > 0x1DF)
                output->sub_height(sprite_y2 - 0x1DF);
        }
        // Road has higher priority, clip sprite
        else
        {
            // 9630:
            int16_t road_elevation = -oroad.road_y[road_y_index + 1] + 0x1DF;
            if (sprite_y1 > road_elevation)
            {
                hide_hwsprite(input, output);
                return;
            }
            else if (sprite_y2 <= road_elevation)
            {
                if (sprite_y2 > 0x1DF)
                    output->sub_height(sprite_y2 - 0x1DF);
            }
            else
            {
                output->sub_height(sprite_y2 - road_elevation);
            }
        }
    }

    // cont2:
    set_hrender(input, output, roms.rom0p->read16(src_offsets + 4), width);
    
    // -------------------------------------------------------------------------
    // Set Sprite Pitch & Priority
    // -------------------------------------------------------------------------
    output->set_pitch(roms.rom0p->read8(src_offsets + 5) << 1);
    output->set_priority(input->shadow << 4); // todo: where does this get set?
}

// Hide Input And Output Entry
void OSprites::hide_hwsprite(oentry* input, osprite* output)
{
    input->control &= ~DRAW_SPRITE; // Hide input sprite
    output->hide();
}

// Sets Sprite Render Point
// 
// Source Address: 0x967C
// Input:          Jump Table Entry, Output Sprite Entry, Width & Height
// Output:         Updated Sprite Output Entry
//

void OSprites::set_sprite_xy(oentry* input, osprite* output, uint16_t width, uint16_t height)
{
    uint8_t anchor = input->draw_props;

    // -------------------------------------------------------------------------
    // Set Y Render Point
    // -------------------------------------------------------------------------

    int16_t y = input->y;

    switch((anchor & 0xC) >> 2)
    {
        // Anchor Center
        case 0:
        case 3:
            height >>= 1;
            y -= height;
            output->set_y(y + 256);
            break;
            
        // Anchor Left
        case 1:
            output->set_y(y + 256);
            break;

        // Anchor Right
        case 2:
            y -= height;
            output->set_y(y + 256);
            break;
    }

    // -------------------------------------------------------------------------
    // Set X Render Point
    // -------------------------------------------------------------------------

    int16_t x = input->x;

    switch(anchor & 0x3)
    {
        // Anchor Center
        case 0:
        case 3:
            width >>= 1;
            x -= width;
            output->set_x(x + 352);
            break;
            
        // Anchor Left
        case 1:
            output->set_x(x + 352);
            break;

        // Anchor Right
        case 2:
            x -= width;
            output->set_x(x + 352);
            break;
    }
}

// Determines whether to render sprite left-to-right or right-to-left
// 
// Source Address: 0x96E4
// Input:          Jump Table Entry, Output Sprite Entry, Offset
// Output:         Updated Sprite Output Entry
//
void OSprites::set_hrender(oentry* input, osprite* output, uint16_t offset, uint16_t width)
{
    uint8_t props = 0x60;
    uint8_t anchor = input->draw_props;

    // Anchor Top Left: Set Backwards & Left To Right Render
    if (anchor & 1)
        props = 0x60;
    // Anchor Bottom Right: Set Forwards & Right To Left Render
    else if (anchor & 2 || input->x < 0)
        props = 0;

    if (input->control & HFLIP)
    {
        if (props == 0) props = 0x40; // If H-Flip & Right To Left Render: Read data backwards
        else props = 0x20; // If H-Flip && Left To Right: Render left to right & Read data forwards
    }

    // setup_bank:

    // if reading forwards, increment the offset
    if ((props & 0x40) == 0)
    {
        output->inc_offset(offset - 1);
    }

    // if right to left, increment the x position
    if ((props & 0x20) == 0)
    {
        output->inc_x(width);
    }

    props |= 0x80; // Set render top to bottom
    output->set_render(props);
}

// Helper function to vary the move distance, based on the current frame-rate.
void OSprites::move_sprite(oentry* sprite, uint8_t shift)
{
    uint32_t addr = SPRITE_ZOOM_LOOKUP + (((sprite->z >> 16) << 2) | sprite_scroll_speed);
    uint32_t value = roms.rom0.read32(addr) >> shift;

    if (config.tick_fps == 60)
        value >>= 1;
    else if (config.tick_fps == 120)
        value >>= 2;

    sprite->z += value;
}
