// skins.cpp - building skins for Workers & Resources: Soviet Republic.
//
//   * CHANGE A SKIN WITH THE GAME'S OWN SPRAY-CAN and every other building of
//     that type follows, with one undo key if you did not mean it. No hotkey.
//   * the picker remembers the skin of the building you picked, and every
//     building you then place of that type is built wearing it
//   * copy a building's skin, paste it onto another
//   * step a selected building's skin forwards and backwards (the game's own
//     button only cycles forwards)
//   * apply one skin to every building of that type in the republic, with undo
//
// SHAPE OF THE THING, because it is what keeps it off the frame budget:
//
//   With nothing happening, one frame of this plugin is a foreground check that
//   is usually one compare, THREE GetAsyncKeyState calls for the shared Ctrl+Alt
//   modifier (not one per hotkey - that was measured and fixed), one pointer
//   read of the build selection, and two reads of the building vector. No walk,
//   no ReadablePtr, no allocation, no string work, no VirtualQuery. Everything
//   expensive is on an edge, and anything O(buildings) is chunked across frames.
//
//   Two IAT patches, no spliced code: C3D_TERRAIN::Render is the heartbeat, and
//   C3D_BUILDING::CollisionRay is how the picker tells us what it picked. Both
//   are restored by the loader on unload. GetType is resolved from its import
//   slot and CALLED, never hooked.
//
// Everything this plugin writes is ONE BYTE per building, clamped to the same
// range the game's own spray-can clamps to. See SKINS-RE-FINDINGS.md.

#define SKINS_VERSION "0.6"


// ---------------------------------------------------------------- settings
static int  g_enabled       = 1;
static int  g_retainOnPick  = 1;     // the headline feature
static int  g_autoApplyNew  = 1;     // paint buildings as they are placed
static int  g_onlyDefault   = 1;     // never repaint a building already off skin 0
static int  g_scanMs        = 250;   // 4 Hz, not per frame
static int  g_chunk         = 4096;  // buildings per frame while a job runs
static int  g_watchChanges  = 1;     // notice the player using the game's own button
static int  g_spread        = 0;     // ...and ALSO repaint the ones already standing.
                                     // Off: a skin change is a rule for what you build
                                     // NEXT, which is what it feels like it should mean.
static int  g_dryRun        = 0;     // detect and log, write nothing
static int  g_debug         = 0;     // counters only; [skins] debug = 1 to diagnose
static int  g_heartbeatSec  = 15;
// g_trace lives in tsm_diag.h, 0..3

static int  K_COPY, K_PASTE, K_APPLYALL, K_UNDO, K_NEXT, K_PREV, K_TOGGLE;
static int  K_SELFTEST, K_DUMP;

// ---------------------------------------------------------------- counters
static int C_PICKS, C_PICK_RAY, C_AUTO, C_MASS, C_MANUAL, C_FAULTS, C_SCANS;
static RateLimit g_rlPick = { 0, 5, 100 };

// ------------------------------------------------------------------- state
static int      g_off;               // set once if we refuse to run at all
static int      g_renderPatched;     // declared here so the report can name it
static unsigned g_worldGen = 1;
static int      g_inWorld;
static int      g_lastYear, g_lastDay;

// what the picker last saw. Written by the detour (possibly another thread),
// read by the render thread. A pick is only acted on if the detour fired in
// this frame or the last one, which is what tells a pick apart from the player
// choosing a building in the build menu.
// The pick is published as ONE 64-bit value - type in the high half, instance
// in the low half - because two separate ints can be read as a type from one
// pick and an index from the next, and the bounds checks downstream would
// happily resolve that into the wrong building's skin.
static volatile LONG64    g_pick;          // (typeId << 32) | (unsigned)instIdx
static volatile ULONGLONG g_pickFrame = ~0ull;   // ~0: "no pick yet", see below
static ULONGLONG          g_frameNo;

static void*    g_lastSel;           // the build menu's selected descriptor

// the clipboard, for copy/paste
static int      g_clipValid, g_clipTypeId, g_clipSkin;

// ---- remembered skins, keyed on render type id -----------------------------
#define WANT_SLOTS 1024              // power of two
#define WANT_MAX    512
struct Want { int typeId; int skin; int used; };
static Want g_want[WANT_SLOTS];
static int  g_wantN;

static Want* WantFind(int typeId, bool create)
{
    int slot = (int)(((unsigned)typeId * 2654435761u) & (WANT_SLOTS - 1));
    for (int probe = 0; probe < WANT_SLOTS; probe++)
    {
        Want* w = &g_want[slot];
        if (w->used && w->typeId == typeId) return w;
        if (!w->used)
        {
            if (!create || g_wantN >= WANT_MAX) return NULL;
            w->used = 1; w->typeId = typeId; w->skin = 0; g_wantN++;
            return w;
        }
        slot = (slot + 1) & (WANT_SLOTS - 1);
    }
    return NULL;
}

// ---- buildings we have already considered ----------------------------------
//
// Pointers only, never dereferenced - this is an identity set, so a stale
// address can cost us one unpainted building and can never fault. Dropped
// whole on every world change.
// 18 BITS, NOT 16. At 16 the table held ~45,000 buildings at a 70% load factor,
// while SKN_MAX_BUILDINGS - this plugin's own idea of a plausible republic - is
// 500,000. So the table filling was not an edge case, it was a Tuesday, and
// what happens when it fills is that the write guard below stops guarding.
// 18 bits is 2 MB of pointers and ~183,000 buildings.
#define SEEN_BITS  18
#define SEEN_SLOTS (1 << SEEN_BITS)
static BYTE* g_seen[SEEN_SLOTS];
static int   g_seenN;
static int   g_seenFull;

static void SeenClear(void) { memset(g_seen, 0, sizeof(g_seen)); g_seenN = 0; g_seenFull = 0; }

static bool SeenAdd(BYTE* p)          // true if it was NOT there before
{
    if (g_seenFull) return false;
    int slot = (int)PtrHash(p, SEEN_BITS);
    for (int probe = 0; probe < SEEN_SLOTS; probe++)
    {
        if (!g_seen[slot])
        {
            if (g_seenN * 10 >= SEEN_SLOTS * 7)
            {
                g_seenFull = 1;
                Dlog("  the building table filled at %d entries. TWO THINGS STOP WORKING:", g_seenN);
                Dlog("    auto-apply to newly placed buildings, and the discovery that");
                Dlog("    finds the selected building - so copy, paste and the self test");
                Dlog("    may report 'no building found'. Raise SEEN_BITS and rebuild.");
                return false;
            }
            g_seen[slot] = p; g_seenN++;
            return true;
        }
        if (g_seen[slot] == p) return false;
        slot = (slot + 1) & (SEEN_SLOTS - 1);
    }
    return false;
}

// Membership without insertion. The discovery scan uses it to ask "is this one
// of the game's buildings" without polluting the set it is searching.
static bool SeenHas(BYTE* p)
{
    if (!p) return false;
    int slot = (int)PtrHash(p, SEEN_BITS);
    for (int probe = 0; probe < SEEN_SLOTS; probe++)
    {
        if (!g_seen[slot]) return false;
        if (g_seen[slot] == p) return true;
        slot = (slot + 1) & (SEEN_SLOTS - 1);
    }
    return false;
}

