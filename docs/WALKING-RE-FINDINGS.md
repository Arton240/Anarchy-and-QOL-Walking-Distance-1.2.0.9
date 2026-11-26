# Walking distance — what the game does and where the number lives

**Provenance, first, because it decides how far to trust the addresses.** None of
this was read out of a disassembly made for this repository. It is ported from
the TesmioLoader `walking` plugin (`plugins/walking/walking.cpp` 1.3, its
`docs/12-walking.md`, and the port notes `docs/17-port-v1.1.1.9.md` and
`docs/19-port-v1.2.0.8.md`), whose author read it from SOVIET64.exe. The runtime
check in `src/mod_walking.inc` is what stands between a wrong address and a
corrupted instruction. Section *What is and is not verified* at the end says
exactly what has been checked and by whom.

## What walking is, in the engine

A building carries a list of the buildings reachable from it on foot. The script
API names it: `nWalkingBuildingNum`, read by `Building_WalkingBuilding_GetID` and
`GetDistance`.

| Building offset | Contents |
|---|---|
| `+0xCA8`…`+0xCB0` | `std::vector<WalkingConnection>`, stride `0xF0` |
| `+0xCC0` | the same for parking / personal cars |

| Connection offset | Contents |
|---|---|
| `+0x08` | the building at the other end |
| `+0x98` | the path length that was found, world units (metres) |

**Everything the simulation does with walking reads that list.** There is no "is
this close enough" test in the job, shop or service code: if the pair is in the
list, the walk happens. So the whole feature is the one number that decides what
goes into it when it is built.

## Where the limit lives

The list is built by a path search, and the search carries its ceiling in the
query object it is handed. `query+0x3C` is the longest path it will accept. Three
places in the path expander read it, with the same shape (v1.1.1.9 RVAs
`0x5799B5`, `0x57A278`, `0x57ABFD`):

```c
limit = query[0x3C];
if (limit > 0.0f && node[0x24] > limit)   // node+0x24 = length accumulated so far
    return -1.0f;                          // this branch is too long, drop it
```

A non-positive limit therefore means **no limit**, and anything positive is a
distance in metres. That is why `distance = 0` is passed through unchanged (and
why it is a bad idea: the search is breadth-first over the road graph, so cost
grows with the square of the limit).

## Eight sites, in three roles — and no shared constant

The walking distance is spelled out separately in four functions.

| Role | Function (v1.1.1.9) | What it reads | Value |
|---|---|---|---|
| builder, walking, batch | `0x12E1D0` | an immediate, `mov dword [rsp+0x7C],480.0f` | **480** |
| builder, walking, queued | `0x12E6E0` | `movss` from the float pool | **480** |
| builder, parking | `0x12F830` | `movss` | **2500** |
| overlay, walking | `0x43EF10` | `movss`, two sites | **480** |
| overlay, parking | `0x43FEA0` | `movss` | **2500** |
| collector, walking | `0x12DE30` | `movss` | 530 |
| collector, parking | `0x12F480` | `movss` | 2600 |

A **builder** clears a building's connection vector and fills it in again; its
limit *is* the walking distance. Walking has two of them, the same logic written
out twice: the batch one takes a whole set of buildings and is called straight
from the save loader, and the queued one takes a single building and is drained
twenty at a time off the queue at `game+0x11F88` by `0x12EC50` — the path
everything in a running game takes. Parking has only the queued form.

A **collector** runs after a road changes to work out which buildings need
rebuilding and hands that set to a builder. Its limit is a *radius*,
deliberately a little wider than the builders'. It is held at
`max(limit, vanilla radius)`: a building 900 m from a road that changed has to be
in the rebuild set, or it never notices the road at all.

The **overlay** is what the building window's walking-distance button
highlights. **It does not read the connection vector**; it runs the search
again with its own copy of the number and draws the path polylines and metre
labels. Patching only the builders gives citizens that walk 1000 m under a
button that still draws 480 m — the version of this feature that shipped
first. All eight sites are patched.

`0x441890` (the hovered-connection and blueprint form of the overlay) uses a
fifth literal, 125 (`0x90ABE8` in v1.1.1.7), for buildings of type `0x69` and for
the not-yet-built case. What it means has not been worked out and it is left
alone.

## Why the instruction is repointed and not the constant

Seven of the eight sites are `movss xmm0,[rip+disp32]` into `.rdata`, and both
`480.0f` and `530.0f` sit in the shared literal pool: dozens of unrelated
instructions read those same four bytes, panel widths and GUI coordinates among
them (31 readers of 480, sixty-odd of 530 in v1.1.1.7). Writing `1000.0` over
the constant would move far more than walking. The displacement of each `movss`
is rewritten instead, to point at a float this part owns, allocated within
±2 GB of the executable so a rel32 reaches it: four floats, one per distinct
value, shared by several sites. The eighth site is an `imm32` inside the `mov`
and is simply replaced.

The encoding of that eighth site changed between builds: v1.1.1.9 addresses the
stack slot off RSP (`C7 44 24 7C imm32`, four opcode bytes), v1.2.0.8 off RBP
(`C7 45 80 imm32`, three). `mod_walking.inc` decodes both shapes and locates the
immediate from whichever it finds.

