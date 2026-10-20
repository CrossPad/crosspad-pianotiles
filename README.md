# crosspad-pianotiles

Piano Tiles, written from scratch for CrossPad: the one app of the
`pianotiles` firmware kit, started from the Firmware app's library like any
other firmware and left through its pause menu's **Quit**, which switches back
to the other firmware slot.

Everything goes through crosspad-core and crosspad-gui (2.x), nothing through
a platform's own API: the pads are a pad logic (`IPadLogicHandler`), the
encoder an LVGL group, the power click a back handler, the sound a node on
the audio module (`IAudioNode`), the progress a file under the GUI platform's
storage path, leaving the firmware the Firmware views' service
(`IFwViewService::flowSwitchAndRestart`).

## The game

Tiles fall down four lanes, the four columns of pads. Each tile is one note of
the tune; play the column of the next tile when it reaches the blue line and
your synth plays that note, so playing well plays the song.

- **The pads show the bottom of the field.** The bottom row is the hit window
  ("play now"), each row above it half a beat further away; the panel draws
  the same rows as faint lines above the blue hit zone. You can play by
  looking at the pads alone.
- **Hold tiles** (notes of two beats or more): press at the head and keep the
  pad down while the tile passes; the note sounds as long as you hold.
  Letting go early only loses the hold bonus.
- **A run ends** on a **MISSED** tile (it passed the line unplayed), a
  **WRONG PAD** (a column with no tile due), or **TOO EARLY** (the right column
  before its tile reached the hit window). The panel says which and why, the
  wrong column turns red and the right one green, on the panel and on the
  pads.
- **Score**: PERFECT (within 0.2 beat of the line) 100, GOOD 50, +5 for each
  PERFECT in a row (up to 40), a hold held through 50 a beat; all times the
  level. The best score of every song and level is kept.
- **Levels**: five per song, each 20 % faster than the one before (level 5 is
  1.8 x the song's tempo). Clearing a level opens the next one; clearing any
  level of a song opens the next song. The song starts when you play its
  first tile (the START tile).

### Songs

All public domain. The beat is a quarter note, except Für Elise (an eighth).

| Song | Composer | Tempo (level 1) | Tiles | Holds |
|---|---|---|---|---|
| Twinkle Twinkle | traditional | 100 | 42 | 6 |
| Frère Jacques | traditional | 100 | 32 | 4 |
| Ode to Joy | L. van Beethoven | 108 | 62 | 4 |
| Jingle Bells (chorus) | J. L. Pierpont | 120 | 51 | 9 |
| Canon in D (theme) | J. Pachelbel | 84 | 35 | 11 |
| Für Elise (opening, twice) | L. van Beethoven | 120 | 70 | 1 |

Melodies are text in `src/PianoTilesGame.cpp` (`C5 D#5*0.5 R*0.5 G4*2 | ...`,
see `parseMelody()`); lanes follow the tune's contour (up = right) and never
repeat a column twice in a row.

## Controls

Pads, by their printed labels (1 = bottom left, 16 = top right):

| Pad | | Pad | | Pad | | Pad | |
|---|---|---|---|---|---|---|---|
| 13 | lane 1 | 14 | lane 2 | 15 | lane 3 | 16 | lane 4 |
| 9 | lane 1 | 10 | lane 2 | 11 | lane 3 | 12 | lane 4 |
| 5 | lane 1 | 6 | lane 2 | 7 | lane 3 | 8 | lane 4 |
| 1 | lane 1 | 2 | lane 2 | 3 | lane 3 | 4 | lane 4 |

- **In play** any pad of a column plays that lane (the bottom row is where the
  tile is due). Pad colours: green = play now, blue = a tile coming, purple =
  a hold tile's body, amber = the hold you are holding, red/green blinking =
  what ended the run.
- **In a menu** the left column moves the highlight (pads 9/13 up, 1/5 down),
  the middle columns choose, the right column goes back (as power).
- **Encoder turn**: the highlight (song, level, menu item); nothing in play.
- **Encoder click**: choose; on a result, skip to its menu.
- **Power click**: in play, the pause menu (RESUME / RESTART / SONG MENU /
  QUIT) and back (a 3-2-1 count-in before the tiles move again); in the level
  choice, back to the songs; in the song list, the highlight to QUIT.
- **QUIT**: back to the firmware in the other slot. With none there the menu
  says so and the game goes on.

## Data on the card

`crosspad/pianotiles/progress.txt`: one line per song,
`<id> <levels open> <best L1> .. <best L5> <levels cleared, bits>`. Written
when a run ends; delete it to start over.

## Sound

`src/PianoTilesSynth.hpp`: eight voices of a small piano-like tone (four
harmonics from a sine table, the upper ones dying faster, longer decay for low
notes, 3 ms attack, short release). The game writes note-on/off commands into
a lock-free ring; the audio thread allocates and locks nothing.

## Layout

| Path | |
|---|---|
| `src/PianoTilesGame.{hpp,cpp}` | the rules in plain C++: songs, melody parser, lanes, hit window, holds, score, levels, what the pads show, saved progress |
| `src/PianoTilesSynth.hpp` | the sound node |
| `src/PianoTilesApp.cpp` | the app: field, menus, encoder, pads and their LEDs, power, Quit, the progress file |
| `tests/test_pianotiles_game.cpp` | host test of the rules, with a balance run (command in the file) |
| `tests/test_pianotiles_synth.cpp` | host test of the synth node (command in the file) |
| `CMakeLists.txt` | the ESP-IDF component: built into the `pianotiles` kit only (`crosspad-app.json` `kits`), empty in every other image |
| `assets/pianotiles.png` | launcher icon |

The game steps on a 16 ms `lv_timer` on the LVGL thread; pad presses carry the
time they happened and are replayed in order against the song clock, so a
press is judged by when it happened, not by the timer tick.

## Not done

- Not built or played on the board yet: only in the crosspad-pc simulator.
- No velocity-sensitive scoring (velocity only sets the note's loudness), no
  two-hand chords (one tile at a time), no songs from the card.
- The progress file is written on the LVGL thread when a run ends (a few
  hundred bytes, the field frozen behind the result).

## Licence

GPL-3.0-or-later (`LICENSE`). No third-party code; the tunes are public
domain.