// Is this pointer one the building vector actually held when we last looked?
//
// THIS IS THE GUARD IN FRONT OF EVERY WRITE. A building pointer can go stale two
// ways - the vector reallocates under a chunked job, or a building is demolished
// between a mass apply and the undo that reverses it - and a stale pointer whose
// +0x318 happens to hold something plausible would otherwise take a byte write
// into whatever now lives at that address. __try does not help: the memory is
// committed and readable, it is simply somebody else's.
//
// The seen set is an identity set of addresses the game itself handed us, so
// membership is the cheapest true statement available. When the table has
// filled or has not been built yet it cannot answer, and then we fall back to
// the structural check alone and say so once.
// WHEN IT CANNOT ANSWER IT SAYS NO.
//
// It used to say yes, which inverted the whole point of the function at the two
// moments it was most likely to be asked: the frames between a world opening and
// the chunked reconcile finishing (g_seenN == 0, and the hotkeys run before
// JobStep in Frame(), so they are live in that window), and every frame after
// the table filled. In both, "the guard in front of every write" returned true
// for any pointer at all - and the info-window discovery scan, which feeds 256
// raw qwords from the game's memory through this test before handing them to
// the engine's GetType, lost its first filter entirely.
//
// A refusing guard costs unpainted buildings and a "no building found" on a
// hotkey. A permitting one costs an arbitrary write. That is not a close call.
static bool VerifiedBuilding(BYTE* p)
{
    if (!p) return false;
    if (g_seenN && !g_seenFull) return SeenHas(p);
    DiagOnce(2, "the building table is empty or full, so nothing is being written"
                " this frame - the guard in front of every write cannot answer and"
                " refuses rather than guesses. If this repeats, raise SEEN_BITS.");
    return false;
}

// ---- undo for mass apply ---------------------------------------------------
#define UNDO_MAX 16384
// `inst` is the building's instance index as it was at apply time, and it is
// here because address identity alone cannot tell a survivor from a successor.
// The game frees a demolished C3D_BUILDING and its allocator can hand the same
// address to the next one; SeenHas still says yes, because nothing prunes the
// set. Without this, undo after a demolish-and-rebuild writes a skin harvested
// from a building that no longer exists into a building the player never
// touched - and that write goes into the save.
struct UndoRec { BYTE* bld; BYTE prev; int inst; };
static UndoRec g_undo[UNDO_MAX];
static int     g_undoN, g_undoOverflow;
static unsigned g_undoGen;

// ---- the chunked job -------------------------------------------------------
enum { JOB_NONE = 0, JOB_RECONCILE, JOB_APPLY };
static int      g_job;
static __int64  g_jobPos;
static int      g_jobTypeId, g_jobSkin;
static int      g_jobTouched, g_jobSeen, g_jobFaults;
// A reconcile after a DEMOLITION must still paint anything new it meets - the
// vector shifted, so we cannot tell new from moved without walking it, and the
// buildings placed while that walk is running would otherwise be recorded as
// seen and never painted. A reconcile on WORLD OPEN must paint nothing, or
// loading a save repaints the whole republic. Same walk, one flag.
static int      g_jobPaint;
static TdCache  g_tdc;               // reset at the head of every job

// ---- watching the game's own skin button -----------------------------------
//
// THE WHOLE POINT OF 0.4. There is already a perfectly good button for changing
// a skin - the spray-can in the building window - and it is right where your
// hand already is. So instead of asking you to learn a hotkey, the plugin
// watches the selected building: when its skin byte changes and we did not
// change it, that was you, and it is treated as an instruction.
//
// Checked on the same 4 Hz tick as everything else. A quarter of a second
// between clicking and the republic repainting is not a delay anybody notices,
// and it keeps an engine call off the per-frame path.
static BYTE*    g_watchBld;
static int      g_watchSkin = -1;
static unsigned g_watchGen;

// Re-baseline without comparing. Used after WE change a skin for a reason that
// must not spread - an undo, most obviously, which would otherwise take the
// value it just restored and push it back over everything.
static void ResyncWatch(void) { g_watchBld = NULL; g_watchSkin = -1; }

// ---- vector tracking -------------------------------------------------------
static BYTE**  g_lastBegin;
static __int64 g_lastN;
static ULONGLONG g_lastScanTick;

// =========================================================================
//  input
// =========================================================================

// The foreground check, the modifier sampling and the hotkey edge detector
// moved up to sovietkit.cpp: three plugins were asking Windows the same three
// questions every frame. 0.2 of this plugin asked them once PER BOUND KEY -
// eleven times a frame - which is how that comment got written in the first
// place. What is left here is which modifier the keys want.
static int g_modifier = 4;


// =========================================================================
//  the picker hook
// =========================================================================
typedef bool (*CollisionRayFn)(void* self, void* a, void* b,
                               int* outType, int* outInst,
                               void* hit, void* nrm,
                               bool b1, bool b2, bool b3, bool b4, bool b5);
static CollisionRayFn o_CollisionRay;
static int            g_rayPatched;

static bool h_CollisionRay(void* self, void* a, void* b,
                           int* outType, int* outInst,
                           void* hit, void* nrm,
                           bool b1, bool b2, bool b3, bool b4, bool b5)
{
    bool r = o_CollisionRay(self, a, b, outType, outInst, hit, nrm, b1, b2, b3, b4, b5);

    // 27 other call sites leave through here on one pointer compare. A game
    // update makes this simply never match, which switches the feature off
    // rather than breaking anything.
    if (r && g_retainOnPick && !g_off)
    {
        BYTE* ret = (BYTE*)_ReturnAddress();
        if (ret == g_exeBase + S.pickRetA || ret == g_exeBase + S.pickRetB)
        {
            if (outType && outInst)
            {
                LONG64 packed = 0;
                // These come out of the game's stack frame, so they get the same
                // guard as every other read of somebody else's memory.
                TSM_TRY { packed = ((LONG64)*outType << 32) | (unsigned)*outInst; }
                TSM_EXCEPT("skins read pick outputs") { return r; }

                InterlockedExchange64(&g_pick, packed);
                g_pickFrame = g_frameNo;

                if (g_debug)
                {
                    BumpFrom(C_PICK_RAY, ret);
                    if (g_trace >= 3 && RateSay(&g_rlPick))
                        TRACE(3, "  ray: hover type %d inst %d from exe+%#08x (#%lld)",
                              (int)(packed >> 32), (int)(unsigned)packed,
                              (unsigned)(ret - g_exeBase), (long long)g_rlPick.seen);
                }
            }
        }
    }
    return r;
}

// =========================================================================
//  world lifecycle
// =========================================================================
static void ForgetWorld(const char* why)
{
    g_worldGen++;
    SeenClear();
    g_job = JOB_NONE; g_jobPos = 0;
    g_undoN = 0; g_undoOverflow = 0;
    g_lastBegin = NULL; g_lastN = 0;
    g_lastSel = NULL;
    g_clipValid = 0;
    Dlog("world %s - caches dropped (gen %u)", why, g_worldGen);
}

