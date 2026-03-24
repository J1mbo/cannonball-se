/***************************************************************************
    Player Name Entry.
    Used after music selection to capture player initials.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include "engine/oname.hpp"
#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/outrun.hpp"
#include "roms.hpp"
#include "sdl2/input.hpp"
#include "video.hpp"

OName oname;

OName::OName()
{
}

OName::~OName()
{
}

// Initialize name entry screen
void OName::enable()
{
    state = 0;
    initial_selected = 0;
    letter_selected = 0;
    acc_curr = 0;
    acc_prev = -1;
    steer = 0;
    flash = 0;
    player_initials = "";
    complete = false;

    video.clear_text_ram();
    video.enabled = true;

    // Display title
    ohud.blit_text_big(8, "ENTER YOUR INITIALS", false);
}

void OName::disable()
{
    // Clean up if needed
}

void OName::tick()
{
    // Get text ram address for initials display (centered on screen)
    const uint32_t adr = 0x110752;

    // Blit alphabet and highlight selected letter
    blit_alphabet();

    // Flash current initial being entered
    flash_entry(adr);

    // Handle input from controls
    do_input(adr);
}

const std::string& OName::get_initials() const
{
    return player_initials;
}

bool OName::is_complete() const
{
    return complete;
}

// Blit Alphabet. Highlight selected letter red.
// Based on ohiscore.cpp:267-288
void OName::blit_alphabet()
{
    // Print Text: "ABCDEFGHIJK..."
    ohud.blit_text2(TEXT2_ALPHABET);

    // Address in text ram for characters
    uint32_t adr = 0x110BF0;

    video.write_text16(&adr,       0x8D00); // Full Stop
    video.write_text16(adr + 0x7E, 0x8D01);
    video.write_text16(&adr,       0x8D04); // Arrow
    video.write_text16(adr + 0x7E, 0x8D05);
    video.write_text16(&adr,       0x8D02); // ED
    video.write_text16(adr + 0x7E, 0x8D03);

    // Colour selected tile red
    const uint16_t RED = 0x80;
    adr = 0x110BBC + (letter_selected << 1);
    video.write_text8(adr,        (video.read_text8(adr) & 1) | RED);
    video.write_text8(adr + 0x80, (video.read_text8(adr + 0x80) & 1) | RED);
}

// Flash current initial that is being entered
// Based on ohiscore.cpp:294-306
void OName::flash_entry(uint32_t adr)
{
    uint16_t tile = 0x20; // Default blank tile
    flash++; // Increment flashing counter

    if (flash & BIT_3)
    {
        tile = (roms.rom0.read8(letter_selected + TILES_ALPHABET) & 0xFF) | 0x8600;
    }

    video.write_text16(adr + (initial_selected << 1), tile);
}

// Name Entry Input
// Based on ohiscore.cpp:310-382
void OName::do_input(uint32_t adr)
{
    // Read Steering Left / Right & Denote Letter To Be Highlighted
    const static uint8_t ENTRIES = 28; // 28 Possible entries we can select from
    const static uint8_t DELETE = 26;
    const static uint8_t END = 27;

    int16_t position = read_controls() + letter_selected;

    if (position > END)
        letter_selected = position = 0;
    else if (position < 0)
        letter_selected = position = END;
    else
        letter_selected = position;

    // Check accelerator for press and depress
    if (!acc_curr || !(acc_prev ^ acc_curr)) return;

    // End option selected
    if (letter_selected == END)
    {
        video.write_text16(adr + (initial_selected << 1), 0x20); // Write blank tile to ram
        complete = true;
    }
    // Delete option selected
    else if (letter_selected == DELETE)
    {
        // Delete if not at first position
        if (initial_selected != 0)
        {
            if (player_initials.length() > 0)
                player_initials.pop_back();

            video.write_text16(adr + (initial_selected << 1), 0x20); // Write blank tile to ram
            initial_selected--;
        }
    }
    // Normal character selected
    else
    {
        uint8_t tile = roms.rom0.read8(TILES_ALPHABET + letter_selected);

        // Add initial to string
        player_initials += (char)tile;

        video.write_text16(adr + (initial_selected << 1), tile | 0x8600); // Write initial tile to ram

        // Move to next position or complete if 3 initials entered
        if (++initial_selected >= 3)
        {
            complete = true;
        }
    }
}

// Read controls for name entry input screen
// Based on ohiscore.cpp:390-432
int8_t OName::read_controls()
{
    // When using digital (keyboard/gamepad button) input, bypass analog ramp
    // simulation for immediate response to key presses.
    if (!input.analog || !input.gamepad)
    {
        // Accelerator: fire on the exact frame the key transitions to pressed
        if (input.has_pressed(Input::ACCEL))
        {
            acc_prev = 0;
            acc_curr = -1;
        }
        else if (input.is_pressed(Input::ACCEL))
        {
            acc_prev = acc_curr;
            acc_curr = -1;
        }
        else
        {
            acc_prev = acc_curr;
            acc_curr = 0;
        }

        // Steering: instant step on initial key press, then repeat after delay
        if (input.has_pressed(Input::LEFT))  { steer = 0; return -1; }
        if (input.has_pressed(Input::RIGHT)) { steer = 0; return 1; }

        if (input.is_pressed(Input::LEFT) && !input.is_pressed(Input::RIGHT))
        {
            if (++steer >= 0x14) { steer = 0; return -1; }
        }
        else if (input.is_pressed(Input::RIGHT) && !input.is_pressed(Input::LEFT))
        {
            if (++steer >= 0x14) { steer = 0; return 1; }
        }
        else
        {
            steer = 0;
        }
        return 0;
    }

    // Analog controls: original ramp-based logic
    if (oinputs.input_acc < 0x30)
    {
        acc_prev = acc_curr;
        acc_curr = 0;
    }
    else if (oinputs.input_acc < 0x60)
    {
        acc_curr = acc_prev;
    }
    else
    {
        acc_prev = acc_curr;
        acc_curr = -1;
    }

    int8_t movement = 1; // default to right
    int16_t steering = (oinputs.input_steering & 0xFF) - 0x80;
    if (steering < 0)
    {
        steering = -steering;
        movement = -1; // left
    }

    if (steering >= 0x30)
        steer += 5;
    else if (steering >= 0x10)
        steer += 1;

    if (steer >= 0x14)
        steer = 0;
    else
        movement = 0;

    return movement;
}
