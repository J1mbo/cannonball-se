# Player Initials Entry Feature

## Overview

This feature adds player initial/name entry at the start of the game and intelligently reuses those initials if the player achieves a high score, eliminating duplicate entry and improving user experience.

**Key Enhancement:** If a player enters their initials at the start and then achieves a high score, the system automatically uses the stored initials without prompting for entry again. The high score is recorded and the table is displayed directly.

## Requirement

Add a new screen/state after the user has chosen their music where the player can enter their initials (similar to the high score entry screen). These initials should be:
1. Recorded in the outermost span of the telemetry
2. Stored for the duration of the game session
3. **Automatically reused if the player achieves a high score** - bypassing the normal high score entry screen and directly displaying the updated high score table with the new entry

## Current Flow

```
GS_MUSIC (music selection)
  ↓
GS_INIT_GAME (initialize game)
  ↓
GS_START1-3 (countdown)
  ↓
GS_INGAME (gameplay)
```

## Desired Flow - Game Start

```
GS_MUSIC (music selection)
  ↓
GS_INIT_NAME (initialize name entry) ← NEW
  ↓
GS_NAME (player enters initials) ← NEW
  ↓
GS_INIT_GAME (initialize game)
  ↓
GS_START1-3 (countdown)
  ↓
GS_INGAME (gameplay)
```

## Desired Flow - Game End (High Score Achieved)

**Current behavior:**
```
GS_GAMEOVER (game over display)
  ↓
GS_INIT_MUSIC (prepare high score entry)
  ↓
GS_MUSIC (high score entry screen)
  ↓
GS_INIT_GAME (display high score table)
```

**New behavior:**
```
GS_GAMEOVER (game over display)
  ↓
Check if player_initials exist
  ↓
  YES → Skip entry screen, use stored initials
        ↓
        GS_INIT_GAME (display high score table with new entry) ← MODIFIED
  ↓
  NO → Follow current flow (ask for initials)
        ↓
        GS_INIT_MUSIC → GS_MUSIC → GS_INIT_GAME
```

## Implementation Plan

### 1. Add New Game States

**File:** `src/main/engine/outrun.hpp` (lines 29-54)

Add two new states after `GS_MUSIC`:
```cpp
GS_INIT_NAME,  // Initialize player name entry
GS_NAME,       // Player name entry screen
```

### 2. Create New Initial Entry Class

**Files to create:** `src/main/engine/oname.hpp` and `src/main/engine/oname.cpp`

Model after the high score entry system in `src/main/engine/ohiscore.cpp`. Key components:

#### UI Elements
- **Alphabet display** (A-Z plus special characters)
  - Reference: `ohiscore.cpp:267-288`
  - 28 entries: A-Z + delete + end
  - Displayed at bottom of screen
  - Selected letter highlighted in red

- **Initial entry display**
  - Reference: `ohiscore.cpp:294-306`
  - Three character positions (0-2)
  - Current position flashes using BIT_3 counter
  - Blank tile (0x20) when not selected

- **Title/prompt text**
  - Display "ENTER YOUR NAME" or similar

#### Input Handling
- **Steering wheel** - Navigate through alphabet
  - Reference: `ohiscore.cpp:392-431`
  - Speed depends on wheel turn amount
  - Wraps around at ends

- **Accelerator** - Confirm letter selection
  - Reference: `ohiscore.cpp:395-408`
  - Uses press/release state machine
  - Acts on release only

- **Letter selection logic**
  - Normal letter (0-25): Store to position, advance cursor
  - Delete option (26): Remove last initial, move cursor back
  - End option (27): Complete entry and transition to next state

#### Data Storage
- Store entered initials in class member variable (e.g., `std::string player_initials`)
- Make accessible via getter method: `const std::string& get_initials() const`
- Initialize to empty string or default value "___"

### 3. Update State Transitions

#### A. Music Selection Exit
**File:** `src/main/engine/omusic.cpp:210`

**Current code:**
```cpp
void OMusic::check_start() {
    if (ostats.credits && input.has_pressed(Input::START)) {
        outrun.game_state = GS_INIT_GAME;  // Current transition
        ologo.disable();
        disable();
    }
}
```

**Change to:**
```cpp
outrun.game_state = GS_INIT_NAME;  // New transition
```

#### B. Main State Handler
**File:** `src/main/engine/outrun.cpp`

Add state handlers in the main game loop (reference existing state handlers):

```cpp
case GS_INIT_NAME:
    oname.enable();
    game_state = GS_NAME;
    break;

case GS_NAME:
    oname.tick();
    if (oname.is_complete()) {
        oname.disable();
        game_state = GS_INIT_GAME;
    }
    break;
```

#### C. Name Entry Completion
**File:** `src/main/engine/oname.cpp` (new file)

When initials entry is complete (END selected or 3rd character entered):
- Set completion flag
- Transition will be handled by `outrun.cpp` state machine

### 4. Update Telemetry