// Returns false when there is no world to work on. Cheap: two reads.
static bool WorldCheck(BldVec* v)
{
    bool ok = ReadBldVec(v) && v->begin && v->n > 0;

    if (!ok)
    {
        if (g_inWorld) { g_inWorld = 0; ForgetWorld("closed"); }
        return false;
    }

    int year = 0, day = 0;
    TSM_TRY
    {
        year = *(int*)(World() + S.wYear);
        day  = *(int*)(World() + S.wDay);
    }
    TSM_EXCEPT("skins read date") { year = g_lastYear; day = g_lastDay; }

    if (!g_inWorld)
    {
        g_inWorld = 1;
        g_lastYear = year; g_lastDay = day;
        ForgetWorld("opened");
        // Everything standing when a world opens counts as already considered,
        // so opening a save does not repaint a republic behind the player's back.
        g_job = JOB_RECONCILE; g_jobPos = 0; g_jobTouched = 0; g_jobSeen = 0;
        g_jobFaults = 0; g_jobPaint = 0; TdReset(&g_tdc);
        return true;
    }

    // Loading save B from inside save A is invisible any other way.
    if (year < g_lastYear || (year == g_lastYear && day < g_lastDay - 1))
    {
        g_lastYear = year; g_lastDay = day;
        ForgetWorld("reloaded (date went backwards)");
        g_inWorld = 1;
        // the SAME reset as the open path: 0.2 inherited g_jobFaults here, so a
        // reconcile after a reload could abandon on its first element and log
        // "fault budget spent" having suffered no faults at all.
        g_job = JOB_RECONCILE; g_jobPos = 0; g_jobTouched = 0; g_jobSeen = 0;
        g_jobFaults = 0; g_jobPaint = 0; TdReset(&g_tdc);
        return true;
    }
    g_lastYear = year; g_lastDay = day;
    return true;
}

// =========================================================================
//  the work
// =========================================================================

// Consider one building: paint it if it is new, of a remembered type, and still
// wearing the default skin. Only the tail scan calls this - the reconcile path
// records without painting and does so inline, which is why there is no longer a
// "record only" parameter here.
static void ConsiderBuilding(void* mgr, BYTE* bld)
{
    if (!bld) return;
    unsigned long long a = (unsigned long long)bld;
    if (a < 0x10000ull || a > 0x7FFFFFFFFFFFull) return;

    bool isNew = SeenAdd(bld);
    if (!isNew || !g_autoApplyNew) return;

    BYTE* td = NULL;
    TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
    TSM_EXCEPT("skins read typedesc") { g_jobFaults++; return; }
    if (!td) return;

    int typeId = TdTypeId(&g_tdc, td);
    if (typeId < 0) return;

    Want* w = WantFind(typeId, false);
    if (!w || w->skin <= 0) return;

    SknRef r;
    if (!SknResolve(mgr, bld, td, &r)) return;
    if (g_onlyDefault && r.cur != 0) return;

    if (!g_dryRun && SknSet(&r, w->skin, bld))
    {
        g_jobTouched++; Bump(C_AUTO);
        TRACE(2, "  auto: new building of type %d painted skin %d", typeId, w->skin);
    }
    else if (g_dryRun)
    {
        g_jobTouched++; Bump(C_AUTO);
        TRACE(2, "  auto: WOULD paint a new type-%d building skin %d (DRY RUN)", typeId, w->skin);
    }
}

static void JobStep(void* mgr, BldVec* v)
{
    if (g_job == JOB_NONE) return;

    __int64 end = g_jobPos + g_chunk;
    if (end > v->n) end = v->n;

    for (__int64 i = g_jobPos; i < end; i++)
    {
        if (g_jobFaults > TSM_FAULT_BUDGET) { Dlog("  fault budget spent - job abandoned"); g_job = JOB_NONE; return; }

        BYTE* bld = NULL;
        TSM_TRY { bld = v->begin[i]; }
        TSM_EXCEPT("skins read vector slot") { g_jobFaults++; continue; }
        if (!bld) continue;

        if (g_job == JOB_RECONCILE)
        {
            if (g_jobPaint) ConsiderBuilding(mgr, bld);   // records, then paints if new
            else            SeenAdd(bld);                 // world open: record only
            g_jobSeen++;
            continue;
        }

        // JOB_APPLY - one type, one skin, everything standing.
        //
        // v->begin was read at the top of THIS FRAME and the simulation thread
        // can reallocate the vector underneath a 4096-entry chunk, so anything
        // we are about to write through is checked against the set of addresses
        // the game gave us, not merely against being readable.
        if (!VerifiedBuilding(bld)) continue;

        BYTE* td = NULL;
        TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
        TSM_EXCEPT("skins apply read typedesc") { g_jobFaults++; continue; }
        if (!td) continue;
        if (TdTypeId(&g_tdc, td) != g_jobTypeId) continue;

        SknRef r;
        if (!SknResolve(mgr, bld, td, &r)) continue;
        if (r.cur == g_jobSkin) continue;

        if (g_undoN < UNDO_MAX)
        {
            int inst = -1;
            TSM_TRY { inst = *(int*)(bld + S.bInstance); }
            TSM_EXCEPT("skins undo record instance") { inst = -1; }
            g_undo[g_undoN].bld = bld; g_undo[g_undoN].prev = (BYTE)r.cur;
            g_undo[g_undoN].inst = inst; g_undoN++;
        }
        else g_undoOverflow = 1;

        if (g_dryRun) { g_jobTouched++; continue; }
        if (SknSet(&r, g_jobSkin, bld)) { g_jobTouched++; Bump(C_MASS); }
    }

    g_jobPos = end;

    if (g_jobPos >= v->n)
    {
        if (g_job == JOB_APPLY)
        {
            Dlog("apply: type %d -> skin %d, %d building%s changed%s%s",
                 g_jobTypeId, g_jobSkin, g_jobTouched, g_jobTouched == 1 ? "" : "s",
                 g_dryRun ? " (DRY RUN, nothing written)" : "",
                 g_undoOverflow ? " - too many to undo" : "");
        }
        else
        {
            // Set the watermark HERE. 0.2 left it at NULL/0 from ForgetWorld, so
            // the next scan saw "the vector moved" and ran a second full
            // reconcile that could only record what the first already had.
            g_lastBegin = v->begin;
            g_lastN     = v->n;
            TRACE(1, "reconcile: %d buildings recorded", g_jobSeen);
        }

        g_job = JOB_NONE; g_jobPos = 0;
    }
}

