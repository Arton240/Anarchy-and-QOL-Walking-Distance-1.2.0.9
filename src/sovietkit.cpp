// sovietkit.cpp - three mods for Workers & Resources: Soviet Republic, in one
// DLL, with one ini, one log and one report.
//
//   anarchy    build it where you want it. Collision, slope, angle, terrain and
//              connection rules off on a key, and back on with the same key.
//   skins      recolour a building with the game's own spray-can and every one
//              of that type you place afterwards is built wearing that skin.
//   trackcrew  rail construction trains ignore signals and each other.
//
// WHY ONE PLUGIN AND NOT THREE. They were three, and three was worse in ways
// that were not obvious until they ran together:
//
//   * Each one patched C3D_TERRAIN::Render to get a frame callback. Three
//     detours chained through each other for one frame tick. Now there is one.
//   * Each one asked Windows for the foreground window and the state of Ctrl,
//     Alt and Shift, every frame, separately. GetAsyncKeyState is a kernel
//     transition; there were about thirty a frame between them. Now the three
//     keys are read ONCE and all seven modifier combinations are derived from
//     that, so the per-frame input cost is three calls no matter how many
//     hotkeys exist.
//   * Their hotkeys collided. All three wanted Ctrl+Alt+K for "reset counters"
//     and two wanted Ctrl+Alt+R, so whichever loaded first won and the others
//     silently did nothing. Now there is one report key and one reset key that
//     cover all three parts.
//   * Three logs to read, three inis to keep in step, three sets of build-stamp
//     checks printing the same line.
//
// WHAT IS SHARED AND WHAT IS NOT. The three parts keep their own code, their
// own offsets and their own settings - each lives in its own namespace below and
// its own [section] in the ini, and any of them can be switched off without
// affecting the others. What they share is the plumbing: the log, the counters,
// the site verification, the frame tick and the keyboard.
//
// Every part still refuses to do anything at all if the executable is not the
// build its offsets were read from.

#define SOVIETKIT_VERSION "1.4"
#define SK_INI "plugins\\sovietkit.ini"

#include "tesmio_plugin.h"
#include "tsm_diag.h"

// =========================================================================
//  shared: reading the ini
// =========================================================================
//
// One parser, one file. Hex is detected by hand because strtoul with base 0
// treats a leading zero as octal, and "08" is a perfectly ordinary thing to
// write in a config file.
static void SkStripInlineComment(char* v)
{
    if (!v) return;
    for (char* p = v; *p; p++)
        if ((*p == ';' || *p == '#') && p != v && (p[-1] == ' ' || p[-1] == '\t'))
        {
            *p = 0;
            while (p > v && (p[-1] == ' ' || p[-1] == '\t')) *--p = 0;
            return;
        }
}

static bool SkCfgRaw(const char* sec, const char* key, char* out, int n)
{
    if (!H->configString(SK_INI, sec, key, out, n, "")) return false;
    SkStripInlineComment(out); Trim(out);
    return out[0] != 0;
}

static unsigned __int64 SkCfgU(const char* sec, const char* key, unsigned __int64 fb)
{
    char v[64];
    if (!SkCfgRaw(sec, key, v, sizeof(v))) return fb;
    int base = 10; const char* p = v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    char* end = NULL;
    unsigned __int64 u = _strtoui64(p, &end, base);
    if (end == p) { Dlog("[%s] %s = \"%s\" is not a number - using %llu", sec, key, v, fb); return fb; }
    return u;
}

// HEX HERE TOO, and a check that the whole value was consumed.
//
// Until 1.2 this read base 10 only. `[keys] modifier = 0x4` therefore parsed as
// 0 - strtol eats the leading zero, stops at the 'x', and reports success - and
// 0 is a legal modifier meaning "no modifier at all". So one plausible typo, in
// a section where the neighbouring keys really are written in hex, silently
// turned every Ctrl+Alt binding into a bare letter, and the only evidence was
// the word "(no modifier)" in one startup line.
//
// `*end` is the other half: without it, "4abc" and "4;comment" both read as 4.
static int SkCfgI(const char* sec, const char* key, int fb, int lo, int hi)
{
    char v[64];
    if (!SkCfgRaw(sec, key, v, sizeof(v))) return fb;
    int base = 10; const char* p = v;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    char* end = NULL;
    long l = strtol(p, &end, base);
    if (end == p || *end)
    { Dlog("[%s] %s = \"%s\" is not a number - using %d", sec, key, v, fb); return fb; }
    if (neg) l = -l;
    if (l < lo || l > hi) { Dlog("[%s] %s = %ld is outside %d..%d - clamped", sec, key, l, lo, hi); l = (l < lo) ? lo : hi; }
    return (int)l;
}

