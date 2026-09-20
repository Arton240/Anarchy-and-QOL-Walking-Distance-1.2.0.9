// trackcrew.cpp - rail construction trains stop waiting.
//
// The complaint: laying rail on realistic is miserable because the track
// builders queue at every signal and behind each other, and a job that is two
// minutes of work takes twenty of watching trains stand still.
//
// What this does: for track-building trains only, the game's own "may I
// proceed?" predicate always answers yes. They run red signals and they do not
// brake for each other. That is not a new behaviour - it is exactly what the
// game does when a player clicks "Ignore red signal" on a train, reached
// through the same function at the same moment.
//
// Every other train on the map is untouched, and the report proves it: each
// vehicle name the hook saw, how often, and whether we classified it as a
// track builder.
//
// WHAT IT CANNOT DO. A track builder that ignores a signal is still a solid
// object to the rest of the simulation. Two of them meeting head-on on a single
// track will still be two trains on one track. This removes the waiting, not
// the geometry.
//
// AND THE SECOND HALF, new in 0.9: ONE-WAY SEMAPHORES.
//
// The hook above answers "may I proceed?". A track builder that has laid track
// out past a set of one-way signals facing the wrong way never gets that far -
// it has no route home at all, because the rail pathfinder treats a one-way
// semaphore as a one-way edge and refuses to search through it. Nobody ever
// asks whether the train may proceed, so nothing we answer can help.
//
// So there is a second, separate switch that patches four bytes in the router
// - two in the A* edge cost, two in the path builder - so that every one-way
// semaphore is read as a TWO-WAY semaphore. Each edit is one byte, each turns a
// conditional jump into the unconditional jump to the same target, and each
// target is the branch the game itself takes for a two-way signal. See
// tc_sites.h for the disassembly and for why the direction values are known
// rather than guessed.
//
// It is OFF by default and it is NOT scoped to track builders - the router's
// search context is a stack local with no vehicle in it. While it is on, any
// train may plan a route the wrong way down a one-way section. Turn it on, let
// the builder come home, turn it off.

#define TRACKCREW_VERSION "1.0"


// ---------------------------------------------------------------- settings
// volatile: toggled by a hotkey on the render thread, read by the detour on the
// simulation thread. Benign on x64 in practice, but "in practice" is not a
// reason to let the compiler hoist it out of the hot path.
static volatile int g_enabled = 1;
static volatile int g_off;
static int  g_learn     = 0;      // OFF by default - see the comment on Learn()
static int  g_relearnMs = 5000;   // how often the classification cache is dropped
static int  g_debug     = 0;      // counters only; [trackcrew] debug = 1 to diagnose
static int  g_heartbeat = 15;

// Substrings, matched case-insensitively against the vehicle's object folder
// name. Defaults cover the vanilla track-laying stock; the census in the report
// is how you find the right string for anything else in one run.
#define MATCH_MAX 48
#define MATCH_LEN 48
static char g_match[MATCH_MAX][MATCH_LEN];
static char g_exclude[MATCH_MAX][MATCH_LEN];
static int  g_nExclude;
static int  g_nMatch;

static int K_TOGGLE;
static int K_ONEWAY;

// ---- the one-way switch ----------------------------------------------------
static int  g_oneWayWanted;          // the ini asked for it at load
static volatile int g_oneWayOn;      // the four bytes are in the game right now

// ---------------------------------------------------------------- counters
static int C_ASK, C_LET, C_DENY, C_UNKNOWN;
static NameTally g_tally;
static RateLimit g_rlLet = { 0, 5, 500 };

// =========================================================================
//  finding the vehicle's name
// =========================================================================
//
// The offset came from another mod's notes rather than from our own
// disassembly, so it is treated as a starting guess. On the first vehicle we
// see, we try the hint and then scan for a field that holds a plausible object
// name; whatever works is cached and logged. If nothing works the plugin says
// so in one line instead of silently classifying everything as "not a track
// builder".
// 0.2 SEARCHED FOR THE WRONG SHAPE. It only ever looked for an inline char
// array at v+offset, found nothing in the first 0x400 bytes of 32 different
// vehicles, and concluded the offset was wrong. It might equally be that the
// name is a POINTER to a string, or that it does not live on the vehicle at all
// but on its type object - `v+0x1708` is a type, because the vehicle window
// reads a kind enum out of it at +0x294.
//
// So 0.3 stops guessing a layout and tries all four, and - more useful than any
// of them - LOGS EVERY CANDIDATE IT FINDS with its offset and value on the first
// few vehicles. Whatever the names turn out to look like, one run now produces
// the answer instead of another round of disassembly.
enum { NM_NONE = 0, NM_DIRECT, NM_PTR, NM_TYPE_DIRECT, NM_TYPE_PTR };
static int g_nameMode = NM_NONE;
static int g_nameOff  = -1;
static int g_nameTries;
#define NAME_TRIES_MAX  6      // full discovery is expensive; a handful is plenty
#define NAME_REPORT_MAX 16     // candidates logged per discovery pass

static const char* ModeName(int m)
{
    switch (m)
    {
        case NM_DIRECT:      return "vehicle + off";
        case NM_PTR:         return "*(vehicle + off)";
        case NM_TYPE_DIRECT: return "type + off";
        case NM_TYPE_PTR:    return "*(type + off)";
        default:             return "none";
    }
}

static bool PlausibleName(const char* s)
{
    if (!s || !s[0]) return false;
    int n = 0, letters = 0;
    for (; s[n] && n < 40; n++)
    {
        char c = s[n];
        if (c >= 'a' && c <= 'z') letters++;
        else if (c >= 'A' && c <= 'Z') letters++;
        else if (c >= '0' && c <= '9') continue;
        else if (c == '_' || c == '/' || c == '-' || c == '.') continue;
        else return false;
    }
    return n >= 3 && letters >= 3;
}

static bool PlausiblePtr(const void* p)
{
    unsigned long long a = (unsigned long long)p;
    return a >= 0x10000ull && a <= 0x7FFFFFFFFFFFull;
}

// Read one candidate. Every dereference is guarded; SafeReadStr additionally
// VirtualQuerys and refuses anything with a byte outside 32..126, which is what
// tells a real name from a stale pointer.
static bool ReadCandidate(BYTE* v, int mode, unsigned off, char* out, size_t cap)
{
    out[0] = 0;
    BYTE* base = v;

    if (mode == NM_TYPE_DIRECT || mode == NM_TYPE_PTR)
    {
        BYTE* t = NULL;
        TSM_TRY { t = *(BYTE**)(v + S.vTypeOff); }
        TSM_EXCEPT("trackcrew read type ptr") { return false; }
        if (!PlausiblePtr(t)) return false;
        base = t;
    }

    if (mode == NM_PTR || mode == NM_TYPE_PTR)
    {
        BYTE* p = NULL;
        TSM_TRY { p = *(BYTE**)(base + off); }
        TSM_EXCEPT("trackcrew read name ptr") { return false; }
        if (!PlausiblePtr(p)) return false;
        return SafeReadStr(p, out, cap) && PlausibleName(out);
    }

    return SafeReadStr(base + off, out, cap) && PlausibleName(out);
}

