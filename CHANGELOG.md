# Changelog

## Unreleased

- Fixed with recomp-common 0.5's menu: until the in-game menu was first opened, its speed was read from
  before the start of the port's list of speeds (the shared menu keeps -1 for "not chosen yet"), so
  run-ahead stayed off and the window's pacing and speed note were wrong. It is full speed now.
- recomp-common 0.5.0 and after (13de85a, and its PR #46: the Your files card says saves): portable mode (`portable.txt` beside the launcher keeps the
  player's folders in `user/` there; the game follows, as it is given its folders by the launcher), a
  launcher that fits a small window, Play at the top of the Game page, dark unless the system says light,
  a sidebar scrolled by a finger, a feature's choices that can be shortened for the in-game menu
  (`kFeatureMenuChoices`), and the in-game menu's Speed and Mute on a page of their own where a port says
  (`MenuPage::rows`). The online, collection and update parts are not used by the toolkit.
- Run-ahead (the port's `run_ahead` feature: off, 1 or 2 frames): the frames after each are run and
  shown, then the game is put back, so a press is seen a frame or two sooner and the game runs exactly
  as without it (state hashes the same frame for frame). `RunningAhead()` for a game's sources;
  `Game::noRunAhead`.
- Features with choices (`Feature::choices`, `FeatureValue`, `SetFeatureValue`; `--features NAME=N`).
- Training aids work only in a training session: a game marks them (`kFeatureTraining`,
  `TrainingAids`) and says when one is on (`Game::inTraining`); elsewhere they read as off, while the
  menu shows and `settings.ini` keeps the player's choice (`FeatureSetting`).
- Widescreen can be switched in the in-game menu (Display page; GPU renderer): the launcher's switch in
  `launcher.ini` follows it (`kFeatureFirstPage`), and the window takes the picture's shape.
- Fixed: the launcher took `.chd`, `.ccd` and `.mds` images, which the port cannot read (it reads a
  `.cue` with its `.bin` files, or an `.iso`); it now says so and asks for one of those.

## 0.2.0 (2026-10-13)

- recomp-common 0.4.0: the launcher works with a gamepad, or the arrow keys, alone, and has a
  "Muted" switch (the game is started with `--mute`).
- The in-game menu's buttons are the same whatever the game's are set to: the arrows, the d-pad and
  the left stick; Space, Enter and a gamepad's A choose; Backspace and B go back (leave a page,
  close the menu from the first). A "Sound" row switches the sound off and on; `--mute` starts with
  it off (the sound device is opened all the same, so that the row can switch it on).

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
