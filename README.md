# satrecomp

[![CI](https://github.com/tmcleroy/satrecomp/actions/workflows/ci.yml/badge.svg)](https://github.com/tmcleroy/satrecomp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/tmcleroy/satrecomp)](https://github.com/tmcleroy/satrecomp/releases)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

**A toolkit for making native PC ports of Sega Saturn games by static recompilation.** A game's
SH-2 code is translated to C++ ahead of time and compiled into the port. There it runs on a small
machine written for this toolkit: two SH-2s, the bus, interrupts and timers, the video chips (drawn
in software or on the GPU), the sound chip and its 68000, and the BIOS's services as C++ routines.
No emulator is part of a port, no BIOS image is needed, and no game data is in this repository or
in a game's.

> satrecomp is not affiliated with or endorsed by Sega. "Sega" and "Sega Saturn" are trademarks of
> their owner, used here only to say what the toolkit works with. **No game is included**: a port
> needs a disc image its player made from their own disc.

## Ports made with satrecomp

No port made with satrecomp is public yet. Each one will be listed here when it is.

To play, download a port, not this toolkit. This repository is for people making or working on
ports.

## What a port gets from it

- **Translation ahead of time.** `tools/recomp.py` turns the game's SH-2 code into C++ from a block
  database that is built up by recording what runs. Code it has not seen yet falls back to an
  interpreter, and every fallback is counted, so that the count can be driven to zero.
- **A machine of its own, not an emulator.** The SH-2s, the system control unit, the system manager,
  video timing, saves in the console's format, CD music, and the BIOS's services rewritten as C++
  ([docs/MACHINE.md](docs/MACHINE.md)).
- **A native renderer.** The VDP2's screens and the VDP1's commands, drawn in software or on SDL's
  GPU API (Metal, Vulkan, Direct3D 12) at the window's resolution, with widescreen for games whose
  sources ask for it ([docs/VIDEO.md](docs/VIDEO.md)). The sound chip and its 68000 are covered in
  [docs/SOUND.md](docs/SOUND.md).
- **Readable rewrites, with proof.** A game's routines can be rewritten by hand as ordinary C++. A
  checker runs the original and the rewrite on every call and compares memory, registers and timing
  ([docs/READABLE.md](docs/READABLE.md)).
- **Features players expect.** Save states, a reset point, speed and pause, cheats and training aids
  that a game declares once, a HUD readout, and the in-game menu and launcher the recomp ports share
  ([recomp-common](https://github.com/tmcleroy/recomp-common); [docs/FEATURES.md](docs/FEATURES.md)).
  Every change to how a game plays is opt-in: with all features off, the game runs as the original.
- **Checks at every step.** Translated code is checked against the interpreter, runs of the same
  input movie must give the same per-frame hashes, and the toolkit's checks cover the rest
  ([docs/VERIFYING.md](docs/VERIFYING.md)).
- **macOS, Linux (Steam Deck included) and Windows** through SDL 3. CI builds and starts the
  toolkit on all three.

## Trying it with no game

`make check` needs no game. It audits the files, checks the copy of recomp-common, builds the program
with no translated code and the launcher for the stand-in game in `example/`, starts both, and tries
`sync-toolkit.sh`.

```sh
git clone https://github.com/tmcleroy/satrecomp
cd satrecomp
make check
```

You need a C++20 compiler (Clang, GCC or MinGW-w64), CMake, Ninja, Python 3 and SDL 3. On macOS
that is
`brew install cmake ninja sdl3`. [CONTRIBUTING.md](CONTRIBUTING.md) has the rest, and
`.github/workflows/ci.yml` shows exactly what each system installs.

## How a game uses it

A game is its own repository:

```
satrecomp/      a copy of this repository, made by satrecomp/tools/sync-toolkit.sh (never changed there)
games/<id>/     game.cfg (name, the disc's product number and version, sync points), src/ (names, features, rewrites), movies/
Makefile        GAME ?= <id>;  SR ?= satrecomp;  include $(SR)/build.mk
```

`build.mk` has the targets (`make`, `make record`, `make check`, `make check-rewrites`,
`make launcher`, `make app`; `make setup`, `make ref` and `make verify` for the reference build).
The checks run `make check-toolkit`, which fails if the game's copy of the toolkit is not what
`sync-toolkit.sh` recorded. `example/` is a game repository with no game in it: start from it.

Generated files go to the game repository's `work/<id>/` (the block database, generated C++, saves)
and builds to its `build/<id>/`. They are made from the game, so git ignores them there, and none of
them is ever part of this repository.

[docs/NEXT_GAME.md](docs/NEXT_GAME.md) lists what in the toolkit is still shaped by the one game it
has carried so far (start-up timing, the CD commands, video features nobody has asked for yet, the
SCU DSP). Read it before you start on a second game.

## Working on the toolkit

Change it here, never in a game's copy. Build and check a game against this clone from the game's
repository:

```sh
make SR=../satrecomp
make SR=../satrecomp check FRAMES=8000 MOVIE=games/<id>/movies/<movie>.txt
make SR=../satrecomp check-rewrites
```

Such a build goes to `build/<id>-dev`, apart from the game's usual one (the check scripts follow:
`B=build/<id>-dev`). Commit here, then in each game: `satrecomp/tools/sync-toolkit.sh`, run the
game's checks, commit. Anything about one game belongs in that game's `game.cfg` or `src/`, not here.

## The reference build

For development only, the same game code can run on a reference machine, the
[Ymir](https://github.com/StrikerX3/Ymir) Saturn emulator (GPL-3.0), for comparison: the translated
code against its interpreter, the renderer against its pixels, the sound chip and the 68000 against
its state ([docs/VERIFYING.md](docs/VERIFYING.md)). Neither Ymir nor the patch that makes it the
reference machine is in this repository or in a game's. `make setup` fetches Ymir into
`third_party/` (which git ignores) and applies the patch from the folder `SATRECOMP_REFERENCE` names.
The patch is not published, so for now only the maintainer can make the reference build. Nothing of
it is needed to build, check or play a game.

## Repository layout

| Path | What |
| --- | --- |
| `tools/` | The translators (`recomp.py`, `sh2emit.py`; `recomp68k.py` for the reference build), `gamecfg.py`, the checks (`check.sh`, `verify.sh`), the block database (`merge_blocks.sh`), reading the game's code (`sh2dis.py`, `liveout.py`, `callgraph.py`, `lifthash.py`, `liftcoverage.py`), `gen_shaders.py`, `pngdiff.py`, `make_app.sh`, `sync-toolkit.sh`, `sync-common.sh`, `audit.sh` |
| `runtime/` | What translated code is written in terms of (the block dispatcher and recorder, calls between translated functions), the start-up code and BIOS services (`hle_bios.*`), saves in the console's format (`backup.*`), CD music (`music.*`), routines replaced by C++ and readable rewrites (`lift.h`), features and settings (`game.h`), save states (`state.h`) |
| `machine/` | The SH-2 (with an interpreter for code that has no translation), bus, system control unit, system manager, video timing, and the CD drive commands that are still needed ([docs/MACHINE.md](docs/MACHINE.md)) |
| `video/`, `sound/` | The renderer: the VDP2's screens and the VDP1's commands, in software and on SDL's GPU API (shaders in `video/shaders/`, compiled into `shaders.inc` by `tools/gen_shaders.py`); the sound chip and the 68000 |
| `port/` | The frontend (SDL 3), the disc image reader, the in-game menu adapter, the HUD, control bindings |
| `recomp-common/` | A copy of [recomp-common](https://github.com/tmcleroy/recomp-common) (the in-game menu, the settings rules, the launcher), made by `tools/sync-common.sh`; never changed here |
| `launcher/` | What this toolkit tells the shared launcher (`recomp-common/launcher`; SDL 3 + Dear ImGui): `src/description.cpp` has the words for a disc image, the check of the player's, the game's arguments, the Display switches and the buttons that can be chosen |
| `example/` | A stand-in for a game's repository, with no game: what a game's `Makefile` and `game.cfg` look like, and what `make check` here builds |
| `docs/` | `MACHINE.md`, `VIDEO.md`, `SOUND.md`, `VERIFYING.md`, `READABLE.md`, `FEATURES.md`, `NEXT_GAME.md` |

## Related projects

| Project | What |
| --- | --- |
| [cps2recomp](https://github.com/tmcleroy/cps2recomp) | The same kind of toolkit for Capcom CPS-2 arcade games |
| [recomp-common](https://github.com/tmcleroy/recomp-common) | The in-game menu, settings rules and launcher this toolkit and cps2recomp share |
| [Vampire Savior Recomp](https://github.com/tmcleroy/vampire-savior-recomp) | A native port of *Vampire Savior* (CPS-2), made with cps2recomp |
| [Muscle March Recomp](https://github.com/tmcleroy/muscle-march-recomp) | A native port of *Muscle March* (WiiWare), by the same author |
| [doaxvb-re](https://github.com/tmcleroy/doaxvb-re) | A native Windows port of *Dead or Alive Xtreme Beach Volleyball* (Xbox), by the same author |

## Contributing

Bug reports and pull requests are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). The hard rules:
nothing from any game, no BIOS, nothing from Sega's development kits or from leaked material, and no
emulator's code. Security reports go by [SECURITY.md](SECURITY.md).

## License and credits

[GPL-3.0-only](LICENSE). [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) says where everything
came from.

- The machine's models were written for this toolkit with Ymir's source open beside them as the
  description of the hardware, and they were checked against Ymir state by state. The notices say
  so in full.
- The launcher's look, and the files that make it, come from the launcher of
  [wiiparty-recomp](https://github.com/arelkair/wiiparty-recomp) by arelkair (GPL-3.0-or-later). They
  are in `recomp-common/launcher/`, with the notices in recomp-common's `THIRD_PARTY_NOTICES.md`.
- The launcher's build downloads SDL 3 (zlib license), Dear ImGui (MIT) and the Inter font (SIL OFL
  1.1, compiled into the program). A launcher given out as a binary must carry those three licenses.
