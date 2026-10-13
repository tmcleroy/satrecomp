// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Which machine the game's code runs on: satrecomp's own (machine/), or the Ymir hardware model, which is
// kept for development as the reference to compare with. Everything else in the runtime and all generated
// code names the CPU and the machine through this header.
#pragma once

#ifdef SATRECOMP_OWN_MACHINE

#include "../machine/sh2.h"

namespace satrecomp {
using SH2 = machine::SH2;
using Machine = machine::Machine;
} // namespace satrecomp

// Counts everything that can change what a waiting CPU would see (see the dispatcher's waiting detection)
#define SATRECOMP_ACTIVITY ::satrecomp::machine::g_activity
// A write went to work RAM at `p`: translated code may lie there
#define SATRECOMP_NOTE_WRITE(p) ::satrecomp::machine::NoteWrite(p)
#define SATRECOMP_USING_TYPES using namespace ::satrecomp::machine::types;
#define SATRECOMP_CODE_BASE ::satrecomp::machine::g_codeBase
#define SATRECOMP_CODE_MAP ::satrecomp::machine::g_codeMap
#define SATRECOMP_CODE_SIZE ::satrecomp::machine::g_codeSize
#define SATRECOMP_CODE_EPOCH ::satrecomp::machine::g_codeEpoch
#ifndef FORCE_INLINE
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

#else

#include <ymir/hw/sh2/sh2.hpp>
#include <ymir/ymir.hpp>

namespace satrecomp {
using SH2 = ymir::sh2::SH2;
using Machine = ymir::Saturn;
} // namespace satrecomp

#define SATRECOMP_ACTIVITY ::ymir::sh2::g_satrecompActivity
#define SATRECOMP_NOTE_WRITE(p) ::ymir::sys::SatrecompNoteWrite(p)
#define SATRECOMP_USING_TYPES using namespace ymir::sh2;
#define SATRECOMP_CODE_BASE ::ymir::sys::g_satrecompCodeBase
#define SATRECOMP_CODE_MAP ::ymir::sys::g_satrecompCodeMap
#define SATRECOMP_CODE_SIZE ::ymir::sys::g_satrecompCodeSize
#define SATRECOMP_CODE_EPOCH ::ymir::sys::g_satrecompCodeEpoch

#endif