## Making an existing city notice

Connections are stored in the save, so a raised limit only affects what is built
afterwards. The base game has the same problem whenever its developers raise the
figure and already ships the fix: the save loader regenerates every walking and
parking connection when the save's format version is below 110.

```asm
cmp  dword [save_version],110     ; 83 3D <disp32> 6E     format is 124-125 in every save today
jge  skip
test al,al                        ; 84 C0                 al = a flag set from the terrain name
jnz  skip
lea  rdx,["Import - Regenerating walking and parking connections "]
```

`regen_on_load = 1` makes both conditions true: the compare's `imm8` becomes
`127`, the largest one can hold, and `test al,al` becomes `xor al,al` — the same
two bytes, which always sets ZF. Nothing is reimplemented; the game does its own
full regeneration on every load with whatever limits are installed. If a future
build raises the save format past 127 this stops working, and it stops by doing
nothing. (If the second write fails the first is put back, so the guard is never
left half-open.)

## The tables

`src/wk_sites.h`. RVAs into SOVIET64.exe.

| site | v1.1.1.9 (`0x6A3EB6AD`) | v1.2.0.8 (`0x6B0097B2`) | reads / is |
|---|---|---|---|
| `walk_limit_op` | `0x12E2CD` | `0x146CBD` | the batch builder's `mov dword [rsp/rbp+..],480.0f` |
| `walk_queued` | `0x12E79F` | `0x14718F` | `movss` → `walk_const` |
| `walk_overlay_1` | `0x43F0EA` | `0x4636EA` | `movss` → `walk_const` |
| `walk_overlay_2` | `0x43F8D5` | `0x463ED5` | `movss` → `walk_const` |
| `car_build` | `0x12F916` | `0x148306` | `movss` → `car_const` |
| `car_overlay` | `0x440053` | `0x464653` | `movss` → `car_const` |
| `walk_rebuild` | `0x12DE97` | `0x146887` | `movss` → `walk_radius_const` |
| `car_rebuild` | `0x12F4F2` | `0x147EE2` | `movss` → `car_radius_const` |
| `walk_const` | `0x90AF20` | `0x952468` | float `480.0` |
| `car_const` | `0x90B104` | `0x95264C` | float `2500.0` |
| `walk_radius_const` | `0x90AF58` | `0x9524A0` | float `530.0` |
| `car_radius_const` | `0x90B108` | `0x952650` | float `2600.0` |
| `regen_cmp` | `0x438406` | `0x45C7E4` | `83 3D <disp32> 6E` |
| `regen_test` | `0x438413` | `0x45C7F1` | `84 C0 0F 85 ...` |
| `save_version` | `0x9E9C3C` | `0xA33CFC` | the `.data` slot `regen_cmp` compares |

Between the two builds the code and the float pool move by one constant per
region: **+0x189F0** for `walk_limit_op`, `walk_queued`, `car_build`,
`walk_rebuild`, `car_rebuild`; **+0x24600** for the three overlay sites;
**+0x243DE** for the two save-loader sites; **+0x47548** for the four floats.
Fourteen addresses, four deltas. That is the property `tools/walking_scan.py`
uses to predict a new build from an anchor in each region.

The ini carries the same sixteen values as `walk_*`, `car_*`, `regen_*` and
`save_version` keys in `[sites]` — all or none, and `walk_build_stamp` must equal
the exe's — so a new build needs no rebuild of the DLL.

## What is and is not verified

**Done here, offline:**

* The v1.1.1.9 table is the one in the TesmioLoader port notes for the same PE
  stamp `0x6A3EB6AD` that `an_sites.h`, `tc_sites.h` and `skn_sites.h` already
  gate on, and the `.data` globals the two repositories both name agree
  (`0x9D4F10` world object, `0x9941F0` object slot, `0x11B08` building vector).
* The two tables agree with each other: fourteen addresses move by four deltas,
  none by an odd one out (above).
* `mod_walking.inc` compiles clean under `g++ -Wall` and was run against a
  synthetic image built from each table. It patches the right bytes, leaves the
  shared literal pool alone, refuses a shifted constant (and then applies
  neither walking builder), refuses an unknown stamp, honours `enabled = 0`,
  survives `VirtualProtect` failing, and reads a complete `[sites]` table while
  rejecting an incomplete one or one with the wrong stamp.
* `tools/walking_scan.py --self-test` does the same for the scanner.

**Not done, because there is no game or SOVIET64.exe here:**

* No address was checked against a real executable, by anyone in this
  repository. The runtime check does that on the player's machine and logs
  `refusing` for any site that does not match.
* **v1.2.0.9 has no table.** Its stamp and addresses are not in either source.
  Nothing is guessed: the part logs the stamp and writes nothing. The scanner
  produces the table from the real exe.
* Not built with MSVC (the toolchain is Windows-only) and not run in game. The
  game-side claims — that citizens use shops past 480 m, that the regeneration
  pass runs, and that the overlay shows the new distance — are the TesmioLoader
  author's report for v1.2.0.8 (the first two) and v1.3 (the overlay, "patched
  and logged but not looked at in game yet").
* The in-game hint text still says "approx. 300m". It is a language-file string
  and is not touched.