// The 4 Hz new-building check. Three reads in the steady state.
static void ScanForNew(void* mgr, BldVec* v)
{
    if (v->begin == g_lastBegin && v->n == g_lastN) return;

    // A JOB IS RUNNING, SO DO NOT MOVE THE WATERMARK. 0.1 updated g_lastN here
    // regardless, which meant any building placed while a reconcile or a mass
    // apply was in flight was skipped forever: the tail it belonged to had
    // already been declared seen. Leaving the watermark alone makes the next
    // idle scan pick it up.
    if (g_job != JOB_NONE) return;

    Bump(C_SCANS);
    TRACE(2, "  scan: vector %s, %lld -> %lld buildings",
          v->begin == g_lastBegin ? "grew in place" : "moved", g_lastN, v->n);

    if (v->begin == g_lastBegin && v->n > g_lastN)
    {
        // The common case by far: the vector grew in place, so only the tail is
        // new. This is O(buildings placed), which is one or two.
        TdReset(&g_tdc);
        __int64 done = v->n;
        for (__int64 i = g_lastN; i < v->n; i++)
        {
            BYTE* bld = NULL;
            TSM_TRY { bld = v->begin[i]; }
            // Stop at the failure AND remember where: advancing the watermark to
            // v->n regardless would retire the rest of the tail unscanned.
            TSM_EXCEPT("skins tail read") { done = i; break; }
            ConsiderBuilding(mgr, bld);
        }
        g_lastBegin = v->begin;
        g_lastN     = done;
        return;
    }
    else
    {
        // Reallocated, or something was demolished and the vector shifted. We
        // cannot tell what moved, so re-record everything - chunked, and
        // painting nothing.
        g_job = JOB_RECONCILE; g_jobPos = 0; g_jobSeen = 0; g_jobFaults = 0;
        g_jobPaint = 1;                       // see the note on g_jobPaint
        TdReset(&g_tdc);
        TRACE(2, "vector moved (%lld -> %lld) - reconciling", g_lastN, v->n);
    }

    g_lastBegin = v->begin;
    g_lastN     = v->n;
}

// =========================================================================
//  the selected building, and the hotkeys that act on it
// =========================================================================
// How far into the info window to look for the building pointer, and where we
// found it last time. -1 means "not known yet".
#define WIN_SCAN_BYTES 0x800
static int g_bldFieldOff = -1;

// [[world + 0xCAC0]] - the info window itself. The game compares this against
// the window it is drawing at 0x6FB0F2, which is how we know what it is.
static BYTE* InfoWindow(void)
{
    BYTE* win = NULL;
    TSM_TRY
    {
        BYTE* outer = *(BYTE**)(World() + S.wInfoWin);
        if (outer) win = *(BYTE**)outer;
    }
    TSM_EXCEPT("skins read info window") { return NULL; }

    unsigned long long a = (unsigned long long)win;
    if (a < 0x10000ull || a > 0x7FFFFFFFFFFFull) return NULL;
    return win;
}

// Does this pointer behave like a building? Not "is it plausible" - does the
// entire chain we depend on resolve through it, which is a far stronger claim
// than a range check and is exactly what we are about to rely on anyway.
static bool LooksLikeBuilding(void* mgr, BYTE* p)
{
    unsigned long long a = (unsigned long long)p;
    if (a < 0x10000ull || a > 0x7FFFFFFFFFFFull) return false;

    SknRef r;
    return SknResolve(mgr, p, NULL, &r);
}

static BYTE* SelectedBuilding(void)
{
    BYTE* win = InfoWindow();
    if (!win) return NULL;

    void* mgr = BuildingMgr();

    // The cached offset, revalidated every time. It is one extra read and it
    // means a stale offset costs a rediscovery rather than a wrong answer.
    if (g_bldFieldOff >= 0)
    {
        BYTE* p = NULL;
        TSM_TRY { p = *(BYTE**)(win + g_bldFieldOff); }
        TSM_EXCEPT("skins read building field") { p = NULL; }
        // Identity first, structure second - the same order the discovery scan
        // uses. Without this, a pinned-but-wrong offset feeds an arbitrary qword
        // from the window straight into the engine's GetType.
        if (VerifiedBuilding(p) && LooksLikeBuilding(mgr, p)) return p;
        Dlog("selected-building field at +%#x stopped working - rediscovering", g_bldFieldOff);
        g_bldFieldOff = -1;
    }

    // ---- discovery -------------------------------------------------------
    //
    // 0.1 assumed the building was at window+0x00 and it is not: that slot
    // holds the window itself, and the self test duly reported a descriptor of
    // 0x0000000200000002, which is two little integers wearing a pointer's
    // clothes.
    //
    // Rather than swap one guess for another, look for it. Every qword in the
    // window is tested two ways, and a candidate has to pass both: it must be a
    // pointer the building vector actually contains, and the whole skin chain
    // must resolve through it. Nothing else in a window is going to do that by
    // accident, and when the game moves the field we find it again instead of
    // reporting a failure.
    for (unsigned off = 0; off + 8 <= WIN_SCAN_BYTES; off += 8)
    {
        BYTE* p = NULL;
        TSM_TRY { p = *(BYTE**)(win + off); }
        TSM_EXCEPT("skins scan window") { continue; }

        if (!VerifiedBuilding(p)) continue;        // not one of the game's buildings
        if (!LooksLikeBuilding(mgr, p)) continue;  // ...or the chain does not resolve

        g_bldFieldOff = (int)off;
        Dlog("selected-building field DISCOVERED at window+%#x", off);
        Dlog("  (put info_building = %#x in [sites] to pin it)", off);
        DiagFlush();
        return p;
    }

    DiagOnce(1, "no building pointer found anywhere in the first %#x bytes of the"
                " info window - is a building window actually open?", WIN_SCAN_BYTES);
    return NULL;
}

static void DescribeType(BYTE* td, char* out, size_t n)
{
    out[0] = 0;
    if (!td) return;
    SafeReadStr(td + S.tdObjName, out, n);
}

static void DoStep(void* mgr, int delta)
{
    BYTE* bld = SelectedBuilding();
    if (!bld) { Dlog("step: no building selected"); return; }

    SknRef r;
    if (!SknResolve(mgr, bld, NULL, &r)) { Dlog("step: could not read that building's skin"); return; }
    if (r.count < 2) { Dlog("step: that building has only one skin"); return; }

    int want = r.cur + delta;
    while (want < 0)        want += r.count;
    while (want >= r.count) want -= r.count;

    if (g_dryRun) { Dlog("step: would set skin %d/%d (DRY RUN)", want, r.count - 1); return; }
    // ResyncWatch for the same reason as paste: this is OUR change, and the
    // watcher must not take it for the player's and spread it.
    if (SknSet(&r, want, bld)) { ResyncWatch(); Bump(C_MANUAL); Dlog("step: skin %d of %d", r.cur, r.count); }
}

