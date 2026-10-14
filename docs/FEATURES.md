# Features: the menu, save states, run-ahead, cheats, training aids, widescreen

Things the port adds to a game. The toolkit has the mechanisms; what they act on in a particular game is
that game's knowledge, in its own sources (`games/<id>/src`).

## The in-game menu

Esc, F1, a gamepad's Back/Select button (alone, or together with Start, as in the other ports) or a stick's
click opens a menu over the game, and closes it. The
game stands still while it is open. Up/Down choose; A, B, C or Start change a row or go into a page;
Left/Right change a row with several values (the slot, the speed) and leave a page. Buttons held as it
closes do not reach the game until let go. Nothing but the menu's "Quit game" row (and closing the window)
ends the game: Esc does not.

The first page, in three groups with a line between them: Save state, Load state, Slot (1-9); a row for
each category of the game's features (which opens a page of them, with Back), Speed, Set reset point, Go to
reset point; Quit game, Resume. It opens on the page and the row it was closed on.

The menu itself (its look, its rules, the pages made from the features) is the one the recomp ports share:
`recomp-common/menu.{h,cpp}` and `recomp-common/text.h` are a copy of the `recomp-common` repository's `src/`, made by
`tools/sync-common.sh` (which records the commit and the files' hashes in `recomp-common/COMMIT`) and never
changed here. `make check-common` fails if the copy is not what was recorded; `make check` and
`make check-rewrites` run it first. To change the menu, change recomp-common, run its tests, commit there,
then run `tools/sync-common.sh` here and commit the copy. `port/menu.{h,cpp}` is what is this toolkit's
about it: the first page's rows, the Saturn pad's buttons turned into the menu's six, the speeds, and the
games' features handed over.

Test hook: `--menu FRAME:KEYS[:shot.png]` opens it at that frame and presses (u d l r a s), headless. A run that
chooses "Quit game" this way ends there.

## Save states

The whole game as it is between two frames, to a file and back (`runtime/state.h`: every part describes its
state once, for writing and reading): the machine (memory, CPUs, interrupts and timers, video timing, the
CD drive's interface), the video registers, the sound chip and its CPU, the music player, the native BIOS,
and the game sources' own state (`Game::state` in `runtime/game.h`).

Keys: F5 saves to the slot, F9 loads it, F6/F7 the slot before and the next; or the menu. States are kept
beside the save memory (`state<N>.bin`). A state is read only by the build that wrote it.

After a load the dispatcher forgets where the CPUs were, and the GPU renderer starts its copy of the VDP1's
framebuffers empty (the sprites are back with the next frame drawn).

Check: `--state-save FRAME:path`, `--state-load FRAME:path` (several, with commas). A state saved in a
fight and loaded into a run that had gone on to something else gives the same state hashes as the original
run from there on (2,000 frames checked; the picture of the first frame after the load differs).

## Run-ahead

The port's own feature `run_ahead` (the menu's Game page and the launcher's Features page): Off, 1 frame or
2 frames. After each frame the next one or two are run on the controls as they are, and the picture of the
last is the one shown (with its readout); then everything is put back as it was after the frame, and the
frame's own sound is the one heard. The game runs exactly as without it; what a press does is seen a frame
or two sooner, which takes away as much of the input lag. It costs a frame's work again for each frame run
ahead.

What is put back (`Port::RunAhead`, `port/main.cpp`): what a save state holds; the renderer whole (a state
leaves out what goes from one frame to the next there: the frame being put together, the layers' pictures),
by keeping a copy of it; the GPU's copies of the VDP1's two framebuffers (`Gpu::KeepSprites`,
`PutSpritesBack`), which a state load would empty; and what the dispatcher keeps about where the CPUs are
(`KeepDispatcher`, `PutDispatcherBack` in `runtime/recomp.h`), which a state load forgets. While frames are
run ahead `RunningAhead()` is set: no sound goes out, and a game's sources leave alone what is not part of
their save state (a file they write, a recording kept across states). Not done while the game is stopped
and gone on a frame at a time, nor slowed, nor when the game says not (`Game::noRunAhead`). The save memory
is not part of states: a game that saves in a frame run ahead writes what it would have a frame or two
later anyway.

Checked (Fighting Vipers, `movies/own_auto_7`, 6,000 frames): the state hashes (memory, CPUs, picture,
sound) frame by frame are the same with it off, on 1 and on 2, in software and on the GPU in widescreen;
the picture shown after frame N with run-ahead on is, byte for byte, that of frame N+1 (or N+2) with it
off. Speed on an Apple silicon Mac, no window, picture not read back, 1920 x 1080 widescreen: 250 frames a
second off, 100 with 1 frame, 74 with 2. (On the Steam Deck, which does 71 to 95 off, it would not keep up.)

## For practice: the reset point, pausing, speed

- **The reset point** is a state kept in memory (not a slot's file) to go back to at a press: F2 sets it, F3
  goes back to it; on a gamepad the right stick's click does both (the first click sets it); or the menu.
  Check: `--reset-test SET:GO`; with nothing pressed, the 98 frames after going back gave the same state
  hashes as those after it was set.
- **F4** stops the game and lets it go on; **F8** goes on one frame (and stops it if it was running).
- **Speed**: 100, 50 or 25 percent (the - and = keys, or the menu). There is no sound in slow motion.

A note at the top of the picture says what was done, or that the game is stopped or slowed.

### Trying the window's own parts with nothing shown

Pausing, the frame step, the speed, the keys and the GPU's readout only exist in the window's loop
(`port/frontend_sdl.cpp`). Three aids run that loop as it is, with a real window and a real GPU device, and
nothing on the screen:

- `SATRECOMP_WINDOW_HIDDEN=1`: the window is made but not shown, and the program stays in the background
  (run with `--mute`).
- `SATRECOMP_WINDOW_KEYS=PASS:KEY,...`: on that pass of the loop the key goes down as if from the keyboard
  (`F1` to `F12`, `ESC`, `MINUS`, `EQUALS`: it is put into SDL's queue and taken by the same code as a real
  press); `SHOT=path` saves the picture as last shown; `QUIT` ends the run.
- `SATRECOMP_WINDOW_LOG=1`: a line whenever the game stops or goes on, the menu opens, or the speed, the
  slot or the note changes, and every 60 frames, each with the time.

What was tried this way (2026-10-12), with the GPU renderer and a movie playing:

- Speed: 60 frames take 1003 ms at 100%, 2005 ms at 50% and 3978 ms at 25%; `-` and `=` step through
  them and the note says so. (This found that the keys did nothing until the menu had been opened once:
  the shared menu only learnt its speeds when opened. `port/menu.cpp` now tells it sooner.)
- F4 stops the game (the frame count stands for as long as it is stopped) and lets it go on; F8 goes on
  exactly one frame a press, and stops a running game.
- F2 and F3 (the notes `RESET POINT SET`, `RESET`), F6 and F7 (the slot goes 1, 2, 3, 2), F5 (the slot's
  file is written beside the save memory) and F9 (read back with no complaint).
- Esc opens the menu, also over a stopped game, and closes it; the game is as it was.
- The readout as the GPU blends it, in a widescreen window, in a fight with the training readouts on
  (and the note alone in a 4:3 one): the lines, the circles over the fighters and the hit view are where the screenshots with
  no window have them. (This found that a note did not appear over a stopped game when a readout was
  already on the picture: the GPU blended the readout once for each picture drawn, and a stopped game
  draws none. `video/gpu.cpp` now keeps the picture from before the readout while one shows, and puts the
  changed readout onto that.) The same with the software renderer's window.

What these cannot tell, and still wants a person at the keyboard: that the operating system's key presses
arrive as these do (and what holding a key down does); a gamepad's buttons (Back, Start with Back, the
sticks' clicks); the picture as the display shows it, as against what the GPU was given (the menu over the
GPU's picture is not in these shots); full screen (F11); sound, and its silence in slow motion; and how
any of it feels to play.

## Features and cheats

A game's sources declare optional features (`runtime/game.h`: name, category, title, description; a flag
for those of a category of which only one may be on; and for a choice of several things, their names, the
first meaning off: `FeatureValue` gives which). `--list-features` prints them for the launcher's Features
page, which writes the player's choices to `settings.ini`; the game reads that (`--settings PATH`) and
`--features A,B` (`-A`: off; `A=2`: a choice's third). The menu switches them too and writes the file as the
launcher would.

**Training aids.** A game marks the features that are training aids (`kFeatureTraining`, or `TrainingAids`)
and says when a training session is on (`Game::inTraining`). Outside one an aid reads as off (`FeatureOn`,
`FeatureValue`), so a readout or a dummy left on does nothing in the game's other modes; the menu still
shows, and `settings.ini` still keeps, what the player chose (`FeatureSetting`).

**Widescreen in the menu.** The port's feature `widescreen` (the menu's Display page; GPU renderer only)
switches the wide picture while playing, from the next frame. Its switch is the launcher's own (on its
first page, so it is left out of `--list-features`: `kFeatureFirstPage`), which says at every start
whether it is on (`--widescreen`); changing it in the menu writes `launcher.ini`'s switch too, or the
launcher would turn it back at the next start. A window (not full screen or maximized) takes the picture's
shape when it changes, and opens in it.

Fighting Vipers (`games/fighting_vipers/src/cheats.cpp`): No damage (player 1's gauge does not go down),
the same for player 2, One-hit knockouts (player 2's first hit taken empties the gauge), Infinite time (the
round's clock stands still). Each is a few lines in the game's rule it changes, in the routines rewritten
as readable code (`games/fighting_vipers/src/fight.cpp`, [READABLE.md](READABLE.md)).

Fighting Vipers' other features hang on the same rewritten routines. All of them but Quick rounds and
Armour are training aids: they work in the game's own TRAINING MODE (its main menu; the game's mode 5) and
nowhere else.

- **Quick rounds** (`round.cpp`, `QuickWait`): the waits of a round are counts the game takes down; they are
  cut to 40 frames before a round (192 before the first and 96 before the others, as the game has it), 20
  before "Fight!" (42), 45 after the round (96) and 60 while the winner is shown (up to 320). From the
  round's set-up to the fight: 67 frames in place of 238.
- **Damage readout** (`training.cpp`): by each gauge, what the last hit took off it, and for a combo the
  number of hits and what they took together. `TakeDamage` reports each hit. A combo is hits between
  which the fighter hit could not act: its state never lost the busy bit (the same bit the frame data
  goes by). Punches 24 frames apart against a standing dummy are single hits (it could have guarded the
  second); the two that still land on the dummy that guards after a hit count as two.
- **Input display** (`training.cpp`): down each side, each fighter's last ten inputs, the newest at the top,
  each with the frames it was held, in number pad notation for a fighter facing right (6 forward, 4 back,
  2 down, 8 up, 5 nothing), then G, P, K for guard, punch, kick. The rewritten input step reports each
  frame's input as it goes into the game's history. The game's own play is shown the same way.
- **Gauges refill** (`training.cpp`): a gauge is whole again 90 frames of the fight after its fighter was
  last hit. (The training mode's fight takes nothing off a gauge: what a hit is worth is reported to the
  readouts all the same, as the game's own DAMAGE display has it.)

- **Frame data** (`training.cpp`): after a move hits or is guarded, by the gauge of whoever did it: the frame
  of the move it hit on, and how many frames sooner (+) or later (-) than the other its fighter can act
  again (`HITS ON 9  HIT +0`, `HITS ON 13  GUARD -1`). A fighter can act again when the "busy" bit of its
  state goes (`fv.h`, `fighter::kBusy`); that this is the frame was tried with a dummy pressing punch out of
  being hit. Knockdowns and the like (a second or more apart) are not shown. The frame hit on can be one
  out: being hit and guarding do not start on the same frame after contact. A throw is shown the same way
  once both can act again: `GRAB 1  DMG ON 75  +112` (the grab came on the frame after the buttons were
  read, the damage on the 75th frame of the throw, and the thrower can act 112 frames before the other),
  or `GRAB 1  ESCAPED  +7` when the thrown fighter struggled free.
- **Move data** (`training.cpp`): by the gauge of whoever attacked, the last attack: `MOVE 25F HIGH` (the
  move's number in the game, and its level: HIGH misses a crouching fighter, MID hits a crouching guard,
  LOW hits a standing guard, MID+ as MID but a crouching guard takes all of it, GROUND hits a fighter lying
  down), `START 9 ACT 1 REC 14` (the frame it first hits on, the frames it can hit for, the frames after
  those until its fighter can act), `DMG 9  ARMOUR 12` (off the other's gauge, onto its armour's damage),
  and `HIT +0  GUARD -2` once each has been seen (`HIT DOWN`: it knocks down). The frames are counted from
  the fighter's state as the move is done, the level and the rest read from the move's own data; a throw
  has `THROW 015` and `FREE AFTER 59`. Every attack done is kept in a table by character and move for as
  long as the game runs, and written half a second after it changes to `moves-<character>.csv` in the folder of
  `settings.ini` (grace, bahn; the other characters by number until their numbers are known): move, kind,
  level, power, the data's own frames (hits from, hits until, free from, length), start, active, recovery,
  damage, armour damage, on hit, on guard, knocks down, times seen. The damage is the most seen and the
  advantages the last seen, and both depend on the other's armour (Grace's punch: 9 and +0 on Bahn's
  armour, 18 and +5 with it off), so a table to set against a published one wants one armour setting
  throughout. Seen with Grace against Bahn, armour on: punch
  9 / 1 / 14, 0 on hit and -2 on guard; kick 12 / 4 / 26, +2 and -1; down and kick, low, 19 / 1 / 35, 0 on
  hit and -10 guarded crouching.
- **Throw range** (`training.cpp`): on the ground before each fighter, dots along the reach of its throw (an
  arc 30 degrees to each side of the way it faces, at the distance the game asks: 1.7 m, 1.6 m from a
  crouching fighter, 1.4 m from behind), lit while the game itself has the throw as possible; in the hit
  view, the same along the ground. And a line each: `THROW 1.07/1.70 OK` (how far apart, the reach, and
  the verdict: OK; FAR; TURNED, one of the two does not face as a throw wants; NO, the other cannot be
  thrown as it is: attacking, hit, down or in the air; WAIT, a hit or a guard was within the last 14
  frames). The verdict is the game's own (the bits its code tests when guard and punch are pressed), the
  rest says why. While a fighter is being thrown its line is `STRUGGLE 12 OF 19`: the presses the game has
  counted, and how many break the throw.
- **Dummy struggles in throws** (`training.cpp`): thrown, the opponent presses the buttons every other frame.
  That is the game's way out of a throw (19 presses in the 52 frames of being thrown by guard and punch):
  the dummy breaks free unhurt, so a throw is only kept by what the player does after it.
- **Hit overlay** (`training.cpp`): the same balls as the hit view below, laid over the fighters: projected
  with the game's own view matrix (taken where the fighters are about to be drawn) and its perspective.
  The balls are the hit task's, worked out before the fighters move, so in fast motion they are a frame
  behind the picture.
- **Hit view** (`training.cpp`): a panel with what the game tests for hits, seen from the side of the line
  between the fighters: each body as the game has it, 22 balls. Outlined: parts that can be hit (grey:
  not now); red: the parts the fighter's move attacks with (the game doubles their size); yellow: parts an
  attack is touching. The balls, their sizes and the hit task's tables are the game's own (`fv.h`, `hit`).
  It shows depth-independent overlap that the overlay cannot.
- **Armour readout** (`training.cpp`): by each gauge, what is left of the upper and the lower armour (its
  strength less the damage it has taken), or OFF.
- **Armour** (`training.cpp`; one of them at a time): the armour stays on (it takes no damage, and armour
  that is off is put back); or the opponent has none; or nobody has. Armour is changed the way the game
  does it when armour is knocked off: the flags in the fighter's record, the fighter's model by the game's
  own routine, the weight and the armoured parts of the body worked out anew. The game's display follows
  by itself when armour goes; when it is put back the display is drawn anew by the game's set-up routine
  (after which "PRESS START BUTTON" is missing until the game next draws it).
- **Dummy** (`training.cpp`; one of them at a time): the opponent stands, crouches, guards (standing, low,
  standing or low by chance, chosen anew as each attack begins; or for a second after each hit it takes, to
  see what is a true combo; or guards and counters as soon as it can after guarding or being hit, to see
  what is safe) or jumps instead of fighting; or **record**: your pad plays the opponent while your own fighter stands, and up to ten
  seconds of it are kept (switching it on starts anew; the readout shows `REC` and the time; the recording
  is not part of states, so the reset point leaves it alone); or **replay**:
  the opponent does what was recorded, over and over. The dummy is the opponent of whoever plays (player 2
  when both do). Its input is put in where a player's input for the frame is taken (`fightinput.cpp`,
  `PlayerInput`, one of the rewritten routines), over what the pad or the game's own play gave. The
  recording holds left and right as
  towards and away from the other fighter. Checked: a player walking in and punching takes gauge off the
  standing dummy with every punch and none off the guarding one.

- **Dummy counter** (`training.cpp`; one of them at a time): what the dummy that guards and counters answers
  with: a punch if none is on; a kick; a low kick (down and kick); a throw (guard and punch: within the 14
  frames after a hit or a guard the game allows no throw, and the same buttons are an attack); or the
  recording, played once from its start on the first frame the dummy can act. Seen against Grace's punch:
  Bahn's punch, kick, low kick and guard-and-punch attack each come out on the first frame he can act.
- **Dummy wake-up** (`training.cpp`; one of them at a time, with any dummy or with the game's own play): what
  the opponent does on the first frame it can act after being down: guards, punches, kicks, kicks low,
  presses guard and punch, or does the recording from its start. It goes with the recovery chosen below (a
  dummy that rolls back and then punches). Seen after a throw: each comes out the frame after the fighter
  is up; with "lands on its feet" that frame is still in the air, and the kick is a jumping one.
- **Dummy recovery** (`training.cpp`; one of them at a time, with any dummy or with the game's own play): what
  the opponent does when it is knocked down. With none on it lies there as long as the game lets it (86
  frames, then 36 to get up). **Lands on its feet**: the buttons pressed from the hit, while still in the
  air. **Gets up fast**: the buttons pressed once it is down (up after 16 frames, by a quick way of its
  own). **Rolls back / forward / aside**: the direction held (up after 46, rolling). **Gets up kicking**,
  **kicking low**: kick, or down and kick, pressed again and again. Each was tried on a fighter the game
  knocks down, and the move it gets up with and the frames are as when the same buttons come from a pad.
  **At random**: one of these or nothing, by chance as it goes down.
- **At random**, the dummy's other choices: **wake-up** at random (one of those above, or nothing, by chance
  as it gets up; seen after 20 knockdowns: low kicks, kicks, throws and punches where "kicks" kicked every
  time); **Dummy struggles in throws** is Off, Always or At random (in a throw in two, by chance as the
  throw begins); and **Dummy answers now and then**: its counter and its wake-up only one time in two, by
  chance (seen: 21 counters of 21 attacks without it, 17 of 27 with it).
- **Move log** (`training.cpp`): every attack done, a line of `frame_data.csv` beside the settings, once
  what came of it is known (or a second after it was over, or when another attack takes its place): the
  player, the character, what was pressed for it (the input as a button was last pressed, in number pad
  notation), the move, its kind and level, start, active and recovery as the readout counts them, the
  result (hit, guard, knockdown, throw, escaped; nothing: it met nothing), the advantage, and the damage
  to the gauge and to the armour. Grace's punch on a guarding Grace: `1,grace,P,25F,attack,HIGH,9,1,14,guard,-2,0,18`.

There is no stun readout: the game keeps no stun or dizzy value (games/fighting_vipers/PROGRESS.md says
what was looked at).

## The readout

`SATRECOMP_HUD_LOG=1` prints the readout's lines whenever they change, so that a test with no window can
read them.

Text over the picture while the game plays (`runtime/game.h`: `SetHud` with lines placed by parts of the
picture's width and height), a diagram of circles on a panel (`SetHudDiagram`), and circles straight over
the picture (`SetHudCircles`). The port draws it all with alpha (`port/hud.*`): onto the picture itself with the software
renderer; with the GPU renderer as one picture of at most 720 lines that the GPU blends over its output
(`Gpu::SetHudLayer`), which screenshots show as the window does.

## Widescreen

The launcher's switch (`--widescreen`), or the menu's Display page while playing (above). See docs/VIDEO.md. What of a game's 2D display belongs at the picture's edges is the game's knowledge too
(`Game::wideSplits`; Fighting Vipers: `games/fighting_vipers/src/widescreen.cpp`, the gauges and names move
out to the edges, the timer and messages stay in the middle).
