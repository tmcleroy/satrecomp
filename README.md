# satrecomp

A toolkit for native ports of Sega Saturn games by static recompilation. A game's SH-2 code is
translated to C++ ahead of time and compiled into the port, where it runs on a small machine of the
toolkit's own: two SH-2s, the bus, interrupts and timers, the video chips (drawn in software or by
the GPU), the sound chip and its 68000, and the BIOS's services as C++ routines. No emulator is
part of a port, no BIOS image is needed, and no game data is part of this repository or of a game's.

satrecomp is not affiliated with or endorsed by Sega. "Sega" and "Sega Saturn" are trademarks of
their owner, used here only to say what the toolkit works with. No game is included: a port needs
a disc image its player made from their own disc.

| Path | What |
| --- | --- |
| `tools/` | The translators (`recomp.py`, `sh2emit.py`; `recomp68k.py` for the reference build), `gamecfg.py`, the checks (`check.sh`, `verify.sh`), the block database (`merge_blocks.sh`), reading the game's code (`sh2dis.py`, `liveout.py`, `callgraph.py`, `lifthash.py`, `liftcoverage.py`), `gen_shaders.py`, `pngdiff.py`, `make_app.sh`, `sync-toolkit.sh`, `sync-common.sh` |
| `runtime/` | What translated code is written in terms of (the block dispatcher and recorder, calls between translated functions), the start-up code and BIOS services (`hle_bios.*`), saves in the console's format (`backup.*`), CD music (`music.*`), routines replaced by C++ and readable rewrites (`lift.h`), features and settings (`game.h`), save states (`state.h`) |
| `machine/` | The SH-2 (with an interpreter for code that has no translation), bus, system control unit, system manager, video timing, what is still asked of the CD drive ([docs/MACHINE.md](docs/MACHINE.md)) |
| `video/`, `sound/` | The renderer: the VDP2's screens, the VDP1's commands, in software and on SDL's GPU API (shaders in `video/shaders/`, compiled into `shaders.inc` by `tools/gen_shaders.py`); the sound chip and the 68000 ([docs/VIDEO.md](docs/VIDEO.md), [docs/SOUND.md](docs/SOUND.md)) |
| `port/` | The frontend (SDL3), the disc image reader, in-game menu adapter, HUD, control bindings |
| `recomp-common/` | A copy of [recomp-common](https://github.com/tmcleroy/recomp-common) (the in-game menu, the settings rules, the launcher), made by `tools/sync-common.sh`; never changed here |
| `launcher/` | What this toolkit tells the launcher the recomp ports share (`recomp-common/launcher`; SDL3 + Dear ImGui): `src/description.cpp` has the words for a disc image, the check of the player's, the game's arguments, the Display switches and the buttons that can be chosen |
| `example/` | A stand-in for a game's repository, with no game: what a game's `Makefile` and `game.cfg` look like, and what `make check` here builds |
| `docs/` | `MACHINE.md`, `VIDEO.md`, `SOUND.md`, `VERIFYING.md`, `READABLE.md`, `FEATURES.md`, and `NEXT_GAME.md`: what in the toolkit is still shaped by its first game |

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
`sync-toolkit.sh` recorded.

Generated files go to the game repository's `work/<id>/` (the block database, generated C++, saves)
and builds to its `build/<id>/`. They are made from the game, so git ignores them there, and none of
them is ever part of this repository.

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

With no game at all, `make check` in this repository audits the files, builds the program with no
translated code and the launcher for the stand-in game in `example/`, starts both, and tries
`sync-toolkit.sh`; CI does the same on macOS, Linux and Windows. See
[CONTRIBUTING.md](https://github.com/tmcleroy/satrecomp/blob/main/CONTRIBUTING.md).

## The reference build

For development only, the same game code can be run on a reference machine, the
[Ymir](https://github.com/StrikerX3/Ymir) Saturn emulator (GPL-3.0), to compare with: the translated
code against its interpreter, the renderer against its pixels, the sound chip and the 68000 against
its state ([docs/VERIFYING.md](docs/VERIFYING.md)). Neither Ymir nor the patch that makes it the
reference machine is in this repository or in a game's: `make setup` fetches Ymir into
`third_party/` (which git ignores) and applies the patch from the folder `SATRECOMP_REFERENCE` names
(`../satrecomp-reference` beside the game's repository by default). Nothing of it is needed to build
or play a game.

GPL-3.0. The machine's own models were written for this toolkit with Ymir's source open beside them
as the description of the hardware;
[THIRD_PARTY_NOTICES.md](https://github.com/tmcleroy/satrecomp/blob/main/THIRD_PARTY_NOTICES.md) says so in full.

The launcher's look, and the files that make it, come from the launcher of
[wiiparty-recomp](https://github.com/arelkair/wiiparty-recomp) by arelkair (GPL-3.0-or-later); they are in
`recomp-common/launcher/`, with the notices in recomp-common's `THIRD_PARTY_NOTICES.md`. Its build downloads
SDL 3 (zlib license), Dear ImGui (MIT) and the Inter font (SIL OFL 1.1, compiled into the program): a
launcher given out as a binary must carry those three licenses.
