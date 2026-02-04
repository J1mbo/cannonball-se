# Player Initials Entry Feature

## Requirement

Add a new screen/state after the user has chosen their music where the player can enter their initials (similar to the high score entry screen). These initials should be recorded in the outermost span of the telemetry.

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

## Desired Flow

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

## Key Files to Modify

1. **`src/main/engine/outrun.hpp`**
   - Add `GS_INIT_NAME` and `GS_NAME` enum values
   - Add `extern OName oname;` declaration

2. **`src/main/engine/outrun.cpp`**
   - Add state handlers for `GS_INIT_NAME` and `GS_NAME`
   - Instantiate `OName oname;`
   - Update telemetry call to pass initials (line ~457)

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

- [ ] New screen displays after music selection
- [ ] Steering wheel navigates through alphabet
- [ ] Accelerator confirms letter selection
- [ ] Three initials can be entered
- [ ] Delete option removes last initial
- [ ] End option transitions to game start
- [ ] Initials recorded in telemetry game_session span
- [ ] Game flow continues normally after initial entry
- [ ] Visual style matches game aesthetic