#### A. Modify Game Session Start Signature
**File:** `src/main/telemetry.hpp` (around line 40)

**Current:**
```cpp
void start_game_session(const std::string& game_mode, int music_selection);
```

**Change to:**
```cpp
void start_game_session(const std::string& game_mode, int music_selection, const std::string& player_initials);
```

#### B. Record Initials in Outermost Span
**File:** `src/main/telemetry.cpp:171-188`

**Current code:**
```cpp
void TelemetryManager::start_game_session(const std::string& game_mode, int music_selection) {
    // ... existing code ...
    span->SetAttribute("game_mode", game_mode);
    span->SetAttribute("music_selection", music_selection);
    // ... existing code ...
}
```

**Add:**
```cpp
span->SetAttribute("player_initials", player_initials);
```

#### C. Update Call Site
**File:** `src/main/engine/outrun.cpp:457`

**Current call:**
```cpp
TelemetryManager::instance().start_game_session(game_mode, music_selected);
```

**Change to:**
```cpp
TelemetryManager::instance().start_game_session(game_mode, music_selected, oname.get_initials());
```

### 5. Class Integration

**File:** `src/main/engine/outrun.hpp`

Add include and instance declaration (similar to other game objects):
```cpp
#include "engine/oname.hpp"

// In class or global scope:
extern OName oname;
```

**File:** `src/main/engine/outrun.cpp`

Instantiate the object:
```cpp
OName oname;
```

### 6. Store Player Initials for Session

To enable automatic reuse of initials at high score entry, we need persistent storage.

#### A. Add Storage to OutRun Class
**File:** `src/main/engine/outrun.hpp`

Add a member variable to store the player's initials:
```cpp
class OutRun {
    // ... existing members ...
    std::string player_initials;  // Stores initials entered at game start

public:
    void set_player_initials(const std::string& initials);
    const std::string& get_player_initials() const;
    bool has_player_initials() const;
};
```

#### B. Implement Storage Methods
**File:** `src/main/engine/outrun.cpp`

```cpp
void OutRun::set_player_initials(const std::string& initials) {
    player_initials = initials;
}

const std::string& OutRun::get_player_initials() const {
    return player_initials;
}

bool OutRun::has_player_initials() const {
    return !player_initials.empty() && player_initials.length() >= 3;
}
```

#### C. Store Initials After Entry
**File:** `src/main/engine/outrun.cpp`

In the `GS_NAME` state handler, after initials are entered:
```cpp
case GS_NAME:
    oname.tick();
    if (oname.is_complete()) {
        set_player_initials(oname.get_initials());  // Store for later use
        oname.disable();
        game_state = GS_INIT_GAME;
    }
    break;
```

#### D. Clear Initials at Session Start
**File:** `src/main/engine/outrun.cpp`

Clear stored initials when returning to attract mode to ensure each game session is independent:
```cpp
case GS_ATTRACT:
    // ... existing attract mode code ...
    player_initials.clear();  // Reset for new session
    break;
```

### 7. Modify High Score Entry Logic

#### A. Check for Existing Initials
**File:** `src/main/engine/ohiscore.cpp`

Add a method to check if initials should be auto-populated:

```cpp
// In ohiscore.hpp - add public method:
bool should_skip_entry() const;

// In ohiscore.cpp - implementation:
bool OHiScore::should_skip_entry() const {
    return outrun.has_player_initials();
}
```

#### B. Auto-Populate High Score Entry
**File:** `src/main/engine/ohiscore.cpp`

Modify the initialization logic to use stored initials when available:

```cpp
void OHiScore::enable() {
    // ... existing setup code ...

    if (outrun.has_player_initials()) {
        // Use initials from game start
        const std::string& initials = outrun.get_player_initials();
        for (int i = 0; i < 3 && i < initials.length(); i++) {
            initial[i] = initials[i];
        }

        // Immediately save high score and skip to display
        write_hiscore_to_ram();
        state = STATE_DONE;  // Skip entry, go straight to display
    } else {
        // No stored initials - use normal entry flow
        state = STATE_ENTRY;
    }
}
```

#### C. Handle State Transitions
**File:** `src/main/engine/ohiscore.cpp`

In the main tick() method, respect the auto-populated state:

```cpp
void OHiScore::tick() {
    switch (state) {
        case STATE_INIT:
            // ... existing init code ...
            break;

        case STATE_ENTRY:
            // Normal manual entry flow
            handle_input();
            break;

        case STATE_DONE:
            // Skip entry - proceed to high score display
            // Let parent state machine handle transition
            break;
    }
}
```

#### D. Skip Entry Screen Rendering
**File:** `src/main/engine/ohiscore.cpp`

Update rendering to skip the alphabet/entry UI when using stored initials:

```cpp
void OHiScore::render() {
    if (state == STATE_DONE && outrun.has_player_initials()) {
        // Render only the high score table
        render_hiscore_table();
    } else {
        // Render full entry screen with alphabet
        render_entry_screen();
        render_hiscore_table();
    }
}
```

