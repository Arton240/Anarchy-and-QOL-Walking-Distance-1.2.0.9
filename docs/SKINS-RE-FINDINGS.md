# Building skins in W&R:SR — reverse-engineering findings

**Target:** `SOVIET64.exe` v1.1.1.9, PE TimeDateStamp `0x6A3EB6AD`. Every RVA below is
worthless against another build — gate on the stamp the way `politburo` does and
put every number in `[sites]` so a game patch is an ini edit, not a rebuild.

Everything here was read **statically out of the executable**, not out of a running
game. No save was touched. The method is the one `tools/statparse.py` already uses:
disassemble `.text`, find the string, follow the code.

---

## 1. What a skin actually is

A skin is **an extra `MATERIAL` line in the building's `renderconfig.ini`**. The mesh is
shared; each skin is a different `.mtl` pointing at a different `.dds`. There is no
`$SKIN` token anywhere in the data — *a skin has no name and no id; it is its ordinal
position in the `MATERIAL` list*, and slot 0 is the default.

```
$TYPE_WORKSHOP
 MODEL 1.nmf
 MATERIAL 1.mtl          <- skin 0
 MATERIALEMISSIVE 1_e.mtl
 MATERIAL 1_1.mtl        <- skin 1
 MATERIALEMISSIVE 1_1e.mtl
 ...
```

Measured over all 530 shipped `renderconfig.ini`: `MATERIALEMISSIVE` is optional but,
when present, is exactly 1:1 with `MATERIAL` and interleaved. Base game ships **zero**
skinned buildings; `dlc3` 143 of 275, `cwc` 9 of 180. Maximum observed is 14 skins
(`dlc3/theatre`, `residential23/24/25`, `hotel2`, `technical_services_medium`). The
wiki's stated engine limit is 16 skins per object.

Workshop skins are a separate item type, `WORKSHOP_ITEMTYPE_BUILDINGSKIN`, declaring
`$TARGET_BUILDING_SKIN <building> <mtl> <emissive_mtl>`. They append to the same list.

The player-facing control is the spray-can on the building info window — GUI element id
`building_skin` (named by the devs in `scripts/SOVIETInstructions.txt`), tooltip btf id
**1982 "Change color/skin"**. It only cycles; there is no way to pick a skin directly,
no way to copy one, and the scripting API exposes none of the 72 `Building` fields for
it.

---

## 2. Where the skin lives at runtime — **this is the whole mod**

Found by locating the `building_skin` string at `0x8F83F0`, its single `lea` xref at
`0x6FB103`, and disassembling the icon-row function around it. The click handler is at
`0x6FB240`:

```asm
006fb240  cmp qword ptr [r12 + 0x318], 0      ; r12 = C3D_BUILDING*, +0x318 = type descriptor
006fb249  je   skip
006fb24f  movsxd rax, dword ptr [r12 + 0x5f4] ; <-- the building's RENDER INSTANCE INDEX
006fb257  imul rcx, rax, 0x118                ; stride 280
006fb25e  mov  rax, qword ptr [rbx + 0x2c0]   ; rbx = C3D_BUILDING::TYPE*, +0x2c0 = instance array
006fb265  inc  byte ptr [rcx + rax + 0x4e]    ; <-- THE SKIN INDEX. ONE BYTE.
006fb269  ...
006fb27f  movsx ecx, byte ptr [rcx + rax + 0x4e]
006fb284  cmp  ecx, dword ptr [rbx + 0x2a8]   ; +0x2a8 = number of skins
006fb28a  jge  wrap
006fb299  cmp  qword ptr [rbx + rcx*8 + 0xa8], 0   ; +0xa8 = C3D_MATERIAL* array
006fb2a2  je   wrap
006fb2bf  call [GetNumSubMaterials]           ; material must actually be loaded
006fb2c7  jne  keep
    wrap: mov byte ptr [rcx + rax + 0x4e], 0
006fb2ee  mov  dword ptr [rcx + 0x4a4], 0x23  ; on the building - UI/refresh marker
```

and the button is hidden entirely unless there is more than one skin:

```asm
006fb0a1  cmp dword ptr [rax + 0x2a8], 1
006fb0a8  jle hide
```

### The offset table

