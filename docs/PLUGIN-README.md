# sovietkit

Four mods for **Workers & Resources: Soviet Republic**, in one tesmio plugin.
Version 1.5. Built against **SOVIET64.exe v1.1.1.9** (PE TimeDateStamp
`0x6A3EB6AD`); on any other build every part says so in the log, hooks nothing
and patches nothing. (Walking is the exception: it also knows v1.2.0.8 and has
its own table per build — see [Walking](#walking) below.)

| part | what it does | key |
|---|---|---|
| **anarchy** | every "you can't build that there" goes quiet — slope, angle, terrain, water, collisions, crossings, connection rules | `Alt+F` |
| **skins** | recolour a building with the game's own spray-can and every one of that type you place afterwards is built wearing it | none needed |
| **better trackbuilders** | rail construction trains ignore signals and each other, and one-way signals can be made two-way for routing | on by default; `Ctrl+Alt+D` for the one-way switch |
| **walking** | the longest path a citizen will walk (and drive) is a setting instead of 480 m / 2500 m | none; `[walking]` in the ini |

Any part can be switched off at the top of `sovietkit.ini` without touching the
other two:

```ini
[sovietkit]
anarchy   = 1
skins     = 1
trackcrew = 1
walking   = 1
```

Every key is rebindable in `[keys]`, and so is the modifier each group uses.

## Keys

| key | what |
|---|---|
| **`Alt+F`** | anarchy on / off — **both layers**. The word ANARCHY appears while it is on. |
| `Ctrl+Alt+T` | anarchy layer 1 — **the panic key**, takes anarchy down with it |
| `Ctrl+Alt+G` | better trackbuilders on / off |
| `Ctrl+Alt+D` | one-way semaphores on / off **for routing** — read the warning below |
| `Ctrl+Alt+R` | the full report: all four parts, then the sites and counters |
| `Ctrl+Alt+K` | zero every counter and census |
| `Ctrl+Alt+` `C V A Z ] [ P S H` | the skins keys — copy, paste, apply-all, undo, next, previous, retain-on-pick, self test, hex dump |

Modifiers are matched **exactly**: `Alt+F` means Alt down with Ctrl and Shift
up, so none of the fourteen `Ctrl+Alt` bindings can reach each other or the
anarchy toggle. If you used the separate plugins, this is a change for skins and
trackcrew — they used to match "at least", so `Ctrl+Alt+Shift+C` fired the skins
copy key. Now it does nothing.

## Why one plugin instead of three

They *were* three, and three was worse in ways that only showed up once they ran
together:

* Each one patched `C3D_TERRAIN::Render` for a frame callback, so three detours
  chained through each other for one frame tick. Now there is one.
* Each one asked Windows for the foreground window and the state of Ctrl, Alt
  and Shift **every frame, separately** — about thirty `GetAsyncKeyState` calls
  a frame between them, and `GetAsyncKeyState` is a kernel transition. Now the
  three keys are read **once** and all seven modifier combinations are derived
  from that, so the per-frame input cost is three calls no matter how many
  hotkeys exist.
* Their hotkeys collided. All three wanted `Ctrl+Alt+K` for "reset counters" and
  two wanted `Ctrl+Alt+R`; whichever loaded first won and the others silently
  did nothing. There is now one report key and one reset key covering all three.
* Three logs, three inis, three build-stamp checks printing the same line.

What is shared is the plumbing — the log, the counters, the site verification,
the frame tick and the keyboard. What is not shared is everything that matters:
each part keeps its own code, its own offsets and its own `[section]`, and lives
in its own namespace.

### Faults are still isolated per part

The obvious merge puts one `__try` around the three parts, and it is wrong: a
fault in any one of them unwinds the whole block, and since every part's hotkeys
live inside its own frame function, one bad part would take down the anarchy
toggle, the skins keys and the trackcrew toggle with it.

So there are three sequential guards, not one. Eight faults retires **that part
alone** — its rules go off, anything it wrote into the game's code goes back,
and the other two carry on. The shared report and reset keys are read *before*
the three parts, so they survive a part that is faulting every frame.

## anarchy

**Layer 1**: three hooks on three of the game's own predicates (building
collision, signal proximity, construction overlap), each of which lies only when
the question came from inside a build tool. Worked out from the return address;
the rest of the game gets the truth.

Until 1.4 layer 1 was *always on* and only layer 2 answered to the key, and that
was wrong in a way that read as a bug: `0x4FCAF0` is not the rail tool, so "layer
1 only" meant collision, signal proximity and overlap were off for **everything
you drag** — track, road, pipe, conveyor, fence, cableway — for the whole
session, with the indicator dark and the log saying ANARCHY OFF. Press the key
to turn it off, drag a pipe through a house, watch it go in. The mod said off;
the game said on; the game was right. The key is now the whole switch.
`layer1_follows_key = 0` puts the old behaviour back, and says so in the log
every time you press the key.

(Rail, road bridges, pipes, water, sewage, heating, conveyors, fences,
cableways, airroads and power lines are all *the same function* with a different
kind code, which is why `0x4FCAF0` is a hundred kilobytes long and why one range
covers everything you drag.)

Layer 1 now also covers the two validators that function hands off to — the
wire-and-pole one at `0x51D160` and the pipe-segment one at `0x5223C0`. Each is
called from exactly one place, `0x4FCAF0`, so a return address inside either is
a build tool as surely as one inside the line tool itself. Neither was in scope
before 1.4, which is why layer 1 told the truth to every check a fence or a pipe
made.

**Layer 2**, on the same key: 370 single-byte edits, 367 of them on by default.

* Drag tools keep a valid flag at `state+0x315`. A failed check is
  `mov byte ptr [state+0x315], 0`; anarchy flips the immediate to `1`.
* The building tool keeps it in `dil`. A failed check is `xor dil, dil`
  (`40 32 FF`); anarchy makes it `cmp dil, dil` (`40 3A FF`) — one byte, and
  `cmp` rather than `mov` because `xor r,r` *writes flags* and the compiler lets
  the next `jcc` consume them. `cmp dil,dil` sets exactly the same flags and
  leaves the register alone.
* And two more in 1.1. The kit hooks the ONE call that reaches the game's tool
  dispatcher and sets a flag for its duration, so while anarchy is on, *any*
  build tool is in scope — including `second_road`, factory connections and road
  waypoints, which have their own bodies and which no address list was going to
  keep up with. `widen_to_buildings = 0` leaves that call alone entirely.
* And the one that matters most: of the 228 refusals the drag tools can raise,
  only **63 clear the flag themselves**. The other **165** just write a category
  and a message id and leave one later instruction to turn that record into a
  refusal. There is exactly one `cmp dword ptr [reg+0x31C], 0` in the whole
  executable, at `exe+0x5116F3`; turning the `jl` after it into a `jmp` is one
  byte and switches off all 165.
* And the fifth family, new in 1.4, which is **why fences refused**. A fence is
  poles and spans, so it is validated by the wire-and-pole validator at
  `0x51D160` — not by the line tool's own body. Ten of that function's refusals
  clear the flag with an immediate and were already in the list. Eight write a
  *register* that happens to hold zero, which is a different instruction meaning
  the same thing, and the generator had only ever looked for the first shape.
  Seven of the eight now have a one-byte fix and the eighth is documented and
  left alone. The same function's copy of *Outside map borders!* is now under
  `allow_outside_map` with the building tool's.
* And the sixth, new in 1.5, which is why it still *looked* like it refused.
  Everything above makes the placement legal; none of it makes the game stop
  saying it is illegal. The tool UI at `0x317DB0` draws the warning straight off
  the recorded message id and never asks whether the placement is valid —
  `mov edx, [state+0x31c]` / `test edx, edx` / `js skip`, fifteen times over. So
  246 sites now set the top byte of that recorded id to `FF`, which makes it
  negative, which is exactly what "no message" means to that `js` and to the
  `jl` in the gate. The build went through the whole time; the red preview and
  the tooltip were the game reporting a refusal that had already been switched
  off.

*Outside map borders!* is left on — it is the only refusal that guards an index
rather than taste. Both copies of it (the building tool's and the wire
validator's) are under `allow_outside_map = 1`, and the log says what you did,
every time.

## skins

Recolouring a building is treated as a decision about the **type**, not a
repaint order: every building of that type you place from then on comes out
wearing that skin, and the ones already standing are left alone.
`spread_on_change = 1` brings back the old behaviour, with an undo key.

Everything it writes is one byte per building, clamped to the same range the
game's own spray-can clamps to.

## better trackbuilders

Rail construction trains ignore signals and each other — and, on a separate key,
the router can be made to read every one-way signal as a two-way one.

(The part is still called `trackcrew` in the ini and in the log: that is the
section name and the key prefix, and renaming it would quietly break every
existing `sovietkit.ini`.)

**It asks the game.** Every train's `script.ini` either declares
`$TRAINGROUP_TRACKBUILDER` or it does not — that is how the game itself is told
what a track builder is. The parser interns that keyword into an index and
stores it in a `vector<int>` on the vehicle type, and the game's own test is
`exe+0x3E9420`, which is short enough to quote:

```c
bool HasFeature(ignored, TYPE* t, int idx)
{
    if (!t || idx < 0) return false;
    int* b = *(int**)(t + 0x96B8);
    int* e = *(int**)(t + 0x96C0);
    for (; b != e; ++b) if (*b == idx) return true;
    return false;
}
```

`rcx` is never touched, so there is nothing to call — two pointer reads and a
walk of a handful of ints. The index for `"trackbuilder"` comes from the feature
table on the world object at `+0x127E8`, resolved once.

That makes it equally right about the base game's eleven, the DLC's, a
disassembled one, and a workshop train written by somebody who has never heard
of this plugin. **Nothing has to be configured for a modded track builder.**

### One-way semaphores — `Ctrl+Alt+D`

The hook above answers *may I proceed?*. It cannot help a track builder that has
laid track out past one-way signals facing the wrong way, because that train is
not waiting at a red light — **it has no route home at all**. The rail
pathfinder treats a one-way semaphore as a one-way edge and refuses to search
through it, so nothing ever gets as far as asking whether it may proceed. That
is why "ignore signals" looked like it did nothing for that case.

A semaphore's direction is at `+0x14` of the semaphore object, which hangs off a
rail node at `+0xB8`. The values are not a guess — `exe+0x41A510` is the handler
for clicking a placed semaphore and it cycles the field itself, writing 0, then
1, then 2; 10 is a waypoint. Four places read it for direction: two in the A*
edge cost at `exe+0x57A1A0`, two in the path builder at `exe+0x583D70`.

Each of the four edits is **one byte**: a conditional jump turned into the
unconditional jump to the *same target*. And that target is not a new path — it
is the branch the game already takes when the semaphore is two-way. Trace
direction 0 through all four tests in the original code and it lands exactly
where the patched code lands. So the change is not "signals are ignored", it is
precisely: **every one-way semaphore is read as a two-way semaphore, by the
router only.** Red is still red, blocks are still blocks, the movement code is
untouched.

**It is not limited to track builders, and it cannot be.** The router's search
context is a stack local built by whichever caller wanted a route —
`exe+0x582400` has eighty-two of them, road and ship and air among them — with
no vehicle pointer at a fixed offset to test. While these four bytes are
patched, *any* train may plan a route the wrong way down a one-way section,
which on a busy network is how you get a head-on meeting. So: off by default,
on a key of its own, and the log says the same thing every time you press it.
Turn it on, let the builder come home, turn it off.

All four sites must verify or the switch stays off entirely. Three out of four
is not a coherent state: the router would plan routes the path builder then
refuses to walk.

Both offset pairs are proved by reading back the game's own instructions, which
literally encode them — `48 8B 82 C0 96 00 00` is `mov rax, [rdx+0x96C0]`. If
those bytes ever stop matching, classification falls back to a name list and the
log says so, rather than reading a pointer out of the wrong place.

## Reading the log

Everything goes to `tesmioloader\build\sovietkit.log`. Startup should end with:

```
ready: anarchy on, skins on, trackcrew on
```

and above that, one block per part. The lines worth checking:

* `370 of 370 match this build; 0 do not` — anarchy's byte edits
* `one-way semaphores: all 4 router bytes match this build.` — trackcrew's
  one-way switch is available (it is still off until you press the key)
* `all 5 sites match …` — skins' offsets, confirmed against the binary
* `hooked exe+0x6ac6e0` — trackcrew's one inline hook

`debug` and `trace` ship **off**, and that is now the honest default rather than
a note to self. Each `debug` count is one interlocked increment on the
simulation thread, on a path the predicate hook reaches hundreds of thousands of
times a session; the census and the return-address histograms are diagnostics,
not telemetry.

When something is wrong, turn them on together: `[debug] trace = 2` plus
`debug = 1` in the part's own section, reproduce it, then press `Ctrl+Alt+R`.
At trace 2 the report prints a histogram of the addresses each hook was called
*from*, which is what turns "it still refuses" into a function name.

One warning about `trace = 3`: all three parts share one log and one lock, and
two of them log from the *simulation* thread while the third logs from the
render thread. At trace 3 with a rail job running and a drag tool open they
serialise on that lock. It is a diagnostic level, not a play level.

## Install

1. Put `sovietkit.cpp`, `sovietkit.ini`, `mod_*.inc`, `an_sites.h`, `skn_*.h`,
   `tc_sites.h`, `wk_sites.h`, `tsm_diag.h`, `tesmio_api.h`, `tesmio_plugin.h` and
   `build-sovietkit.bat` in one folder.
2. Run `build-sovietkit.bat`. It finds MSVC with `vswhere`, builds one
   translation unit, copies the DLL into `tesmioloader\build\plugins\`, and adds
   `sovietkit=1` to `tesmioloader.ini`.
3. Start the game **through `tesmiolauncher.exe`** — Steam's Play button starts
   it without the loader.

MSVC rather than a cross-build, because of `__try` / `__except`: without
structured exception handling a bad read takes the game down instead of
switching a part off and saying why.

If you still have `skins`, `anarchy`, `railfree` or `trackcrew` installed, take
them out. They hook the same functions and only the first one to get there wins.

## Walking

`[walking]` in the ini. No key, no frame work: the limits are patched once at
start and the part is done.

| key | default | what |
|---|---|---|
| `enabled` | 1 | `0` installs nothing (the same as `[sovietkit] walking = 0`) |
| `distance` | 1000 | metres of **path** a citizen will walk to a shop, job or service. Base game 480. `0` removes the limit and makes placement slow; above 20000 is clamped; negative reads as 480 |
| `car_distance` | 2500 | the same for a citizen with a car. Base game 2500, so by default it is left alone |
| `regen_on_load` | 1 | make the game's own regeneration pass run on every load so an existing city picks up the change. A few seconds of loading screen on a big city |
| `probe` | 0 | log the bytes and values at every site before anything is written |

`distance` is a length along roads and footpaths, not a straight line. A
building 400 m away across a river may be 1200 m of walking.

**Existing cities.** Each building's list of walkable neighbours is stored in the
save, so a new `distance` only reaches what is built afterwards unless
`regen_on_load` is on. Lowering it again works the same way in reverse: a save
made while it was raised keeps its long connections until something rebuilds
them. The save format is unchanged either way.

**What is patched.** The game writes the walking limit out in four separate
functions, with no shared constant, and every one that is missed does something
half-true: the batch and the queued connection builders (the limit itself),
the road-change collectors (a rebuild *radius*, held at `max(limit, 530)` and
`max(car, 2600)`) and the overlay behind the walking-distance button (which
re-runs the search with its own copy of the number). Seven of the eight are
`movss xmm0,[rip+disp32]` reading the shared float pool, so the instruction's
displacement is repointed at a float the part owns instead of overwriting the
constant, which dozens of GUI readers share. The eighth is an `imm32` and is
replaced. `regen_on_load` turns the save loader's `cmp [version],110` into a
compare against 127 and `test al,al` into `xor al,al`.

Everything is verified before it is written: the instruction shape, the address
its displacement resolves to, and the value stored there. The two walking
builders are applied together or not at all. See
[WALKING-RE-FINDINGS.md](WALKING-RE-FINDINGS.md) for the sites and
[`src/wk_sites.h`](../src/wk_sites.h) for the tables.

**Game builds.** Built-in tables: v1.1.1.9 and v1.2.0.8. Any other build logs its
stamp and is left alone. A new build is a `[sites]` edit and not a rebuild: run
`tools/walking_scan.py SOVIET64.exe`, which checks a known build site by site or
predicts a new one and prints the sixteen keys.

## What this cannot promise

Refusals usually mean the simulation was not written for what you are about to
do. Track through a building has a building inside it. A sewage pipe that runs
uphill will be built and will not flow — anarchy removes the *refusal*, not the
physics behind it.

**Keep a backup save.** Turn anarchy on for the bit you need it for and off
again afterwards; that is what the key is for.
