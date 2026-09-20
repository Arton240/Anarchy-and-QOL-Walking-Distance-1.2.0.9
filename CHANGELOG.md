# sovietkit changelog

## 1.3 — anarchy 1.4, skins 0.6, trackcrew 1.0

The release pass: one reported bug traced to its cause, one feature dropped for
a reason, and an adversarial read of all five source files with the findings
fixed rather than noted.

### anarchy 1.4

**Fences.** A fence is poles and spans, so it is validated by the wire-and-pole
validator at `exe+0x51D160`, not by the line tool's own body. Two separate holes
met there:

* Layer 1 scoped itself to `0x4FCAF0..0x518A14` and the building tool. A
  collision question asked from inside the wire validator returned a return
  address outside both, so layer 1 told it the truth — every time. The pipe
  validator at `0x5223C0` had the same problem. Both are called from exactly one
  place, the line tool, so both are now in scope.
* Layer 2's generator looked for `mov byte ptr [reg+0x315], 0` and found all 82
  in the executable. The wire validator has **eight more** that write a register
  which happens to hold zero — a different instruction meaning the same thing.
  Seven now have a one-byte fix (four store→load, three branch→jump); the eighth
  is inside a loop with two entries, so neither edit is provable and it is
  documented and left alone.

124 sites now, 122 on by default. The wire validator's own copy of *Outside map
borders!* is the second risky row and is under `allow_outside_map` with the
building tool's.

**`Alt+F` is the whole switch.** Layer 1 used to stay on permanently, which
sounded conservative and was not: `0x4FCAF0` is rail *and* roads *and* pipes
*and* conveyors *and* fences, so "layer 1 only" meant collision, signal
proximity and overlap were off for everything you drag, for the session, with
the indicator dark and the log saying OFF. `layer1_follows_key = 0` restores the
old behaviour and says so every time you press the key.

**Fixed**

* `RestoreDistCalls` zeroed its record of which call sites were still redirected
  even for the ones it had failed to restore, so a site left pointing at plugin
  code could never be retried. That is a crash on unload, and it is the exact
  crash the code's own comment was written to prevent.
* `InScope` truncated a 64-bit pointer difference to 32 bits with no check that
  the caller was in the executable at all. A return address from another module
  could alias into a tool range by coincidence.
* The four tool-range overrides in `[sites]` were checked only for `hi > lo`, so
  `0..0xFFFFFFFF` was accepted and made layer 1 lie to the whole game. All four
  are now bounded by the image and by a plausible function size.
* `ToolLeaveAll` could drive the dispatcher flag to −1 by racing the
  `__finally`, leaving a simulation thread permanently "inside a build tool". It
  now only clears a flag the calling thread set.
* A fault partway through the byte loop left `g_anarchy` at 0 with N checks
  still disabled — the mod saying off while the game said on. The mode flag is
  now set before the writes going on and after them coming off.
* `ApplySites` bookkeeping: a site retired at runtime did not update the
  verified/unusable totals; the stranded count counted events rather than sites;
  `VirtualProtect` failures past the eighth vanished entirely; and a site
  contested by another mod was never re-read, so it stayed miscounted as ours
  for the session.
* `start_on = 1` with a hotkey that resolved to 0 wrote ~120 bytes into the game
  with no key able to put them back. It now refuses and says why.
* The on-screen indicator read a fixed address as a pointer with no
  readability check, protected only by the build stamp — which is overridable
  from the ini.

### trackcrew 1.0

**One-way semaphores, on `Ctrl+Alt+D`.** A track builder past wrong-way signals
is not waiting at a red light; it has no route at all, because the rail
pathfinder treats a one-way semaphore as a one-way edge. Four one-byte edits in
the router make every one-way signal read as a two-way one. Each is a
conditional jump turned into the unconditional jump to the same target, and each
target is the branch the game already takes for a two-way signal.

It is **not** scoped to track builders and cannot be: neither per-vehicle route
request stores the vehicle pointer into the search context, so by the time
execution is inside the edge-cost function there is nothing to test. Off by
default, its own key, and the log says so every time.

**Fixed**

* `Panic()` restored the four bytes in an unguarded loop inside a caller's
  single `__try`, so a fault on the first left the other three in the game with
  no frame tick to retry. One guard per site.
* The write path could leave the router three-of-four patched, which `ModStart`
  explicitly refuses to do at load. It is now all-or-nothing in both directions,
  with a rollback.
* A site could be retired *during a revert*, stranding a byte we owned with
  nothing willing to put it back, while the report said the switch was off.
* `ParseList` documented that it drops a truncated pattern and then added it. A
  cut `russian_service_` becomes `russian_`, which makes every Russian rail
  vehicle signal-blind.
* `learn_from_button` defaulted to 1 in code against 0 everywhere else —
  including the comment explaining why 1 is a trap.
* The feature-table scan retried up to 4096 records with a `VirtualQuery` each,
  every frame forever, if the keyword was never found. It now gives up once and
  says so.
* The classification cache went from 256 to 2048 slots: at 256, two busy trains
  in one bucket evicted each other on every call and both took the slow path —
  which takes the log lock on the simulation thread.
* `Report()` read the census and the learned list without the lock that the
  writer takes.

### skins 0.6

**Fixed**

* Three sites were registered as `required` and **never asked about**. The log
  printed "this site is required - the feature that uses it stays off" and skins
  installed and started writing bytes through those offsets anyway.
* `VerifiedBuilding` — "the guard in front of every write" — returned **true**
  when the building table was empty or full. Both are reachable: empty for the
  frames between a world opening and the reconcile finishing, full at ~45,000
  buildings. It now refuses, and the table is 18 bits.
* The skin byte's address was bounded only by a 200,000-entry sanity constant,
  which is 56 MB past the array. `ReadablePtr` before the write.
* Paste and step did not resync the change watcher, so with
  `spread_on_change = 1` pressing `[` once could repaint the republic.
* Undo matched buildings by address alone, so a demolished-and-rebuilt building
  at a recycled address got a skin from a building that no longer existed — into
  the save. It now also checks the instance index.
* `BuildingMgr` dereferenced two ini-supplied offsets unguarded; a bad one
  unwound the whole skins frame including the undo key.
* Eighteen of the twenty `[sites]` offsets were unbounded, including
  `b_instance`, which is the direct input to the byte that gets written.

### shared

* The frame hook went live before the parts were started — several frames, in
  which every `SiteOk()` answered no and a keypress could permanently disable
  anarchy for the session.
* The whole-kit panic put three parts in one `__try`, so a fault in the first
  skipped the other two and then logged that everything was back. Same shape in
  the per-part path. One guard each, flag cleared first.
* The module is now pinned the instant the IAT patch lands. Unloading after that
  is a crash, and `DllMain` cannot prevent it.
* With all three parts off in the ini, the frame hook is no longer installed at
  all. The two "nothing was hooked" paths now return non-zero, which is what the
  loader's contract asks for.
* `SkCfgI` accepts hex and rejects trailing garbage. `modifier = 0x4` used to
  parse as 0 — a legal value meaning "no modifier" — and silently turned every
  `Ctrl+Alt` binding into a bare letter.
* `BumpFrom` clamps every read of the histogram length, not just the dump's. Two
  threads could push it past the array and the loop bound then walked into the
  next counter.
* `debug` and `trace` ship **off**. Turn them on together to diagnose; at
  trace 2 the report prints which addresses each hook was called from.