// =========================================================================
//  SELF-TEST - the single most useful diagnostic in the plugin
// =========================================================================
//
// "Is it working" has three separate answers and this tells them apart in one
// keypress: can we FIND the selected building, can we READ its skin, and does
// a WRITE actually stick. It writes a different skin, reads the byte back, and
// then puts the original value back whatever happened. If the read-back does
// not match, the write went somewhere that is not the byte the renderer reads,
// and that is a different bug from "the offsets are wrong".
static void DoSelfTest(void* mgr)
{
    Dlog("--- self test ---");
    Dlog("  manager %s   GetType %s   world exe+%#x",
         mgr ? "resolved" : "NULL", o_GetType ? "resolved" : "NULL", S.rvaWorld);

    BldVec v;
    if (ReadBldVec(&v)) Dlog("  building vector: %lld buildings at %p", v.n, (void*)v.begin);
    else                Dlog("  building vector: COULD NOT READ - no world loaded?");

    BYTE* bld = SelectedBuilding();
    if (!bld) { Dlog("  no building selected. Click a building first, then press this again."); DiagFlush(); return; }

    BYTE* td = NULL;
    TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
    TSM_EXCEPT("selftest typedesc") { td = NULL; }

    char name[128]; DescribeType(td, name, sizeof(name));
    Dlog("  selected building %p, descriptor %p, object name %s",
         (void*)bld, (void*)td, name[0] ? name : "(unreadable)");

    SknRef r;
    if (!SknResolve(mgr, bld, td, &r))
    {
        Dlog("  RESOLVE FAILED. The chain broke somewhere - dump the structures");
        Dlog("  with the dump key and compare against SKINS-RE-FINDINGS.md.");
        DiagFlush(); return;
    }

    Dlog("  resolved: type %d, skin %d of %d, byte at %p", r.typeId, r.cur, r.count, (void*)r.byte);

    if (r.count < 2)
    {
        Dlog("  this building has only one skin, so a write test would prove nothing.");
        Dlog("  Try it on a DLC3 building - most of the skinned ones are there.");
        DiagFlush(); return;
    }
    if (g_dryRun) { Dlog("  dry_run is on, so the write test is skipped."); DiagFlush(); return; }

    int original = r.cur;
    int probe    = (original + 1) % r.count;

    if (!SknSet(&r, probe, bld)) { Dlog("  WRITE REFUSED by our own bounds check - offsets suspect."); DiagFlush(); return; }

    int readBack = -1;
    TSM_TRY { readBack = *r.byte; }
    TSM_EXCEPT("selftest read back") { readBack = -2; }

    Dlog("  wrote %d, read back %d  ->  %s", probe, readBack,
         readBack == probe ? "PASS - the write path works end to end"
                           : "FAIL - the byte did not take the value we wrote");

    SknSet(&r, original, bld);
    int restored = -1;
    TSM_TRY { restored = *r.byte; }
    TSM_EXCEPT("selftest restore") { restored = -2; }
    Dlog("  restored to %d (was %d)  ->  %s", restored, original,
         restored == original ? "ok" : "THE ORIGINAL VALUE WAS NOT RESTORED");

    Dlog("  If this says PASS but skins still do not change on screen, the byte");
    Dlog("  is right and something else is overriding it - check that the");
    Dlog("  building really has more than one MATERIAL in its renderconfig.ini.");
    DiagFlush();
}

// A raw look at the three structures the plugin reads. This is what to paste
// into a bug report when the self test fails.
static void DoDump(void* mgr)
{
    BYTE* bld = SelectedBuilding();
    if (!bld) { Dlog("dump: no building selected"); DiagFlush(); return; }

    Dlog("--- structure dump ---");
    HexDump("C3D_BUILDING + 0x300", bld + 0x300, 0x40);   // covers +0x318
    HexDump("C3D_BUILDING + 0x5E0", bld + 0x5E0, 0x40);   // covers +0x5F4 and +0x604

    BYTE* td = NULL;
    TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
    TSM_EXCEPT("dump typedesc") { td = NULL; }
    if (td)
    {
        HexDump("descriptor + 0x000 (object name)", td, 0x40);
        HexDump("descriptor + 0x400 (render type id at +0x410)", td + 0x400, 0x20);
    }

    SknRef r;
    if (SknResolve(mgr, bld, td, &r))
    {
        Dlog("  skin byte %p = %d, of %d", (void*)r.byte, r.cur, r.count);
        HexDump("instance record", r.byte - S.instSkin, 0x60);
    }
    else Dlog("  could not resolve the skin - nothing further to dump");
    DiagFlush();
}

static void DoCopy(void* mgr)
{
    BYTE* bld = SelectedBuilding();
    if (!bld) { Dlog("copy: no building selected"); return; }

    BYTE* td = NULL;
    TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
    TSM_EXCEPT("skins copy typedesc") { return; }

    SknRef r;
    if (!SknResolve(mgr, bld, td, &r)) { Dlog("copy: could not read that building's skin"); return; }

    g_clipValid = 1; g_clipTypeId = r.typeId; g_clipSkin = r.cur;

    char name[128]; DescribeType(td, name, sizeof(name));
    Dlog("copied skin %d of %d from %s", r.cur, r.count, name[0] ? name : "(unnamed type)");

    // A copy is also an instruction about the future: remember it for this type
    // so anything placed from now on is built wearing it.
    Want* w = WantFind(r.typeId, true);
    if (w) w->skin = r.cur;
}

static void DoPaste(void* mgr)
{
    if (!g_clipValid) { Dlog("paste: nothing copied yet"); return; }

    BYTE* bld = SelectedBuilding();
    if (!bld) { Dlog("paste: no building selected"); return; }

    SknRef r;
    if (!SknResolve(mgr, bld, NULL, &r)) return;

    if (r.typeId != g_clipTypeId)
        Dlog("paste: different building type - skin %d clamped into 0..%d", g_clipSkin, r.count - 1);

    if (g_dryRun) { Dlog("paste: would set skin %d (DRY RUN)", g_clipSkin); return; }

    // The comparison is against the CLAMPED value, not the clipboard value.
    // SknSet clamps into 0..count-1 before its "already this skin" early-out, so
    // pasting skin 5 onto a three-skin type already wearing skin 2 used to
    // report "the write did not take - run the self test" about a no-op.
    int wanted = g_clipSkin;
    if (wanted < 0) wanted = 0;
    if (wanted >= r.count) wanted = r.count - 1;

    if (SknSet(&r, g_clipSkin, bld))
    {
        // RESYNC, or the watcher sees a change it did not make.
        // WatchSelected samples the selected building's skin four times a second
        // and treats any difference as "the player used the spray-can". Our own
        // paste is exactly such a difference, so without this it is reported back
        // as the player's, it overwrites the clipboard, and with
        // spread_on_change = 1 it starts a mass apply across every building of
        // that type. One keypress, whole republic. DoUndo has always done this;
        // paste and step never did.
        ResyncWatch();
        Bump(C_MANUAL);
        Dlog("paste: skin %d of %d", r.cur, r.count);
    }
    else if (r.cur == wanted) Dlog("paste: already wearing that skin");
    else Dlog("paste: the write did not take - run the self test");
}

static void DoApplyAll(void* mgr)
{
    if (!g_clipValid) { Dlog("apply all: copy a skin first"); return; }
    if (g_job != JOB_NONE) { Dlog("apply all: busy"); return; }

    g_job = JOB_APPLY; g_jobPos = 0; g_jobTypeId = g_clipTypeId; g_jobSkin = g_clipSkin;
    g_jobTouched = 0; g_jobFaults = 0; g_undoN = 0; g_undoOverflow = 0;
    // Stamped at the START, not on completion: a job abandoned on the fault
    // budget still leaves entries in the undo buffer, and 0.2 then told the
    // player they belonged to a world that was no longer open.
    g_undoGen = g_worldGen;
    TdReset(&g_tdc);
    Dlog("apply all: type %d -> skin %d, starting", g_jobTypeId, g_jobSkin);
    (void)mgr;
}

