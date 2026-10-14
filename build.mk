# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# satrecomp build rules. Included by the top-level Makefile with GAME set.
#
#   make            translate the block database to C++ and build the game (build/$(GAME)/$(GAME))
#   make run        run it (windowed)
#   make record     run it and add every block of code it reaches to the block database
#   make check      checks of the game on its own machine (interpreter against translated code, two runs alike)
#   make check-rewrites  the game's rewritten routines against the code they replace (docs/READABLE.md)
#   make check-toolkit the toolkit (satrecomp/) is what its tools/sync-toolkit.sh recorded
#   make check-common  the copy of recomp-common (recomp-common/) is what tools/sync-common.sh recorded
#   make launcher   build the launcher (build/$(GAME)/$(GAME)-launcher)
#   make app        macOS app bundle (build/$(GAME)/<Name>.app)
#
# For development, the same game code on a reference machine (the Ymir Saturn emulator) to compare with:
#   make setup      fetch Ymir (into third_party/, which git ignores) and apply the reference patch to it
#                   (the patch is not in this repository: SATRECOMP_REFERENCE names the folder it is in)
#   make ref        build build/$(GAME)-ref/$(GAME)-ref
#   make verify     translated code against Ymir's interpreter there (FRAMES=n, MOVIE=path)
#
# Per-game data lives in games/$(GAME)/ (game.cfg, movies); generated files in work/$(GAME)/
# (block database, generated code, saves); builds in build/$(GAME)/. The disc image is never part of
# the repository: DISC points at your own file.

# (SR: where the toolkit is. A game's repository has a copy in satrecomp/; to work on the toolkit itself,
# build a game against a clone of it: make SR=../satrecomp)
SR ?= satrecomp
CFG := python3 $(SR)/tools/gamecfg.py
WORK := work/$(GAME)
# (a build against a clone of the toolkit is kept apart from the usual one)
B := build/$(GAME)$(if $(filter satrecomp,$(SR)),,-dev)
GEN := $(WORK)/gen
GEN_REF := $(WORK)/gen-ref
B_REF := $(B)-ref
REF := $(B_REF)/$(GAME)-ref
BLOCKS := $(WORK)/blocks.txt
YMIR := $(SR)/third_party/Ymir
PORT := $(B)/$(GAME)
export GAME B B_REF SR

