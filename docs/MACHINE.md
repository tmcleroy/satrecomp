# The machine

What the game's translated code runs on (`satrecomp/machine/`). It is satrecomp's own: the game binary has
no emulator in it. It is also not an emulator in ambition: it does not model the Saturn's timing, only what
the code of the games ported so far needs, and where a choice had to be made it is the simple one.

| File | What |
| --- | --- |
| `sh2.h`, `sh2.cpp` | One SH-2: registers as the translated code uses them, memory access, the on-chip interrupt controller, free-running timer, divider and DMA controller, exceptions, and an interpreter for code with no translation |
| `sh2_ops.h` | The instruction set as a table of bit patterns (the generator reads it too) |
| `machine.h`, `machine.cpp` | Memory and the bus; the system control unit (interrupts to the master CPU, its two timers, DMA); the system manager (slave and sound CPU on/off, clock change, time of day, control pads); the sound chip's clock; the run loop |
| `vdp.cpp` | The video chips' registers and timing: the parts of a line and a frame, blanking interrupts, framebuffer erase and swap, the VDP1's command table (walked at once; the renderer draws) |
| `cd.cpp` | The CD drive's command interface, as far as the games still use it: status, table of contents, buffer set-up, seek, and data sectors read into a buffer of 200 and taken through the data port (one partition, no filters) |

The picture is made by `video/` and the samples by `sound/` (chip and 68000); the machine owns their memory
and tells them what the game writes and when a line or a sample is due.

## Time

Time is counted in SH-2 clock cycles, nominal ones: every instruction counts a fixed number by its kind (the
translated code and the interpreter count the same). `Machine::Run` lets the CPUs run up to the next thing
due (the end of a part of a video line, a sound sample, a command finishing), then does that.

The two CPUs take turns. Each runs the whole stretch to the next event, unless exactly one of them is known
to be only waiting; then they alternate in slices of 32 cycles so the one waiting sees what it waits for
soon. When one CPU signals the other (a write to the MINIT/SINIT area pulses the other's timer input), its
turn ends there and the two alternate in short slices for a while: games start the slave on a job and go
on, expecting it to react meanwhile. `game.cfg` can also name sync points, addresses where a CPU gives the
other time, and `fine_slices`, code during which the two always alternate in short slices.

There is no cache, with one exception for what a cache does to code a CPU keeps going round: each SH-2's
cache is its own, so a CPU spinning in a short loop goes on running it while the other CPU overwrites that
memory. The dispatcher notes the loop a CPU comes back to, and runs it from what memory held when it was
noted while the CPU stays there (`CachedLoop` in runtime/recomp.cpp; the interpreter takes its instructions
from that copy too). Writes to the cache's purge area do nothing.

Things that take time on the hardware and none here: DMA, the VDP1's drawing (the game is told it is done
a little later), bus waits. A frame is 263 lines of 1708 or 1820 cycles.

## What it was measured against

`SATRECOMP_IO_STATS=1` on the reference build lists every hardware address the game's CPUs touch and how
often; the machine implements those. The reference build's `--stats` and `SATRECOMP_CD_TRACE=1` list the
commands given to the system manager and the CD drive; on this machine the game gives the same ones, in
kind and number.

It cannot be compared with the reference frame by frame, because it does not keep the same time: the first
750 frames of Fighting Vipers come out pixel for pixel the same, after that the two drift (a fight starts a
frame earlier, the input of a recorded movie lands differently). What is compared instead:

- the instruction decode table, bit for bit with the reference's (`--dump-decode`);
- the interpreter against the translated code, state after every frame (`make check`): this checks every
  instruction the game uses, since the translated code is itself checked against the reference's
  interpreter (`make verify`);
- the renderer, the sound chip and the 68000, each against the reference's on the reference build
  (docs/VIDEO.md, docs/SOUND.md);
- that two runs are the same (`make check`).

## Debugging aids

`--stats` (commands given to the SMPC and the CD drive, blocks interpreted), `--dump-ram PREFIX` (work RAM
and registers at exit), `SATRECOMP_NO_BLOCKS=1` (interpreter only), `SATRECOMP_NO_FUNCTIONS=1`,
`SATRECOMP_NO_CALLS=1`, `SATRECOMP_TRACE=path` with `SATRECOMP_TRACE_FROM/TO` (every block entered, with
registers), `SATRECOMP_WATCH_WRITE=address` (long word writes by interpreted code), `SATRECOMP_PEEK`.
