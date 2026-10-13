**What this changes and why**

**Checklist**
- [ ] Nothing from a game (files, generated code, block lists, dumps, save states, input movies, screenshots), no BIOS, nothing from an SDK and no emulator's code is added. `make audit` passes.
- [ ] Nothing here is about one game: what a game needs goes in that game's `game.cfg` or sources.
- [ ] `make check` passes here. If the translator, the machine, the renderer or the sound changed: `make SR=../satrecomp check` and `check-rewrites` pass in a game's repository (say which game).
- [ ] No added input lag or slowdown.
- [ ] Docs and `CHANGELOG.md` updated if behavior changed.