DISC ?= $(firstword $(wildcard disc/*.cue disc/*.chd disc/*.iso))
# No BIOS image is needed: the game is started by satrecomp's own code (runtime/hle_bios.h). The reference
# build can start through an image instead (BIOS=path; needed for 'make verify KIND=exact').
BIOS ?=
FRAMES ?= 3000
RUN_ARGS = --disc "$(DISC)" --backup $(WORK)/backup.bin
# Blocks to translate: the game's (work RAM above the BIOS's area); with a BIOS image, everything recorded
GEN_REGIONS = --region 00200000-00300000 --region 06002000-08000000

.PHONY: all setup configure port gen run record check check-rewrites check-common check-toolkit ref gen-ref record-ref verify launcher app clean
all: port

# Neither Ymir nor the patch that makes it the reference machine is part of this repository (a patch quotes the
# source it changes). Ymir is fetched here, at the commit the patch is for, into a folder git ignores; the patch
# is in the folder SATRECOMP_REFERENCE names (it is not published).
SATRECOMP_REFERENCE ?=
YMIR_PATCH := $(abspath $(SATRECOMP_REFERENCE))/ymir-satrecomp.patch
YMIR_URL := https://github.com/StrikerX3/Ymir.git
YMIR_COMMIT := 58e7643cf4f12a4b21207b7fa86f9deb535acb2b
setup:
	@[ -n "$(SATRECOMP_REFERENCE)" ] || { echo "make setup: the reference build needs the reference patch: set SATRECOMP_REFERENCE to the folder it is in"; exit 1; }
	@[ -f "$(YMIR_PATCH)" ] || { echo "make setup: no $(YMIR_PATCH): the reference build needs the reference patch (set SATRECOMP_REFERENCE to its folder)"; exit 1; }
	@if [ ! -d $(YMIR)/.git ]; then mkdir -p $(YMIR) && git -C $(YMIR) init -q && git -C $(YMIR) remote add origin $(YMIR_URL); fi
	@if [ "$$(git -C $(YMIR) rev-parse -q --verify HEAD 2>/dev/null)" != "$(YMIR_COMMIT)" ]; then \
		git -C $(YMIR) fetch -q --depth 1 origin $(YMIR_COMMIT) && git -C $(YMIR) checkout -q FETCH_HEAD; fi
	cd $(YMIR) && git submodule update --init --depth 1 vendor/mio vendor/concurrentqueue/concurrentqueue \
		vendor/xxHash/xxHash vendor/libchdr/libchdr vendor/fmt vendor/stb/stb
	cd $(YMIR) && (git apply --check "$(YMIR_PATCH)" 2>/dev/null \
		&& git apply "$(YMIR_PATCH)" && echo "Ymir: patch applied" \
		|| echo "Ymir: patch already applied (or it needs updating for this Ymir version)")

# Generated code: C++ functions for the blocks in the database (tools/recomp.py)
gen:
	@mkdir -p $(GEN)
	@$(CFG) --header $(GEN)/game_info.h $(GAME)
	@if [ -f $(BLOCKS) ]; then \
		python3 $(SR)/tools/recomp.py --blocks $(BLOCKS) --ops $(SR)/machine/sh2_ops.h --out $(GEN) \
			--stops "$(shell $(CFG) sync_points $(GAME))" $(GEN_REGIONS); \
	fi

configure:
	@cmake -S $(SR) -B $(B) -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME=$(GAME) \
	  -DGAME_DIR=$(CURDIR)/games/$(GAME) -DWORK_DIR=$(CURDIR)/work/$(GAME) -DSATRECOMP_REFERENCE=OFF > /dev/null

port: gen configure
	@ninja -C $(B) $(GAME)

run: port
	$(PORT) $(RUN_ARGS) $(ARGS)

record: port
	$(PORT) $(RUN_ARGS) --record-blocks $(BLOCKS) --stats $(ARGS)

check: port check-common check-toolkit
	@$(SR)/tools/check.sh $(FRAMES) "$(MOVIE)"

# The code shared with the other recomp ports (recomp-common/: the in-game menu) is a copy of recomp-common's, made
# by tools/sync-common.sh and never changed here; this fails if it was
check-common:
	@$(SR)/tools/sync-common.sh --check

# The toolkit itself (satrecomp/) is a copy of the satrecomp repository's, made by its tools/sync-toolkit.sh
# and never changed here; this fails if it was. (Not asked of a build against a clone of the toolkit.)
check-toolkit:
	@if [ "$(SR)" = satrecomp ]; then $(SR)/tools/sync-toolkit.sh --check; fi

# The game's hand-rewritten routines against the code they replace (docs/READABLE.md); the game says how
check-rewrites: port check-common check-toolkit
	@games/$(GAME)/check_rewrites.sh

# --- the reference build ---

# Its generated code has, besides ours, blocks made of Ymir's interpreter handlers (cycle exact), and the
# sound driver as blocks of Ymir's 68000 handlers. The decode table for those comes from the build itself,
# so the first build of a fresh tree is an interpreter-only one, and is done twice.
gen-ref:
	@mkdir -p $(GEN_REF)
	@$(CFG) --header $(GEN_REF)/game_info.h $(GAME)
	@if [ -x $(REF) ] && [ -f $(BLOCKS) ]; then \
		$(REF) --dump-decode $(WORK)/decode.bin && \
		python3 $(SR)/tools/recomp.py --blocks $(BLOCKS) --decode $(WORK)/decode.bin --ymir $(YMIR) --out $(GEN_REF) \
			--stops "$(shell $(CFG) sync_points $(GAME))" $(if $(BIOS),,$(GEN_REGIONS)) && \
		if [ -f $(BLOCKS).m68k ]; then \
			python3 $(SR)/tools/recomp68k.py --blocks $(BLOCKS).m68k --decode $(WORK)/decode.bin --ymir $(YMIR) --out $(GEN_REF); \
		fi; \
	fi

ref: gen-ref
	@cmake -S $(SR) -B $(B_REF) -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME=$(GAME) \
	  -DGAME_DIR=$(CURDIR)/games/$(GAME) -DWORK_DIR=$(CURDIR)/work/$(GAME) -DSATRECOMP_REFERENCE=ON > /dev/null
	@ninja -C $(B_REF) $(GAME)-ref
	@if [ ! -f $(WORK)/decode.bin ]; then $(MAKE) --no-print-directory ref; fi

# (also records the sound CPU's blocks, which only the reference build has as blocks)
record-ref: ref
	$(REF) $(if $(BIOS),--bios "$(BIOS)") $(RUN_ARGS) --record-blocks $(BLOCKS) --stats $(ARGS)

verify: ref
	BIOS="$(BIOS)" $(SR)/tools/verify.sh $(FRAMES) "$(MOVIE)" $(KIND)

# The launcher (SDL3 + Dear ImGui; satrecomp/launcher): the player's disc image and BIOS, features,
# controllers, Play. Built beside the port, which it runs.
launcher: port
	@cmake -S $(SR)/launcher -B $(B)/launcher -G Ninja -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(CURDIR)/$(B) \
	  -DGAME_NAME="$(shell $(CFG) name $(GAME))" -DGAME_EXE=$(GAME) -DDISC_NAME="$(shell $(CFG) disc_name $(GAME))" \
	  -DDISC_PRODUCT="$(shell $(CFG) disc_product $(GAME))" -DDISC_VERSION="$(shell $(CFG) disc_version $(GAME))" \
	  -DAPP_ID=$(shell $(CFG) app_id $(GAME)) -DHOME_ENV=$(shell echo $(GAME) | tr a-z A-Z)_HOME \
	  -DDISCLAIMER="$(shell $(CFG) disclaimer $(GAME))" > /dev/null
	@cmake --build $(B)/launcher
	@echo "built $(B)/$(GAME)-launcher"

# macOS app bundle: the launcher, the port and SDL3 (no game data)
app: launcher
	@$(SR)/tools/make_app.sh $(GAME)

clean:
	rm -rf $(B) $(B_REF)
