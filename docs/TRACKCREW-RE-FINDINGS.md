# Rail construction trains — reverse-engineering findings

**Target:** `SOVIET64.exe` v1.1.1.9, PE TimeDateStamp `0x6A3EB6AD`. Read statically
out of the binary.

The goal: track-laying trains that do not queue at signals and do not brake for
each other, because laying rail on realistic is mostly watching them stand still.

---

## 1. The game already has this feature, per train

`sovietEnglish.btf` has two strings that turn out to be the whole story:

```
3081 (0xC09)  Ignore red signal
3082 (0xC0A)  Cancel ignore red signal
```

Both are used in the vehicle info window (`0x7D5460`–`0x7EBB7C`), in a tooltip
that picks between them:

```asm
007d9703  cmp byte ptr [r15 + 0x5E0], 0
007d970b  je  0x7d9736
007d970d  mov edx, 0xC0A            ; "Cancel ignore red signal"
...
007d9736  mov edx, 0xC09            ; "Ignore red signal"
```

So **`vehicle + 0x5E0` is the ignore-red-signal flag**, and the button at
`0x7DA8A1` (`mov byte ptr [rdi + 0x5E0], al`) is what toggles it.

## 2. What the flag actually does

One function reads it: `0x6AC6E0`.

```asm
006ac6e0  push rbx
006ac6e2  sub  rsp, 0x70
006ac6e6  cmp  byte ptr [rcx + 0x5E0], 0
006ac6ed  mov  rbx, rcx
006ac6f0  je   the_real_check
006ac6f2  mov  al, 1                    ; flag set -> proceed, full stop
006ac6f4  add  rsp, 0x70
006ac6f8  pop  rbx
006ac6f9  ret
the_real_check:
006ac6fa  mov  rax, qword ptr [rcx + 0x6A8]    ; the path node vector, end
006ac704  sub  rax, qword ptr [rcx + 0x6A0]    ; ... begin
006ac70b  movsxd rdx, dword ptr [rcx + 0x700]  ; the current index along it
  ... walks the nodes ahead, reading node+0xB8 -> +0x14 (a kind) and +0x1AA,
      and comparing successive nodes' owners - the block/reservation check ...
```

So it is the train's **"may I proceed?"** predicate: `1` = go, `0` = brake.

## 3. Both callers, and why one hook covers both complaints

`0x6AC6E0` is called from exactly two places, both inside the train movement
routine `0x6A7520`, and both have the same shape:

```asm
006a8b83  mov  rcx, rsi        ; rsi = the vehicle
006a8b86  call 0x6AC6E0
006a8b8b  test al, al
006a8b8d  jne  0x6a8c97        ; proceeding skips the ENTIRE brake block
006a8b93  movss xmm1, dword ptr [r14 + 0x11C]    ; distance to the thing ahead
006a8ba1  movss xmm0, dword ptr [rsi + 0x7A4]    ; this train's braking distance
006a8ba9  comiss xmm0, xmm1
006a8bac  jbe  0x6a8c97
```

and the second at `0x6A8DD7`, identically.

The jump on `al != 0` skips past **both** the signal wait and the
following-distance comparison against whatever is ahead. That is the useful
part: one hook gives "runs red signals" and "does not queue behind the other
track builder" together, because in the game's own code they are the same
decision.

The game clears the flag itself at `0x6A8E9A` (`mov byte ptr [rsi+0x5E0], ch`)
once the train is through — which is why the player-facing version is one-shot
and a permanent version has to be a hook rather than a written byte.

## 4. Identifying a track builder

The game has a train-group concept. `0x3EE7E5` onward is the parser:

```asm
003ee7e5  lea rdx, "$TRAINGROUP_TRACKBUILDER"
003ee7f3  call strcmp
003ee7fc  lea rdx, "trackbuilder"          ; -> group id via 0x25C7B0
003ee808  lea rdx, "$TRAINGROUP_TRACKBUILDER_STEAM"
003ee823  lea rdx, "trackbuilder"          ; the same group
```