static void DoUndo(void* mgr)
{
    if (g_job != JOB_NONE) { Dlog("undo: busy"); return; }
    if (!g_undoN)        { Dlog("undo: nothing to undo"); return; }
    if (g_undoGen != g_worldGen)
    {
        // The pointers in the undo buffer belong to a world that has closed.
        Dlog("undo: those buildings belong to a world that is no longer open");
        g_undoN = 0;
        return;
    }

    if (g_dryRun) { Dlog("undo: dry_run is on - would restore %d buildings", g_undoN); return; }

    int done = 0, gone = 0;
    for (int i = 0; i < g_undoN; i++)
    {
        BYTE* bld = g_undo[i].bld;

        // A building demolished since the apply leaves a dangling pointer here.
        // The world-generation check above only catches a whole world going
        // away; this catches one building going away.
        if (!VerifiedBuilding(bld)) { gone++; continue; }

        // ...and this catches one building being REPLACED by another at the
        // same address, which the pointer alone cannot.
        int inst = -1;
        TSM_TRY { inst = *(int*)(bld + S.bInstance); }
        TSM_EXCEPT("skins undo read instance") { inst = -1; }
        if (inst != g_undo[i].inst) { gone++; continue; }

        SknRef r;
        if (!SknResolve(mgr, bld, NULL, &r)) continue;
        if (SknSet(&r, g_undo[i].prev, bld)) done++;
    }
    Dlog("undo: %d building%s restored%s", done, done == 1 ? "" : "s",
         gone ? " (some no longer exist and were skipped)" : "");
    g_undoN = 0;
    ResyncWatch();     // or the restored value looks like a fresh instruction
}

// The kit's shared report key lands here. DoReport wants the building manager,
// which is one resolve away and cheap to do once on a keypress.
static void DoReport(void* mgr);

// The kit's shared report key lands here, and it can be pressed at the main
// menu, on a loading screen or while a world is being torn down. In the old
// plugin DoReport was only ever reached from inside the frame body, behind a
// world check and a null-manager check; BuildingMgr dereferences a global slot
// at +0xFF8 with no guard of its own, so reaching it from a keypress is a new
// way to fault. Guarded here rather than in BuildingMgr, because every other
// caller already knows the world is open.
static void Report(void)
{
    void* mgr = NULL;
    TSM_TRY { mgr = BuildingMgr(); }
    TSM_EXCEPT("skins report manager") { mgr = NULL; }
    DoReport(mgr);
}

// The kit's shared fault path calls this. The picker hook is an import swap and
// stays live; g_off is what stops it doing anything.
static void Panic(void) { g_off = 1; }

static void DoReport(void* mgr)
{
    BldVec v;
    ReadBldVec(&v);
    Dlog("--- skins %s report ---", SKINS_VERSION);
    Dlog("  world gen %u, in world %d, buildings %lld, recorded %d%s",
         g_worldGen, g_inWorld, v.n, g_seenN, g_seenFull ? " (FULL)" : "");
    Dlog("  retain on pick %d, auto-apply %d, only-default %d, dry run %d",
         g_retainOnPick, g_autoApplyNew, g_onlyDefault, g_dryRun);
    Dlog("  watch the game's skin button %d, repaint ones already built %d",
         g_watchChanges, g_spread);
    Dlog("  remembered types: %d", g_wantN);
    for (int i = 0; i < WANT_SLOTS; i++)
        if (g_want[i].used) Dlog("     type %-6d -> skin %d", g_want[i].typeId, g_want[i].skin);
    if (g_clipValid) Dlog("  clipboard: type %d skin %d", g_clipTypeId, g_clipSkin);

    BYTE* bld = SelectedBuilding();
    if (bld)
    {
        BYTE* td = NULL;
        TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); } TSM_EXCEPT("skins report") { td = NULL; }
        char name[128]; DescribeType(td, name, sizeof(name));
        SknRef r;
        if (SknResolve(mgr, bld, td, &r))
            Dlog("  selected: %s  type %d  skin %d of %d", name[0] ? name : "?", r.typeId, r.cur, r.count);
        else
            Dlog("  selected: %s  - SKIN COULD NOT BE RESOLVED", name[0] ? name : "?");
    }
    else Dlog("  no building selected");

    Dlog("  hooks: frame %d, picker %d, GetType %s",
         g_renderPatched, g_rayPatched, o_GetType ? "resolved" : "MISSING");

    if (g_debug)
    {
        Dlog("  faults in the current/last job: %d   undo entries: %d%s",
             g_jobFaults, g_undoN, g_undoOverflow ? " (overflowed)" : "");
        Dlog("  how to read this:");
        Dlog("    'picker raycasts' climbing while the pick tool hovers a building");
        Dlog("    means the hook is live. If it is 0, the IAT patch did not take or");
        Dlog("    the two return addresses moved - the sites above say which.");
        Dlog("    'skins remembered' only climbs when the SELECTION changes, which");
        Dlog("    is the moment you actually click with the picker.");
    }
    DiagFlush();
}

// Did the player just change this building's skin by hand?
static void WatchSelected(void* mgr)
{
    if (!g_watchChanges) return;

    BYTE* bld = SelectedBuilding();
    if (!bld) { ResyncWatch(); return; }

    SknRef r;
    if (!SknResolve(mgr, bld, NULL, &r)) { ResyncWatch(); return; }

    bool sameBuilding = (bld == g_watchBld) && (g_watchGen == g_worldGen) && (g_watchSkin >= 0);

    if (sameBuilding && r.cur != g_watchSkin)
    {
        BYTE* td = NULL;
        TSM_TRY { td = *(BYTE**)(bld + S.bTypeDesc); }
        TSM_EXCEPT("skins watch typedesc") { td = NULL; }
        char name[128]; DescribeType(td, name, sizeof(name));

        Dlog("you set %s to skin %d of %d", name[0] ? name : "that building", r.cur, r.count);
        Bump(C_MANUAL);

        Want* w = WantFind(r.typeId, true);
        if (w) w->skin = r.cur;
        g_clipValid = 1; g_clipTypeId = r.typeId; g_clipSkin = r.cur;

        // THE DEFAULT IS TO REMEMBER, NOT TO REPAINT. Recolouring one building
        // says what the NEXT one of that type should look like; the ones already
        // standing are left exactly as you built them. `auto_apply_new` above is
        // what does the remembering, and it is on.
        if (!g_spread)
            Dlog("  every %s you place from now on will be built wearing skin %d",
                 name[0] ? name : "building of that type", r.cur);
        else if (!g_dryRun && g_job == JOB_NONE)
        {
            g_job = JOB_APPLY; g_jobPos = 0;
            g_jobTypeId = r.typeId; g_jobSkin = r.cur;
            g_jobTouched = 0; g_jobFaults = 0;
            g_undoN = 0; g_undoOverflow = 0; g_undoGen = g_worldGen;
            TdReset(&g_tdc);
            Dlog("  spread_on_change is on, so repainting the ones already standing"
                 " too - undo with the undo key");
        }
        else if (g_dryRun)
            Dlog("  would spread it to every building of that type (DRY RUN)");
    }

    g_watchBld  = bld;
    g_watchSkin = r.cur;
    g_watchGen  = g_worldGen;
}