| Where | Offset | Meaning |
|---|---|---|
| `C3D_BUILDING` | `+0x318` | type descriptor pointer (already known, used by `reputation`) |
| `C3D_BUILDING` | `+0x5f4` | **int — render instance index** |
| `C3D_BUILDING` | `+0x604` | float — built progress, `>= 1.0` finished (already known) |
| `C3D_BUILDING` | `+0x4a4` | int — set to `0x23` by the skin button; treat as a refresh marker |
| type descriptor | `+0x410` | **int — render type id** (the argument to `GetType`) |
| `C3D_BUILDING::TYPE` | `+0x0a8` | `C3D_MATERIAL*[]` — one per skin |
| `C3D_BUILDING::TYPE` | `+0x2a8` | **int — skin count** |
| `C3D_BUILDING::TYPE` | `+0x2c0` | pointer to the instance array, **stride `0x118`** |
| instance record | `+0x4e` | **byte — the skin index** |
| globals | `exe+0x9941F0` | pointer; `[[0x9941F0] + 0xff8]` = the `C3D_BUILDING` manager |

### Getting from a building to its skin byte

```c
BYTE* td   = *(BYTE**)(b + 0x318);
int   tid  = *(int*)(td + 0x410);
TYPE* type = GetType(mgr, tid);                 // C3DDLL64 import, see below
BYTE* inst = *(BYTE**)((BYTE*)type + 0x2c0);
BYTE* skin = inst + (size_t)*(int*)(b + 0x5f4) * 0x118 + 0x4e;
```

`?GetType@C3D_BUILDING@@QEAAPEAUTYPE@1@H@Z` is an **import from `C3DDLL64.dll`**
(IAT slot `exe+0x86D058`, 47 call sites). So we do not hook it — we resolve the slot
with `FindIatSlot` and **call it ourselves**. Read-only, no patch, no chaining hazard.

### Every place the game touches that byte — there are only four

| RVA | what it is |
|---|---|
| `0x1EEA8D` | reads it into a stack local — the **save** path |
| `0x1FAD8D` | writes it from a loaded value, then clamps `>= count` back to 0 — the **load** path |
| `0x2DFE2A`, `0x2E0B95` | reads it and bounds-checks against `+0x2a8` — the two **render** paths |
| `0x6FB257…` | the spray-can button |

Two consequences that make this mod cheap and safe:

* **The skin is persisted in the save** (there is a load path that clamps it), so writing
  the byte is durable — we are not fighting the game.
* **The render path reads the byte every frame.** Changing a skin needs no refresh call,
  no texture load, no reallocation. It is a single byte store. Mass-applying to 10,000
  buildings is 10,000 byte stores — microseconds, not a stutter.

**Caveat worth stating plainly:** the skin lives on the *render instance*, not on the
building. `building+0x5f4` is the index into a per-type instance array. That index is
almost certainly reassigned when buildings are destroyed. We therefore never cache an
instance pointer — we re-derive it from the building every time, and we re-derive it
inside a `__try`.

---

## 3. The picker tool

* Tool id `pick_tool_from_map`, registered into the terraintools tab at `0x754FB`
  (`FindBuilding(g_ui,"pick_tool_from_map")`, `GroupInit 0x51B80`).
* The tool dispatch is a `strcmp` chain at `0x312000…0x314950` (`strcmp` = `0x84F520`).
  `pick_tool_from_map` → **handler `0x3CB0D0`**. (The same chain gives every other tool's
  handler; all `rail_*` tools share **`0x418F90`**, which matters for the track request.)
* `0x3CB0D0` raycasts via `?CollisionRay@C3D_BUILDING@@…` — **an import**, IAT slot
  `exe+0x86CFC0` — at `0x3CB1D7` and `0x3CB2E5`. Under the x64 ABI the two `C3DVECTOR3`
  by-value args are passed as pointers, so the signature on the wire is:

  ```
  bool CollisionRay(this, C3DVECTOR3* a, C3DVECTOR3* b,
                    int* outRenderTypeId,        // rcx-arg 4 = r9
                    int* outInstanceIndex,       // stack [rsp+0x20]
                    C3DVECTOR3* hit, C3DVECTOR3* normal, bool x5)
  ```

  Proof: immediately after the call the game walks the building vector comparing exactly
  those two outputs against `building+0x5f4` and `typedesc+0x410`:

  ```asm
  003cb24b  cmp dword ptr [rcx + 0x5f4], r9d
  003cb254  cmp dword ptr [rax + 0x410], r8d
  ```