// NOT UNDER THE LOCK, and the comment here used to say it was.
//
// Its only caller is VehicleName, reached from h_CanProceed BEFORE the
// DiagLock() that guards the census - deliberately, because everything in here
// reads the game's memory and a fault under the log lock is a frozen
// simulation. The consequence is that g_nameMode, g_nameOff and g_nameTries are
// written unlocked and read unlocked in Report(). They are three ints written
// once each, during at most six discovery passes, and the worst case is one
// wasted pass or one wrong line in a report - so this is a documented race, not
// a fixed one. Writing them under the lock would mean holding it across
// thousands of VirtualQuery calls on the simulation thread, which is worse.
static void DiscoverName(BYTE* v)
{
    static const int modes[4] = { NM_DIRECT, NM_PTR, NM_TYPE_DIRECT, NM_TYPE_PTR };
    int found = 0;

    Dlog("---- vehicle name discovery, attempt %d of %d (vehicle %p) ----",
         g_nameTries, NAME_TRIES_MAX, (void*)v);
    {
        BYTE* t = NULL;
        TSM_TRY { t = *(BYTE**)(v + S.vTypeOff); }
        TSM_EXCEPT("trackcrew discovery type") { t = NULL; }
        Dlog("  type pointer at vehicle+%#x = %p%s", S.vTypeOff, (void*)t,
             PlausiblePtr(t) ? "" : "  (not a plausible pointer)");
    }

    for (int m = 0; m < 4; m++)
    {
        for (unsigned off = 0; off + 8 <= S.vNameScanMax; off += 8)
        {
            char probe[64];
            if (!ReadCandidate(v, modes[m], off, probe, sizeof(probe))) continue;

            if (found < NAME_REPORT_MAX)
                Dlog("  candidate: %-18s +%#06x  \"%s\"", ModeName(modes[m]), off, probe);

            if (g_nameMode == NM_NONE)
            {
                g_nameMode = modes[m];
                g_nameOff  = (int)off;
            }
            found++;
        }
    }

    if (!found)
        Dlog("  nothing that looks like a name anywhere in the first %#x bytes,"
             " by any of the four layouts.", S.vNameScanMax);
    else
        Dlog("  %d candidate(s); using %s +%#x. If that is the wrong one, put"
             " name_mode and name_offset in the ini from the list above.",
             found, ModeName(g_nameMode), g_nameOff);
    DiagFlush();
}

// Fills `out`. Returns false when we could not read a name at all.
static bool VehicleName(BYTE* v, char* out, size_t cap)
{
    out[0] = 0;
    if (!v) return false;

    if (g_nameMode != NM_NONE)
        return ReadCandidate(v, g_nameMode, (unsigned)g_nameOff, out, cap);

    // DO NOT LATCH ON ONE FAILURE - 0.1 did, and whichever vehicle happened to
    // arrive first decided classification for the whole session.
    if (g_nameTries >= NAME_TRIES_MAX) return false;
    g_nameTries++;

    DiscoverName(v);

    if (g_nameMode != NM_NONE)
        return ReadCandidate(v, g_nameMode, (unsigned)g_nameOff, out, cap);

    if (g_nameTries >= NAME_TRIES_MAX)
    {
        Dlog("vehicle name: NOT FOUND after %d discovery passes. Every train will"
             " be left alone. Raise name_scan (currently %#x), check type_offset"
             " (currently %#x), or send me the candidate lists above.",
             NAME_TRIES_MAX, S.vNameScanMax, S.vTypeOff);
        DiagFlush();
    }
    return false;
}