alongside a construction-skill enum in the same parser —
`$SKILL_CONSTRUCTION_RAIL_TRACK_LAYER` is skill `0x1A` (`0x3EEE27`), with
siblings `RAIL_CRANE`, `RAIL_CRANE2`, `RAIL_TUNNEL_BORING`, `ROLLING`,
`ASPHALT_LAYING`, `GROUNDWORKS`, `BULLDOZER`.

**This is the one part not finished statically.** Knowing the group exists is not
the same as knowing which field on a live vehicle carries it, and guessing an
offset here would mean either missing every track builder or classifying every
train as one. Neither failure announces itself.

So the plugin does it the other way round: it reads the vehicle's **object
folder name** and matches it against a configurable substring list, and — this is
the point — it keeps a census of every name the hook actually saw, with its
verdict, and prints it on demand. One minute of a real construction job turns
"which vehicles are these" from a disassembly problem into an ini edit.

The name offset itself (`+0x200`) comes from another mod's notes rather than
from the disassembly above, so it is treated as a hint: the plugin tries it,
and if it does not yield a plausible name it scans the object for a field that
does and logs where it found it.

## 5. What this cannot do

A track builder that ignores a signal is still a solid object to the rest of the
simulation. Two of them meeting head-on on a single track are still two trains
on one track. This removes the waiting, not the geometry — the user's phrase was
that they should be "kind of invisible but at the same time they aren't", and
that is exactly the line: invisible to the signalling, present in the world.

`0x6AC6E0` is hooked inline with 16 verified bytes:

```
40 53              push rbx
48 83 EC 70        sub  rsp, 0x70
80 B9 E0 05 00 00 00   cmp byte ptr [rcx+0x5E0], 0
48 8B D9           mov  rbx, rcx          = 16, on an instruction boundary
```

no relative branches in the stolen range, and the hook refuses rather than
writes if those bytes ever differ.

---

# Addendum, 0.9: one-way semaphores and the rail router

All RVAs read statically out of `SOVIET64.exe`, PE `TimeDateStamp 0x6A3EB6AD`,
with capstone and `pefile`, `.pdata` for function boundaries.

## The problem the can-proceed hook could not touch

A track builder that has laid track out past one-way signals facing the wrong
way is not waiting at a red light. It has no route home at all. `can_proceed`
(`0x6AC6E0`) is never consulted, because the route search failed first — which
is why the log could honestly say `asked 249476, let through 108959` while the
train sat still.

## The semaphore's direction field

A rail node holds its semaphore at `+0xB8`. The semaphore's direction is the
`int` at `+0x14`, and the values are not inferred — `0x41A510` is the handler
for clicking a placed semaphore, and it cycles the field itself:

```
0041a768  mov dword ptr [rax + 0x14], 0        ; two-way
0041a7f0  mov dword ptr [rax + 0x14], 1        ; one-way, one sense
0041a80e  mov dword ptr [rax + 0x14], 2        ; one-way, the other sense
```

`0xA` is a waypoint; `can_proceed` returns "go" for it without further tests.
`+0x1A8`/`+0x1A9` are the per-direction red/green bytes and `+0x1AA..0x1AD` the
chain/pre-signal aspects — none of which this change touches.

## Where the router reads it

The rail search is `0x57DBC0`, driven from the generic route entry `0x582400`
(82 callers, all transport modes). Its edge-cost function is `0x57A1A0`
(a multi-chunk function running to about `0x57AA2B`), which returns a float cost
and `-1.0f` — the constant at `0x90B300` — for "impassable".

```
0057a44f  83 79 14 01   cmp dword ptr [rcx + 0x14], 1
0057a453  75 05         jne 0x57a45a
0057a455  80 fa 01      cmp dl, 1              ; dl = direction of travel
0057a458  74 15         je  0x57a46f           ; -> call 0x580EB0, return -1.0f
0057a465  83 79 14 02   cmp dword ptr [rcx + 0x14], 2
0057a469  75 37         jne 0x57a4a2           ; -> carry on
0057a46b  84 d2         test dl, dl
0057a46d  75 33         jne 0x57a4a2
```

`dl` comes from `cmp r8, r9 / sete dl` at `0x57a3a5`, where `r8 = edge->[8]` and
`r9` is the previous node — inverted when `[ctx+3]` says the search is running
backwards.

