# Readable code: rewriting the game's routines by hand

The translated code (`work/<id>/gen`) is the game's machine code as C++: it runs, and nobody can read it.
Where a feature needs the game's rules to be understood and changed, the routine in question is rewritten
by hand as ordinary C++ with names, in the game's sources (`games/<id>/src`), and takes the place of the
game's own. This is done routine by routine, where it is wanted; everything else keeps running as
translated code. A rewrite is checked against the code it was written from, automatically, so it is known
to do the same and not merely believed to.

Fighting Vipers' are in `games/fighting_vipers/src/fight.cpp` (damage, the clock, the fighters' frame),
`round.cpp` (a round from its set-up to who won it), `fightinput.cpp` (the pads' buttons to the
fighters' input and its history) and `throws.cpp` (a fighter's request for the frame, and a throw answered
by the other's being thrown), with the names of the game's memory in `fv.h`.

## What a rewrite is

A lift (`runtime/lift.h`: address, size and hash of the code, a C++ function) marked `rewrite`. It must:

- leave the same memory as the game's routine, given the same memory and registers before;
- leave the same registers, except those named in `Lift::free`;
- write the same to hardware (video, sound, on-chip registers), in the same order.

It works on the game's registers and memory through `Guest` (`R(n)`, `Read16`, `Write16`, ...), and calls
routines of the game's that are not rewritten with `Guest::Call(address)`, which runs that routine as the
rest of the game runs (translated code, other rewrites) until it comes back. So a routine can be rewritten
before the ones below it are. The address may be inside a routine: a rewrite of a routine's first part
calls the rest of it (Fighting Vipers' `InputStep` does, for the reading of commands), with the registers
as the game's code has them there; `liveout.py DUMP ">ADDRESS"` lists the ones the rest reads. A rewrite
may decline a call (return false) before doing anything, and the
game's code runs: Fighting Vipers' `FightStep` does, for the states whose routines wait for the disc.

The registers matter more than one would like. This game's rules are hand-written assembly: the fighter
being worked on is in r12 everywhere, routines have several entries, and a caller may go on to use what a
routine happened to leave in r0 or r3. A rewrite therefore ends by putting such values where the game's
code leaves them (lines marked `// left` in `fight.cpp`), unless no caller reads them:

```sh
satrecomp/tools/liveout.py DUMP 060117D4            # DUMP: high work RAM with the code in it (--dump-ram)
satrecomp/tools/liveout.py DUMP 06024CFC@06004594   # a routine called through a pointer: where it returns to
```

finds the callers in the block database and follows their code after the call, into the routines they
call next: which of r0-r14 and T are read before they are written. Those not read by any caller go into
`Lift::free`. (It errs on the side of "read": the arguments of a call it cannot follow count as read, and
r8-r14 still open when the caller returns count as unknown. This game's routines do not always put r8-r14
back.) As more of the callers are rewritten these lines go away.

What calls what, and what is a leaf to start from:

```sh
SATRECOMP_NO_BLOCKS=1 SATRECOMP_CALL_GRAPH=calls.txt build/<id>/<id> --disc ... --headless --frames 9000 --movie ...
satrecomp/tools/callgraph.py calls.txt @06004590 2    # the trees below what the task list calls
```

## The check

```sh
SATRECOMP_LIFT_CHECK=1 build/<id>/<id> --disc ... --headless --frames 15000 --movie movies/own_auto_7.txt
```

For every call of a rewrite (`=n`: every nth), the rewrite runs; what it left is noted and the whole
machine is put back as a save state would (so hardware it wrote to is as it was); the game's code runs,
by the interpreter, until it returns; and the two results are compared: all of both work RAMs but the
stack below the routine's, every register but those in `Lift::free`, and the writes made to hardware. The
game goes on from what its own code left. Differences are printed as they happen (the first few, with the
arguments of the call; `SATRECOMP_LIFT_CHECK_WRITES=1` lists both sides' hardware writes), and at the end,
for each rewrite:

```
rewrite RoundClock: 11785 calls, 11785 compared with the game's code: 0 differ; 0 could not be compared; ...
    not reached in the game's routine (15 of 41 words): 06024D16-06024D32
```

The second line is what matters as much as the first: the parts of the game's routine that no compared
call ran, for which the rewrite has therefore not been checked. (Data among the code, such as its
constants, is listed too.) A rewrite that was never called says nothing at all. To get there:

- `SATRECOMP_LIFT_POKE='name:address=byte,address=byte;name:...'` sets bytes of work RAM before each call
  of a rewrite, for both sides: another mode, the clock run out, both fighters down. What the game does
  afterwards may be nonsense; each compared call still stands.
- `SATRECOMP_LIFT_SKIP=name,name` runs the game's code for those rewrites. While a rewrite is being
  checked, what it calls runs as the game's code on both sides, so a rewrite called only from another is
  not met itself; skipping the outer one lets it be.
- `SATRECOMP_LIFT_COVERAGE=file` adds what the run reached to the file; `tools/liftcoverage.py FILE...`
  puts several runs together and lists what none reached.

A game keeps its runs in a script: `make check-rewrites` (`games/fighting_vipers/check_rewrites.sh`: the
movie as it is and some hundred variants, about twenty minutes; 2,005,937 calls compared, none differ, and every instruction of the
game's routines reached but one that cannot be).

`SATRECOMP_NO_REWRITES=1` runs the game's own code in place of every rewrite. The reference build
(`make ref`) always does.

## Steps

1. Find the routine: who writes the value the feature is about (`SATRECOMP_WRITERS=lo-hi` with
   `SATRECOMP_NO_BLOCKS=1`, `SATRECOMP_WATCH_WRITE`), who calls that (`SATRECOMP_LOG_PC=address` prints the
   registers, PR among them).
2. Read it (`satrecomp/tools/sh2dis.py`); name what it reads and writes in the game's header.
3. `liveout.py` for the registers its callers read; `lifthash.py` for the hash.
4. Write it; register it with `rewrite = true`.
5. `SATRECOMP_LIFT_CHECK=1` over movies that reach it, until nothing differs and, with pokes, nothing of
   the game's routine is left unreached; the runs go into the game's script. Then `make check`.

## Features in rewrites

A feature that changes the game's rules is then a few lines in the rule (Fighting Vipers' cheats in
`TakeDamage` and `RoundClock`), under `FeatureOn("name")`. With such a feature on the routine does not do
what the game's does, so the check is run with them off.

## Limits

- `Guest::Call` runs its routine to the end at once: no interrupt is taken and no time passes for the rest
  of the machine, so a routine that waits (for the disc, the other CPU, the next frame) cannot be called.
- A routine that reads a timer cannot be compared (the two sides take different times): the game's task
  list caller (0x06004560), which times each task, is one.
- The rewrite's time is a number given to `Return(cycles)`, not the original's count, so a run with
  rewrites is not cycle for cycle a run without. (`make check` compares like with like.)
- The second CPU's code can be rewritten the same way; nothing has been yet.