// =========================================================================
//  the heartbeat
// =========================================================================
static void Frame(void)
{
    if (g_off || !g_enabled) return;

    g_frameNo++;

    BldVec v;
    if (!WorldCheck(&v)) return;

    void* mgr = BuildingMgr();
    if (!mgr) { DiagOnce(0, "building manager not resolved - nothing will be applied"); return; }

    // ---- the picker ----
    if (g_retainOnPick)
    {
        void* sel = NULL;
        TSM_TRY { sel = *(void**)(World() + S.wBuildSel); }
        TSM_EXCEPT("skins read build selection") { sel = g_lastSel; }

        if (sel != g_lastSel)
        {
            g_lastSel = sel;

            // A selection change while the picker was hovering a building IS a
            // pick: the hover is what set the stamp, and only the picker's two
            // return addresses can set it. Choosing a type from the build menu
            // changes the selection without ever touching CollisionRay, so it
            // falls through here untouched.
            // g_pickFrame starts at ~0, so an unpicked first frame cannot pass
            // this - 0.2 did, and treated type 0 / instance 0 as a real pick.
            bool fresh = (g_pickFrame != ~0ull) && (g_frameNo - g_pickFrame) <= 1;
            if (sel && fresh)
            {
                LONG64  packed  = g_pick;
                int     pickType = (int)(packed >> 32);
                int     pickInst = (int)(unsigned)packed;
                SknRef src;
                if (SknResolveByType(mgr, pickType, pickInst, &src))
                {
                    // 0.1 read the render type id out of the build selection
                    // here, and it was wrong. `world+0xD428` holds an entry of
                    // the build-menu REGISTRY (stride 0x2D0, 720 bytes), which
                    // is a different structure from the type descriptor a
                    // building points at - so +0x410 read 0x150 bytes past the
                    // end of it, into the next entry. The log said
                    // "remembered for type 2 (mirrored variant)" and type 2 was
                    // two bytes of somebody else's record.
                    //
                    // The picked building's own render type id is the thing we
                    // actually want anyway: "build more of what I just clicked".
                    int destType = pickType;

                    Want* w = WantFind(destType, true);
                    if (w && w->skin != src.cur)
                    {
                        w->skin = src.cur;
                        Bump(C_PICKS);
                        Dlog("picked: skin %d of %d remembered for type %d",
                             src.cur, src.count, destType);
                    }
                    else
                        TRACE(2, "  pick: type %d already remembered as skin %d", destType, src.cur);
                    g_clipValid = 1; g_clipTypeId = destType; g_clipSkin = src.cur;
                }
                else
                {
                    TRACE(2, "  pick: selection changed but type %d inst %d could not be"
                             " resolved - GetType %s, manager %s",
                          pickType, pickInst,
                          o_GetType ? "ok" : "MISSING", mgr ? "ok" : "MISSING");
                }
            }
        }
    }

    // ---- hotkeys ----
    {
        static unsigned char wCopy, wPaste, wAll, wUndo, wNext, wPrev, wTog;
        if (SkHotkeyEdge(K_COPY,     g_modifier, &wCopy))  DoCopy(mgr);
        if (SkHotkeyEdge(K_PASTE,    g_modifier, &wPaste)) DoPaste(mgr);
        if (SkHotkeyEdge(K_APPLYALL, g_modifier, &wAll))   DoApplyAll(mgr);
        if (SkHotkeyEdge(K_UNDO,     g_modifier, &wUndo))  DoUndo(mgr);
        if (SkHotkeyEdge(K_NEXT,     g_modifier, &wNext))  DoStep(mgr, +1);
        if (SkHotkeyEdge(K_PREV,     g_modifier, &wPrev))  DoStep(mgr, -1);
        if (SkHotkeyEdge(K_TOGGLE,   g_modifier, &wTog))
        {
            g_retainOnPick = !g_retainOnPick;
            Dlog("retain-on-pick %s", g_retainOnPick ? "ON" : "OFF");
            DiagFlush();
        }

        static unsigned char wSelf, wDump;
        if (SkHotkeyEdge(K_SELFTEST, g_modifier, &wSelf)) DoSelfTest(mgr);
        if (SkHotkeyEdge(K_DUMP,     g_modifier, &wDump)) DoDump(mgr);
    }

    if (g_debug)
    {
        
        HEARTBEAT_F(g_heartbeatSec,
                  "[skins] world %d, buildings %lld, raycasts %lld, remembered %lld,"
                  " auto %lld, mass %lld",
                  g_inWorld, v.n,
                  (long long)g_ctr[C_PICK_RAY].n, (long long)g_ctr[C_PICKS].n,
                  (long long)g_ctr[C_AUTO].n, (long long)g_ctr[C_MASS].n);
    }

    // ---- the chunked job, then the 4 Hz scan ----
    if (g_job != JOB_NONE) { JobStep(mgr, &v); return; }

    ULONGLONG now = GetTickCount64();
    if (now - g_lastScanTick < (ULONGLONG)g_scanMs) return;
    g_lastScanTick = now;

    WatchSelected(mgr);
    if (g_job != JOB_NONE) return;     // the watch may have just started one

    ScanForNew(mgr, &v);
}

static void LoadConfig(void)
{
    LoadSites();

    g_enabled      = CfgB("skins", "enabled",          true);
    g_retainOnPick = CfgB("skins", "retain_on_pick",   true);
    g_autoApplyNew = CfgB("skins", "auto_apply_new",   true);
    g_onlyDefault  = CfgB("skins", "only_default_skin",true);
    g_watchChanges = CfgB("skins", "watch_changes",    true);
    g_spread       = CfgB("skins", "spread_on_change", false);
    g_dryRun       = CfgB("skins", "dry_run",          false);
    g_scanMs       = CfgI("skins", "scan_ms",   250,  50, 5000);
    g_chunk        = CfgI("skins", "chunk",     4096, 128, 65536);

    // debug ships ON for now. It costs a counter increment per event; set both
    // of these to 0 before publishing.
    // -1 (the default) means "discover it and tell me what you found".
    //
    // CfgU, NOT CfgI. This lives in [sites], the plugin prints the value to
    // paste in as hex, and CfgI parses decimal only - so "0x1a8" would come back
    // as 0 with no complaint, which is precisely the offset 0.1 got wrong.
    {
        unsigned __int64 raw = CfgU("sites", "info_building", (unsigned __int64)-1);
        if (raw == (unsigned __int64)-1) g_bldFieldOff = -1;
        else if (raw % 8 || raw + 8 > WIN_SCAN_BYTES)
        {
            Dlog("[sites] info_building = %#llx is not an 8-aligned offset inside"
                 " the first %#x bytes - discovering instead", raw, WIN_SCAN_BYTES);
            g_bldFieldOff = -1;
        }
        else g_bldFieldOff = (int)raw;
    }

    g_debug        = CfgI("skins", "debug",             0, 0, 1);
    // trace is the kit's, read once in sovietkit.cpp.
    g_heartbeatSec = CfgI("skins", "heartbeat_seconds", 15, 0, 3600);

    g_modifier = CfgI("keys", "modifier", 4, 0, 6);
    K_COPY     = CfgKey("skin_copy",      'C');
    K_PASTE    = CfgKey("skin_paste",     'V');
    K_APPLYALL = CfgKey("skin_apply_all", 'A');
    K_UNDO     = CfgKey("skin_undo",      'Z');
    K_NEXT     = CfgKey("skin_next",      VK_OEM_6);   // ]
    K_PREV     = CfgKey("skin_prev",      VK_OEM_4);   // [
    K_TOGGLE   = CfgKey("skin_retain",    'P');
    K_SELFTEST = CfgKey("skin_selftest",  'S');
    K_DUMP     = CfgKey("skin_dump",      'H');
}

