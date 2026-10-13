# Changelog

## 0.1.0

The first public version: the toolkit as its first game, Fighting Vipers Recomp, uses it.

- The translator of SH-2 code to C++ (`tools/recomp.py`), with the block database and its recorder.
- The machine: two SH-2s with an interpreter for code that has no translation, the bus, the system
  control unit and system manager, video timing, the BIOS's services, saves in the console's format.
- The renderer (the VDP2's screens and the VDP1's commands; in software, and on SDL's GPU API at the
  window's resolution, with widescreen), the sound chip and its 68000.
- Routines of a game replaced by C++, readable rewrites and their checker; features and settings;
  save states; the in-game menu and the launcher the recomp ports share (recomp-common).
- A game's repository takes a copy with `tools/sync-toolkit.sh`, checked by `make check-toolkit`;
  `make SR=../satrecomp` builds a game against a clone.
- `make check` here, with no game: the audit, the stand-in game in `example/`, the sync script.
