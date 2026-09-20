# How placement validity works in SOVIET64.exe v1.1.1.9

Everything below was read statically out of `SOVIET64.exe`, PE `TimeDateStamp`
`0x6A3EB6AD`, with capstone and `pefile`, using `.pdata` for function
boundaries. RVAs, not VAs — add `0x140000000` for the image base.

This supersedes `RAIL-RE-FINDINGS.md`, whose central claim (that `0x4FCAF0` is
"the rail build tool") turns out to be wrong in an interesting and useful way.

---

## 1. There is one tool dispatcher, and it is enormous

`0x30D1A0 .. 0x3178F6` (about 43 KB, 9445 instructions) is called from exactly
one place — `0x105F45`, inside the per-frame UI update at `0x105E39` — and
contains **403 calls to `strcmp` (`0x84F520`)**. It is the function that asks
"which tool is selected" and runs it.

Pulling the string literal out of each `strcmp` gives the complete tool
vocabulary: `rail_wood`, `rail_concrete`, `rail_bridge_curved_t`, `pipe_steam`,
`waterpipe_0`, `sewagepipe_4`, `heating_pipe_big_underground`, `eletric_high7`,
`eletric_ulow2`, `fence_zp250`, `pedestrianway_brick_lamps`, `conveyor`,
`create_cableway`, `create_airroad_fly_mud`, `road_bricktram`,
`factoryconnection_oneway`, `bojka_red`, `terrain_smooth_editor`,
`pick_tool_from_map` … 403 of them (with duplicates; the chain is walked more
than once for different phases).

A single hook here would put a "we are inside a build tool" flag around every
tool in the game. It was considered and rejected in favour of what section 3
found, which is cheaper and more precise.

## 2. `0x4FCAF0` is THE line tool, not the rail tool

This is the finding that made the rest easy. Building a call graph over `.text`
(direct `E8 rel32`, attributed to the containing `.pdata` function — 6287 nodes,
20657 edges) and doing a BFS from each tool handler:

| handler | tool family | reaches `0x4FCAF0` at depth |
|---|---|---|
| `0x3D4730` | rail | 1 |
| `0x3B0E60` | pipes / steam | 1 |
| `0x3B12B0` | water and sewage | 1 |
| `0x3B3820` | fences | 1 |
| `0x3AEC30` | conveyors | 1 |
| `0x3CBFA0` | cableways | 1 |
| `0x3C9130` | airroads | 1 |
| `0x3B8780`, `0x3BAED0`, `0x3D00F0`, `0x3D68B0` | more of the same | 1 |

**Every tool you drag is the same function with a different kind code.** That is
why `0x4FCAF0 .. 0x518A14` is 100 KB and why its refusal vocabulary mixes rail
turns, wire types, metro connections and pipe slope in one body.

The practical consequence: railfree's `InRail()` range was never rail-only. It
had been covering pipes, fences and conveyors all along.

Two validators hang off it:

* `0x51D160 .. 0x51F9B1` — wires and poles. Owns *Can't connect two different
  types of wire*, *Distance between poles too far*, *Crossing not allowed*.
* `0x5223C0` / `0x52246E` / `0x522647` / `0x522930` — pipe segments. Owns *Too
  low*, *Too steep of an incline*, *Too short*.

Buildings are the other family: `0x2BADE0 .. 0x2D8355`.

## 3. The validity flag

Every refusal in the line tool has the same three-instruction shape:

```
41 C6 85 15 03 00 00 00    mov byte  ptr [r13+0x315], 0      ; not valid
41 C7 85 18 03 00 00 06..  mov dword ptr [r13+0x318], 6      ; category
41 C7 04 24 55 04 00 00    mov dword ptr [r12], 0x455        ; btf 1109
```

so the shared tool-state object is:

| offset | meaning |
|---|---|
| `+0x310`, `+0x311`, `+0x312`, `+0x313` | preview/hint flags |
| **`+0x315`** | **placement valid (byte)** |
| `+0x318` | refusal category, `-1` = none |
| `+0x31C` | btf string id of the message shown |

`+0x315` is the one that matters. The checks run as a chain and each is guarded
by the result of the last:

```
005125fb mov  byte ptr [r13+0x315], 0     ; a check just failed
00512603 mov  dword ptr [r13+0x318], 6
0051260e mov  dword ptr [r15], 0x104e
00512615 cmp  byte ptr [r13+0x315], 0     ; ...so skip the rest
0051261d je   0x5126d9
```

and the error text at the end is behind the same test:

```
00517be3 cmp  byte ptr [r13+0x315], 0
00517beb jne  0x517cb1                    ; valid -> don't draw a message
```

There are **107 references to `+0x315` in the whole executable**, in 7
functions. 82 of them are `mov byte ptr [reg+0x315], 0` — the refusals — split
68 in the line tool, 10 in the wire validator, 4 in the building tool (where it
is reached through a pointer, `mov rax, [r13+0xFA40]` then
`mov byte ptr [rax+0x315], 0` — same struct).

