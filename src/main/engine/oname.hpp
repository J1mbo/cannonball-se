/***************************************************************************
    Player Name Entry.
    Used after music selection to capture player initials.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#pragma once

#include "stdint.hpp"
#include <string>

class OName
{
public:
    OName();
    ~OName();

    void enable();                          // Initialize name entry screen
    void disable();                         // Clean up
    void tick();                            // Main update loop
    const std::string& get_initials() const; // Return entered initials
    bool is_complete() const;               // Check if entry is done

private:
    // State of name entry logic
    uint8_t state;

    // Selected Initial (0-2)
    int8_t initial_selected;

    // Selected Letter
    int16_t letter_selected;

    // Acceleration Value Current
    int16_t acc_curr;

    // Acceleration Value Previous
    int16_t acc_prev;

    // Steering Value
    int16_t steer;

    // Flashing counter
    uint8_t flash;

    // Stored player initials
    std::string player_initials;

    // Entry complete flag
    bool complete;

    void blit_alphabet();               // Draw alphabet
    void flash_entry(uint32_t adr);     // Flash current initial
    void do_input(uint32_t adr);        // Handle input
    int8_t read_controls();             // Read steering/accelerator
};

extern OName oname;
