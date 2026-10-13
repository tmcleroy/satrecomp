# Verifying recompiled code

On the game's own machine: `make check FRAMES=n MOVIE=path` (`satrecomp/tools/check.sh`). It runs the game
with everything in the SH-2 interpreter (`SATRECOMP_NO_BLOCKS=1`) and with the translated code block by
block (`SATRECOMP_NO_FUNCTIONS=1`), which must give the same state after every frame; and the game as it
is played twice, which must too. Each run starts from fresh save memory (a save carries play time over
to the next run).

The rest of this page is about the reference build (`make ref`: `build/<id>-ref/<id>-ref`, the same game
code on the Ymir emulator), where the translated code is compared with Ymir's interpreter.

That build has five CPU modes (`--cpu`), in two families:

| Mode | What runs the SH-2 code | Timing |
| --- | --- | --- |
| `native` | satrecomp's own translation (default); the interpreter for blocks the build lacks | free running |
| `free` | The interpreter, block by block: the reference for `native` | free running |
| `recomp` | Blocks made of the interpreter's handlers with constant opcodes | cycle exact |
| `interp` | The interpreter, through satrecomp's block dispatcher | cycle exact |
| `pure` | Ymir's interpreter, untouched: the reference for `recomp` and `interp` | cycle exact |

Free running: a block runs to its end whatever the time slice, and an instruction costs a fixed
number of cycles by its kind (`NominalCycles` in runtime/recomp.cpp). `native` and `free` leave
blocks at the same points and count the same cycles, so they must produce the same machine.
Cycle exact: `recomp`, `interp` and `pure` must produce the same machine, cycle for cycle.

`satrecomp/tools/verify.sh` (`make verify FRAMES=n MOVIE=path [KIND=exact]`) runs a pair
(`free`/`native`, or `pure`/`recomp`) with the same input and compares a hash of work RAM, CPU
registers and the picture after every frame. An input movie only replays in the timing it was
recorded in.

## When they differ

1. (Exact family) `--cpu interp` against `--cpu pure` tells whether the dispatcher or the generated
   code is wrong.
2. `SATRECOMP_TRACE=path` writes a running hash of the CPU state after every dispatch (one line per
   million). Run it for both modes and diff: the first differing line gives the window.
3. `SATRECOMP_TRACE_FROM=n SATRECOMP_TRACE_TO=m` adds the full CPU state for dispatches n..m. Diff
   again: the first differing line names the block (`blk=`). Disassemble it with
   `satrecomp/tools/sh2dis.py --hex PC BYTES` (bytes from the block database) and read its function
   in `work/<id>/gen/`.
4. `SATRECOMP_WATCH_PC=hex` prints the last 64 blocks before a given address is reached.
5. `SATRECOMP_WATCH_WRITE=hex` with `--cpu free` reports every write to an address with the PC that made
   it (how the routine that owns a variable is found). `SATRECOMP_LOG_PC=hex` prints the registers each
   time a block is entered.
6. `--dump-ram PREFIX` writes work RAM and prints the CPU registers at exit;
   `satrecomp/tools/sh2dis.py FILE BASE START [COUNT]` disassembles from a dump.

## What the generator relies on

After an instruction that only works on registers, generated code checks just the time slice. After
any other instruction it also checks that execution continues with the next instruction and that no
interrupt is due. Which handlers are "register only", and which hold off interrupts for one
instruction (`ldc`, `lds`, `stc`, `sts`), is read from Ymir's handler source by `recomp.py`
(`parse_handler_traits`). If Ymir changes how a handler is written, that is the place to look, and
`make verify` is what catches it.

## What native code relies on

- It leaves a block early only where `InterpretBlockFree` (runtime/recomp.cpp) does: a taken
  conditional branch, a hardware access that has to wait for the bus, an interrupt that became
  pending after a hardware access or a write to SR.
- A function of several blocks (`FunctionDesc`) goes from block to block exactly as
  `InterpretFunctionFree` does: only along branches with fixed targets and off the end of a block,
  never with an interrupt pending, and on a backward branch only while the time slice lasts.
- Loops back to the block's own start stay inside the native function until the time slice is over.
  A loop that only waits (`only_waits` in sh2emit.py) spends the rest of the slice at once; a
  countdown (`dt Rn; bf`) is counted at once. Both give the same registers and cycle count as running
  every pass.

## Sound CPU

The 68000 runs recompiled in every mode except `pure` (`SATRECOMP_NO_M68K=1` puts it back on the
interpreter). Its blocks are cycle exact, so `KIND=exact` (against `pure`) is what checks them; the
state hash includes a running hash of every sound sample. Its block database is the SH-2 one's name
plus `.m68k`.

## Waiting detection

Free running, the dispatcher marks a CPU as waiting when its registers at a block entry equal those
at an earlier entry and `ymir::sh2::g_satrecompActivity` has not moved in between (it counts every
memory write and every access to anything but work RAM and ROM, by either CPU, plus interrupts,
native BIOS routines and lifted routines). While both CPUs are marked, `Saturn::Run` gives each a
slice up to the next hardware event instead of 32 cycles. `native` and `free` make the same marks at
the same points, so they still match. The exact modes never mark.
