# Sound: the sound chip and its CPU

The Saturn's sound chip (SCSP) plays up to 32 voices from wave memory, each with a pitch, a loop, an
envelope and LFOs, optionally modulated by other voices (FM), and runs a small effects processor (DSP)
on mixes of them. The game's sound driver, a 68000 program in sound RAM, sets it up.

Both are satrecomp's own, with no dependence on any hardware model:

- `satrecomp/sound/chip.{h,cpp}`: the whole chip. Its registers decoded from what the CPUs write (voices,
  DSP program and memories, common registers), three timers, the interrupts to the sound CPU and to the
  main side, its DMA, CD audio input, and the sample generation below.
- `satrecomp/sound/m68k.{h,cpp}`: the 68000, an interpreter of the whole instruction set. The sound driver
  runs in it (256 cycles of it before each sample).

What the chip does for each output sample (32 steps; a step moves seven voices one stage each through
the chip's pipeline and runs four DSP instructions):

- key on and off, the pitch with vibrato, the position in the wave and the four kinds of loop;
- reading wave memory (8 or 16 bit), modulation from the sound stack, interpolation;
- the envelope generator (attack, two decays, release, key rate scaling), tremolo, the levels;
- the DSP: its 128 instructions a sample, with the ring buffer in wave memory and its floating point
  format;
- the mix: direct and effect sends, pans, master volume, CD audio input.

## Checking (on the reference build, `make ref`)

`satrecomp/sound/generator.*` connects the chip to Ymir there: every register write, reset and sector of
CD audio is passed on to it through hooks added by the reference patch (kept outside this repository; build.mk, `setup`).

- `SATRECOMP_SOUND=check`: Ymir's chip and ours both do every step, each on its own state, and after each
  the two are compared field by field: the seven voices in the pipeline, the DSP, the sound stack, the
  output sums, and before each step the registers, timers and interrupt state. Fighting Vipers: no step
  differs in 635 million (9,000 frames each of three movies).
- `SATRECOMP_M68K=check`: every instruction of the sound driver is run by our 68000 as well as Ymir's,
  from the same state; registers, flags, cycle count, and what was written to sound RAM and to the chip
  are compared. No instruction differs in 401 million. (The cycle counts are Ymir's rules, so that the
  two stay in step; they are close to the real chip's but not everywhere the same.)
- `SATRECOMP_SOUND=reference`: Ymir's own generation.
- `SATRECOMP_AUDIO_DUMP=path` (either build): every sample that goes out, 16-bit stereo, 44,100 Hz.