* **This is the gift.** `outRenderTypeId` + `outInstanceIndex` are *precisely* the two
  numbers needed to reach the skin byte. We never have to find the building at all:

  ```c
  skinOfPickedBuilding = ((BYTE*)GetType(mgr, *outRenderTypeId))[0x2c0] deref
                         + (*outInstanceIndex) * 0x118 + 0x4e;
  ```

* Having found the building, the picker calls `0x40BD60(world, building, …)` and sets
  `world + 0xD428` = the build selection (a descriptor from `FindBuilding` `0x3AAA0`),
  plus a flag byte at `world + 0x11DB0`. `MIRRORZ_` handling is a `strstr` on the
  descriptor for mirrored variants.

So "the picker loses the skin" is not a bug we have to undo — the picker only ever sets a
*type*. Buildings are placed on skin 0 and the spray-can is the only way to change one.
What we add is the missing half: remember the skin at pick time, apply it at build time.

### How we hook it — no code patched anywhere

An **IAT patch** on `CollisionRay` (the loader's most-preferred mechanism, and what
`reputation` uses exclusively), plus **return-address discrimination**, which this
project already relies on:

```c
if ((BYTE*)_ReturnAddress() == g_exeBase + 0x3CB1DD ||
    (BYTE*)_ReturnAddress() == g_exeBase + 0x3CB2EB)   /* the picker, and only the picker */
```

27 other call sites pass straight through with one pointer compare. Nothing is spliced,
nothing is unloadable, and a game update makes the compare simply never match — the
feature switches itself off instead of crashing.

---

## 4. Selected building, for the copy/apply UI

`world + 0xCAC0` → a struct whose `[0]` is the `C3D_BUILDING*` the info window is showing
(`0x6FB0EB` compares it against the window's building; `0x6FB2E4` writes `+0x4a4` through
it). That is how the plugin knows which building "copy skin" means, without owning any
part of the game's panel.

---

## 5. The track-builder request — what is and is not known yet

Known: every `rail_*` tool routes to **`0x418F90`**. The collision primitives it will be
using are imports and therefore IAT-hookable with the same return-address trick:

| symbol | IAT slot | call sites |
|---|---|---|
| `?Collision2DBBox@C3D_BUILDING@@QEAA_NVC3DVECTOR3@@PEAH1HH_NM@Z` | `0x86CFE0` | 39 |
| `?Collision2DBBoxIgnore@C3D_BUILDING@@QEAA_NVC3DVECTOR3@@HH@Z` | `0x86CFD0` | 1 |
| `?CollisionRay@C3D_BUILDING@@…` | `0x86CFC0` | 27 |

**Not yet known:** which of those call sites sit inside `0x418F90`'s call graph, and where
the *signal* rule lives (signal spacing / "cannot place here" is a separate check from
box collision, and none of the `SIGNAL` strings in the binary belong to it — they are
`config.ini` warning-message toggles). That needs one more pass: disassemble `0x418F90`
and its callees and find the refusal branch. It is a different piece of work from the
skin mod and should not be bolted onto it.

**A warning to put on the record now:** "ignore collisions" on track is *not* symmetric
with the skin change. A skin byte is cosmetic and clamped by the game on load. Geometry
that the game's own placement rules rejected can produce track the pathfinder cannot
solve, trains that collide, or a save the game refuses to reload. Whatever we build there
gets a hard off-switch, a warning in the log, and a recommendation to keep a backup save.

---

## 6. Sources

Static analysis of `SOVIET64.exe` v1.1.1.9 and `C3DDLL64.dll` (capstone + the PE import
table), the shipped `media_soviet` data, plus:

- [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) — `docs/01-architecture.md`, `03-reverse-engineering.md`, `07-pitfalls.md`
- [General modding — official wiki](https://wiki.hoodedhorse.com/Workers_Resources_Soviet_Republic/General_modding)
- [How to make a skin (Steam guide)](https://steamcommunity.com/sharedfiles/filedetails/?id=1802735632)
- [Dev reply: skins are applied post-construction from the building window](https://steamcommunity.com/app/784150/discussions/0/3108017414036468463/)
- [Dev reply: deferred contexts abandoned; the bottleneck is CPU/simulation](https://steamcommunity.com/app/784150/discussions/9/580552228184598002/)
- This project's own `HANDOVER/01-MODDING-WORKERS-AND-RESOURCES.md`, `reputation-src/docs/ui-feasibility.md`, `politics/pol_*.h`