// =========================================================================
//  classification
// =========================================================================
static bool ContainsNoCase(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return false;
    for (const char* h = hay; *h; h++)
    {
        const char *a = h, *b = needle;
        while (*a && *b)
        {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

// =========================================================================
//  THE GAME'S OWN ANSWER
// =========================================================================
//
// Everything below this block is the fallback. THIS is the test: does the
// vehicle's type carry the "trackbuilder" feature, the one its script.ini
// declares with $TRAINGROUP_TRACKBUILDER? That is true of the base game's
// eleven, of the DLC's, of a disassembled one, and of a workshop train written
// by somebody who has never heard of this plugin - because it is the same flag
// the game itself reads to decide what a track builder is.
//
// See tc_sites.h for the two functions whose bytes encode these four offsets.
// Nothing here calls into the game: it is two pointer reads and a walk of a
// handful of ints, all of it guarded.
static int  g_featureIdx = -1;      // index of "trackbuilder", -1 = not resolved
static int  g_featureOk;            // the offsets verified against the binary

// Resolve "trackbuilder" to its index once. The feature table is built while
// the game loads its data, so this is retried until it succeeds rather than
// decided at plugin start.
// A SCAN THAT COMPLETED AND FOUND NOTHING IS NOT RETRIED.
//
// Retrying is right for "the table is not built yet" - that case bails after
// one pointer read. It is wrong once the table IS built and the keyword is not
// in it under this spelling: then the loop runs to completion, every frame,
// forever, with a VirtualQuery inside SafeReadStr per record. Up to 4096 of
// them at 60 fps is a quarter of a million kernel calls a second on the render
// thread, for an answer that is not going to change. The name list already
// covers the functional side; this just has to stop asking.
static int g_featureGaveUp;

static void ResolveFeatureIdx(void)
{
    if (g_featureIdx >= 0 || !g_featureOk || g_featureGaveUp) return;

    BYTE* world = g_exeBase + RVA_WORLD_OBJ;
    BYTE *b = NULL, *e = NULL;
    TSM_TRY
    {
        b = *(BYTE**)(world + W_OFF_FEATURES);
        e = *(BYTE**)(world + W_OFF_FEATURES_END);
    }
    TSM_EXCEPT("trackcrew feature table") { return; }

    if (!b || e <= b) return;                       // not built yet
    size_t n = (size_t)(e - b) / FEATURE_STRIDE;
    if (n == 0 || n > 4096) return;                 // not plausible

    for (size_t i = 0; i < n; i++)
    {
        char name[64];
        if (!SafeReadStr(b + i * FEATURE_STRIDE, name, sizeof(name))) continue;
        if (_stricmp(name, FEATURE_TRACKBUILDER) != 0) continue;
        g_featureIdx = (int)i;
        Dlog("the game's own track-builder flag: \"%s\" is feature %d of %d."
             " Every train is now classified by the same thing its script.ini"
             " declares - workshop trains included.",
             FEATURE_TRACKBUILDER, g_featureIdx, (int)n);
        DiagFlush();
        return;
    }

    // The table is built, it is a plausible size, and "trackbuilder" is not in
    // it. Say so once and stop looking.
    g_featureGaveUp = 1;
    Dlog("the world's feature table has %d entries and none of them is \"%s\","
         " so classification stays on the name list for this session. Nothing is"
         " read from the wrong place; the census in the report says how each"
         " train was judged.", (int)n, FEATURE_TRACKBUILDER);
    DiagFlush();
}

// -1 unknown, 0 no, 1 yes.
static int TypeHasTrackBuilder(BYTE* type)
{
    if (!g_featureOk || g_featureIdx < 0 || !type) return -1;

    int* b = NULL; int* e = NULL;
    TSM_TRY
    {
        b = *(int**)(type + T_OFF_GROUPS);
        e = *(int**)(type + T_OFF_GROUPS_END);
    }
    TSM_EXCEPT("trackcrew group vector") { return -1; }

    if (!b || e < b) return -1;
    size_t n = (size_t)((BYTE*)e - (BYTE*)b) / sizeof(int);
    if (n > 256) return -1;                          // not a plausible group list
    if (n == 0) return 0;
    if (!ReadablePtr(b, n * sizeof(int))) return -1;

    for (size_t i = 0; i < n; i++)
        if (b[i] == g_featureIdx) return 1;
    return 0;
}

// A SHORT PATTERN IS A DIFFERENT KIND OF PATTERN. "russian_service_" is
// sixteen characters and can only match what it means. "pk1" and "kzh" are
// three, in a namespace of transliterated Russian where `zh` is the standard
// spelling of the letter Zhe and turns up in half the rail vocabulary - and the
// cost of being wrong is not symmetric. A missed track builder makes you wait;
// a passenger express wrongly classified stops braking for other trains and
// runs red signals, for every vehicle of that model, for the session.
//
// So anything five characters or shorter has to sit on a token boundary: the
// start or end of the name, or next to something that is not a letter or a
// digit. That still catches russian_pk1, pk1_vagon and workshop/pk-1, and stops
// pk10, kzhde and uk250.
static bool IsWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool MatchPattern(const char* name, const char* pat)
{
    size_t pn = strlen(pat);
    if (!pn) return false;
    if (pn > 5) return ContainsNoCase(name, pat);

    for (const char* h = name; *h; h++)
    {
        const char *a = h, *b = pat;
        while (*a && *b)
        {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (*b) continue;
        bool leftOk  = (h == name)  || !IsWordChar(h[-1]);
        bool rightOk = (*a == 0)    || !IsWordChar(*a);
        if (leftOk && rightOk) return true;
    }
    return false;
}

static bool IsExcluded(const char* name)
{
    for (int i = 0; i < g_nExclude; i++)
        if (ContainsNoCase(name, g_exclude[i])) return true;
    return false;
}

static bool IsTrackBuilder(const char* name)
{
    // Exclusions win. They exist so that one false positive costs one line in
    // the ini rather than retyping the whole match list to drop a pattern.
    if (IsExcluded(name)) return false;
    for (int i = 0; i < g_nMatch; i++)
        if (MatchPattern(name, g_match[i])) return true;
    return false;
}

// ---- learning from the player ---------------------------------------------
//
// Mark one track builder by hand and the whole fleet follows, with no offsets
// to guess and no list to maintain.
//
// OFF BY DEFAULT, AND THAT IS DELIBERATE. The button is a statement about ONE
// VEHICLE AND ONE SIGNAL - the game clears the flag again once that train is
// through. What we do with it is the opposite on both axes: we apply it to
// every train of that MODEL, PERMANENTLY. So a player who clicks it once to
// free a stuck passenger locomotive would silently enrol that whole class as
// signal-blind, which is the likeliest route to a pile-up this plugin could
// possibly create. As an opt-in teaching mode it is genuinely useful; as a
// default it is a trap.
#define LEARNED_MAX 16
static char g_learned[LEARNED_MAX][MATCH_LEN];
static int  g_nLearned;

static bool IsLearned(const char* name)
{
    for (int i = 0; i < g_nLearned; i++)
        if (!_stricmp(g_learned[i], name)) return true;
    return false;
}

// caller holds the lock
static void Learn(const char* name)
{
    if (!name || !name[0] || g_nLearned >= LEARNED_MAX) return;
    if (IsLearned(name)) return;
    strncpy_s(g_learned[g_nLearned], MATCH_LEN, name, _TRUNCATE);
    g_nLearned++;
    // NO DiagFlush HERE. This runs on the simulation thread with the shared lock
    // held; FlushFileBuffers is a synchronous round trip to the disk and the
    // render thread would wait behind it. The lines are written; the flush can
    // wait for the next report.
    Dlog("LEARNED: you set \"Ignore red signal\" on a \"%s\" - every train of", name);
    Dlog("         that model is now treated as a track builder for this session.");
    Dlog("         To keep it after a restart, add it to  match =  in trackcrew.ini");
}

// ---- the per-vehicle cache -------------------------------------------------
//
// This hook runs inside the train movement routine and is asked several times a
// second per train, so the steady state has to be a load and a compare. The
// slow path - reading a name out of the game, classifying it, tallying it -
// happens once per vehicle per relearn interval and takes the lock.
//
// THE WHOLE TABLE IS CLEARED EVERY FEW SECONDS, which does three jobs at once:
// it notices a player ticking the button after we cached them, it bounds how
// long a stale pointer can be trusted after a vehicle is scrapped or a world is
// closed, and it costs one memset of 4 KB.
// 2048 SLOTS, NOT 256. The key space is every rail vehicle on the map, and a
// mature republic runs hundreds. At 256 slots with four-way probing, two busy
// trains that hash to the same bucket evict each other on every call and BOTH
// take the slow path every time the predicate is asked - and the slow path
// takes the log's CRITICAL_SECTION on the simulation thread, which is the one
// thing the comment above sets out to avoid. 32 KB of table and a 32 KB memset
// per relearn interval is a good trade for that.
#define VCACHE_BITS 11
#define VCACHE (1 << VCACHE_BITS)
#define VPROBE 4

// `v` is volatile because CachePut publishes the verdict first and the key
// second, and that ordering is load-bearing: a reader that matched the key
// before the verdict landed would return the previous occupant's answer. Two
// plain stores into the same object are free for the compiler to reorder or
// merge; one volatile store is not.
struct VEnt { BYTE* volatile v; int verdict; };
static VEnt g_vcache[VCACHE];

static void CacheClear(void) { memset(g_vcache, 0, sizeof(g_vcache)); }

// -1 = not cached
static int CacheGet(BYTE* v)
{
    int slot = (int)PtrHash(v, VCACHE_BITS);
    for (int i = 0; i < VPROBE; i++)
    {
        VEnt* e = &g_vcache[(slot + i) & (VCACHE - 1)];
        if (e->v == v) return e->verdict;
        if (!e->v) return -1;
    }
    return -1;
}

static void CachePut(BYTE* v, int verdict)
{
    int slot = (int)PtrHash(v, VCACHE_BITS);
    for (int i = 0; i < VPROBE; i++)
    {
        VEnt* e = &g_vcache[(slot + i) & (VCACHE - 1)];
        if (!e->v || e->v == v) { e->verdict = verdict; e->v = v; return; }
    }
    // All four probes taken: evict the LAST one we looked at rather than the
    // first. Evicting slot 0 every time makes a colliding pair ping-pong on one
    // entry and never settle; spreading the eviction across the probe window
    // lets both of them end up cached.
    VEnt* e = &g_vcache[(slot + VPROBE - 1) & (VCACHE - 1)];
    e->verdict = verdict;       // VALUE BEFORE KEY, as in the loop above
    e->v = v;
}

// Is the game's own ignore-red-signal flag set on this vehicle right now?
// One byte, and x64 SEH is table-based so the guard costs nothing until it
// actually faults.
static bool PlayerFlagged(BYTE* v)
{
    int f = 0;
    TSM_TRY { f = v[S.vIgnoreRed]; }
    TSM_EXCEPT("trackcrew read ignore flag") { return false; }
    return f != 0;
}

// =========================================================================
//  the hook
// =========================================================================
typedef char (*CanProceedFn)(void* vehicle);
static CanProceedFn o_CanProceed;
static int          g_hooked;

// THIS RUNS ON THE SIMULATION THREAD, not the render thread. The loader's own
// pitfalls doc is blunt about what that costs people, so the split here is
// deliberate:
//
//   fast path   a cache probe and a return. No lock, no engine call, no
//               VirtualQuery, no formatting, no allocation.
//   slow path   once per vehicle per relearn interval: reads a name out of the
//               game, classifies, tallies. TAKES THE LOCK, because the tally
//               and the learned list are shared with the render thread and a
//               half-written name in a shared table is a real corruption.
//
// Counters are interlocked, so they need no lock of their own.
static char h_CanProceed(void* vehicle)
{
    if (!g_enabled || g_off || !vehicle) return o_CanProceed(vehicle);

    BYTE* v = (BYTE*)vehicle;
    if (g_debug) Bump(C_ASK);

    int verdict = CacheGet(v);

    if (verdict < 0)
    {
        char name[64]; name[0] = 0;
        int  got = 0, flagged = 0;

        TSM_TRY
        {
            got     = VehicleName(v, name, sizeof(name)) ? 1 : 0;
            flagged = PlayerFlagged(v) ? 1 : 0;
        }
        TSM_EXCEPT("trackcrew classify") { return o_CanProceed(vehicle); }

        // THE GAME'S ANSWER FIRST. The name list only gets a say when the
        // feature flag could not be read at all - a game update that moved the
        // offsets, or a type we could not follow.
        BYTE* vtype = NULL;
        TSM_TRY { vtype = *(BYTE**)(v + S.vTypeOff); }
        TSM_EXCEPT("trackcrew type ptr") { vtype = NULL; }
        int byFeature = TypeHasTrackBuilder(vtype);

        DiagLock();
        if (byFeature >= 0)
            verdict = byFeature ? 1 : ((got && IsLearned(name)) ? 1 : 0);
        else
            verdict = (got && (IsTrackBuilder(name) || IsLearned(name))) ? 1 : 0;

        // The player ticking the button is a statement about this vehicle.
        // Only promote to a track builder if the name was actually recorded -
        // otherwise the vehicle is let through and nothing is remembered, which
        // repeats silently after every cache clear.
        if (!verdict && flagged && g_learn && got && g_nLearned < LEARNED_MAX)
        { Learn(name); verdict = 1; }

        // THE MODEL LINE IS NOT BEHIND g_debug. The classification runs whether
        // or not counters are kept, so the evidence of a WRONG classification
        // has to run too - otherwise turning debug off to quieten the log also
        // removes the only way to discover that a passenger fleet is being let
        // through red signals. It is one line per train model per session.
        if (got && !TallyHas(&g_tally, name))
            Dlog("train model seen: \"%s\"  -> %s   [%s]", name,
                 verdict ? "TRACK BUILDER, signals and collisions off"
                         : "left alone",
                 (byFeature >= 0) ? "the game's own flag"
                                  : "name list - the flag could not be read");

        if (g_debug)
        {
            if (got)
            {
                // SAY EACH NAME THE FIRST TIME IT IS SEEN. The census at the
                // bottom of the report has always had this, but the report
                // needs a keypress, and "1 names seen" in the heartbeat tells
                // you the read worked and nothing about WHAT it read - which is
                // the only thing you need when nothing is matching. One line
                // per train model per session is a price worth paying.
                TallyAdd(&g_tally, name, verdict);
            }
            else Bump(C_UNKNOWN);
        }
        DiagUnlock();

        CachePut(v, verdict);

        if (g_debug && g_trace >= 3 && verdict && RateSay(&g_rlLet))
            TRACE(3, "  let through: %s (#%lld)", got ? name : "(unnamed)",
                  (long long)g_rlLet.seen);
    }

    if (!verdict) { if (g_debug) Bump(C_DENY); return o_CanProceed(vehicle); }

    if (g_debug) Bump(C_LET);
    return 1;                          // exactly what +0x5E0 being set does
}

// =========================================================================
//  frame, hotkeys, report
// =========================================================================
// =========================================================================
//  one-way semaphores: four bytes, verified, or nothing
// =========================================================================
//
// The rules this follows are the ones anarchy's layer 2 follows, for the same
// reason - these are the game's own instructions:
//
//   * one byte per site, so no instruction boundary can move and no thread can
//     read a half-written value;
//   * the whole window is compared against the recorded bytes at load, and the
//     one byte is compared again immediately before each write. A mismatch
//     means somebody else owns that byte and we leave it alone;
//   * going OFF, a site whose byte is not ours is NOT retired - ours may still
//     be in there and retiring it would strand it with no way back.
//
// Nothing here points at plugin memory, so even a hard unload leaves the game
// with four of its own bytes flipped and nothing dangling.
struct OneWaySite
{
    const char*          name;
    const unsigned char* expect;
    int                  len;
    int                  at;        // which byte of the window changes
};

static const OneWaySite ONEWAY[4] =
{
    { "one-way: route type 1", EXPECT_ONEWAY_PF1, ONEWAY_PF1_LEN, ONEWAY_PF1_AT },
    { "one-way: route type 2", EXPECT_ONEWAY_PF2, ONEWAY_PF2_LEN, ONEWAY_PF2_AT },
    { "one-way: path head",    EXPECT_ONEWAY_PB1, ONEWAY_PB1_LEN, ONEWAY_PB1_AT },
    { "one-way: path tail",    EXPECT_ONEWAY_PB2, ONEWAY_PB2_LEN, ONEWAY_PB2_AT },
};

static unsigned char g_owOrig[4];     // what was there before we touched it
static unsigned char g_owMine[4];     // 1 = our byte is in the game right now
static unsigned char g_owDead[4];     // 1 = never write to this one

static bool TcPokeByte(BYTE* p, BYTE value)
{
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = value;
    DWORD back = 0;
    VirtualProtect(p, 1, old, &back);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return true;
}

static inline BYTE* OneWayAt(int i) { return g_exeBase + S.oneWay[i] + ONEWAY[i].at; }

// Put back whatever we wrote during THIS call, for the all-or-nothing rule
// below. Only touches bytes that are still ours.
static void OneWayRollback(const unsigned char* wroteNow)
{
    for (int i = 0; i < 4; i++)
    {
        if (!wroteNow[i] || !g_owMine[i]) continue;
        BYTE* p = OneWayAt(i);
        if (*p == ONEWAY_PATCH_BYTE && TcPokeByte(p, g_owOrig[i])) g_owMine[i] = 0;
    }
}

// Returns how many bytes it actually changed this call.
//
// ALL FOUR OR NONE, in both directions, because ModStart already decided that
// and then this function quietly did not enforce it: a router that ignores
// signal direction in two places out of four plans routes the path builder then
// refuses to walk. ModStart's check is at load; this is the one that matters
// when VirtualProtect fails on one page at 3am.
static int OneWayWrite(int on)
{
    int changed = 0, refused = 0, live = 0;
    unsigned char wroteNow[4] = { 0, 0, 0, 0 };

    // ---- going ON: look before writing anything --------------------------
    if (on)
    {
        int usable = 0, already = 0;
        for (int i = 0; i < 4; i++)
        {
            if (g_owDead[i]) continue;
            BYTE* p = OneWayAt(i);
            if (!ReadablePtr(p, 1)) { g_owDead[i] = 1; continue; }
            if (g_owMine[i]) { already++; usable++; continue; }
            if (*p != ONEWAY[i].expect[ONEWAY[i].at])
            {
                Dlog("  %s: exe+%#x holds %02X, expected %02X - left alone",
                     ONEWAY[i].name, S.oneWay[i] + ONEWAY[i].at, *p,
                     ONEWAY[i].expect[ONEWAY[i].at]);
                g_owDead[i] = 1; continue;
            }
            usable++;
        }

        if (usable < 4)
        {
            Dlog("  only %d of the 4 one-way sites are usable right now, so NOTHING"
                 " was written. Three quarters of this patch is not a state to play"
                 " in.", usable);
            g_oneWayOn = already ? 1 : 0;
            return 0;
        }

        for (int i = 0; i < 4; i++)
        {
            if (g_owMine[i]) { live++; continue; }
            BYTE* p = OneWayAt(i);
            g_owOrig[i] = *p;
            if (TcPokeByte(p, ONEWAY_PATCH_BYTE))
            { g_owMine[i] = 1; wroteNow[i] = 1; changed++; live++; }
            else
            {
                Dlog("  %s: VirtualProtect refused - putting back the %d byte(s)"
                     " already written and leaving the switch off",
                     ONEWAY[i].name, changed);
                OneWayRollback(wroteNow);
                g_oneWayOn = 0;
                return 0;
            }
        }
    }
    // ---- going OFF: try every site, never retire one we still hold -------
    else
    {
        for (int i = 0; i < 4; i++)
        {
            if (!g_owMine[i]) continue;

            BYTE* p = OneWayAt(i);
            // NOT g_owDead HERE. Retiring a site on the way out would strand our
            // byte in the game with no path back - the key would skip it and
            // Panic() would find g_owMine still set with nothing willing to act
            // on it. A site we are holding stays live however unreadable it
            // looks; it is counted, reported, and tried again next time.
            if (!ReadablePtr(p, 1)) { refused++; live++; continue; }

            if (*p != ONEWAY_PATCH_BYTE)
            {
                // Somebody else is holding this byte now. NOT retired: ours may
                // still be underneath and every later toggle should try again.
                Dlog("  %s: exe+%#x is no longer our byte - still counted as patched",
                     ONEWAY[i].name, S.oneWay[i] + ONEWAY[i].at);
                refused++; live++; continue;
            }
            if (TcPokeByte(p, g_owOrig[i])) { g_owMine[i] = 0; changed++; }
            else { refused++; live++; }
        }
    }

    g_oneWayOn = (live > 0) ? 1 : 0;
    if (refused) Dlog("  %d of the 4 one-way sites were left alone", refused);
    return changed;
}

// The full switch, including the message. Kept separate from OneWayWrite so
// that Panic() can put the bytes back without going anywhere near the log.
static void SetOneWay(int on)
{
    if (g_off) return;

    int usable = 0;
    for (int i = 0; i < 4; i++) if (!g_owDead[i]) usable++;
    if (!usable)
    {
        Dlog("one-way: none of the four sites match this build - nothing to switch");
        DiagFlush();
        return;
    }

    int changed = OneWayWrite(on);

    if (g_oneWayOn)
    {
        Dlog("ONE-WAY SEMAPHORES OFF FOR ROUTING - %d of 4 bytes changed now", changed);
        Dlog("  every one-way signal is now read as a two-way signal BY THE ROUTER.");
        Dlog("  Red is still red and blocks are still blocks; this is only about");
        Dlog("  which routes the pathfinder is willing to find.");
        Dlog("  IT IS NOT LIMITED TO TRACK BUILDERS - the router's search context");
        Dlog("  has no vehicle in it to test. Every train on the map can plan a");
        Dlog("  wrong-way route while this is on. Let the builder come home, then");
        Dlog("  press the key again.");
    }
    else
        Dlog("one-way semaphores are back ON for routing (%d bytes restored)", changed);
    DiagFlush();
}

// The foreground check, the modifier sampling and the hotkey edge detector are
// sovietkit's now - three plugins were asking Windows the same questions every
// frame. What is left is which modifier this part's keys want.
static int g_modifier = 4;

static void Report(void)
{
    Dlog("--------------------------------------------------------------");
    Dlog("trackcrew %s report", TRACKCREW_VERSION);
    Dlog("  enabled %d   hook %d   debug %d   trace %d", g_enabled, g_hooked, g_debug, g_trace);
    Dlog("  classifying by %s",
         (g_featureOk && g_featureIdx >= 0)
            ? "the game's own $TRAINGROUP_TRACKBUILDER flag - every train, workshop included"
            : (g_featureOk ? "the name list for now; the feature table is not built yet"
                           : "the name list - the flag offsets did not verify"));
    if (g_nameMode != NM_NONE)
        Dlog("  vehicle name field: %s +%#x", ModeName(g_nameMode), g_nameOff);
    else
        Dlog("  vehicle name field: NOT FOUND YET (%d of %d discovery passes used)",
             g_nameTries, NAME_TRIES_MAX);
    Dlog("  match list (%d):", g_nMatch);
    for (int i = 0; i < g_nMatch; i++)
        Dlog("     \"%s\"%s", g_match[i], strlen(g_match[i]) <= 5 ? "   (must sit on a token boundary)" : "");
    if (g_nExclude)
    {
        Dlog("  never a track builder, whatever else matches:");
        for (int i = 0; i < g_nExclude; i++) Dlog("     \"%s\"", g_exclude[i]);
    }
    Dlog("  learn from the button: %d   relearn every %d ms", g_learn, g_relearnMs);
    {
        int usable = 0, mine = 0;
        for (int i = 0; i < 4; i++) { if (!g_owDead[i]) usable++; if (g_owMine[i]) mine++; }
        Dlog("  one-way semaphores: %s  (%d of 4 sites usable, %d patched right now)",
             g_oneWayOn ? "IGNORED BY THE ROUTER - every train, not just builders"
                        : "respected (normal)", usable, mine);
        if (K_ONEWAY)
            Dlog("     %s+%c switches it", SkModifierName(g_modifier), K_ONEWAY);
    }
    // UNDER THE LOCK. g_learned and g_tally are written by the detour on the
    // simulation thread; this runs on the render thread. The names happen to be
    // safe by the order TallyAdd publishes them in, but the counts and verdicts
    // can be read torn, and the lock exists for exactly this table. Dlog takes
    // the same CRITICAL_SECTION and it is recursive, so nesting is fine.
    DiagLock();
    if (g_nLearned)
    {
        Dlog("  LEARNED from you ticking \"Ignore red signal\" (%d) - paste these", g_nLearned);
        Dlog("  into  match =  in trackcrew.ini to keep them across restarts:");
        for (int i = 0; i < g_nLearned; i++) Dlog("     %s", g_learned[i]);
    }
    TallyDump(&g_tally, "vehicles asked to proceed", "TRACK BUILDER", "left alone");
    DiagUnlock();
    Dlog("  how to read this:");
    Dlog("    every rail vehicle that moves shows up in the census above. If the");
    Dlog("    track builders are there but say 'left alone', copy their name into");
    Dlog("    match = in trackcrew.ini and restart - no rebuild needed.");
    Dlog("    If the census is empty while trains are moving, the name field was");
    Dlog("    not found and the line above says so.");
    Dlog("--------------------------------------------------------------");
    DiagFlush();
}

// Called by the kit's shared reset key. The classification cache goes with the
// census, because a cache entry is a verdict and the point of zeroing is to
// watch the verdicts being made again from nothing.
static void CacheClear(void);

// The kit's shared fault path calls this. h_CanProceed is a 14-byte inline
// splice on the SIMULATION thread and it cannot be removed while the game is
// running - the only thing that can be done is close its gate, and that gate is
// g_off. Without this, a kit that has announced it stopped would keep letting
// track builders through red signals with the toggle key already dead, because
// the toggle lives in Frame() and Frame() is no longer being called.
// 0.9 adds the four one-way bytes to what this has to undo, and they go back
// AFTER the gate is closed, for anarchy's reason: a VirtualProtect-and-write
// can fault too, and if it does, the caller's guard swallows it and anything
// after it never runs. The gate is a store that cannot fault, so it goes first.
// No logging and no locks here - see anarchy's Panic for why.
static void Panic(void)
{
    g_off = 1;
    g_oneWayOn = 0;

    // ONE GUARD PER SITE. The reasoning above is exactly as true for site 1
    // relative to site 0 as it is for the gate relative to all four: both call
    // sites in sovietkit.cpp wrap this whole function in a single __try, so a
    // fault at *p or inside TcPokeByte for the first site would unwind out of
    // the loop and leave the other three patched, with g_liveCrew already 0 and
    // therefore no frame tick left to retry. A permanently half-patched rail
    // router, in a process the kit has just announced it stood down from.
    for (int i = 0; i < 4; i++)
    {
        if (!g_owMine[i]) continue;
        TSM_TRY
        {
            BYTE* p = g_exeBase + S.oneWay[i] + ONEWAY[i].at;
            if (*p == ONEWAY_PATCH_BYTE) { if (TcPokeByte(p, g_owOrig[i])) g_owMine[i] = 0; }
            else g_owMine[i] = 0;
        }
        TSM_EXCEPT("trackcrew panic restore") { }
    }
}

static void ResetCensus(void)
{
    DiagLock();
    memset(&g_tally, 0, sizeof(g_tally));
    DiagUnlock();
    CacheClear();
    Dlog("  trackcrew: census and classification cache cleared"
         " (learned names kept - restart to forget them)");
}

static void Frame(void)
{
    if (g_off) return;               // switched off means switched off

    // Drop the classification cache on a timer. One memset, and it is what
    // makes the plugin notice a player ticking the button after we already
    // cached that train - and what bounds how long a recycled vehicle address
    // can carry an old verdict.
    {
        static ULONGLONG last;
        ULONGLONG now = GetTickCount64();
        if (now - last >= (ULONGLONG)g_relearnMs) { last = now; CacheClear(); }
    }

    ResolveFeatureIdx();

    static unsigned char wTog, wOne;
    if (SkHotkeyEdge(K_TOGGLE, g_modifier, &wTog))
    {
        if (!g_hooked)
            Dlog("trackcrew: the ignore-signals hook is not installed on this"
                 " build, so this key has nothing to switch. The one-way key"
                 " still works.");
        else
        {
            g_enabled = !g_enabled;
            Dlog("trackcrew %s", g_enabled ? "ON" : "OFF");
        }
        DiagFlush();
    }

    // A SEPARATE KEY, not part of the toggle above. The can-proceed hook is
    // scoped to track builders and reversible the moment the gate closes; this
    // one writes to the game's code and affects every train's routing. Two very
    // different promises should not share a switch.
    if (K_ONEWAY && SkHotkeyEdge(K_ONEWAY, g_modifier, &wOne))
        SetOneWay(!g_oneWayOn);

    if (g_debug)
        HEARTBEAT_F(g_heartbeat, "[crew] asked %lld, let through %lld, left alone %lld, %d names seen",
                  (long long)g_ctr[C_ASK].n, (long long)g_ctr[C_LET].n,
                  (long long)g_ctr[C_DENY].n, g_tally.used);
}

// The ini parser is shared now; see SkCfgI/SkCfgU/SkCfgRaw in sovietkit.cpp.

static void AddMatch(const char* s)
{
    if (!s || !s[0] || g_nMatch >= MATCH_MAX) return;
    strncpy_s(g_match[g_nMatch], MATCH_LEN, s, _TRUNCATE);
    g_nMatch++;
}

static void AddExclude(const char* s)
{
    if (!s || !s[0] || g_nExclude >= MATCH_MAX) return;
    strncpy_s(g_exclude[g_nExclude], MATCH_LEN, s, _TRUNCATE);
    g_nExclude++;
}

// A comma or semicolon separated list into `add`. The buffer is sized for a
// full list; a token that ran into the end of it is DROPPED rather than kept,
// because a truncated "russian_service_" becomes "russian_" and matches every
// Russian vehicle in the game - the failure mode of silent truncation here is a
// pattern that is broader than the one that was written.
// AND IT NOW ACTUALLY DROPS IT. The old version computed `full`, added every
// token including the truncated one, and then printed a warning - so the stated
// safety property was a comment and nothing else. A truncated "russian_service_"
// arriving as "russian_" is longer than five characters, so MatchPattern takes
// the plain-substring path, and every Russian rail vehicle on the map becomes a
// track builder: signal-blind, and not braking for the train in front.
static void ParseList(char* buf, size_t cap, void (*add)(const char*))
{
    size_t len = strlen(buf);
    const bool full = (len >= cap - 1);
    // Where the last token starts, if the buffer was filled. strtok_s writes
    // NULs over the separators, so this has to be worked out before the loop.
    const char* cutStart = NULL;
    if (full)
    {
        cutStart = buf;
        for (size_t i = 0; i < len; i++) if (buf[i] == ',' || buf[i] == ';') cutStart = buf + i + 1;
    }

    char* ctx = NULL;
    for (char* tok = strtok_s(buf, ",;", &ctx); tok; tok = strtok_s(NULL, ",;", &ctx))
    {
        const bool cut = (cutStart && tok == cutStart);
        Trim(tok);
        if (!tok[0]) continue;
        if (cut)
        {
            Dlog("  the list filled the %d-byte buffer, so \"%s\" was cut short."
                 " It has been DROPPED rather than used - a shortened pattern"
                 " matches more than the one you wrote, not less. Shorten the"
                 " list and it will come back.", (int)cap, tok);
            continue;
        }
        add(tok);
    }
}

static void LoadMatchList(void)
{
    // A `list =` must be ONE LINE. There is no continuation character, and a
    // trailing backslash becomes part of the value. Sized for a full list -
    // MATCH_MAX entries of MATCH_LEN plus separators - because the old 1 KB
    // buffer could be overrun by a list the ini invites you to write.
    static char buf[MATCH_MAX * MATCH_LEN + 64];

    if (SkCfgRaw("trackcrew", "exclude", buf, sizeof(buf)))
    {
        ParseList(buf, sizeof(buf), AddExclude);
        Dlog("exclude list: %d pattern(s) that will never be a track builder", g_nExclude);
    }

    if (!SkCfgRaw("trackcrew", "match", buf, sizeof(buf)))
    {
        // THE REAL NAMES, read out of the game's own data rather than guessed.
        // Every rail construction vehicle that ships with the game lives in
        // media_soviet\\trains\\ under one of these two prefixes, and its
        // script.ini carries $TRAINGROUP_TRACKBUILDER:
        //
        //   russian_service_agmu / _vagon        russian_service_muv69 / _vagon
        //   russian_service_dkgu5 / _v2 / _vagon russian_service_tbm_kt1_*
        //   russian_service_edk300 / _vagon...   russian_service_uk25 / _v2 / _vagon
        //   russian_crane / _v2 / _vagon
        //
        // Matching is a case-insensitive substring test, so the two prefixes
        // cover all nineteen of them and every variant added later.
        //
        // 0.3 shipped five guessed English words and matched nothing, because
        // the game's internal names are transliterated Russian. The census at
        // the bottom of the report now prints the real name of every train it
        // sees, so a workshop track builder is one line in `match` away.
        // THE BASE GAME'S TRACK BUILDERS, by folder prefix. Every one of them
        // lives in media_soviet\\trains\\ under one of these two and carries
        // $TRAINGROUP_TRACKBUILDER in its script.ini.
        AddMatch("russian_service_");
        AddMatch("russian_crane");

        // AND THE MODEL NAMES THEMSELVES, because DLC and workshop track
        // builders do not live under those prefixes. Read out of the game's own
        // string table, where btf 5190/5194/5204/5206/5289/5709 name them:
        //   AGMu, DKGU-5, UK25, MUV-69, Kzh-461, EDK-300
        // plus the PK-1 and the TBM, which the vehicle window labels "Track
        // builder" under Features. Matching is a case-insensitive substring
        // test, so these catch the usual spelling variants.
        AddMatch("agmu");
        AddMatch("dkgu");
        AddMatch("uk25");   AddMatch("uk-25");
        AddMatch("muv69");  AddMatch("muv-69");
        AddMatch("kzh");
        AddMatch("edk300"); AddMatch("edk-300");
        AddMatch("tbm_kt1");
        AddMatch("pk1");    AddMatch("pk-1");

        // And the obvious English words, for anything that spells it out.
        AddMatch("trackbuilder");
        AddMatch("track_builder");
        AddMatch("tracklayer");
        AddMatch("track_layer");

        Dlog("match list: not set, using the %d built-in patterns. Every train"
             " model is named in the log the first time it is seen - if yours is"
             " not on the list, that line tells you what to add.", g_nMatch);
        return;
    }

    ParseList(buf, sizeof(buf), AddMatch);
    if (!g_nMatch) Dlog("match list is empty - nothing will be classified as a track builder");
}

static void LoadConfig(void)
{
    g_enabled   = SkCfgI("trackcrew", "enabled", 1, 0, 1);
    // 0, matching the declaration, the rationale on Learn() and the shipped
    // ini. It was 1 here, which is what applies to anyone who deletes the line,
    // trims the ini, or saves it with a BOM - and in that case every setting
    // falls back silently and this one fell back to the trap.
    g_learn     = SkCfgI("trackcrew", "learn_from_button", 0, 0, 1);
    g_relearnMs = SkCfgI("trackcrew", "relearn_ms", 5000, 500, 120000);
    g_debug     = SkCfgI("trackcrew", "debug", 0, 0, 1);
    // trace is the kit's, read once in sovietkit.cpp.
    g_heartbeat = SkCfgI("trackcrew", "heartbeat_seconds", 15, 0, 3600);

    S.buildStamp   = (unsigned)SkCfgU("sites", "build_stamp",  S.buildStamp);
    S.canProceed   = (unsigned)SkCfgU("sites", "can_proceed",  S.canProceed);
    S.vIgnoreRed   = (unsigned)SkCfgU("sites", "v_ignore_red", S.vIgnoreRed);
    S.vNameHint    = (unsigned)SkCfgU("sites", "name_offset",  S.vNameHint);
    S.vNameScanMax = (unsigned)SkCfgU("sites", "name_scan",    S.vNameScanMax);
    S.vTypeOff     = (unsigned)SkCfgU("sites", "type_offset",  S.vTypeOff);
    {
        // NM_TYPE_DIRECT (3) at +0x30 is where the name actually is, found by
        // 0.3's discovery pass and confirmed against the game's folder names:
        //   type + 0x00  "workshop_subscribed/3704539601/pk1"   <- the SOURCE
        //   type + 0x30  "parni_vuz_m124"                       <- the NAME
        // 0.3 defaulted to mode 0, which let discovery pick +0 and made every
        // name a mod folder path. That is why nothing ever matched.
        int m = SkCfgI("sites", "name_mode", 3, 0, 4);
        if (m) { g_nameMode = m; g_nameOff = (int)SkCfgU("sites", "name_offset", 0x30); }
    }

    if (S.vNameScanMax > 0x4000) S.vNameScanMax = 0x4000;

    // EVERY OFFSET FROM THE INI IS BOUNDED. __try catches a bad pointer; it does
    // not catch a correct read from the wrong place, and these are all used as
    // `vehicle + offset` or `type + offset`. Anything a human can type in a text
    // file gets a range here, and anything outside it goes back to the built-in
    // value with a line saying so rather than silently reading the wrong field.
    {
        struct Lim { const char* name; unsigned* v; unsigned lo, hi, def; };
        Lim lim[] = {
            { "v_ignore_red", &S.vIgnoreRed, 0, 0x20000, V_OFF_IGNORE_RED },
            { "type_offset",  &S.vTypeOff,   0, 0x20000, V_OFF_TYPE },
            { "name_offset",  &S.vNameHint,  0, 0x20000, V_OFF_OBJNAME_HINT },
        };
        for (int i = 0; i < (int)(sizeof(lim)/sizeof(lim[0])); i++)
            if (*lim[i].v < lim[i].lo || *lim[i].v > lim[i].hi)
            {
                Dlog("  [sites] %s = %#x is not a plausible structure offset - using %#x",
                     lim[i].name, *lim[i].v, lim[i].def);
                *lim[i].v = lim[i].def;
            }
        if (g_nameOff < 0 || g_nameOff > 0x20000)
        {
            Dlog("  [sites] name_offset for the chosen name_mode is out of range - using 0x30");
            g_nameOff = 0x30;
        }
        for (int i = 0; i < 4; i++)
            if ((size_t)S.oneWay[i] >= (size_t)g_exeSize)
            {
                Dlog("  [sites] one_way_%d = %#x is outside the executable - the"
                     " one-way switch is off this session", i + 1, S.oneWay[i]);
                S.oneWay[i] = 0; g_owDead[i] = 1;
            }
    }

    g_oneWayWanted = SkCfgI("trackcrew", "ignore_signal_direction", 0, 0, 1);
    for (int i = 0; i < 4; i++)
    {
        char key[24];
        sprintf_s(key, sizeof(key), "one_way_%d", i + 1);
        S.oneWay[i] = (unsigned)SkCfgU("sites", key, S.oneWay[i]);
    }

    g_modifier = SkCfgI("keys", "modifier", 4, 0, 6);
    K_TOGGLE = (int)SkCfgU("keys", "trackcrew_toggle", 'G') & 0xFF;
    K_ONEWAY = (int)SkCfgU("keys", "trackcrew_oneway", 'D') & 0xFF;

    LoadMatchList();
}

// =========================================================================
//  lifecycle
// =========================================================================
static bool ModInit(void)
{
    LoadConfig();
    Dlog("--- trackcrew %s: rail construction trains ignore signals and each other ---",
         TRACKCREW_VERSION);

    C_ASK     = CounterNew("crew: proceed? asked");
    C_LET     = CounterNew("crew:   track builders let through");
    C_DENY    = CounterNew("crew:   other trains left alone");
    C_UNKNOWN = CounterNew("crew:   no readable name");

    SiteAdd("can-proceed", S.canProceed, EXPECT_CAN_PROCEED, CAN_PROCEED_STOLEN, 1);

    // Not hooks - these two are the game's own instructions, and they ENCODE
    // the four offsets the track-builder flag is read through. If they match,
    // our notes match the binary; if they do not, we fall back to names rather
    // than reading a pointer out of the wrong place.
    SiteAdd("trackbuilder flag", RVA_VERIFY_HASFEATURE, EXPECT_HASFEATURE, VERIFY_HASFEATURE_LEN, 0);
    SiteAdd("feature table",     RVA_VERIFY_FEATIDX,    EXPECT_FEATIDX,    VERIFY_FEATIDX_LEN,    0);

    // The four one-way sites. `required = 0`: if they do not match, that switch
    // simply is not available and the rest of trackcrew is unaffected.
    for (int i = 0; i < 4; i++)
        SiteAdd(ONEWAY[i].name, S.oneWay[i], ONEWAY[i].expect, ONEWAY[i].len, 0);

    unsigned got = 0;
    BuildStampRead(&got);
    if (got != S.buildStamp)
    {
        g_off = 1;
        Dlog("build 0x%08X, offsets came from 0x%08X - the game updated.", got, S.buildStamp);
        Dlog("Nothing is hooked. This is the message, not a crash.");
        SiteVerifyAll();
        DiagFlush();
        return false;
    }

    Dlog("  build 0x%08X - the one every offset was read off", got);
    return true;
}

static bool ModStart(void)
{
    // NO g_enabled CHECK HERE, deliberately. For this part `enabled` is the
    // RUNTIME switch the toggle key flips: the hook installs either way and
    // h_CanProceed reads the flag per call. Refusing to install on enabled = 0
    // would make the setting permanent and the toggle key unreachable, because
    // the key lives in Frame() and Frame() is only called for a part that
    // installed. Use [sovietkit] trackcrew = 0 to install nothing at all.
    if (g_off) { Dlog("trackcrew: disabled"); DiagFlush(); return false; }

    g_featureOk = (SiteOk("trackbuilder flag") && SiteOk("feature table")) ? 1 : 0;
    if (!g_featureOk)
        Dlog("trackcrew: the two instructions that encode the track-builder flag"
             " offsets do not match this build, so classification falls back to"
             " the name list. Nothing is read from the wrong place.");

    // ---- the one-way switch, decided BEFORE the hook ------------------------
    // Deliberately above the can-proceed check: the two halves of this part are
    // independent (one is a detour on a predicate, the other is four bytes in
    // the router) and a build where one does not verify should not take the
    // other down with it.
    //
    // A site that did not verify is marked dead HERE, once, rather than being
    // discovered on the first keypress. The count is what the log reports, and
    // "3 of 4" is worth knowing before you press anything, because three of
    // these four edits is not a coherent state to play in.
    int oneWayUsable = 0;
    for (int i = 0; i < 4; i++)
    {
        if (SiteOk(ONEWAY[i].name)) oneWayUsable++;
        else g_owDead[i] = 1;
    }

    if (oneWayUsable == 4)
    {
        Dlog("  one-way semaphores: all 4 router bytes match this build.");
        Dlog("    %s+%c makes the pathfinder read every one-way signal as a"
             " two-way one, which is what gets a track builder home past signals"
             " facing the wrong way. Off by default, and it is NOT limited to"
             " track builders - see the ini.",
             SkModifierName(g_modifier), K_ONEWAY ? K_ONEWAY : '?');
    }
    else if (oneWayUsable == 0)
        Dlog("  one-way semaphores: none of the 4 router bytes match this build,"
             " so that switch is not available. Nothing else is affected.");
    else
    {
        for (int i = 0; i < 4; i++) g_owDead[i] = 1;
        oneWayUsable = 0;
        Dlog("  one-way semaphores: only some of the 4 router bytes match this"
             " build. The switch is OFF ENTIRELY rather than half applied - a"
             " router that ignores direction in two places out of four would"
             " plan routes the path builder then refuses to walk.");
    }

    // SiteOk by name, NOT SiteVerifyAll's total: the site table is shared with
    // anarchy and skins now, and skins registers two sites it is happy to have
    // fail. Judging our one hook by their count had trackcrew refusing to
    // install - and blaming an address that had verified perfectly.
    if (!SiteOk("can-proceed"))
    {
        g_enabled = 0;
        Dlog("the bytes at exe+%#x are not what this build expects - the"
             " ignore-signals rule is off and NOTHING WAS WRITTEN INTO GAME"
             " CODE. (The frame hook above is an import swap the loader undoes"
             " on unload.)", S.canProceed);
        if (!oneWayUsable) { g_off = 1; DiagFlush(); return false; }
        Dlog("  the one-way switch is unaffected and its key still works, so this"
             " part stays loaded for that alone.");
        if (g_oneWayWanted) SetOneWay(1);
        DiagFlush();
        return true;
    }

    if (InstallInlineHook(g_exeBase + S.canProceed, (void*)h_CanProceed,
                          (void**)&o_CanProceed, EXPECT_CAN_PROCEED,
                          CAN_PROCEED_STOLEN, "trackcrew can-proceed"))
    {
        g_hooked = 1;
        SiteMarkPatched("can-proceed");
        Dlog("hooked exe+%#x (%d bytes relocated)", S.canProceed, CAN_PROCEED_STOLEN);
    }
    else
    {
        g_off = 1;
        Dlog("the loader refused the hook - nothing written");
    }

    if (g_oneWayWanted && oneWayUsable) SetOneWay(1);

    Dlog("  %s+%c turns this part on and off. Start a rail construction job, let",
         SkModifierName(g_modifier), K_TOGGLE ? K_TOGGLE : '?');
    Dlog("  the trains run for a minute, then press the report key: the census at");
    Dlog("  the bottom names every train model it saw and how it classified it.");
    DiagFlush();
    return g_hooked != 0;
}