static bool SkCfgB(const char* sec, const char* key, bool fb)
{
    return SkCfgI(sec, key, fb ? 1 : 0, 0, 1) != 0;
}

// =========================================================================
//  shared: the keyboard
// =========================================================================
//
// THE WHOLE POINT OF THIS BLOCK is that Ctrl, Alt and Shift are read exactly
// three times a frame and every hotkey in every part reads the answer out of a
// bitmask. Before the merge each plugin sampled them for itself, and one of
// them (skins 0.2) sampled them once PER BOUND KEY - eleven times a frame, six
// hundred and sixty times a second, to learn the same three facts.
//
// Modifiers are matched EXACTLY: Alt+F means Alt down with Ctrl and Shift UP.
// Without that, "Alt" also matches while Ctrl+Alt is held, and the report key
// fires the anarchy toggle on the way past.
static int  g_fgIsGame;
static HWND g_gameWnd;          // kept for the on-screen indicator's geometry

static void SkRefreshForeground(void)
{
    static HWND lastFg; static int lastIs;
    HWND fg = GetForegroundWindow();
    if (fg == lastFg) { g_fgIsGame = lastIs; return; }
    lastFg = fg;
    if (!fg) { lastIs = 0; g_fgIsGame = 0; return; }
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    lastIs = (pid == GetCurrentProcessId());
    g_fgIsGame = lastIs;
    if (lastIs) g_gameWnd = fg;
}

