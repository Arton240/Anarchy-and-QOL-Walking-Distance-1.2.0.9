// tsm_diag.h - the diagnostics layer shared by the skins and railfree plugins.
//
// WHY THIS FILE IS AS BIG AS IT IS. Every one of these mods works by reading a
// number out of somebody else's process at an address nobody documented. When
// one stops working there are exactly four possible causes, and this file is
// built to tell them apart without a debugger:
//
//   1. the game updated and the address moved      -> SITE VERIFICATION
//   2. the hook never installed                    -> INSTALL REPORT
//   3. the hook installed but never fires          -> CALL COUNTERS
//   4. it fires but from somewhere unexpected      -> RETURN-ADDRESS HISTOGRAM
//
// All of it is optional and all of it is cheap when off: a disabled trace level
// is one integer compare, and the counters are one increment. Nothing here
// allocates, nothing here formats a string unless it is about to write one, and
// nothing here runs a loop per frame.
//
// SET debug = 0 BEFORE PUBLISHING. The one thing in here with a real cost is
// trace level 3, which writes a line per event.

#ifndef TSM_DIAG_H
#define TSM_DIAG_H

// ------------------------------------------------------------------ hashing
//
// `(ptr >> 4) & mask` looks fine and is a trap: it only ever reads bits 4..N of
// the address, so any pool whose stride is a multiple of (mask+1)<<4 maps EVERY
// object to the same bucket and the table degenerates to its probe length. For
// a 256-slot table that period is 4096 bytes, which is exactly the kind of
// stride a large game object has.
//
// Fibonacci hashing mixes the whole pointer into the high bits before the
// shift, so no stride can collapse it. One multiply.
static inline unsigned PtrHash(const void* p, unsigned bits)
{
    unsigned long long x = (unsigned long long)p;
    return (unsigned)((x * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

// ---------------------------------------------------------------- the lock
//
// TsmBind creates g_lock; this is the flag that says so. A plugin whose hooks
// only ever run on the render thread can ignore all of this - but one hooked
// into the SIMULATION thread cannot, and the difference is not visible from the
// call site. A CRITICAL_SECTION is recursive, so a locked path that logs (and
// the logging takes the same lock) cannot deadlock itself.
static int g_lockReady;
static void DiagLock(void)   { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void DiagUnlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

// ---------------------------------------------------------------- the log
static HANDLE g_log = INVALID_HANDLE_VALUE;
static const char* g_tag = "?";

// Trace levels:
//   0  errors and one-off diagnostics only
//   1  + lifecycle: load, hooks, world open/close, a heartbeat
//   2  + every decision the plugin makes that changes something
//   3  + every call into a detour, rate limited. Noisy on purpose.
static int g_trace = 1;

// DELIBERATELY NO FlushFileBuffers PER LINE. WriteFile has already handed the
// bytes to Windows, so a game that dies still leaves a complete log. A flush
// per line is a synchronous round trip to disk, on the render thread, inside
// the frame hook. DiagFlush is for the paths that really are about to lose the
// process.
static void Dlog(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line) - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    // _TRUNCATE returns -1 rather than the wanted length, but it HAS written a
    // NUL-terminated prefix - so keep it instead of throwing the line away, and
    // mark it so nobody reads a truncated line as a complete one.
    if (n < 0)
    {
        line[sizeof(line) - 3] = 0;
        n = (int)strlen(line);
        const char* cut = "[cut]";
        if (n > (int)(sizeof(line) - 3 - strlen(cut)))
            n = (int)(sizeof(line) - 3 - strlen(cut));
        memcpy(line + n, cut, strlen(cut));
        n += (int)strlen(cut);
    }

    line[n++] = '\r';
    line[n++] = '\n';

    // Two threads writing to one handle interleave at the byte level and
    // produce lines spliced through each other. One lock, only on the paths
    // that actually write - which are edges, never the steady state.
    DiagLock();
    DWORD wrote = 0;
    WriteFile(g_log, line, (DWORD)n, &wrote, NULL);
    DiagUnlock();
}

#define TRACE(lvl, ...) do { if (g_trace >= (lvl)) Dlog(__VA_ARGS__); } while (0)

static void DiagFlush(void) { if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log); }

// One-shot diagnostics. A condition that is wrong is wrong sixty times a
// second, and a log that repeats itself is a log nobody reads.
static int g_diagSaid[32];
static void DiagOnce(int slot, const char* fmt, ...)
{
    if (slot < 0 || slot >= 32 || g_diagSaid[slot]) return;
    g_diagSaid[slot] = 1;
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    Dlog("  DIAG  %s", line);
}

// ---------------------------------------------------------------- SEH shim
//
// MSVC only. A plugin that reads the game's memory will eventually read a
// pointer that has gone, and SEH is the difference between "the feature
// switches itself off and logs why" and "the player's game closes". The mingw
// path exists so the source can be compiled for warnings on a machine without
// Visual Studio - it is not a shippable build.
#if defined(_MSC_VER)
#  define TSM_TRY    __try
#  define TSM_EXCEPT(what) __except (FaultFilter(what, GetExceptionInformation()))
#  define TSM_FINALLY __finally
#else
#  define TSM_TRY    if (1)
#  define TSM_EXCEPT(what) else
#  define TSM_FINALLY if (1)
#endif

#define TSM_FAULT_BUDGET 64

// ---------------------------------------------------------- hex formatting
static void HexStr(const BYTE* p, int n, char* out, int outSize)
{
    int w = 0;
    out[0] = 0;
    for (int i = 0; i < n && w + 3 < outSize; i++)
        w += _snprintf_s(out + w, outSize - w, _TRUNCATE, "%02x", p[i]);
}

// A 16-byte-per-line hex + ascii dump of something read out of the game. This
// is what you paste into a bug report when a structure turns out not to be the
// structure we thought it was.
static void HexDump(const char* what, const void* p, int n)
{
    if (!p) { Dlog("  %s: null", what); return; }
    if (!ReadablePtr(p, n)) { Dlog("  %s: %p is not readable for %d bytes", what, p, n); return; }

    Dlog("  %s at %p, %d bytes:", what, p, n);
    const BYTE* b = (const BYTE*)p;
    for (int off = 0; off < n; off += 16)
    {
        char hex[64], asc[24];
        int  run = (n - off < 16) ? (n - off) : 16;
        int  w = 0;
        for (int i = 0; i < run; i++)
        {
            w += _snprintf_s(hex + w, sizeof(hex) - w, _TRUNCATE, "%02x ", b[off + i]);
            asc[i] = (b[off + i] >= 32 && b[off + i] < 127) ? (char)b[off + i] : '.';
        }
        asc[run] = 0;
        Dlog("    +%04x  %-48s %s", off, hex, asc);
    }
}

// ============================================================ SITE VERIFY
//
// The first question when anything stops working is "is the game still the game
// we read these numbers off". A site is an RVA plus the bytes we expect to find
// there. At load we read what is actually there and say so, in hex, both sides.
// A mismatch is not a crash and not a guess - it is a line in the log naming
// the site, what we wanted and what we got.
#define DIAG_MAX_SITES 24

struct DiagSite
{
    const char* name;
    unsigned    rva;
    const BYTE* expect;
    int         expectLen;
    int         verified;       // 1 pass, -1 fail, 0 not checked
    int         required;       // a failed required site disables its feature
    int         patched;        // WE overwrote it with a jump - see below
};

static DiagSite g_sites[DIAG_MAX_SITES];
static int      g_nSites;

static void SiteAdd(const char* name, unsigned rva, const BYTE* expect, int len, int required)
{
    if (g_nSites >= DIAG_MAX_SITES) return;
    DiagSite* s = &g_sites[g_nSites++];
    s->name = name; s->rva = rva; s->expect = expect; s->expectLen = len;
    s->verified = 0; s->required = required; s->patched = 0;
}

// Called right after an inline hook lands on a site.
//
// WITHOUT THIS, THE PLUGIN ACCUSES ITSELF. An inline hook replaces the target's
// prologue with a jump, so the very next verification finds `ff 25 ...` instead
// of the expected bytes and prints a FAIL naming a site that is working
// perfectly. The first report railfree ever produced did exactly that, and it
// is the most alarming possible way to say "everything is fine".
static void SiteMarkPatched(const char* name)
{
    for (int i = 0; i < g_nSites; i++)
        if (!strcmp(g_sites[i].name, name)) { g_sites[i].patched = 1; return; }
}

// Returns the number of sites that FAILED.
static int SiteVerifyAll(void)
{
    int bad = 0;
    Dlog("site verification (%d sites):", g_nSites);
    for (int i = 0; i < g_nSites; i++)
    {
        DiagSite* s = &g_sites[i];
        BYTE* at = g_exeBase + s->rva;

        if (s->patched)
        {
            char got[96]; HexStr(at, s->expectLen, got, sizeof(got));
            Dlog("  ours  %-26s rva %#08x  hooked by us, now %s", s->name, s->rva, got);
            continue;               // verified before we wrote; not a finding
        }

        if (!ReadablePtr(at, s->expectLen))
        {
            s->verified = -1; bad++;
            Dlog("  FAIL  %-26s rva %#08x is not readable", s->name, s->rva);
            continue;
        }

        bool same = memcmp(at, s->expect, s->expectLen) == 0;
        s->verified = same ? 1 : -1;

        if (same)
        {
            // The guard, not just TRACE's: HexStr formats one _snprintf_s per
            // byte, and at trace 0 nothing would be printed. This header's own
            // claim is that nothing formats a string unless it is about to
            // write one, and this was the one place that did not honour it.
            if (g_trace >= 1)
            {
                char got[96]; HexStr(at, s->expectLen, got, sizeof(got));
                TRACE(1, "  ok    %-26s rva %#08x  %s", s->name, s->rva, got);
            }
        }
        else
        {
            bad++;
            char got[96], want[96];
            HexStr(at, s->expectLen, got, sizeof(got));
            HexStr(s->expect, s->expectLen, want, sizeof(want));
            Dlog("  FAIL  %-26s rva %#08x", s->name, s->rva);
            Dlog("          expected %s", want);
            Dlog("          found    %s", got);
            if (s->required) Dlog("          this site is required - the feature that uses it stays off");
        }
    }
    if (!bad) Dlog("  all %d sites match the build these offsets were read from", g_nSites);
    return bad;
}

static int SiteOk(const char* name)
{
    for (int i = 0; i < g_nSites; i++)
        if (!strcmp(g_sites[i].name, name)) return g_sites[i].verified == 1;
    return 0;
}

// ============================================================ COUNTERS
//
// "It installed but nothing happens" and "it fires but from the wrong place"
// are different bugs with the same symptom. One counter per named thing, and a
// small histogram of the return addresses each hook was reached from, answers
// both. Incrementing is one add; the histogram is a linear scan of at most
// eight entries and only runs when a hook actually fires.
#define DIAG_MAX_COUNTERS 32
#define DIAG_MAX_RETS     24      // 8 overflowed on the first real run

struct DiagCounter
{
    const char*    name;
    volatile LONG64 n;
    struct { unsigned rva; LONG64 n; } rets[DIAG_MAX_RETS];
    int            nRets;
    int            lastHit;     // temporal locality: call sites come in runs
    int            overflowRets;
};

static DiagCounter g_ctr[DIAG_MAX_COUNTERS];
static int         g_nCtr;

// -1, not 0: returning 0 would silently alias a new counter onto the FIRST one
// and every count after it would be wrong rather than missing.
static int CounterNew(const char* name)
{
    if (g_nCtr >= DIAG_MAX_COUNTERS) return -1;
    int id = g_nCtr++;
    g_ctr[id].name = name;
    return id;
}

static void Bump(int id) { if (id >= 0 && id < g_nCtr) InterlockedIncrement64(&g_ctr[id].n); }

// Bump, and remember where it was called from. `ret` is an absolute address; it
// is stored as an RVA so a log line can be compared with a disassembly.
//
// The histogram is a linear scan, and raising it from 8 to 24 slots tripled the
// worst case on a path that ran 150,000 times in one session. Call sites arrive
// in runs - the same one over and over inside a loop - so remembering the last
// match turns almost every call into a single compare.
static void BumpFrom(int id, const void* ret)
{
    if (id < 0 || id >= g_nCtr) return;
    InterlockedIncrement64(&g_ctr[id].n);

    unsigned rva = (unsigned)((const BYTE*)ret - g_exeBase);
    DiagCounter* c = &g_ctr[id];

    // EVERY READ OF nRets IS CLAMPED, not just the dump's.
    //
    // The bound check and the increment below are two separate operations and
    // this runs on the simulation thread. Two threads that both read nRets at
    // DIAG_MAX_RETS - 1 both append, and the second one's ++ stores
    // DIAG_MAX_RETS + 1. After that the loop bound is past the end of the array:
    // rets[24] overlaps nRets and lastHit, which feeds straight back into the
    // bound, and rets[25] and beyond walk into the next counter. It needs trace
    // >= 2 and several simulation threads on one counter - which is to say, it
    // needs exactly the configuration you switch into when you are already
    // trying to work out what is wrong.
    //
    // One local, clamped once, costs nothing and makes the overflow bounded.
    int n = c->nRets;
    if (n > DIAG_MAX_RETS) n = DIAG_MAX_RETS;

    if (c->lastHit >= 0 && c->lastHit < n && c->rets[c->lastHit].rva == rva)
    { c->rets[c->lastHit].n++; return; }

    for (int i = 0; i < n; i++)
        if (c->rets[i].rva == rva) { c->rets[i].n++; c->lastHit = i; return; }

    if (n < DIAG_MAX_RETS) { c->rets[n].rva = rva; c->rets[n].n = 1; c->nRets = n + 1; }
    else c->overflowRets++;
}

static void CountersDump(void)
{
    Dlog("counters:");
    for (int i = 0; i < g_nCtr; i++)
    {
        DiagCounter* c = &g_ctr[i];
        Dlog("  %-30s %lld", c->name, (long long)c->n);
        int nr = c->nRets;                      // two threads can push this past
        if (nr > DIAG_MAX_RETS) nr = DIAG_MAX_RETS;   // the array; read it clamped
        for (int k = 0; k < nr; k++)
            Dlog("      from exe+%#08x   %lld", c->rets[k].rva, (long long)c->rets[k].n);
        if (c->overflowRets)
            Dlog("      (+%d more distinct call sites not shown)", c->overflowRets);
    }
}

static void CountersReset(void)
{
    for (int i = 0; i < g_nCtr; i++)
    {
        g_ctr[i].n = 0; g_ctr[i].nRets = 0; g_ctr[i].overflowRets = 0;
        g_ctr[i].lastHit = 0;
        memset(g_ctr[i].rets, 0, sizeof(g_ctr[i].rets));
    }
    Dlog("counters reset");
}

// ---------------------------------------------------------- rate limiting
//
// Trace level 3 writes a line per event, and some of these events happen a
// hundred times in one drag of the track tool. A limiter per call site keeps
// that readable: the first N, then one in every M, then a total at the end.
struct RateLimit { volatile LONG64 seen; int first; int every; };

// Interlocked because a rate limiter on a hook that runs on the simulation
// thread is shared state like any other. A torn counter here costs a wrong
// trace line, but it is free to get right.
static bool RateSay(RateLimit* r)
{
    LONG64 n = InterlockedIncrement64(&r->seen);
    if (n <= r->first) return true;
    return (r->every > 0) && (n % r->every == 0);
}

// ============================================================ NAME CENSUS
//
// "Which of these objects are the ones I mean" is a question a disassembly
// answers slowly and a running game answers in ten seconds. A tally of the
// object names a hook actually saw, with a count and our verdict on each, turns
// "the classifier is wrong" from another reverse-engineering round into a
// one-line ini edit.
//
// Bounded and allocation-free: 64 names, 48 characters each. A name that does
// not fit is counted as "other".
#define TALLY_MAX  64
#define TALLY_NAME 48

struct NameTally
{
    char   name[TALLY_MAX][TALLY_NAME];
    LONG64 n[TALLY_MAX];
    int    verdict[TALLY_MAX];      // whatever the caller means by it
    int    used;
    LONG64 other;
    LONG64 unnamed;
};

// "Have I already seen this name" without recording it - so a caller can say
// something once per name even when the census itself is switched off.
static bool TallyHas(NameTally* t, const char* name)
{
    if (!name || !name[0]) return false;
    for (int i = 0; i < t->used; i++)
        if (!strcmp(t->name[i], name)) return true;
    return false;
}

static void TallyAdd(NameTally* t, const char* name, int verdict)
{
    if (!name || !name[0]) { t->unnamed++; return; }

    for (int i = 0; i < t->used; i++)
        if (!strcmp(t->name[i], name)) { t->n[i]++; t->verdict[i] = verdict; return; }

    if (t->used >= TALLY_MAX || strlen(name) >= TALLY_NAME) { t->other++; return; }

    strncpy_s(t->name[t->used], TALLY_NAME, name, _TRUNCATE);  // never strcpy_s:
    t->n[t->used] = 1;                                         // it __fastfails
    t->verdict[t->used] = verdict;                             // past SEH
    t->used++;
}

static void TallyDump(NameTally* t, const char* what,
                      const char* yes, const char* no)
{
    Dlog("  %s - %d distinct names:", what, t->used);
    for (int i = 0; i < t->used; i++)
        Dlog("     %-40s %8lld   %s", t->name[i], (long long)t->n[i],
             t->verdict[i] ? yes : no);
    if (t->other)   Dlog("     (%lld calls with a name too long or past the table)", (long long)t->other);
    if (t->unnamed) Dlog("     (%lld calls whose name could not be read at all)", (long long)t->unnamed);
    if (!t->used)
    {
        Dlog("     nothing yet. If this stays empty while the vehicles are moving,");
        Dlog("     the name offset is wrong - see the auto-discovery line above.");
    }
}

// -------------------------------------------------------------- heartbeat
//
// "Is the frame hook even running" is the second question after "did it
// install", and it is worth one line every few seconds at trace 1. Without it,
// a plugin whose render hook never fired looks exactly like a plugin whose
// feature is broken.
static ULONGLONG g_frames;

// ONE CLOCK PER CALLER, and it has to be, because this header is now included
// once into a DLL that has three parts each wanting their own heartbeat. With a
// single shared g_hbLast, whichever part asked first each interval got its line
// and the other two were told "not yet" - so two of the three looked dead.
//
// The slot is a function-local static in the CALLER, passed in. Each call site
// gets its own; there is nothing to register and nothing to size.
//
// g_frames is deliberately still shared: it counts frames, and there is one
// frame. It is bumped by the frame tick, not by asking about a heartbeat -
// three parts asking would otherwise count every frame three times.
static bool HeartbeatDueAt(ULONGLONG* last, int seconds)
{
    if (g_trace < 1 || seconds <= 0 || !last) return false;
    ULONGLONG now = GetTickCount64();
    if (*last && now - *last < (ULONGLONG)seconds * 1000) return false;
    *last = now;
    return true;
}

static void HeartbeatAt(ULONGLONG* last, int seconds, const char* extraFmt, ...)
{
    if (!HeartbeatDueAt(last, seconds)) return;

    char extra[256]; extra[0] = 0;
    if (extraFmt)
    {
        va_list ap; va_start(ap, extraFmt);
        _vsnprintf_s(extra, sizeof(extra), _TRUNCATE, extraFmt, ap);
        va_end(ap);
    }
    Dlog("  alive: %llu frames  %s", (unsigned long long)g_frames, extra);
}

// The form to use when the arguments cost something to build.
//
// `fmt` IS A SEPARATE PARAMETER ON PURPOSE. Written as `HEARTBEAT(secs, ...)`
// the format literal and its arguments both land inside __VA_ARGS__, so
// g_frames gets appended AFTER them while the format consumes it FIRST - every
// value in the line shifts one place and the log quietly reports nonsense.
// railfree spent a session insisting it had suppressed more collisions than it
// had seen, which is how this was noticed.
#define HEARTBEAT(secs, fmt, ...) do { static ULONGLONG hb_; if (HeartbeatDueAt(&hb_, secs)) \
    Dlog("  alive: %llu frames  " fmt, (unsigned long long)g_frames, ##__VA_ARGS__); } while (0)

// And the same for the function form, so a call site does not have to declare
// its own slot by hand.
#define HEARTBEAT_F(secs, fmt, ...) do { static ULONGLONG hb_; \
    HeartbeatAt(&hb_, secs, fmt, ##__VA_ARGS__); } while (0)

// --------------------------------------------------------- the build gate
static bool BuildStampRead(unsigned* got)
{
    *got = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_exeBase;
    if (!ReadablePtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    if (!ReadablePtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *got = nt->FileHeader.TimeDateStamp;
    return true;
}

// ------------------------------------------------------------- the header
static void DiagHeader(const char* tag, const char* version, const char* summary)
{
    g_tag = tag;
    g_lockReady = 1;          // TsmBind created g_lock before we got here
    SYSTEMTIME t; GetLocalTime(&t);
    Dlog("=====================================================================");
    Dlog(" %s %s - %s", tag, version, summary);
    Dlog(" %04d-%02d-%02d %02d:%02d:%02d   trace level %d",
         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_trace);
    Dlog("=====================================================================");
}

#endif // TSM_DIAG_H