static bool ModInit(void)
{
    LoadConfig();
    Dlog("--- skins %s: the picker and the spray-can remember ---", SKINS_VERSION);

    C_PICK_RAY = CounterNew("skins: picker raycasts seen");
    C_PICKS    = CounterNew("skins: remembered from a pick");
    C_AUTO     = CounterNew("skins: new buildings painted");
    C_MASS     = CounterNew("skins: mass-applied");
    C_MANUAL   = CounterNew("skins: stepped by hand");
    C_SCANS    = CounterNew("skins: building-vector changes");
    C_FAULTS   = CounterNew("skins: guarded faults (this job)");

    // These prove the OFFSETS, not a patch: they are the game's own skin-cycle
    // instructions, which encode +0x5F4, 0x118, +0x2C0, +0x4E, +0x2A8, +0x318
    // and +0x410. If they match, our notes match the binary.
    SiteAdd("skin cycle core",   RVA_VERIFY_CYCLE,  EXPECT_CYCLE,  VERIFY_CYCLE_LEN, 1);
    SiteAdd("skin count compare",RVA_VERIFY_COUNT,  EXPECT_COUNT,  VERIFY_COUNT_LEN, 1);
    SiteAdd("typedesc/rendertype",RVA_VERIFY_TYPE,  EXPECT_TYPE,   VERIFY_TYPE_LEN,  1);
    SiteAdd("picker call A",     RVA_VERIFY_PICK_A, EXPECT_PICK_A, VERIFY_PICK_LEN,  0);
    SiteAdd("picker call B",     RVA_VERIFY_PICK_B, EXPECT_PICK_B, VERIFY_PICK_LEN,  0);

    unsigned got = 0;
    if (!BuildStampOk(&got))
    {
        g_off = 1;
        Dlog("build 0x%08X, offsets came from 0x%08X - the game updated.", got, S.buildStamp);
        Dlog("Nothing runs until [sites] is checked. This is the message, not a crash.");
        SiteVerifyAll();          // still worth printing exactly what moved
        DiagFlush();
        return false;
    }

    Dlog("  build 0x%08X - the one every offset was read off", got);
    return true;
}

static bool ModStart(void)
{
    if (g_off)      { Dlog("skins: disabled - no hooks installed"); DiagFlush(); return false; }
    if (!g_enabled) { Dlog("skins: enabled = 0 - no hooks installed"); DiagFlush(); return false; }

    // THE THREE VERIFICATION SITES ARE NOW ASKED, which they were not.
    //
    // ModInit registers them with required = 1 and skn_sites.h explains exactly
    // what they buy: "If these three sites match, then +0x5F4, 0x118, +0x4E,
    // +0x2C0, +0x2A8, +0x318 and +0x410 are all confirmed by the binary
    // itself." Nothing then consulted that confirmation. SiteVerifyAll printed
    // three FAIL lines, each ending "this site is required - the feature that
    // uses it stays off", and skins installed the picker and started writing
    // bytes through those offsets on the next frame anyway.
    //
    // It matters because build_stamp is overridable from the ini, so the
    // plausible route is: game updates, player sets the new stamp and guesses
    // the offsets from a forum post, and the only thing left standing between
    // that and a one-byte write into an arbitrary object was a log line.
    if (!SiteOk("skin cycle core") || !SiteOk("skin count compare")
                                   || !SiteOk("typedesc/rendertype"))
    {
        g_off = 1;
        Dlog("skins: the three instructions that encode this plugin's offsets do"
             " not match this build. NOTHING IS HOOKED AND NOTHING IS WRITTEN -");
        Dlog("  every skin read and every skin write goes through those offsets,");
        Dlog("  and a wrong one is a byte written into some other object. If you");
        Dlog("  know the new addresses, put them in [sites] and try again.");
        DiagFlush();
        return false;
    }

    // The frame hook is sovietkit's now and is already in by the time this runs.
    g_renderPatched = ::g_renderHooked;

    // GetType is RESOLVED AND CALLED, never hooked - it is how we read a type's
    // skin list, and hooking a function we only want to call would be silly.
    {
        void** slot = FindIatSlot(g_exe, DLL_ENGINE, SYM_GET_TYPE);
        if (slot && *slot) o_GetType = (GetTypeFn)*slot;
        else
        {
            o_GetType = (GetTypeFn)GetProcAddress(g_engine, SYM_GET_TYPE);
            if (!o_GetType) Dlog("C3D_BUILDING::GetType not found - skins cannot be read");
        }
    }

    if (g_retainOnPick && !g_rayPatched)
    {
        if (PatchIat(g_exe, DLL_ENGINE, SYM_COLLISION_RAY,
                     (void*)h_CollisionRay, (void**)&o_CollisionRay, "skins picker"))
            g_rayPatched = 1;
        else
        {
            g_retainOnPick = 0;
            Dlog("could not patch CollisionRay - the picker will not remember skins;"
                 " everything else still works");
        }
    }

    // The shared site table holds anarchy's and trackcrew's rows too now, so
    // its total is not ours to report on. The kit verifies once, before any
    // part installs, and names every row.

    Dlog("hooks: frame %d, picker %d, GetType %s",
         g_renderPatched, g_rayPatched, o_GetType ? "resolved" : "MISSING");
    Dlog("  keys (%s): copy %#x, paste %#x, apply-all %#x, undo %#x, next %#x,"
         " prev %#x, retain-on-pick %#x, self test %#x, hex dump %#x",
         SkModifierName(g_modifier), K_COPY, K_PASTE, K_APPLYALL, K_UNDO,
         K_NEXT, K_PREV, K_TOGGLE, K_SELFTEST, K_DUMP);
    Dlog("  you should not need any of them: recolour a building with the game's");
    Dlog("  own spray-can and every one of that type you place AFTERWARDS is built");
    Dlog("  wearing it. The ones already standing are left as you built them%s",
         g_spread ? " - except spread_on_change is on, so they get repainted too." : ".");
    DiagFlush();
    return true;
}