static bool SkKeyDown(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

//   0 = none (bare key)   1 = Ctrl   2 = Alt   3 = Shift
//   4 = Ctrl+Alt          5 = Ctrl+Shift       6 = Alt+Shift
#define SK_MOD_MAX 6
static unsigned g_modMask;

static void SkRefreshModifiers(void)
{
    if (!g_fgIsGame) { g_modMask = 0; return; }

    const bool c  = SkKeyDown(VK_CONTROL);
    const bool a  = SkKeyDown(VK_MENU);
    const bool sh = SkKeyDown(VK_SHIFT);

    unsigned m = 0;
    if (!c && !a && !sh) m |= 1u << 0;
    if ( c && !a && !sh) m |= 1u << 1;
    if (!c &&  a && !sh) m |= 1u << 2;
    if (!c && !a &&  sh) m |= 1u << 3;
    if ( c &&  a && !sh) m |= 1u << 4;
    if ( c && !a &&  sh) m |= 1u << 5;
    if (!c &&  a &&  sh) m |= 1u << 6;
    g_modMask = m;
}

static bool SkModHeld(int which)
{
    if (which < 0 || which > SK_MOD_MAX) which = 4;
    return ((g_modMask >> which) & 1u) != 0;
}

static const char* SkModifierName(int which)
{
    switch (which)
    {
        case 0: return "(no modifier)";
        case 1: return "Ctrl";
        case 2: return "Alt";
        case 3: return "Shift";
        case 5: return "Ctrl+Shift";
        case 6: return "Alt+Shift";
        default: return "Ctrl+Alt";
    }
}

static bool SkHotkeyEdge(int vk, int modifier, unsigned char* was)
{
    if (!vk) return false;
    bool down = SkModHeld(modifier) && SkKeyDown(vk);
    bool edge = down && !*was;
    *was = down ? 1 : 0;
    return edge;
}

// =========================================================================
//  shared: which parts are switched on
// =========================================================================
static int g_useAnarchy = 1;
static int g_useSkins   = 1;
static int g_useCrew    = 1;

// Set when a part refuses to install - the build stamp did not match, a hook
// was declined, its bytes had moved. A part that is off here is off for the
// session and its frame work is skipped entirely.
static int g_liveAnarchy, g_liveSkins, g_liveCrew;

// Shared hotkeys: one report, one reset, for all three parts.
static int g_sharedModifier = 4;
static int K_REPORT = 'R';
static int K_RESET  = 'K';

// The frame hook itself lives at the bottom, but the parts need to know it
// landed - skins used to install its own and still reports on it.
typedef void (*TerrainRenderFn)(void* self, bool a, void* c1, void* c2, int i1, int i2);
static TerrainRenderFn o_TerrainRender;
static int             g_renderHooked;

// =========================================================================
//  the three parts
// =========================================================================
//
// Each is the plugin it used to be, in a namespace, with four things taken
// away: its copy of the frame hook, its copy of the foreground and modifier
// sampling, its ini parser, and its DllMain. Everything else - the offsets,
// the hooks, the comments explaining why each one is shaped the way it is -
// is unchanged, because it works.
namespace anarchy {
#include "an_sites.h"
#include "mod_anarchy.inc"
}

namespace skins {
#include "skn_sites.h"
#include "skn_cfg.h"
#include "skn_skin.h"
#include "mod_skins.inc"
}

namespace crew {
#include "tc_sites.h"
#include "mod_trackcrew.inc"
}

// =========================================================================
//  the frame tick - ONE hook, three callbacks
// =========================================================================
static int g_frameOff;        // the whole kit gave up


static void SkReportAll(void)
{
    Dlog("==============================================================");
    Dlog("sovietkit %s - full report", SOVIETKIT_VERSION);
    Dlog("  parts: anarchy %s, skins %s, trackcrew %s",
         g_useAnarchy ? (g_liveAnarchy ? "on" : "ON BUT NOT INSTALLED") : "off",
         g_useSkins   ? (g_liveSkins   ? "on" : "ON BUT NOT INSTALLED") : "off",
         g_useCrew    ? (g_liveCrew    ? "on" : "ON BUT NOT INSTALLED") : "off");
    Dlog("  frame hook %d", g_renderHooked);
    if (g_liveAnarchy) anarchy::Report();
    if (g_liveSkins)   skins::Report();
    if (g_liveCrew)    crew::Report();

    // ONCE, not once per part. All three share one site table and one counter
    // table, so three parts each calling these printed the same forty lines
    // three times over.
    Dlog("--------------------------------------------------------------");
    SiteVerifyAll();
    CountersDump();
    if (!g_liveAnarchy && !g_liveSkins && !g_liveCrew)
    {
        Dlog("  nothing is installed, so there is nothing to report. The lines");
        Dlog("  above the first heartbeat say why.");
    }
    Dlog("==============================================================");
    DiagFlush();
}

// ONE GUARD PER PART, NOT ONE FOR ALL THREE.
//
// The obvious merge is a single __try around the three Frame() calls, and it is
// wrong in a way that only shows up when something goes bad: a fault in any one
// part unwinds the whole block, so the other two lose their frame AND all their
// hotkeys - the keys live inside each part's Frame(). One misbehaving part
// would take down the anarchy toggle, the skins keys and the trackcrew toggle
// with it. Three sequential guards (not nested - each returns before the next
// begins) cost three filter registrations a frame, which is nothing next to
// what they buy.
//
// Eight faults retires that part ALONE: its Panic() runs, its Frame() is never
// called again, and the other two carry on. Every part's Panic() is written to
// be callable from here - no locks, no logging, no allocation - because the
// thing that faulted may well have been the log.
static int g_partFaults[3];

static void SkPartFaulted(int which)
{
    if (which < 0 || which > 2) return;
    if (++g_partFaults[which] < 8) return;

    const char* name = (which == 0) ? "anarchy" : (which == 1) ? "skins" : "trackcrew";

    // THE FLAG IS CLEARED BEFORE THE CALL, not after it, and that ordering is
    // the whole point. Panic() writes to the game's code - a hundred and
    // twenty-four VirtualProtect-and-write pairs in anarchy's case - and any one
    // of them can fault. With the assignment after the call, a fault unwinds
    // past it, the part stays marked live, HudDraw keeps being called, the
    // report keeps printing "on" for a part that is internally dead, and the
    // all-parts-gone test below can never fire. Clearing first costs nothing and
    // makes the bad case the same as the good one.
    if (which == 0) g_liveAnarchy = 0;
    if (which == 1) g_liveSkins   = 0;
    if (which == 2) g_liveCrew    = 0;

    TSM_TRY
    {
        if (which == 0) anarchy::Panic();
        if (which == 1) skins::Panic();
        if (which == 2) crew::Panic();
    }
    TSM_EXCEPT("sovietkit part panic") { }

    // Logged out here, and in its own guard, because the fault may have been
    // inside the logger.
    TSM_TRY
    {
        Dlog("eight faulting frames in %s - that part has stood down. Its rules", name);
        Dlog("are off and anything it wrote into the game's code is back. The");
        Dlog("other parts are unaffected and still running.");
        DiagFlush();
    }
    TSM_EXCEPT("sovietkit part panic log") { }

    if (!g_liveAnarchy && !g_liveSkins && !g_liveCrew) g_frameOff = 1;
}

static void SkRunAnarchy(void) { TSM_TRY { anarchy::Frame(); } TSM_EXCEPT("anarchy frame")   { SkPartFaulted(0); } }
static void SkRunSkins(void)   { TSM_TRY { skins::Frame();   } TSM_EXCEPT("skins frame")     { SkPartFaulted(1); } }
static void SkRunCrew(void)    { TSM_TRY { crew::Frame();    } TSM_EXCEPT("trackcrew frame") { SkPartFaulted(2); } }

// THE HOOK LANDS BEFORE THE PARTS ARE STARTED, and that window is not short.
//
// TsmPluginStart patches C3D_TERRAIN::Render first - deliberately, so that a
// part which needs a frame tick to undo itself has one from its first
// instruction - and only then runs SiteVerifyAll() and the three ModStart()s,
// which between them do two dozen byte comparisons and a handful of synchronous
// log flushes. That is milliseconds: several frames at 60 Hz, during which the
// render thread was already calling into parts that had not started.
//
// It showed up as two real faults. Every SiteOk() in that window answers 0,
// because SiteVerifyAll() has not run yet - the exact bug that moving
// verification into Start was supposed to fix. And anarchy's layer-1 key is
// live in it: pressing Ctrl+Alt+T during startup flipped g_enabled, which
// anarchy::ModStart then read and turned into "enabled = 0 - nothing hooked"
// for the whole session.
//
// One flag, set as the last thing Start does.
static int g_started;

static void SkFrame(void)
{
    if (!g_started || g_frameOff) return;

    // THE THREE CALLS THE WHOLE KEYBOARD COSTS.
    SkRefreshForeground();
    SkRefreshModifiers();
    g_frames++;              // the one frame counter the heartbeats quote

    // The shared keys are read BEFORE the parts, so they survive a part that is
    // faulting every frame.
    static unsigned char wRep, wRes;
    if (SkHotkeyEdge(K_REPORT, g_sharedModifier, &wRep)) SkReportAll();
    if (SkHotkeyEdge(K_RESET,  g_sharedModifier, &wRes))
    {
        CountersReset();
        if (g_liveCrew) crew::ResetCensus();
        Dlog("counters zeroed - every part");
        DiagFlush();
    }

    if (g_liveAnarchy) SkRunAnarchy();
    if (g_liveSkins)   SkRunSkins();
    if (g_liveCrew)    SkRunCrew();
}

static void h_TerrainRender(void* self, bool a, void* c1, void* c2, int i1, int i2)
{
    // The parts guard themselves; this one catches the shared work around them
    // - the foreground check, the modifier sample, the two shared keys.
    TSM_TRY { SkFrame(); }
    TSM_EXCEPT("sovietkit frame")
    {
        static int faults;
        if (++faults == 8)
        {
            // ONE GUARD EACH, for the reason spelled out in SkPartFaulted: with
            // all three inside one __try, a fault in anarchy's restore loop
            // unwinds past skins::Panic() and crew::Panic() entirely, and the
            // log line below then says every part is off and everything is back
            // while trackcrew's four router bytes are still in the game and
            // there is no frame tick left to remove them. The flag goes first so
            // a fault cannot leave a dead part marked live.
            if (g_liveAnarchy) { g_liveAnarchy = 0; TSM_TRY { anarchy::Panic(); } TSM_EXCEPT("panic anarchy") {} }
            if (g_liveSkins)   { g_liveSkins   = 0; TSM_TRY { skins::Panic();   } TSM_EXCEPT("panic skins")   {} }
            if (g_liveCrew)    { g_liveCrew    = 0; TSM_TRY { crew::Panic();    } TSM_EXCEPT("panic crew")    {} }
            g_frameOff = 1;
            TSM_TRY
            {
                Dlog("eight faulting frames in sovietkit's own frame work - the whole");
                Dlog("kit has stood down. Every rule in every part is off and anything");
                Dlog("written into the game's code is back. The two inline hooks stay");
                Dlog("(they cannot be removed while the game runs) but nothing behind");
                Dlog("them does anything now.");
                DiagFlush();
            }
            TSM_EXCEPT("sovietkit panic log") { }
        }
    }

    o_TerrainRender(self, a, c1, c2, i1, i2);

    // After the terrain pass, so an immediate-mode font manager does not get
    // painted over. Only anarchy draws anything.
    if (g_started && g_liveAnarchy)
    {
        TSM_TRY { anarchy::HudDraw(); }
        TSM_EXCEPT("sovietkit indicator") { anarchy::HudKill(); }
    }
}

// =========================================================================
//  lifecycle
// =========================================================================
extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void) { return TSM_API_VERSION; }

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "sovietkit";
    info->version = SOVIETKIT_VERSION;

    g_log = TsmOpenLog("sovietkit.log");

    // The shared settings first, because the parts' own logging obeys them.
    g_useAnarchy = SkCfgB("sovietkit", "anarchy",   true);
    g_useSkins   = SkCfgB("sovietkit", "skins",     true);
    g_useCrew    = SkCfgB("sovietkit", "trackcrew", true);
    g_trace      = SkCfgI("debug", "trace", 0, 0, 3);

    g_sharedModifier = SkCfgI("keys", "modifier", 4, 0, SK_MOD_MAX);
    K_REPORT = (int)SkCfgU("keys", "report", 'R') & 0xFF;
    K_RESET  = (int)SkCfgU("keys", "reset",  'K') & 0xFF;
    // & 0xFF turns 0x100 into 0, and SkHotkeyEdge treats 0 as "no key at all".
    // Say so rather than leaving a dead key and one hex number in a startup line
    // as the only evidence.
    if (!K_REPORT) Dlog("[keys] report resolved to 0 - there is no report key this session");
    if (!K_RESET)  Dlog("[keys] reset resolved to 0 - there is no reset key this session");
    if (K_REPORT && K_REPORT == K_RESET)
        Dlog("[keys] report and reset are both %#x - one press will do both", K_REPORT);

    DiagHeader("sovietkit", SOVIETKIT_VERSION,
               "anarchy, skins and trackcrew in one plugin");
    Dlog("parts wanted: anarchy %d, skins %d, trackcrew %d",
         g_useAnarchy, g_useSkins, g_useCrew);


    if (g_useAnarchy) g_liveAnarchy = anarchy::ModInit() ? 1 : 0;
    if (g_useSkins)   g_liveSkins   = skins::ModInit()   ? 1 : 0;
    if (g_useCrew)    g_liveCrew    = crew::ModInit()    ? 1 : 0;

    Logf("sovietkit  %s loaded", SOVIETKIT_VERSION);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    // NOTHING WANTED, NOTHING HOOKED. With all three parts off in the ini there
    // is nothing for a frame tick to run, and installing one anyway would cost a
    // GetForegroundWindow, three GetAsyncKeyState and two hotkey edges every
    // frame forever for three parts that are switched off. "Switch it off in the
    // ini" has to switch the hook off too. Returning non-zero tells the loader
    // it may unload us, which is true: nothing is patched at this point.
    if (!g_useAnarchy && !g_useSkins && !g_useCrew)
    {
        Dlog("all three parts are off in the ini - nothing hooked, nothing patched.");
        DiagFlush();
        return 1;
    }

    // THE CLOCK FIRST, AND IT IS SHARED. Without it there is no heartbeat, no
    // hotkey, and - the part that matters - no way to switch anarchy's byte
    // edits back off. So nothing else installs until this lands.
    if (!g_renderHooked)
    {
        if (PatchIat(g_exe, DLL_ENGINE, "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z",
                     (void*)h_TerrainRender, (void**)&o_TerrainRender, "sovietkit frame"))
        {
            g_renderHooked = 1;

            // PIN THE MODULE, HERE, the instant unloading stops being survivable.
            //
            // From this line on the game's import table holds the address of a
            // function inside this DLL, and within a few more lines the parts
            // will have written bytes into the game's own code. FreeLibrary on
            // this module after that is a crash on the next frame - not a
            // leak, not a degraded mode, a crash - and DllMain cannot prevent
            // it: by the time DLL_PROCESS_DETACH arrives the decision has been
            // made, and its lpReserved parameter is the only thing that would
            // even tell us which kind of detach it is.
            //
            // Pinning here rather than in Init keeps the two "nothing was
            // hooked, unload me" paths (above and below) genuinely unloadable.
            {
                HMODULE self = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   (LPCSTR)(void*)&h_TerrainRender, &self);
            }
        }
        else
        {
            Dlog("could not patch C3D_TERRAIN::Render. There would be no frame tick");
            Dlog("and no hotkeys, which means no way to undo anything - so nothing");
            Dlog("is being installed at all.");
            g_liveAnarchy = g_liveSkins = g_liveCrew = 0;
            DiagFlush();
            // Non-zero, because it is TRUE here: no part has installed anything
            // (ModInit only reads the ini and registers sites), the IAT patch
            // just failed, and a dead DLL should not stay resident for the life
            // of the process.
            return 1;
        }
    }

    // VERIFY BEFORE ANYONE ASKS. SiteOk() reports a cached verdict and
    // SiteVerifyAll() is what computes it - so with no verification pass in
    // front of them, every SiteOk() in every part answers "no" and every hook
    // politely refuses itself. 1.0 of the merge did exactly that: the log said
    // "the bytes at exe+0x129f30 are not what this build expects" about bytes
    // that matched perfectly, two of anarchy's three rules never installed and
    // trackcrew never installed at all.
    //
    // It moved here when each part stopped judging itself by the SHARED
    // failure count (it was picking up the other parts' failures). Each part
    // still asks SiteOk about its own site by name - that part was right. What
    // was missing was anyone doing the reading.
    {
        int bad = SiteVerifyAll();
        if (bad) Dlog("  %d site(s) did not match. Each part decides below whether"
                      " that matters to it.", bad);
    }

    if (g_liveAnarchy) g_liveAnarchy = anarchy::ModStart() ? 1 : 0;
    if (g_liveSkins)   g_liveSkins   = skins::ModStart()   ? 1 : 0;
    if (g_liveCrew)    g_liveCrew    = crew::ModStart()    ? 1 : 0;

    Dlog("--------------------------------------------------------------");
    Dlog("ready: anarchy %s, skins %s, trackcrew %s",
         g_liveAnarchy ? "on" : (g_useAnarchy ? "FAILED" : "off"),
         g_liveSkins   ? "on" : (g_useSkins   ? "FAILED" : "off"),
         g_liveCrew    ? "on" : (g_useCrew    ? "FAILED" : "off"));
    Dlog("shared keys (%s): report %#x, zero counters %#x",
         SkModifierName(g_sharedModifier), K_REPORT, K_RESET);
    DiagFlush();

    // LAST. Everything above happens while the frame hook is already live; this
    // is what lets SkFrame stop returning immediately. See the comment on
    // g_started.
    g_started = 1;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    // NOTHING THAT TAKES A LOCK. DllMain runs under the loader lock, and on
    // process exit Windows has already killed every other thread - if one of
    // them died inside Dlog holding the log lock, taking it here deadlocks and
    // the game hangs on exit instead of closing.
    //
    // And no unpatching. Two inline splices cannot be removed while other
    // threads might be executing them; restoring everything else and leaving
    // those would turn "unloading this is fatal" into "unloading this looked
    // handled and is still fatal". If the process is exiting, none of it
    // matters anyway.
    if (reason == DLL_PROCESS_DETACH && g_log != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_log);
    return TRUE;
}
