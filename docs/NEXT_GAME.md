# What a second game will meet

Fighters Megamix was the second game (see "What Fighters Megamix met" at the end). This page was first
written when satrecomp had carried one game, Fighting Vipers, and the machine was built to what that game's
code asks for ([MACHINE.md](MACHINE.md): "only what the code of the games ported so far needs"). This
is a list of the places where that shows, for whoever starts the next game. **None of it is to be tidied in advance**: each item waits until a second game needs it,
and is then solved for both games, checked on both.

## In the machine

- **The start-up race** (`machine/machine.cpp`, the write to the MINIT / SINIT area; `kFineCyclesAfterSignal`
  in `machine.h`). When one CPU signals the other, its turn ends and the two alternate in 32-cycle slices
  for 20,000 cycles. The number was found for Fighting Vipers, whose master resets the command queue the
  two share and writes a command about twenty instructions later, relying on the slave running in between.
  Another game may need longer, or need none of it. Fighting Vipers also needs one sync point in its
  `game.cfg` (`sync_points`); expect other games to need their own.
- **Time is not the hardware's.** DMA, the VDP1's drawing and bus waits take no time; instructions cost a
  fixed number of cycles by kind. A game that times something by these will differ.
- **The CD drive** (`machine/cd.cpp`) knows the commands Fighting Vipers' library gives at start-up and when
  its movie player starts, and no others: 00, 02, 03, 04, 06, 10, 11, 30, 42, 44, 46, 48, 50, 51, 60, 75, E0.
  "Play" only has to be seen to go; no sector comes from the drive. Files, the movie's data and CD music are
  read by the port, through routines replaced in the game's own sources (`games/<id>/src`: its file loader,
  its CD library's open / start / poll / close, its music calls). A second game needs its own such routines
  (if it shares the library version, the same ones by code hash), or more of the drive.
- **The system manager** (`machine/machine.cpp`) knows commands 02, 03, 06, 07, 0D, 0E, 0F, 10, 17, 18, 19
  and 1A (Fighting Vipers gives 02, 03, 06, 07, 0E, 10, 19 and 1A; the others are ignored); control pads are read at the level of the game's peripheral library (`games/<id>/src/input.cpp`),
  not through the chip's own report.
- **The system control unit**: DMA levels 0 to 2, the two timers, the interrupt mask and status as written;
  the game never reads them back. Its DSP is not there at all (Fighting Vipers runs no DSP instruction).
- **The SH-2's own devices**: the free-running timer, the divider, DMA channels 0 and 1, the interrupt
  controller, the cache register as a plain register. No serial port, and no watchdog timer beyond its interrupt vector register.
- **The hardware addresses** the machine answers are the ones `SATRECOMP_IO_STATS=1` listed for this game on
  the reference build. Run it for the next game first, and compare the lists.
- **No cartridge**: nothing at the cartridge slot (no extra RAM, no ROM cartridge).

## At start-up (`runtime/hle_bios.cpp`)

- The drive is left as this game expects it: authenticated, seeked to frame address 150, paused, the
  "disc changed" request cleared.
- The services given are the ones this game calls (interrupt entry and exit, setting handlers and the mask,
  the clock change, semaphores, the slave's start, the backup memory library). Saves are always there, in a
  file; no cartridge or external device.
- `build.mk` translates only blocks in low work RAM and in high work RAM above 0x06002000 (`GEN_REGIONS`).

## In the picture ([VIDEO.md](VIDEO.md), "What is not done")

Not drawn, because Fighting Vipers does not ask for them (each is counted and reported at exit when a game
does): colour calculation between layers, the line colour screen, mosaic, vertical cell scroll, RBG1,
framebuffer rotation; on the GPU the VDP1 modes that read the framebuffer (shadow, half transparency, MSB
on), end codes cutting a line, the game reading or writing the framebuffer itself, the sprite window as
another layer's window, 8-bit framebuffers. The normal screens are drawn at the original resolution.

Widescreen is a renderer switch plus, for the fight display at the edges, splits the game names
(`TheGame().wideSplits`); that Fighting Vipers' 3D code culls by depth and facing only (so nothing
is missing at the sides) is a fact about that game, to be found again for each game.

## In the sound ([SOUND.md](SOUND.md))

The 68000's sound driver is run by the interpreter (it is 2% of the time). Checked against the reference
with this game's driver only; another driver will use other parts of the chip (the DSP's programs, FM
between slots, other loop kinds).

## In the tools and names

- Defaults that name the first game: `GAME` in `CMakeLists.txt`, `tools/check.sh`, `tools/verify.sh`,
  `tools/gamecfg.py`, `tools/liveout.py` (all take `GAME` from `make`, so a second game is not affected).
- `docs/` gives Fighting Vipers' figures and addresses as its examples, and writes paths as a game's
  repository has them (`satrecomp/...`, `games/<id>/...`).
- The launcher's description (`launcher/src/description.cpp`) checks a disc image by the product number and
  version in `game.cfg`: one disc, one version.

## What is already the game's, not the toolkit's

`games/<id>/game.cfg` (name, app id, disc, sync points, disclaimer), `games/<id>/src/` (names, replaced and
rewritten routines, features, widescreen splits), `games/<id>/movies/`, `games/<id>/check_rewrites.sh`.
Movies replay only on the machine and timing they were recorded on: a change to the machine's timing for
a second game means recording the first game's movies again, and its `make check` and
`make check-rewrites` passing before the toolkit change is kept.

## What Fighters Megamix met

Built on Fighting Vipers' engine, it carries many of the same routines byte for byte, at other addresses:
Sega's file, CD and peripheral libraries and AM2's file loader and pad reader. Those replacements moved into
the toolkit (`runtime/shared_lifts.h`); a game finds its copies by searching its program for the bytes of
each (the hash covers the code, not its constant pool) and registers them. Fighters Megamix also needed:

- Two sync points: its own copy of the master's command queue reset (the same code as Fighting Vipers'),
  and the copy in its opening movie's player, a second program loaded at 060C0000.
- Data sectors from the CD drive (the movie player's own file library reads the disc's directory and the
  movie through the drive), a drive that pauses at the end of what was asked for at once, and a subcode
  read reported with the drive stopped.
- Cache purges (writes to 0x40000000 and up) that leave memory alone.
- `fine_slices` for the movie player, which hands the slave a job every frame.

Still open (in Fighters Megamix's PROGRESS.md): its opening movie freezes on its first frame on the own
machine (the movie's clock follows the sound driver's stream position, which stops); skipping the movie
with Start leaves the slave CPU running stale commands when started by `runtime/hle_bios.cpp` (it works
with a BIOS image). The game itself boots, plays and passes `make check`.
