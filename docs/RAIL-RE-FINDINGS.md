# The track builder's refusals — reverse-engineering findings

**Target:** `SOVIET64.exe` v1.1.1.9, PE TimeDateStamp `0x6A3EB6AD`. Read statically
out of the binary; no save was touched and the game was not run.

---

## 1. Finding the track builder at all

The game dispatches tools by name through a `strcmp` chain (`strcmp` = `0x84F520`)
running from about `0x312000` to `0x314950`. Every entry is the same shape:

```asm
lea  rdx, "rail_wood"
mov  rcx, rbx
call 0x84F520          ; strcmp
test eax, eax
jne  next
...                    ; set edx = kind, r8d = variant
jmp  0x3140A2
```

and all twenty rail entries — `rail_wood`, `rail_concrete`, `rail_eletric`,
`rail_metro`, `rail_destroy`, every `rail_bridge_*`, `rail_tunel`,
`rail_tunel_metro` — converge on one call:

```asm
003140a2  mov rcx, r14
003140a5  call 0x3D4730          ; the rail build tool, (world, kind, variant)
```

`0x3D4730` is a small dispatcher; the body is **`0x4FCAF0`–`0x518A14`**, 114 KB.
That address range *is* "the track builder", and every override in `railfree` is
scoped to return addresses inside it.

Two other handlers came out of the same chain and are worth writing down:
`pick_tool_from_map` → `0x3CB0D0` (the picker, see `SKINS-RE-FINDINGS.md`), and
all the `rail_semaphore*` tools → **`0x418F90`**, which is the semaphore tool and
a different thing from the track tool.

## 2. How a refusal works

Every refusal writes two fields on the tool's state object:

```asm
mov dword ptr [r13 + 0x318], 6        ; a reason category
mov dword ptr [r13 + 0x31C], 0x455    ; a btf string id -> what you see on screen
```

So the complete vocabulary of the track tool can be read straight out of the
binary and looked up in `sovietEnglish.btf`:

| id | message | overridden? |
|---|---|---|
| 1068 | Semaphore/signals/waypoint in the way | **yes** — rule 2 |
| 1107 | Too sharp of a turn | no |
| 1109 | Building in the way | **yes** — rule 1 |
| 1110 | Underwater, or too close to water level | no |
| 1119 | Overlaps with existing construction! | **yes** — rule 3 |
| 1120 | Tunnel is above the terrain at some stage | no |
| 1121 | Railway crossing is too close to road intersection or road end | no |
| 1123 | Railway crossing is too close to road intersection | no |
| 20137 | Metro track cannot be connected here | no |
| 20138 | Only metro track can be connected here | no |

The three that are overridden are the three that mean *something is in the way*.
The rest mean *this track is not buildable*, which is a different claim, and
switching those off would produce geometry rather than annoyance.

## 3. Rule 1 — "Building in the way" (1109)

`0x129F30` is the building-collision predicate.

```
char Collide(void* world, C3DVECTOR3* pos, int ignoreType, int ignoreInst,
             char a5, void* a6, char a7, float radius)
    1 = something is in the way,  0 = clear
```

It calls `C3D_BUILDING::Collision2DBBox` at `0x129FD4`, returns 0 early when the
hit is the building it was told to ignore, checks a per-type flag at
`GetType(...)+0x2324`, and then walks `world+0x11C28..0x11C30` — a second vector,
the things currently under construction.

Nineteen call sites, **eleven of them inside the rail body**:

```
0x503FEE  0x504066  0x5078C9  0x507910  0x507957  0x507B6F
0x507BC2  0x507C75  0x507CC8  0x507E4F  0x507EA2
```

and the consequence is visible immediately at two of them:

```asm
00503fee  call 0x129F30
00503ffa  test al, al
00503ffc  je   ok
00503ffe  mov dword ptr [r13 + 0x318], 6
00504009  mov dword ptr [r12], 0x455        ; 1109, "Building in the way"
```

