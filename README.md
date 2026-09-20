# Anarchy / QOL 

Three mods for **Workers & Resources: Soviet Republic**, in one
[TesmioLoader](https://steamcommunity.com/sharedfiles/filedetails/?id=3620742954)
plugin.

| part | what it does | key |
|---|---|---|
| **anarchy** | every "you can't build that there" goes quiet — slope, angle, terrain, water, collisions, crossings, connection rules — and comes back on the same key | `Alt+F` |
| **skins** | recolour a building with the game's own spray-can and every one of that type you place afterwards is built wearing it | none needed |
| **better trackbuilders** | rail construction trains ignore signals and each other; one-way signals can be read as two-way for routing | on by default, `Ctrl+Alt+D` for the one-way switch |

The third part is still called `trackcrew` in the ini, in the source and in the
log — that is its section name and its key prefix, and renaming it would quietly
break every existing `sovietkit.ini`.

Built against **SOVIET64.exe v1.1.1.9** (PE `TimeDateStamp 0x6A3EB6AD`). On any
other build every part says so in the log, hooks nothing and patches nothing —
that is the message, not a crash.

---

## Installing

Steam cannot put a loader plugin where it has to go, so this is two copies by
hand.

1. Build `sovietkit.dll` (see below), or take it from the
   [Workshop item](https://steamcommunity.com/sharedfiles/filedetails/?id=3805103047).
2. Copy `sovietkit.dll` and `plugins/sovietkit.ini` into
   `...\SovietRepublic\tesmioloader\build\plugins\`.
3. Add `sovietkit=1` to `tesmioloader\build\tesmioloader.ini`.
4. Start the game through **`tesmiolauncher.exe`**. Steam's Play button starts
   it without the loader.

`build-sovietkit.bat` does steps 2 and 3 for you.

## Building

```
src\build-sovietkit.bat
```

It finds MSVC with `vswhere`, builds one translation unit with
`/LD /MT /O2 /EHsc`, copies the DLL into `tesmioloader\build\plugins\` and adds
the loader line.

**MSVC, not a cross-build**, because of `__try` / `__except`. Without
structured exception handling a bad read takes the game down instead of
switching one part off and writing down why.

## How it works, briefly

The interesting half is anarchy, and the whole of it is **one-byte edits to the
game's own instructions**, applied and reverted on a key.

Every build tool keeps a "can this be built here" flag. The drag-a-line tool
keeps it at `state+0x315` and a failed check is `mov byte ptr [state+0x315], 0`
— anarchy flips that final immediate to `1`. The building tool keeps it in
`dil` and clears it with `xor dil, dil` (`40 32 FF`); anarchy makes that
`cmp dil, dil` (`40 3A FF`), which sets exactly the same flags and leaves the
register alone.

Most refusals do not touch the flag at all. Of the 228 the drag tools can
raise, 165 only write a category and a message id and leave **one** instruction
— `exe+0x5116FB` — to turn that record into a refusal. Turning that `jl` into a
`jmp` is one byte and switches off all 165.

370 sites in total, in six families. Each one:

* changes **exactly one byte**, so no instruction boundary can move and no
  thread can read a half-written value;
* is compared against the whole recorded instruction **before** anything is
  written, so a game update makes this refuse rather than corrupt code;
* goes back to the byte it replaced when you press the key again.

`src/an_sites.h` is the table, with the disassembly and the reasoning for every
family. `docs/` has the reverse-engineering notes the table was generated from.

### What it cannot promise

A refusal usually means the simulation was not written for what you are about
to do. Track through a building has a building inside it. A sewage pipe that
runs uphill will be built and will not flow. Anarchy removes the *refusal*, not
the physics behind it.

*Outside map borders!* is the one check left on: it is the only refusal that
guards an index rather than taste. `allow_outside_map = 1` removes it and the
log says what you did, every time.

**Keep a backup save.**

## Reading the log

Everything goes to `tesmioloader\build\sovietkit.log`. Startup should end with

```
ready: anarchy on, skins on, trackcrew on
```

and above it, one block per part. The lines worth checking:

* `370 of 370 match this build; 0 do not` — anarchy's byte edits
* `hooked exe+0x6ac6e0` — trackcrew's one inline hook
* `one-way semaphores: all 4 router bytes match this build.`

`debug` and `trace` ship **off**. To diagnose something, set `[debug] trace = 2`
and `debug = 1` in the part's own section, reproduce it, then press `Ctrl+Alt+R`
— at trace 2 the report prints a histogram of the addresses each hook was
called *from*, which turns "it still refuses" into a function name.

## Layout

```
src/         one translation unit; the .inc files are included into namespaces
plugins/     sovietkit.ini, the whole configuration, commented line by line
docs/        the reverse-engineering notes behind every offset in src/*_sites.h
workshop/    what the Steam Workshop item contains
```

## Credits

TesmioLoader by **Tesmio**. This is a plugin and does not run without it.

## Licence

MIT — see `LICENSE`.