The second pair is in `0x583D70`, the path builder that turns a found route into
a drivable one (10 callers, among them `0x67FA20`, `0x6B5EC0` and `0x6C4790`,
which are the per-vehicle route requests):

```
0058450c  83 fa 0a      cmp edx, 0xa
0058450f  74 72         je  0x584583           ; waypoint -> no restriction
...
005847e8  41 83 f8 0a   cmp r8d, 0xa
005847ec  74 16         je  0x584804
```

## The four edits, and why they are safe to describe precisely

| rva | before | after | what it was |
|---|---|---|---|
| `0x57A453` | `75 05` | `EB 05` | `jne 0x57A45A` -> `jmp 0x57A45A` |
| `0x57A469` | `75 37` | `EB 37` | `jne 0x57A4A2` -> `jmp 0x57A4A2` |
| `0x58450F` | `74 72` | `EB 72` | `je 0x584583` -> `jmp 0x584583` |
| `0x5847EC` | `74 16` | `EB 16` | `je 0x584804` -> `jmp 0x584804` |

Every one is a conditional jump turned into the unconditional jump **to the same
target**. One byte, same length, no instruction boundary moved, nothing that can
be read half-written.

And the target is not a new path. Trace direction `0` — a two-way semaphore —
through the original code at each of the four sites and it reaches exactly the
address the patched jump goes to:

* `0x57A453`: `0 != 1`, so the `jne` is taken -> `0x57A45A`.
* `0x57A469`: `0 != 2`, so the `jne` is taken -> `0x57A4A2`.
* `0x58450F`: `setne al` gives `al = 1` for direction 0 in both arms, so the
  `jne` at `0x584527` is taken -> `0x584583`, the same target as the waypoint
  `je`.
* `0x5847EC`: `cl` is set to `1` at `0x5847B2` and `setne cl` gives `1` again for
  direction 0, and the waypoint `je` skips the `setne` leaving `cl = 1`. Same
  state, same address.

So the four edits do not invent behaviour. They make every one-way semaphore
look to the router exactly like a two-way one.

## Why it is not scoped to track builders

The search context is a **stack local**: `lea rcx, [rbp-0x40]` / `call 0x582400`
in `0x6B5EC0`, `lea rcx, [rbp-0x60]` in `0x6C4790`, `lea rcx, [rbp-0x80]` in
`0x67FA20`. Different frames, different layouts, no vehicle pointer at a fixed
offset to test, and `0x582400` serves roads, ships and aircraft as well as rail.
There is nothing inside `0x57A1A0` to ask "is this a track builder".

The vehicle IS reachable one level out — `0x6C4790` holds it in `r15` and reads
its type from `[r15+0x1708]`, which is the same pointer trackcrew already walks
for the `$TRAINGROUP_TRACKBUILDER` flag — so a scoped version would mean hooking
those callers and flipping the bytes around each call. That trades a stable,
player-controlled switch for code writes on a hot path, which is a worse deal
than a key and an honest warning.

## Dead ends on the way here

* **btf 13804 "Using old pathfinding."** The `imm32` scan matched `0x35EC` in
  `lea rcx, [rbp+0x35ec]` inside a UI layout function. A displacement, not a
  string id. Any "find the references to message N" scan needs this filter.
* **`0x564E20`**, which does carry btf 4066 *Some vehicle cannot find route to
  destination* and has the `0xA`/`2` shape, is in the `0x552090` UI group and
  reads `[rsi+0x120]`/`[rsi+0x12c]` — a different struct entirely.
* **The inlined `HasFeature` loops** in `0x68B330` and `0x699420` (16 and 2 sites
  on `+0x96B8`) are set intersections between a vehicle type's feature vector and
  a station's requirement list, not a track-builder test.
* **`0x6C6770`**, the third reader of the per-vehicle ignore-red byte `+0x5E0`,
  is the vehicle info window copying fields into a display struct.
* **`0x6A8E90`**, the second, is the game clearing the flag once the train is
  through the signal — which is why "Ignore red signal" is one-shot.