The other eight callers are other tools — road, pipe, building placement — and
are why the override tests the return address rather than simply lying to
everybody.

**Hook:** inline, 15 stolen bytes, byte-verified.

```
4C 8B DC              mov r11, rsp
49 89 5B 08           mov [r11+0x08], rbx
49 89 6B 10           mov [r11+0x10], rbp
49 89 73 18           mov [r11+0x18], rsi     = 15, on an instruction boundary
```

No relative branches in the stolen range, so there is nothing for the
relocator to get wrong.

## 4. Rule 2 — "Semaphore/signals/waypoint in the way" (1068)

This one is not a collision test at all. It is a distance test:

```asm
0050e441  call [C3DCalcDistanceSq]        ; xmm0 = distance squared
0050e447  movaps xmm1, xmm6
0050e44a  mulss  xmm1, xmm11
0050e44f  mulss  xmm1, xmm6
0050e453  mulss  xmm1, xmm11              ; xmm1 = (radius * k)^2
0050e458  comiss xmm1, xmm0
0050e45b  jbe    ok                       ; refuse only when radius^2 > distance^2
0050e45d  mov byte  ptr [r13 + 0x315], 0
0050e465  mov dword ptr [r13 + 0x318], 3
0050e470  mov dword ptr [r13 + 0x31C], 0x42C    ; 1068
```

`?C3DCalcDistanceSq@@YAMAEBVC3DVECTOR3@@0@Z` is an **import**, which makes this
the cheapest and safest hook in either plugin — nothing is patched at all. It has
eighteen call sites in the whole executable; exactly four of them are this test:

```
call site   return address
0x50CF2E -> 0x50CF34
0x50D020 -> 0x50D026
0x50E441 -> 0x50E447
0x50E51C -> 0x50E522
```

Returning a large number for those four makes every comparison fall through. The
plugin returns `1e30f` rather than infinity, because two of the four sites
multiply the result through a chain of `mulss` and a NaN would set the parity
flag on `comiss` and take a branch nobody intended.

## 5. Rule 3 — "Overlaps with existing construction!" (1119)

```
char Overlap(void* state, C3DVECTOR3* pos, float radius, int flag)
```
at `0x56C820`. Four call sites, **all four inside the rail body**
(`0x50752D`, `0x5075C7`, `0x50760C`, `0x51150A`), so this one needs no scoping at
all — it keeps the return-address test anyway, because a counter that records
where a hook was reached from is how the next game update announces itself.

**Hook:** inline, 15 stolen bytes.

```
48 89 5C 24 08        mov [rsp+0x08], rbx
48 89 74 24 10        mov [rsp+0x10], rsi
48 89 7C 24 18        mov [rsp+0x18], rdi     = 15
```

## 6. What was looked at and left alone

- **The semaphore tool (`0x418F90`).** It has its own refusals — btf 1053 "Can't
  place semaphore too close to building", 1054 "Can't place semaphore on switch",
  1055, 1151 "on unfinished infrastructure". Nothing in this plugin touches them.
  Placing signals is a different job from laying track, and the request was about
  the track builder.
- **`Collision2DBBoxIgnore`**, which has exactly one call site in the whole
  executable (`0x12A6D7`) and is not on the rail path.
- **The bare `SIGNAL` and `COLLISION` strings** at `0x891B5E` and friends: these
  are `config.ini` warning-message toggles (`WARNING_MESSAGE_TRAIN_SIGNAL`,
  `WARNING_MESSAGE_TRAIN_COLLISION`), nothing to do with building rules. Worth
  recording so nobody chases them again.

## 7. The honest risk note

The three suppressed rules are the ones that mean "something is in the way", and
the game's simulation was not written on the assumption that they could be
ignored. Track through a building is track through a building; track laid over a
signal has a signal inside it. What this plugin cannot do is make the rest of the
game agree that this is fine.

Concretely: keep a backup save, turn one rule on at a time, and if something goes
strange the first diagnostic step is `enabled = 0` and a restart — if the strange
thing survives that, it was not this.