**Flipping that immediate from `0` to `1` is one byte.**

## 4. The building tool keeps its flag in a register

`0x2BADE0` does not use `+0x315` for its own checks. It keeps the validity flag
in `dil` for the whole pass:

```
002c9351 40 B7 01     mov dil, 1          ; start of the validity pass
...
002c96ae mov  dword ptr [r13+0x11a10], ebx     ; category
002c96b5 mov  dword ptr [r13+0x11a14], 0x48b   ; btf 1163 "Need flat terrain"
002c96c0 40 32 FF     xor dil, dil        ; not valid
```

`xor dil, dil` is `40 32 FF`. `mov dil, dil` is `40 8A FF` — a no-op that leaves
the flag alone. Opcode `32` (XOR r8, r/m8) becomes `8A` (MOV r8, r/m8).
**One byte.**

There are 33 `xor dil, dil` in the function. 31 of them are in the contiguous
validity block `0x2C9385 .. 0x2CAA74` and are the ones anarchy uses; the two
outside it (`0x2CB534`, `0x2D5E97`) are in a region where `dil` is compared
against `1` and `2` and therefore holds something else, and are left alone.

## 5. The refusal vocabulary

`sovietEnglish.btf` is a flat table: a 12-byte header whose first `u32be` is the
entry count (8556), then that many 10-byte records of
`(id:u32be, charOffset:u32be, charLen:u16be)`, then a UTF-16BE string blob at
`12 + count*10`. Offsets and lengths are in **characters**, so byte offsets are
doubled.

Scanning `.text` for `mov`/`cmp` instructions whose `imm32` is one of the
placement-refusal ids found 196 sites. The ones that matter:

| id | text | where it is set |
|---|---|---|
| 1104 | Too low | `0x522F4C` |
| 1105 | Too steep of an incline | `0x5225B9` |
| 1106 | Too steep of a descent | `0x51ED22` |
| 1107 | Too sharp of a turn | line tool ×3 |
| 1108 | Some other infrastructure is in the way | line tool ×6, wire ×2, pipe ×2 |
| 1109 | Building in the way | line tool ×3 |
| 1110 | Underwater, or too close to water level | |
| 1114 | Crossing not allowed | wire ×5 |
| 1119 | Overlaps with existing construction! | |
| 1120 | Tunnel is above the terrain at some stage | line tool ×3 |
| **1134** | **Requires proper slope** | line tool ×5 |
| 1155 | Angle of connection is too sharp | line ×1, wire ×2, building ×1 |
| 1160 | Distance between poles too far | wire ×1 |
| **1162** | **Outside map borders!** | building ×1, wire ×1 |
| 1163 | Need flat terrain | building ×1 |
| 1164 | Can't build this building on the water | building ×2 |
| 1165 | Can't build building due to infrastructure | building ×2 |
| 1166 | Can't build building, too close to existing building | building ×1 |
| 1177 | Can't connect two different types of wire | wire ×1 |
| 1197 | Connection cannot be in another building | building ×1 |
| 20137/20138 | metro connection rules | line tool |

1162 is the only one on the list that is about memory rather than taste, and it
is the only one anarchy leaves on by default.

## 6. What was tried and did not work

**`+0x318` as a universal gate.** The first idea was to find one "is this
placement valid" consumer and hook it. Scanning for reads and compares of
`+0x318` returned **3328 sites across 834 functions** — `0x318` is a common
offset on many unrelated structures (it is a vtable-ish pointer field on at
least one hot object; `mov rcx, [rax+0x318]` appears dozens of times inside the
line tool alone for something else entirely). Dead end, and a dangerous one:
hooking on the offset rather than the object would have hit unrelated code.

**Hooking the dispatcher at `0x30D1A0`.** Workable — one caller, a relocatable
18-byte prologue (five pushes plus `lea rbp, [rsp-0x2420]`, no rip-relative, no
branches) — but it would have meant a detour on the frame path and a context
flag read by three hot predicates, to achieve something the byte edits achieve
with no runtime cost at all.

**Patching the `+0x318` category writes to `-1`.** Would have needed 4-byte
writes, many of them `mov [r13+0x318], r15d` with no immediate to change. The
`+0x315` flag makes it unnecessary: the checks downstream give up on the flag,
not on the category.

## 7. Why one byte, specifically

A single-byte store on x86 is atomic. That is the whole reason anarchy can be a
toggle rather than a launch option: `Ctrl+A` can be pressed with the game
running, mid-frame, with the build tool executing on another thread, and no
thread can observe a half-written instruction. Every site is verified against
its recorded bytes before the first write, and a mismatch marks the site dead
rather than writing anything — so a game update degrades this plugin one site at
a time instead of corrupting instruction boundaries.

The generated table lives in `anarchy/an_sites.h`: 113 entries, each with its
RVA, its length, the bytes that must be there, which byte changes, what it
becomes, and — for 70 of them — the message the game would have shown.