### 8. Update State Machine Flow

**File:** `src/main/engine/outrun.cpp`

Modify the high score entry state to detect and handle auto-population:

```cpp
case GS_MUSIC:  // This state is reused for high score entry
    ohiscore.tick();

    if (ohiscore.is_complete()) {
        // High score entry complete (either manual or auto)
        ohiscore.disable();

        if (outrun.has_player_initials()) {
            // Initials were auto-used, proceed directly to table display
            game_state = GS_INIT_GAME;  // Or appropriate next state
        } else {
            // Manual entry was used, follow normal flow
            game_state = GS_INIT_GAME;
        }
    }
    break;
```

## Key Files to Modify

### Initial Entry Screen (Steps 1-5)

1. **`src/main/engine/outrun.hpp`**
   - Add `GS_INIT_NAME` and `GS_NAME` enum values
   - Add `extern OName oname;` declaration
   - Add `player_initials` member variable
   - Add methods: `set_player_initials()`, `get_player_initials()`, `has_player_initials()`

2. **`src/main/engine/outrun.cpp`**
   - Add state handlers for `GS_INIT_NAME` and `GS_NAME`
   - Instantiate `OName oname;`
   - Update telemetry call to pass initials (line ~457)
   - Implement player initials storage methods
   - Store initials after entry in `GS_NAME` handler
   - Clear initials in attract mode

3. **`src/main/engine/omusic.cpp`**
   - Change state transition from `GS_INIT_GAME` to `GS_INIT_NAME` (line ~210)

4. **`src/main/telemetry.hpp`**
   - Add `player_initials` parameter to `start_game_session()`

5. **`src/main/telemetry.cpp`**
   - Update `start_game_session()` implementation
   - Add attribute to outermost span

6. **`src/main/engine/oname.hpp`** (NEW FILE)
   - Class declaration for initial entry screen

7. **`src/main/engine/oname.cpp`** (NEW FILE)
   - Class implementation (borrow heavily from `ohiscore.cpp`)

### High Score Entry Auto-Population (Steps 6-8)

8. **`src/main/engine/ohiscore.hpp`**
   - Add `should_skip_entry()` method declaration
   - Add state constants if not already present (STATE_INIT, STATE_ENTRY, STATE_DONE)

9. **`src/main/engine/ohiscore.cpp`**
   - Implement `should_skip_entry()` method
   - Modify `enable()` to check for stored initials and auto-populate
   - Update `tick()` to handle auto-populated state
   - Update rendering to skip entry UI when using stored initials
   - Ensure `write_hiscore_to_ram()` is called with stored initials

## Technical Notes

### Reference Implementation
The high score entry screen (`src/main/engine/ohiscore.cpp`) provides excellent reference code:
- **Alphabet UI**: Lines 267-288
- **Initial display**: Lines 294-306
- **Input handling**: Lines 310-432
- **Steering control**: Lines 392-431
- **Accelerator logic**: Lines 395-408

### Telemetry Architecture
- Uses OpenTelemetry with hierarchical spans
- Game session is the outermost/root span
- Started in `outrun.cpp:457` (after countdown completes)
- Ended in `outrun.cpp:635` (after high score screen)
- All game events (stages, crashes, overtakes) are child spans/events

### Visual Consistency
Match the visual style of the high score entry screen:
- Red shaded palette background
- Same font styles and sizes
- Similar layout and positioning
- Reuse sprite/tile resources where possible

## Build Considerations

- Update build system (CMakeLists.txt or Makefile) to include new `oname.cpp` file
- Ensure proper linking order for new object file

## Testing Checklist

### Initial Entry Screen (Game Start)
- [ ] New screen displays after music selection
- [ ] Steering wheel navigates through alphabet
- [ ] Accelerator confirms letter selection
- [ ] Three initials can be entered
- [ ] Delete option removes last initial
- [ ] End option transitions to game start
- [ ] Initials recorded in telemetry game_session span
- [ ] Game flow continues normally after initial entry
- [ ] Visual style matches game aesthetic

### High Score Auto-Population (Game End)
- [ ] **With stored initials + high score:**
  - [ ] High score entry screen is skipped
  - [ ] Stored initials are automatically used for high score
  - [ ] High score table displays with new entry
  - [ ] No manual entry is required

- [ ] **Without stored initials + high score:**
  - [ ] Normal high score entry screen displays
  - [ ] Manual entry works as before
  - [ ] (Fallback behavior preserved)

- [ ] **Stored initials persistence:**
  - [ ] Initials stored after entry at game start
  - [ ] Initials available throughout game session
  - [ ] Initials cleared when returning to attract mode
  - [ ] New session requires new initial entry

- [ ] **Edge cases:**
  - [ ] Multiple game sessions with different initials
  - [ ] Game over without high score (no entry screen shown)
  - [ ] Very high score with stored initials auto-populates correctly
