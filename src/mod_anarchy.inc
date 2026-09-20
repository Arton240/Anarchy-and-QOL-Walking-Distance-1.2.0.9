// anarchy.cpp - build it where you want it.
//
// Workers & Resources refuses a lot of placements. Most of those refusals are
// the game protecting its own simulation and a few of them are just the game
// being fussy, and from inside the game there is no way to tell which is which.
// This plugin turns them off, on a key, all at once, and turns them back on the
// same way. What it does NOT do is pretend that is free - see "What this cannot
// promise" at the bottom.
//
// THREE LAYERS, and they are deliberately not the same size.
//
//   Layer 1 (this is railfree, renamed and widened). Three hooks on three
//   predicates - building collision, signal distance, construction overlap -
//   each of which lies only when the call came from inside a build tool.
//   Dragging track through a building works, everything else in the game is
//   untouched. This is what shipped as railfree 0.3 and what has been running
//   in a real save. As of 1.3 it answers to the same key as layer 2, because
//   "off" has to mean off; layer1_follows_key = 0 makes it permanent again.
//
//   Layer 2, on the same key. one hundred and fourteen single-byte edits, one
//   per "this cannot be built here" instruction in the two tool bodies. Slope,
//   angle, terrain, water, infrastructure, crossings, connection rules - all of
//   them. See an_sites.h for what the bytes are and why one byte is enough.
//
//   Layer 3 does not exist. There is no "and also ignore the map boundary":
//   that one check is the only thing between the build tools and memory the
//   game does not own, and it stays on unless you go and ask for it by name in
//   the ini, where it is labelled for what it is.
//
// WHY ONE BYTE MATTERS, and what that does and does not prove. Every edit in
// layer 2 changes exactly one byte of one instruction. That means no other
// thread can ever read a half-written value, and it means an edit can never
// move an instruction boundary - the two ways code patching usually goes wrong.
//
// It is NOT, on its own, a proof that cross-modifying code is safe: the Intel
// manual's one-byte relaxation is written for a processor patching its OWN
// instruction stream, and asks for a serialising handshake when one core
// rewrites code another core is fetching. What is true is that a one-byte
// opcode write from another thread is exactly what every debugger in the world
// does to set a breakpoint, all day, on this hardware. So: very well-trodden,
// not formally guaranteed, and the honest reason this is a key rather than a
// launch option is that pressing it back puts every byte where it was.
//
// The 114 writes are also not a transaction. They take tens of microseconds,
// and a validity pass running concurrently can see a mix of patched and
// unpatched checks within one evaluation. That costs a frame of odd previews
// and nothing else.
//
// WHAT THIS CANNOT PROMISE. A refusal usually means the simulation was not
// written for the thing you are about to do. Track through a building has a
// building inside it. A sewage pipe that runs uphill will be built and will not
// flow. A power line through a hill is a power line through a hill. The game
// will not crash on these - the checks are about placement, not memory - but it
// will not do anything clever with them either. Keep a backup save.

#define ANARCHY_VERSION "1.5"


// ---------------------------------------------------------------- settings
// volatile: toggled by a hotkey on the render thread, read by three detours on
// whatever thread the build tool runs on.
static volatile int g_enabled      = 1;   // layer 1
static volatile int g_anarchy      = 0;   // layer 2 - OFF until you press the key
static volatile int g_off;                // hard stop: the build gate failed

static int g_ignoreBuild    = 1;
static int g_ignoreSignals  = 1;
static int g_ignoreOverlap  = 1;
static int g_widenToBuildings = 1;  // layer 1 also covers the place-a-building tool
                                    // while anarchy is on
static int g_layer1Follows    = 1;  // the key turns layer 1 off as well as layer 2
static int g_doMessages       = 1;  // also silence the refusal the game records
static int g_doPipes          = 1;  // the pipe segment validator's own refusals
static int g_allowOutsideMap  = 0;  // the one check that is not ours to break
static int g_startOn          = 0;  // anarchy already on at load
static int g_debug          = 0;    // counters only; [anarchy] debug = 1 to diagnose
static int g_heartbeatSec   = 15;

// ---- the hot-path gates -----------------------------------------------------
//
// C3DCalcDistanceSq is the game's distance-squared primitive. Hooking an import
// is the cheapest thing to INSTALL and it is emphatically not the cheapest
// thing to RUN: all eighteen call sites in the executable now pay our detour
// and only four of them are the signal test. The first session logged nineteen
// million calls. So the flags each detour needs are folded into one byte,
// recomputed whenever a setting changes, and the common case is a single load
// and a branch before the return address is even fetched.
static volatile char g_sigActive;
static volatile char g_bldActive;
static volatile char g_ovlActive;
static volatile char g_wideActive;   // layer 1 covers every tool, not just the
                                     // shared drag-a-line one
static volatile int g_dispatchHooked;   // written in Frame(), read in the detours
static int g_wantToolHook;

// WHAT "OFF" MEANS, and why this changed in 1.3.
//
// Until now layer 1 was always on and only layer 2 answered to the key. The
// reasoning was that layer 1 is the conservative half - three predicates,
// scoped to the shared drag-a-line tool - and leaving it on is what made track
// go through a building without a key at all.
//
// It is still the conservative half, and it was still the wrong default,
// because 0x4FCAF0 is not the rail tool: it is rail AND roads AND pipes AND
// conveyors AND fences AND cableways. So "layer 1 only" is not a quiet
// exception for track. It is collision, signal proximity and construction
// overlap switched off for every single thing you drag, permanently, with the
// indicator dark and the log saying ANARCHY OFF. Press the key to turn it off,
// drag a pipe through a house, watch it go in: the mod is telling you it is off
// and the game is telling you it is on, and the game is right.
//
// So the key is now the whole switch. layer1_follows_key = 0 puts the old
// behaviour back for anyone who wants the drag tools permissive all the time.
static void RecomputeGates(void)
{
    int on = g_enabled && !g_off && (!g_layer1Follows || g_anarchy);
    g_bldActive  = (char)(on && g_ignoreBuild);
    g_sigActive  = (char)(on && g_ignoreSignals);
    g_ovlActive  = (char)(on && g_ignoreOverlap);
    g_wideActive = (char)(on && g_widenToBuildings && g_anarchy);
}

static int K_ANARCHY, K_TOGGLE;

// ---------------------------------------------------------------- counters
static int C_COLLIDE_ALL, C_COLLIDE_SUPPRESSED, C_COLLIDE_FOREIGN;
static int C_DIST_SUPPRESSED;
static int C_OVERLAP_ALL, C_OVERLAP_SUPPRESSED;

static RateLimit g_rlCollide = { 0, 5, 200 };
static RateLimit g_rlDist    = { 0, 5, 200 };
static RateLimit g_rlOverlap = { 0, 5, 200 };

// =========================================================================
//  scoping: which callers are we willing to lie to
// =========================================================================
//
// 0x4FCAF0 is not "the rail tool". It is THE tool: rail, road bridges, pipes,
// water, sewage, heating, conveyors, fences, cableways, airroads and power
// lines are all the same function with a different kind code, which is why it
// is a hundred kilobytes long. So one range covers everything you drag.
//
// The building placement tool at 0x2BADE0 is separate, and layer 1 only
// includes it while anarchy is on - otherwise placing a house would stop
// noticing the house next to it, which nobody asked for.
static unsigned g_lineLo = RVA_LINE_LO, g_lineHi = RVA_LINE_HI;
static unsigned g_bldLo  = RVA_BLD_LO,  g_bldHi  = RVA_BLD_HI;
static unsigned g_wireLo = RVA_WIRE_LO, g_wireHi = RVA_WIRE_HI;
static unsigned g_pipeLo = RVA_PIPE_LO, g_pipeHi = RVA_PIPE_HI;

static inline unsigned Rva(const void* p) { return (unsigned)((const BYTE*)p - g_exeBase); }

// IS THIS EVEN THE GAME'S EXECUTABLE. The subtraction above is 64-bit and the
// cast is not: a return address from C3DDLL64, from another plugin, or from
// anything more than 4 GB away wraps into a 32-bit number that can land inside
// one of the ranges below by coincidence. The hooks are inline hooks on the
// functions themselves, so every caller in the process reaches them - not just
// the game's. One compare fixes it, and g_exeSize was already there.
static inline bool InExe(const void* p)
{
    return (size_t)((const BYTE*)p - g_exeBase) < (size_t)g_exeSize;
}

// The line tool's own body and nothing else. Used by the install-time guard on
// the four signal call sites, which must stay as narrow as it was.
static inline bool InLineBody(unsigned rva)
{
    return rva >= g_lineLo && rva < g_lineHi;
}

static inline bool InLineTool(unsigned rva)
{
    // The line tool proper, plus the two validators it is the only caller of.
    // A fence is poles and spans, so it is validated by the WIRE half; a
    // pipe's slope and depth by the pipe half. Neither was in scope before 1.3,
    // and that is why layer 1 never lied for either of them.
    return (rva >= g_lineLo && rva < g_lineHi)
        || (rva >= g_wireLo && rva < g_wireHi)
        || (rva >= g_pipeLo && rva < g_pipeHi);
}

static inline bool InBuildingTool(unsigned rva)
{
    return rva >= g_bldLo && rva < g_bldHi;
}

// ---- "are we inside a build tool right now" --------------------------------
//
// Set for the duration of the tool dispatcher's call, and only honoured on the
// thread that set it - the dispatcher runs on the main thread, and a collision
// test arriving from the simulation thread at the same moment is not ours to
// answer. See an_sites.h for why this exists and what it covers that a return
// address cannot.
static volatile LONG  g_inTool;
static volatile DWORD g_inToolThread;

static inline bool InToolNow(void)
{
    return g_inTool != 0 && g_inToolThread == GetCurrentThreadId();
}

static inline bool InScope(const void* ret)
{
    if (!InExe(ret)) return false;
    unsigned rva = Rva(ret);

    // Always: the shared drag-a-line tool. This is the conservative half and it
    // is what makes track work with anarchy off.
    if (InLineTool(rva)) return true;

    if (!g_wideActive) return false;

    // With anarchy on: the building tool by address, and ANY tool at all by
    // the dispatcher flag. The flag is what makes second_road, factory
    // connections and road waypoints work - they have their own bodies and no
    // address list was ever going to keep up with them.
    if (InBuildingTool(rva)) return true;
    if (g_dispatchHooked && InToolNow()) return true;
    return false;
}

// =========================================================================
//  layer 1, rule 1 - "Building in the way"
// =========================================================================
typedef char (*CollideFn)(void* world, void* pos, int ignoreType, int ignoreInst,
                          char a5, void* a6, char a7, float radius);
static CollideFn o_Collide;
static int       g_collideHooked;

// The one thread fact worth knowing. Layer 2 writes to the game's code, and it
// is only self-evidently safe to do that while the game runs because every edit
// is one byte. Recording which thread the build tools run on costs a store on a
// path that already does more than that, and it turns "I believe this is the
// render thread" into something the log can state.
static volatile DWORD g_toolThread;
static DWORD          g_frameThread;

static char h_Collide(void* world, void* pos, int ignoreType, int ignoreInst,
                      char a5, void* a6, char a7, float radius)
{
    if (!g_bldActive) return o_Collide(world, pos, ignoreType, ignoreInst, a5, a6, a7, radius);

    // BumpFrom keeps a histogram of where the call came from, which walks up to
    // 24 slots. That is the right thing to pay while you are working out
    // whether a hook is wired up and the wrong thing to pay forever on the
    // simulation thread, so above trace 1 it is a plain counter.
    void* ret = _ReturnAddress();
    if (g_debug) { if (g_trace >= 2) BumpFrom(C_COLLIDE_ALL, ret); else Bump(C_COLLIDE_ALL); }

    if (InScope(ret))
    {
        // NOT behind g_debug: this is the one fact the report has about
        // threading, and debug goes off before release.
        if (!g_toolThread) g_toolThread = GetCurrentThreadId();
        if (g_debug)
        {
            Bump(C_COLLIDE_SUPPRESSED);
            if (g_trace >= 3 && RateSay(&g_rlCollide))
                TRACE(3, "  collide: suppressed for exe+%#08x (radius %.2f, #%lld)",
                      Rva(ret), (double)radius, (long long)g_rlCollide.seen);
        }
        return 0;                     // 0 = the spot is clear
    }

    if (g_debug) Bump(C_COLLIDE_FOREIGN);

    return o_Collide(world, pos, ignoreType, ignoreInst, a5, a6, a7, radius);
}

// =========================================================================
//  layer 1, rule 2 - "Semaphore/signals/waypoint in the way"
// =========================================================================
//
// The test is `if (radius*radius > distanceSq) refuse`, so a distance big
// enough makes every one of the four comparisons fall through. 1e30 rather than
// infinity: it survives the mulss chains at two of the sites without becoming a
// NaN, and a NaN would make comiss set the parity flag and take a branch nobody
// intended.
//
// HOW THIS IS ATTACHED, AND WHY IT CHANGED IN 1.1.
//
// C3DCalcDistanceSq is the game's distance-squared primitive and it is called
// about nineteen million times a session. 1.0 hooked its import slot, which is
// the cheapest thing to INSTALL and close to the most expensive thing to RUN:
// every one of its eighteen call sites paid a detour, a return-address fetch
// and a four-way compare, and only FOUR of those sites are the signal test. The
// other fourteen got nothing out of it and they are on the simulation thread.
//
// 1.1 leaves the import alone and edits the four call sites instead. Each is
//     ff 15 <rel32>      call qword ptr [rip + rel32]
// and the rel32 points at the import slot. We allocate one pointer-sized cell
// near the executable, put our detour's address in it, and rewrite those four
// rel32s to point at our cell. Nothing else in the game changes at all, and the
// detour no longer has to ask where it was called from - it can only have been
// called from one of the four.
//
// That is a four-byte write per site rather than one byte, which is why it is
// done ONCE at load, before the game has a simulation running, and is not part
// of what the anarchy key toggles.
#define AN_FAR 1.0e30f

typedef float (*DistSqFn)(const void* a, const void* b);
static DistSqFn o_DistSq;
static int      g_distHooked;
static int      g_distSites;

static unsigned g_distRet[4];

static float h_DistSq(const void* a, const void* b)
{
    // Every caller that reaches here IS a signal test - that is what the call
    // site redirection bought. One load, one branch, done.
    if (!g_sigActive) return o_DistSq(a, b);
    if (g_debug) Bump(C_DIST_SUPPRESSED);
    return AN_FAR;
}

// The cell the four call sites will point at. It must be within 2 GB of them,
// which is what AllocNear is for.
static void** g_distCell;

// What each redirected call site used to hold, so the edit is reversible. This
// matters more than it looks: unlike an IAT patch, the loader cannot undo a
// call-site edit for us, and a call site still pointing at a cell inside an
// unloaded DLL is a crash on the next distance calculation.
static BYTE* g_distCallAt[4];
static int   g_distCallOld[4];
static int   g_distCallN;

static void RestoreDistCalls(void)
{
    for (int i = 0; i < g_distCallN; i++)
    {
        BYTE* call = g_distCallAt[i];
        if (!call) continue;

        // The same rule ApplySites follows: only put back a byte that still
        // holds what we left there. If another mod redirected this call site
        // after us, restoring it would point it back at the import slot and
        // silently break them.
        if (call[0] != 0xFF || call[1] != 0x15) continue;
        __int64 ours = (__int64)((BYTE*)g_distCell - (call + 6));
        if (*(int*)(call + 2) != (int)ours) continue;

        DWORD old = 0;
        if (!VirtualProtect(call + 2, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *(int*)(call + 2) = g_distCallOld[i];
        DWORD back = 0; VirtualProtect(call + 2, 4, old, &back);
        FlushInstructionCache(GetCurrentProcess(), call, 6);

        // Cleared ONLY on success, and the count is left alone.
        //
        // Every `continue` above leaves a call site still pointing at
        // g_distCell, and g_distCell holds the address of h_DistSq - code inside
        // this DLL. Zeroing g_distCallN unconditionally, which is what this used
        // to do, threw away the record that a site was still ours, so nothing
        // could ever try again. That is precisely the crash the comment above
        // describes: a call site pointing into an unloaded module, on the next
        // distance calculation. ApplySites already follows this rule in the OFF
        // direction, for the same reason.
        g_distCallAt[i] = NULL;
    }
}

static bool RedirectDistCalls(void)
{
    g_distCell = (void**)AllocNear(g_exeBase, sizeof(void*));
    if (!g_distCell) { Dlog("signals: no memory near the exe - rule off"); return false; }

    // AllocNear promises executable memory, not writable memory. Ask for both
    // rather than assume, because the alternative is an access violation at
    // plugin start with no handler anywhere on the stack.
    {
        DWORD old = 0;
        if (!VirtualProtect(g_distCell, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        { Dlog("signals: the cell near the exe is not writable - rule off"); return false; }
    }
    *g_distCell = (void*)h_DistSq;

    void** iat = FindIatSlot(g_exe, DLL_ENGINE, SYM_DIST_SQ);
    if (!iat) { Dlog("signals: C3DCalcDistanceSq is not in the import table - rule off"); return false; }
    o_DistSq = (DistSqFn)*iat;

    int done = 0;
    for (int i = 0; i < 4; i++)
    {
        // g_distRet[i] is the instruction AFTER the call, so the call starts
        // six bytes earlier and its rel32 four bytes before that.
        // These four RVAs are overridable from the ini, and unlike 1.0 the
        // detour no longer checks at runtime where it was called from - that
        // check is what the redirect replaced. So the install-time test is the
        // ONLY guard, and it has to include "is this even in the build tool".
        // A mistyped dist_ret_N that happened to land on one of the other
        // fourteen callers would otherwise feed 1e30 to an unrelated subsystem
        // for the whole session, with nothing to switch it off.
        if (!InLineBody(g_distRet[i] - 6))
        {
            Dlog("signals: exe+%#x is outside the drag-tool body - left alone", g_distRet[i] - 6);
            continue;
        }
        BYTE* call = g_exeBase + g_distRet[i] - 6;
        if (!ReadablePtr(call, 6)) { Dlog("signals: exe+%#x is not readable", g_distRet[i] - 6); continue; }
        if (call[0] != 0xFF || call[1] != 0x15)
        {
            Dlog("signals: exe+%#x is %02X %02X, not an indirect call - left alone",
                 g_distRet[i] - 6, call[0], call[1]);
            continue;
        }
        int   rel = *(int*)(call + 2);
        void** slot = (void**)(call + 6 + rel);
        if (slot != iat)
        {
            Dlog("signals: exe+%#x calls through %p, not the import slot %p - left alone",
                 g_distRet[i] - 6, (void*)slot, (void*)iat);
            continue;
        }

        __int64 delta = (__int64)((BYTE*)g_distCell - (call + 6));
        if (delta > 2147483647LL || delta < -2147483648LL)
        {
            Dlog("signals: our cell is %lld bytes away, too far for a rel32", (long long)delta);
            continue;
        }

        DWORD old = 0;
        if (!VirtualProtect(call + 2, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        g_distCallAt[g_distCallN]  = call;
        g_distCallOld[g_distCallN] = rel;
        g_distCallN++;
        *(int*)(call + 2) = (int)delta;
        DWORD back = 0; VirtualProtect(call + 2, 4, old, &back);
        FlushInstructionCache(GetCurrentProcess(), call, 6);
        done++;
    }
    g_distSites = done;
    return done > 0;
}

// =========================================================================
//  layer 1, rule 3 - "Overlaps with existing construction!"
// =========================================================================
typedef char (*OverlapFn)(void* state, void* pos, float radius, int flag);
static OverlapFn o_Overlap;
static int       g_overlapHooked;

static char h_Overlap(void* state, void* pos, float radius, int flag)
{
    if (!g_ovlActive) return o_Overlap(state, pos, radius, flag);

    void* ret = _ReturnAddress();
    if (g_debug) { if (g_trace >= 2) BumpFrom(C_OVERLAP_ALL, ret); else Bump(C_OVERLAP_ALL); }

    if (InScope(ret))
    {
        if (g_debug)
        {
            Bump(C_OVERLAP_SUPPRESSED);
            if (g_trace >= 3 && RateSay(&g_rlOverlap))
                TRACE(3, "  overlap: suppressed for exe+%#08x (#%lld)",
                      Rva(ret), (long long)g_rlOverlap.seen);
        }
        return 0;
    }
    return o_Overlap(state, pos, radius, flag);
}

// =========================================================================
//  the tool dispatcher hook
// =========================================================================
//
// Attached by rewriting the rel32 of the ONE call instruction that reaches the
// dispatcher, exactly as the four distance call sites are. Four bytes, written
// once at load, verified first, and put back by RestoreToolCall.
//
// THE SIGNATURE, CHECKED RATHER THAN ASSUMED. Putting a C function in front of
// somebody else's call means getting three things right, and "it looks like a
// one-argument update function" is not evidence for any of them:
//
//   Arguments.  The caller sets rcx and nothing else in the seventy bytes
//               before the call. Forwarding four integer registers is free and
//               covers the function growing a parameter later.
//   Floats.     Nothing touches xmm0-3 on the caller's side, and the callee's
//               first use of xmm0 is at exe+0x30D344, where it is consuming the
//               RETURN VALUE of the call at 0x30D33E - not a live-in argument.
//               A C detour cannot forward xmm registers, so if this were wrong
//               no amount of extra parameters would have saved it.
//   Return.     The function has exactly one epilogue (exe+0x3178E9) and it is
//               five pops and a ret - nothing is loaded into eax. The caller's
//               next instruction is `lea rcx, [rip+...]`, so nothing reads it
//               either. Returning void here is safe; if it were not, the
//               InterlockedDecrement below would be silently handing the game
//               its own counter as a return value.
typedef void (*ToolUpdateFn)(void* a, void* b, void* c, void* d);
static ToolUpdateFn o_ToolUpdate;
static BYTE*  g_toolStub;
static BYTE*  g_toolCallAt;
static int    g_toolCallOld;

// __finally, NOT a plain statement after the call. __except unwinds without
// running anything on the way out, and so does a C++ throw: forty-three
// kilobytes of string comparison and allocation is not a place where "this
// never throws" is a safe assumption. A leaked increment would leave
// InToolNow() true for the rest of the session on the main thread, and with
// anarchy on that means every collision and overlap question from that thread
// is answered "clear" - build tool or not, forever, with nothing on screen to
// say so.
//
// ToolLeaveAll() below is the second belt: the frame tick clears the flag
// outright, so even if the __finally were somehow skipped the leak is bounded
// to one frame rather than one session.
static void h_ToolUpdate(void* a, void* b, void* c, void* d)
{
    g_inToolThread = GetCurrentThreadId();
    InterlockedIncrement(&g_inTool);
    TSM_TRY     { o_ToolUpdate(a, b, c, d); }
    TSM_FINALLY { InterlockedDecrement(&g_inTool); }
}

// Called once at the top of every frame, on the render thread. The dispatcher
// runs once per frame and has returned by then, so a non-zero count here means
// something unwound past the __finally.
// ONLY A FLAG THIS THREAD LEFT BEHIND.
//
// The plain store here races the InterlockedDecrement in the __finally. If the
// dispatcher is mid-call on another thread with the count at 1 and this runs,
// the decrement that follows takes it to -1, and InToolNow() tests != 0 - so
// that thread is now "inside a build tool" for everything it does, and stays
// that way until the next frame, or forever if the frame tick has stopped.
// Checking the thread id first makes that impossible: the only flag this
// function can clear is one this very thread left set, which is the only case
// it was written for.
static void ToolLeaveAll(void)
{
    if (g_inTool != 0 && g_inToolThread == GetCurrentThreadId())
    {
        g_inTool = 0;
        DiagOnce(4, "dispatcher: the in-tool flag was still set at the start of a"
                    " frame, so something unwound out of the tool update. Cleared."
                    " If this repeats, set widen_to_buildings = 0.");
    }
}

static bool HookToolCall(void)
{
    BYTE* call = g_exeBase + RVA_TOOL_CALL;
    if (!ReadablePtr(call, TOOL_CALL_LEN)) return false;
    if (memcmp(call, EXPECT_TOOL_CALL, TOOL_CALL_LEN) != 0)
    {
        char want[32], got[32];
        HexStr(EXPECT_TOOL_CALL, TOOL_CALL_LEN, want, sizeof(want));
        HexStr(call, TOOL_CALL_LEN, got, sizeof(got));
        Dlog("dispatcher: exe+%#x is %s, expected %s - not hooking it",
             RVA_TOOL_CALL, got, want);
        return false;
    }

    // A 14-byte jmp [rip+0] near the exe, so the 4-byte rel32 can reach us
    // however far away this DLL was loaded. jmp-through-memory rather than
    // mov rax/jmp rax, because it clobbers no register at all.
    g_toolStub = AllocNear(g_exeBase, 16);
    if (!g_toolStub) { Dlog("dispatcher: no memory near the exe - not hooking it"); return false; }
    {
        DWORD old = 0;
        if (!VirtualProtect(g_toolStub, 16, PAGE_EXECUTE_READWRITE, &old))
        { Dlog("dispatcher: the stub is not writable - not hooking it"); return false; }
    }
    g_toolStub[0] = 0xFF; g_toolStub[1] = 0x25;
    *(int*)(g_toolStub + 2) = 0;                          // jmp qword ptr [rip+0]
    *(void**)(g_toolStub + 6) = (void*)h_ToolUpdate;
    FlushInstructionCache(GetCurrentProcess(), g_toolStub, 16);

    o_ToolUpdate = (ToolUpdateFn)(g_exeBase + RVA_TOOL_UPDATE);

    __int64 delta = (__int64)(g_toolStub - (call + 5));
    if (delta > 2147483647LL || delta < -2147483648LL)
    { Dlog("dispatcher: the stub is %lld bytes away, too far for a rel32", (long long)delta); return false; }

    DWORD old = 0;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    g_toolCallAt  = call;
    g_toolCallOld = *(int*)(call + 1);
    *(int*)(call + 1) = (int)delta;
    DWORD back = 0; VirtualProtect(call + 1, 4, old, &back);
    FlushInstructionCache(GetCurrentProcess(), call, TOOL_CALL_LEN);
    return true;
}

static void RestoreToolCall(void)
{
    BYTE* call = g_toolCallAt;
    if (!call || !g_toolStub) return;
    if (call[0] != 0xE8) { g_toolCallAt = NULL; return; }
    if (*(int*)(call + 1) != (int)(__int64)(g_toolStub - (call + 5))) { g_toolCallAt = NULL; return; }

    DWORD old = 0;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *(int*)(call + 1) = g_toolCallOld;
    DWORD back = 0; VirtualProtect(call + 1, 4, old, &back);
    FlushInstructionCache(GetCurrentProcess(), call, TOOL_CALL_LEN);
    g_toolCallAt = NULL;
    // NOT g_inTool = 0 unconditionally: if a dispatcher call were somehow in
    // flight its __finally would take the count to -1 and InToolNow() would be
    // permanently true. Let the flag drain on its own; ToolLeaveAll clears it
    // at the next frame anyway.
}

// =========================================================================
//  layer 2 - the hundred and fourteen bytes
// =========================================================================
//
// Each site is one instruction that says "this placement is not allowed".
// AnPatchOffset picks which of its bytes carries that meaning; `after` is what
// it becomes. Everything else about the instruction is left exactly as it was,
// which is why this can be undone perfectly: we put the original byte back.
//
// The verification is the point. Before the first write, every site is compared
// against the bytes recorded in an_sites.h. A site that does not match is
// marked dead, never written to, and named in the log. A game update therefore
// turns this plugin off one site at a time instead of turning the game into
// confetti.
//
// The one asymmetry is deliberate and lives in ApplySites: a site that goes
// wrong on the way OUT is not retired, because our byte is still in the game's
// code and retiring it would mean nothing could ever put it back.

struct SiteState
{
    unsigned char ok;        // bytes matched at startup
    unsigned char on;        // currently patched
    unsigned char orig;      // the byte we took out
    unsigned char contested; // somebody else owns this byte now - re-read it
                             // before trusting `on` in either direction
    unsigned char saidStuck; // already counted in g_sitesStranded
};
static SiteState g_ss[AN_SITE_COUNT];
static int g_sitesOk, g_sitesDead, g_sitesOn, g_sitesWanted, g_sitesStranded;
static int g_verified;

// Which families of refusal are in play. These exist so that if something does
// go wrong you can halve the problem: turn off the drag tools and keep the
// buildings, or the other way round, and find out which family it was.
static int g_doLines = 1, g_doWires = 1, g_doBuildings = 1;

// Is this site one we are willing to touch at all?
static bool SiteWanted(const AnSite* s)
{
    if (s->risky && !g_allowOutsideMap) return false;
    if (s->kind == 4 && !g_doMessages)   return false;
    if (s->owner == 0 && !g_doLines)     return false;
    if (s->owner == 1 && !g_doWires)     return false;
    if (s->owner == 2 && !g_doBuildings) return false;
    if (s->owner == 3 && !g_doPipes)     return false;
    return true;
}

static void RecountWanted(void)
{
    g_sitesWanted = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
        if (g_ss[i].ok && SiteWanted(&AN_SITES[i])) g_sitesWanted++;
}

static bool VerifySites(void)
{
    g_sitesOk = g_sitesDead = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
    {
        const AnSite* s = &AN_SITES[i];
        BYTE* p = g_exeBase + s->rva;
        SiteState* st = &g_ss[i];
        st->on = 0;

        // an_sites.h is GENERATED, and a regenerated table is exactly when a
        // bad row would appear. AnPatchOffset(len - 1) on a zero length is a
        // write four gigabytes away, and a length past 8 reads off the end of
        // expect[] and lands in the NEXT instruction. Neither is allowed to
        // reach VirtualProtect.
        // sizeof(expect), not a literal: the record widened to 12 bytes in 1.5
        // so that the eleven-byte `mov dword ptr [r13+0x31c], imm32` form fits,
        // and a hard-coded 8 here would have retired every one of those rows
        // as "a nonsense row" - which is exactly how the kind-2 gate spent a
        // whole version doing nothing.
        if (s->len < 2 || s->len > (unsigned char)sizeof(s->expect)
            || s->kind > AN_KIND_MAX || AnPatchOffset(s) >= s->len)
        {
            st->ok = 0; g_sitesDead++;
            Dlog("  site %2d exe+%#08x has a nonsense row (len %u, kind %u) - refused",
                 i, s->rva, (unsigned)s->len, (unsigned)s->kind);
            continue;
        }
        if (!ReadablePtr(p, s->len)) { st->ok = 0; g_sitesDead++; continue; }

        bool match = (memcmp(p, s->expect, s->len) == 0);
        st->ok = match ? 1 : 0;
        st->orig = s->expect[AnPatchOffset(s)];
        if (match) g_sitesOk++;
        else
        {
            g_sitesDead++;
            char want[32], got[32];
            HexStr(s->expect, s->len, want, sizeof(want));
            HexStr(p, s->len, got, sizeof(got));
            Dlog("  site %2d exe+%#08x does not match: expected %s, found %s%s%s",
                 i, s->rva, want, got,
                 s->text[0] ? "   - " : "", s->text);
        }
    }
    RecountWanted();
    g_verified = 1;
    return g_sitesDead == 0;
}

// The write itself. VirtualProtect is taken and dropped around each byte rather
// than left open, so .text is writable for the few microseconds it takes to
// store one byte and not for the rest of the session.
static int g_protectStuck;      // a page we could not put back to read-execute

static bool PokeByte(BYTE* p, BYTE value)
{
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = value;                    // one byte: nothing can read half of it
    DWORD back = 0;
    // VirtualProtect works a page at a time, so this window is 4 KiB of .text
    // writable, not one byte. Failing to close it again is worth knowing about
    // - it means somebody else changed the protection under us.
    if (!VirtualProtect(p, 1, old, &back)) g_protectStuck++;
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return true;
}

static int ApplySites(int on)
{
    int changed = 0, refused = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
    {
        const AnSite* s = &AN_SITES[i];
        SiteState* st = &g_ss[i];
        if (!st->ok) continue;
        if (on && !SiteWanted(s)) continue;

        BYTE* p = g_exeBase + s->rva + AnPatchOffset(s);

        // A CONTESTED SITE IS RE-READ RATHER THAN TRUSTED.
        //
        // If an OFF pass found a third value at a site - neither our byte nor
        // the original, because some other mod wrote there - it left `on` set,
        // which is the right call: our byte may still be underneath and
        // retiring it would strand it. But the next ON pass then saw
        // `st->on == 1 == on` and skipped the site forever, and g_sitesOn went
        // on counting it as ours. Stable, wrong, and flattering. So: clear the
        // mark by looking, not by assuming.
        if (st->contested)
        {
            st->contested = 0;
            if      (*p == s->after) st->on = 1;
            else if (*p == st->orig) st->on = 0;
            else                     { st->ok = 0; continue; }   // still not ours
        }

        if (st->on == (unsigned char)(on ? 1 : 0)) continue;

        BYTE want  = on ? s->after : st->orig;
        BYTE isnow = on ? st->orig : s->after;

        // Somebody else changed this byte while we were not looking - another
        // mod, or us having got the offset wrong. Either way it is not ours to
        // overwrite.
        //
        // WHICH DIRECTION MATTERS. Going ON, nothing of ours is in the code
        // yet, so the site is simply retired. Going OFF, OUR byte is still
        // there: retiring it would strand it in the game's code with no path
        // back, so the site stays live and every later toggle tries again.
        if (*p != isnow)
        {
            if (refused < 8)
                Dlog("  site %d exe+%#08x now holds %02X, expected %02X - left alone",
                     i, s->rva, *p, isnow);
            refused++;
            if (on) st->ok = 0;
            else
            {
                st->contested = 1;
                // Per SITE, not per attempt. This used to increment on every
                // failed restore, so pressing the key four times with one
                // stubborn site reported four stranded sites.
                if (!st->saidStuck) { st->saidStuck = 1; g_sitesStranded++; }
            }
            continue;
        }

        if (PokeByte(p, want)) { st->on = (unsigned char)(on ? 1 : 0); changed++; }
        else
        {
            // refused++ OUTSIDE the log guard, unlike before: with it inside,
            // every VirtualProtect failure past the eighth was neither logged
            // nor counted nor included in the "and %d more" line. It simply
            // vanished.
            if (refused < 8) Dlog("  site %d exe+%#08x: VirtualProtect refused", i, s->rva);
            refused++;
        }
    }
    if (refused > 8) Dlog("  ...and %d more sites left alone", refused - 8);

    // ALL THREE TOTALS, not just g_sitesOn. Retiring a site at runtime (st->ok
    // = 0 above) used to leave g_sitesOk and g_sitesDead at whatever VerifySites
    // computed once at startup, so the report's "%d verified, %d unusable"
    // over-counted the first, under-counted the second, and the two stopped
    // summing to the table size.
    g_sitesOn = g_sitesOk = g_sitesDead = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
    {
        if (g_ss[i].on) g_sitesOn++;
        if (g_ss[i].ok) g_sitesOk++; else g_sitesDead++;
    }
    RecountWanted();            // retiring a site changes the denominator too
    if (!on && g_sitesOn)
        Dlog("  WARNING: %d site(s) could not be restored and are still patched."
             " Press the key again; if it does not clear, restart the game.", g_sitesOn);
    return changed;
}

static void SetAnarchy(int on)
{
    if (g_off) return;
    if (!g_verified) return;

    // THE MODE FLAG IS NEVER OPTIMISTIC ABOUT BEING OFF.
    //
    // ApplySites can fault - it is a hundred and twenty-four
    // VirtualProtect-and-write pairs - and the caller's guard swallows it, which
    // means the two lines after it may never run. With the assignment after the
    // call in both directions, a fault on the way ON left N checks disabled in
    // the game's code, g_anarchy at 0, the indicator dark and the report saying
    // ANARCHY IS OFF: the mod claiming off while the game is on, which is the
    // one failure this file spends its first page arguing against.
    //
    // So: going ON, say so first. Going OFF, say so after, because until the
    // bytes are actually back the honest answer is still "on".
    int changed;
    if (on) { g_anarchy = 1; RecomputeGates(); changed = ApplySites(1); }
    else    { changed = ApplySites(0); g_anarchy = 0; RecomputeGates(); }

    if (on)
    {
        Dlog("ANARCHY ON - %d of %d refusals switched off (%d changed now)",
             g_sitesOn, g_sitesWanted, changed);
        if (!g_allowOutsideMap)
            Dlog("  the map boundary check is still on - the only refusal we"
                 " found that guards an index rather than taste.");
        Dlog("  place what you like. Press the same key to put it all back.");
    }
    else
    {
        Dlog("ANARCHY OFF - every byte restored (%d changed back, %d still patched)",
             changed, g_sitesOn);
        if (g_layer1Follows)
            Dlog("  and layer 1 with it: collision, signal proximity and overlap"
                 " are back on for every tool. The game is refusing again.");
        else
            Dlog("  NOTE: layer1_follows_key = 0, so the three predicate hooks are"
                 " STILL ON. Everything you drag - track, road, pipe, conveyor,"
                 " fence, cableway - still ignores buildings, signals and overlap."
                 " Set it back to 1 if you wanted the key to be the whole switch.");
    }
    DiagFlush();
}

// =========================================================================
//  the on-screen indicator
// =========================================================================
//
// Anarchy is a mode, and a mode you cannot see is a mode you forget you are in.
// So while it is on there is a line of text on screen, the way the Cities:
// Skylines mod does it.
//
// It is drawn with the game's own font manager - the same call its own HUD
// uses - rather than anything of ours, so it matches the game's font and
// scaling and costs one call a frame. If the font pointer is not there yet, or
// the call faults, the indicator switches itself off permanently and says so;
// it is a label, and a label is never worth a crash.
typedef void (*PrintLeftFn)(void* self, void* font, float x, float y,
                            unsigned colour, const wchar_t* text);
static PrintLeftFn o_PrintLeft;
static int      g_hudReady;
static int      g_hudDead;
static int      g_hudDrawn;      // it has actually been on screen at least once
static int      g_hudSaid;
static int      g_showIndicator = 1;
static int      g_hudFromRight = 1;          // measure x from the right edge
static float    g_hudX = 200.0f, g_hudY = 120.0f;
static unsigned g_hudColour = 0xFFFF3020;      // the game writes 0xFF990000 here
static wchar_t  g_hudText[64] = L"ANARCHY";

static void HudResolve(void)
{
    void** slot = FindIatSlot(g_exe, DLL_ENGINE, SYM_PRINT_LEFT);
    if (!slot) { g_hudDead = 1; Dlog("indicator: PrintLeftUnicodeNoArg is not imported - no on-screen text"); return; }
    o_PrintLeft = (PrintLeftFn)*slot;
    g_hudReady = 1;
}

// WHERE THE TEXT GOES. PrintLeftUnicodeNoArg draws LEFT-aligned from the x it
// is given, and the font manager will not tell us how wide a string is without
// more reverse engineering than a label deserves. So "put it on the right" is
// done by measuring x from the right edge of the game's client area and leaving
// the margin as a setting: x = 200 means "start 200 pixels in from the right",
// and if the word does not sit where you want it, that is one number in the ini
// rather than a rebuild.
//
// GetClientRect and not GetSystemMetrics, because the two disagree in windowed
// mode and the text would sit off the edge of the window. Sampled once a second
// rather than once a frame - it is a cheap call, but nothing about a window
// size needs 120 Hz.
static float g_hudRightEdge;

static void RefreshHudGeometry(void)
{
    if (!g_hudFromRight) return;
    if (g_frames % 120 != 0 && g_hudRightEdge > 0.0f) return;
    if (!g_gameWnd) return;

    RECT rc;
    if (!GetClientRect(g_gameWnd, &rc)) return;
    float w = (float)(rc.right - rc.left);
    if (w > 0.0f) g_hudRightEdge = w;
}

static void HudKill(void) { g_hudDead = 1; }

static void HudDraw(void)
{
    if (!g_hudReady || g_hudDead || !g_showIndicator || !g_anarchy) return;

    float x = g_hudX;
    if (g_hudFromRight)
    {
        RefreshHudGeometry();
        if (g_hudRightEdge <= 0.0f) return;   // no window size yet - next frame
        x = g_hudRightEdge - g_hudX;
        if (x < 0.0f) x = 0.0f;
    }

    // The font pointer is filled in when the game builds its HUD, so it is null
    // for the first frames and during a load. That is not an error, just "not
    // yet" - come back next frame. exe+0x994208 is a SLOT holding a C3D_FONT*,
    // not the font object: the game reads it with `mov rdx, [rip+...]` at
    // exe+0x7373AF, a load and not a lea.
    // ReadablePtr ON THE SLOT ITSELF, not just a null test on what it holds.
    //
    // RVA_FONT_SLOT and RVA_FONTMGR are the two addresses in this file that are
    // NOT byte-verified - there is no instruction to compare, only a pointer to
    // read - so the build-stamp gate is all that stands behind them. And
    // build_stamp is overridable from the ini, which is a reasonable thing to
    // offer for the 124 sites (they verify themselves) and not for these two.
    // On a different build this reads whatever happens to live at that address.
    // The caller's __except would catch an unmapped read; it would not catch a
    // plausible-looking garbage pointer being handed to the font manager.
    if (!ReadablePtr(g_exeBase + RVA_FONT_SLOT, sizeof(void*))) { g_hudDead = 1; return; }
    void* font = *(void**)(g_exeBase + RVA_FONT_SLOT);
    if (!font || !ReadablePtr(font, sizeof(void*))) return;

    // NO TSM_TRY AROUND THE CALL ITSELF, and that is deliberate. __except
    // unwinds without running anything, so catching a fault inside C3DDLL64
    // would leave whatever the font manager was holding - a glyph cache lock, a
    // half-appended batch - stuck in that state on the render thread, which is
    // the frozen-simulation failure this file argues against in the panic path.
    // A label is not worth that trade. The caller wraps this, so a fault takes
    // the indicator down; it does not get to pretend the engine is fine.
    o_PrintLeft(g_exeBase + RVA_FONTMGR, font, x, g_hudY, g_hudColour, g_hudText);
    g_hudDrawn = 1;
}

// =========================================================================
//  the heartbeat, the hotkeys and the report
// =========================================================================

// The foreground check, the modifier sampling and the hotkey edge detector all
// moved up to sovietkit.cpp when the three plugins became one: they were three
// identical copies asking Windows the same questions every frame. What is left
// here is which modifier each key wants.
//   0 none   1 Ctrl   2 Alt   3 Shift   4 Ctrl+Alt   5 Ctrl+Shift   6 Alt+Shift
static int  g_modifier   = 4;   // the layer-1 key
static int  g_anModifier = 2;   // the anarchy key: Alt

static void Report(void)
{
    Dlog("--------------------------------------------------------------");
    Dlog("anarchy %s report", ANARCHY_VERSION);
    Dlog("  ANARCHY IS %s", g_anarchy ? "ON" : "OFF");
    Dlog("  layer 1: enabled %d  buildings %d  signals %d  overlap %d  widened %d",
         g_enabled, g_ignoreBuild, g_ignoreSignals, g_ignoreOverlap, (int)g_wideActive);
    Dlog("  layer 1 right now: collision %s, signals %s, overlap %s  (follows the key: %s)",
         g_bldActive ? "LYING" : "honest", g_sigActive ? "LYING" : "honest",
         g_ovlActive ? "LYING" : "honest", g_layer1Follows ? "yes" : "NO");
    Dlog("  hooks: collide %d, distance %d, overlap %d, dispatcher %d",
         g_collideHooked, g_distHooked, g_overlapHooked, g_dispatchHooked);
    // The raw values, not InToolNow(). This runs on the render thread and the
    // dispatcher has returned by report time, so the predicate could only ever
    // print "no" and was useless as a diagnostic. What is worth knowing is
    // whether the count is stuck and which thread owns it.
    Dlog("  dispatcher flag: count %ld, owner thread %lu (this is thread %lu)",
         (long)g_inTool, (unsigned long)g_inToolThread,
         (unsigned long)GetCurrentThreadId());
    Dlog("  layer 2: %d sites verified, %d unusable, %d in scope, %d currently patched",
         g_sitesOk, g_sitesDead, g_sitesWanted, g_sitesOn);
    Dlog("  layer 2 families: lines %d, wires %d, buildings %d, outside-map %d",
         g_doLines, g_doWires, g_doBuildings, g_allowOutsideMap);
    Dlog("  signal call sites redirected: %d of 4", g_distSites);
    Dlog("  indicator: ready %d, dead %d, shown %d", g_hudReady, g_hudDead, g_showIndicator);
    if (g_sitesStranded)
        Dlog("  %d site(s) have failed to restore at least once - something else"
             " in the process is writing to the same bytes", g_sitesStranded);
    if (g_protectStuck)
        Dlog("  %d page protection(s) could not be put back to read-execute",
             g_protectStuck);
    Dlog("  the drag-a-line tool: exe+%#08x .. exe+%#08x", g_lineLo, g_lineHi);
    Dlog("  the building tool:    exe+%#08x .. exe+%#08x", g_bldLo, g_bldHi);
    Dlog("  build tools ran on thread %lu, this frame is thread %lu%s",
         (unsigned long)g_toolThread, (unsigned long)g_frameThread,
         (g_toolThread && g_toolThread == g_frameThread) ? " (same)" : "");
    Dlog("  how to read this:");
    Dlog("    collide/overlap calls whose return address is OUTSIDE the tool");
    Dlog("    bodies are the rest of the game and are never touched. If");
    Dlog("    'suppressed' is 0 while you are dragging track, the hook is");
    Dlog("    installed but the game is not reaching it.");
    Dlog("    'signal distance faked' only counts the four redirected call");
    Dlog("    sites, so 0 while you drag track past a signal means either the");
    Dlog("    redirect did not take (see 'redirected: N of 4') or the rule is off.");
    Dlog("    the return-address histogram needs trace 2; at trace 1 the counters");
    Dlog("    above are totals only, which is the cheap setting on purpose.");
    Dlog("    'sites currently patched' is layer 2. It should be 0 with anarchy");
    Dlog("    off and %d with it on.", g_sitesWanted);
    Dlog("--------------------------------------------------------------");
    DiagFlush();
}

static void ListRefusals(void)
{
    Dlog("  the refusals anarchy switches off, in the game's own words:");
    int shown = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
    {
        const AnSite* s = &AN_SITES[i];
        if (!s->text[0]) continue;
        // Dedupe on the message id, but ONLY among rows that have one - the
        // gate row carries a description and no id, and keying on id alone
        // collapsed it into the first of the forty-three anonymous rows, which
        // is how the single most consequential entry in the table managed not
        // to appear in the list of what anarchy switches off.
        bool dup = false;
        if (s->msg)
            for (int j = 0; j < i; j++)
                if (AN_SITES[j].msg && AN_SITES[j].msg == s->msg) { dup = true; break; }
        if (dup) continue;
        Dlog("     %-5d %s%s", s->msg, s->text, s->risky ? "   [LEFT ON - guards the map]" : "");
        shown++;
    }
    int named = 0, unnamed = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++)
        if (AN_SITES[i].msg) named++; else unnamed++;
    Dlog("     %d lines above, covering %d of the %d sites; the other %d could not be",
         shown, named, AN_SITE_COUNT, unnamed);
    Dlog("     paired with a message - their string is loaded through a register");
    Dlog("     we did not follow. They are switched off too, just anonymously.");
    Dlog("     If something goes wrong, `lines`, `wires` and `buildings` in the");
    Dlog("     ini split the list three ways so you can find out which half.");
}

static bool HookToolCall(void);

static void Frame(void)
{
    if (g_off) return;
    g_frameThread = GetCurrentThreadId();
    ToolLeaveAll();

    // THE FOUR-BYTE SPLICE HAPPENS HERE, on the first frame, and not during
    // startup. Every other edit this plugin makes is one byte, which is the
    // whole reason it can be made while the game runs; this one is four, into
    // the middle of a live `call` in the per-frame UI path. Doing it from the
    // loader thread while the main thread might already be in that loop is the
    // one case the one-byte argument does not cover. Doing it HERE means it is
    // written by the same thread that executes it, between frames.
    if (g_wantToolHook && !g_dispatchHooked)
    {
        g_wantToolHook = 0;
        if (HookToolCall())
        {
            g_dispatchHooked = 1;
            Dlog("dispatcher: exe+%#x now runs through us, so while anarchy is on"
                 " EVERY build tool is in scope - including the ones with their"
                 " own body (second_road, factory connections, road waypoints)",
                 RVA_TOOL_CALL);
            DiagFlush();
        }
    }

    // Said out here, not in the fault handler: the handler may be running
    // because logging is what faulted.
    if (g_hudDead && !g_hudSaid)
    {
        g_hudSaid = 1;
        Dlog("indicator: the font manager call faulted, so the on-screen text is"
             " off for the rest of the session. Nothing else is affected.");
        DiagFlush();
    }

    static unsigned char wAn, wTog;
    if (SkHotkeyEdge(K_ANARCHY, g_anModifier, &wAn))
        SetAnarchy(!g_anarchy);

    if (SkHotkeyEdge(K_TOGGLE, g_modifier, &wTog))
    {
        // TURNING LAYER 1 OFF TURNS LAYER 2 OFF TOO. This is the key somebody
        // reaches for when something has gone strange mid-build, and a message
        // saying OFF while a hundred checks are still disabled in the game's
        // code would be a lie at the worst possible moment.
        if (g_enabled && g_anarchy) SetAnarchy(0);
        g_enabled = !g_enabled;
        RecomputeGates();
        Dlog("layer 1 %s (collision, signals, overlap)%s",
             g_enabled ? "ON" : "OFF",
             g_enabled ? "" : " - and anarchy with it");
        DiagFlush();
    }

    if (g_debug)
        HEARTBEAT(g_heartbeatSec,
                  "[anarchy] %s  collide %lld (%lld suppressed)  signal %lld  overlap %lld (%lld suppressed)",
                  g_anarchy ? "ON" : "off",
                  (long long)g_ctr[C_COLLIDE_ALL].n,
                  (long long)g_ctr[C_COLLIDE_SUPPRESSED].n,
                  (long long)g_ctr[C_DIST_SUPPRESSED].n,
                  (long long)g_ctr[C_OVERLAP_ALL].n,
                  (long long)g_ctr[C_OVERLAP_SUPPRESSED].n);
}

// THE RECOVERY PATH, and it is written to the rules of the worst case it can
// be called in.
//
// __except unwinds without running anything on the way out, so if the fault
// happened inside Dlog between its EnterCriticalSection and its matching Leave,
// the log lock is now held by this thread and will never be released. A
// CRITICAL_SECTION is recursive, so calling Dlog from here would succeed, look
// fine, and leave the count at one - and the next build-tool thread that
// reaches a trace line would block on it forever. A frozen simulation with a
// happy renderer is a much worse failure than a crash.
//
// If the fault was a stack overflow, the guard page is already gone and Dlog's
// kilobyte of stack buffer is a second, fatal one.
//
// So this function takes no locks, logs nothing, allocates nothing and touches
// no more stack than a loop counter. It puts the bytes back. Whether anyone
// ever hears about it is a separate question, asked afterwards.
static void Panic(void)
{
    // GATES FIRST, AND THAT ORDER IS THE WHOLE POINT. We are here because
    // something already faulted, and the two things below - a hundred and
    // sixteen VirtualProtect-and-write pairs, then four call-operand rewrites -
    // can fault too. If they do, the caller's guard swallows it and anything
    // AFTER them never runs. The three detours read these gates, not g_off, so
    // leaving them set would mean the plugin announces it has stopped and then
    // keeps lying to the game for the rest of the session with no key left to
    // stop it. Cheap, cannot fault, goes first.
    g_off     = 1;
    g_anarchy = 0;
    RecomputeGates();

    // THE CALL SITES BEFORE THE BYTE LOOP. These two are the only edits that
    // leave plugin code wired into the game - four bytes each, five writes
    // between them - and the loop below is a hundred and sixteen operations,
    // any one of which can fault and take the rest of this function with it.
    // Cheapest and most important first.
    RestoreDistCalls();
    RestoreToolCall();

    for (int i = 0; i < AN_SITE_COUNT; i++)
    {
        SiteState* st = &g_ss[i];
        if (!st->on) continue;
        const AnSite* s = &AN_SITES[i];
        BYTE* p = g_exeBase + s->rva + AnPatchOffset(s);
        // st->on is cleared only when the byte is actually back. Clearing it
        // unconditionally destroyed the record that a byte was still ours -
        // harmless today because nothing runs after Panic(), and exactly the
        // kind of "harmless because of something else" this file tries not to
        // rely on.
        if (*p == s->after) { if (PokeByte(p, st->orig)) st->on = 0; }
        else st->on = 0;
    }
    g_sitesOn = 0;
    for (int i = 0; i < AN_SITE_COUNT; i++) if (g_ss[i].on) g_sitesOn++;
}


// =========================================================================
//  config
// =========================================================================
// The ini parser is shared now; see SkCfgI/SkCfgU/SkCfgRaw in sovietkit.cpp.

static void LoadConfig(void)
{
    g_enabled         = SkCfgI("anarchy", "enabled",           1, 0, 1);
    g_ignoreBuild     = SkCfgI("anarchy", "ignore_buildings",  1, 0, 1);
    g_ignoreSignals   = SkCfgI("anarchy", "ignore_signals",    1, 0, 1);
    g_ignoreOverlap   = SkCfgI("anarchy", "ignore_overlap",    1, 0, 1);
    g_widenToBuildings= SkCfgI("anarchy", "widen_to_buildings",1, 0, 1);
    g_layer1Follows   = SkCfgI("anarchy", "layer1_follows_key",1, 0, 1);
    g_allowOutsideMap = SkCfgI("anarchy", "allow_outside_map", 0, 0, 1);
    g_startOn         = SkCfgI("anarchy", "start_on",          0, 0, 1);
    g_doLines         = SkCfgI("anarchy", "lines",             1, 0, 1);
    g_doWires         = SkCfgI("anarchy", "wires",             1, 0, 1);
    g_doBuildings     = SkCfgI("anarchy", "buildings",         1, 0, 1);
    g_doPipes         = SkCfgI("anarchy", "pipes",             1, 0, 1);
    g_doMessages      = SkCfgI("anarchy", "messages",          1, 0, 1);

    // trace is the kit's, read once in sovietkit.cpp. debug and the heartbeat
    // are per-part and therefore live in the PART'S OWN section - "count what
    // anarchy does" and "count what skins does" are separate questions.
    g_debug           = SkCfgI("anarchy", "debug",             0, 0, 1);
    g_heartbeatSec    = SkCfgI("anarchy", "heartbeat_seconds",15, 0, 3600);

    S.buildStamp = (unsigned)SkCfgU("sites", "build_stamp", S.buildStamp);
    S.collide    = (unsigned)SkCfgU("sites", "collide",     S.collide);
    S.overlap    = (unsigned)SkCfgU("sites", "overlap",     S.overlap);
    S.distRet[0] = (unsigned)SkCfgU("sites", "dist_ret_1",  S.distRet[0]);
    S.distRet[1] = (unsigned)SkCfgU("sites", "dist_ret_2",  S.distRet[1]);
    S.distRet[2] = (unsigned)SkCfgU("sites", "dist_ret_3",  S.distRet[2]);
    S.distRet[3] = (unsigned)SkCfgU("sites", "dist_ret_4",  S.distRet[3]);
    for (int i = 0; i < 4; i++) g_distRet[i] = S.distRet[i];

    g_lineLo = (unsigned)SkCfgU("sites", "line_lo", g_lineLo);
    g_lineHi = (unsigned)SkCfgU("sites", "line_hi", g_lineHi);
    g_bldLo  = (unsigned)SkCfgU("sites", "building_lo", g_bldLo);
    g_bldHi  = (unsigned)SkCfgU("sites", "building_hi", g_bldHi);
    g_wireLo = (unsigned)SkCfgU("sites", "wire_lo", g_wireLo);
    g_wireHi = (unsigned)SkCfgU("sites", "wire_hi", g_wireHi);
    g_pipeLo = (unsigned)SkCfgU("sites", "pipe_lo", g_pipeLo);
    g_pipeHi = (unsigned)SkCfgU("sites", "pipe_hi", g_pipeHi);

    // THESE FOUR PAIRS DECIDE WHO LAYER 1 IS WILLING TO LIE TO, so `hi > lo` is
    // not enough of a check. `line_lo = 0` with `line_hi = 0xFFFFFFFF` passes
    // that test and makes InLineTool true for every return address in the
    // process - the simulation, the vehicle pathing, everything - with layer 1
    // answering "nothing in the way" game-wide, permanently, while the log and
    // the indicator report normally. A range has to be inside the executable and
    // it has to be a plausible size for a function.
    {
        struct Rng { const char* name; unsigned* lo; unsigned* hi; unsigned dlo, dhi; };
        Rng r[4] = {
            { "line",     &g_lineLo, &g_lineHi, RVA_LINE_LO, RVA_LINE_HI },
            { "building", &g_bldLo,  &g_bldHi,  RVA_BLD_LO,  RVA_BLD_HI  },
            { "wire",     &g_wireLo, &g_wireHi, RVA_WIRE_LO, RVA_WIRE_HI },
            { "pipe",     &g_pipeLo, &g_pipeHi, RVA_PIPE_LO, RVA_PIPE_HI },
        };
        const unsigned cap = 0x200000u;     // 2 MB; the biggest of these is 100 KB
        for (int i = 0; i < 4; i++)
        {
            unsigned lo = *r[i].lo, hi = *r[i].hi;
            bool ok = hi > lo
                   && (size_t)hi <= (size_t)g_exeSize
                   && (hi - lo) <= cap;
            if (!ok)
            {
                *r[i].lo = r[i].dlo; *r[i].hi = r[i].dhi;
                Dlog("  %s_lo/%s_hi (%#x..%#x) is not a range inside this executable"
                     " - using the built-in one", r[i].name, r[i].name, lo, hi);
            }
        }
    }

    g_modifier   = SkCfgI("keys", "modifier",         4, 0, 6);
    g_anModifier = SkCfgI("keys", "anarchy_modifier", 2, 0, 6);
    K_ANARCHY = (int)SkCfgU("keys", "anarchy",        'F') & 0xFF;

    g_showIndicator = SkCfgI("indicator", "show",       1, 0, 1);
    g_hudFromRight  = SkCfgI("indicator", "from_right", 1, 0, 1);
    g_hudX          = (float)SkCfgI("indicator", "x", g_hudFromRight ? 200 : 24, 0, 8000);
    g_hudY          = (float)SkCfgI("indicator", "y", 120, 0, 8000);
    g_hudColour     = (unsigned)SkCfgU("indicator", "colour", g_hudColour);
    {
        char t[48];
        if (SkCfgRaw("indicator", "text", t, sizeof(t)) && t[0])
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, g_hudText,
                                        (int)(sizeof(g_hudText)/sizeof(g_hudText[0])));
            if (n <= 0) wcscpy_s(g_hudText, sizeof(g_hudText)/sizeof(g_hudText[0]), L"ANARCHY");
        }
    }
    K_TOGGLE  = (int)SkCfgU("keys", "anarchy_layer1", 'T') & 0xFF;
}

// =========================================================================
//  lifecycle
// =========================================================================
// Returns false when this part is not going to run at all, which is the
// signal sovietkit.cpp uses to stop calling it.
static bool ModInit(void)
{
    LoadConfig();
    Dlog("--- anarchy %s: build it where you want it ---", ANARCHY_VERSION);

    C_COLLIDE_ALL        = CounterNew("anarchy: collision calls");
    C_COLLIDE_SUPPRESSED = CounterNew("  of those, suppressed");
    C_COLLIDE_FOREIGN    = CounterNew("  of those, the rest of the game");
    C_DIST_SUPPRESSED    = CounterNew("signal distance faked");
    C_OVERLAP_ALL        = CounterNew("overlap calls");
    C_OVERLAP_SUPPRESSED = CounterNew("  of those, suppressed");

    SiteAdd("collide", S.collide, EXPECT_COLLIDE, COLLIDE_STOLEN, 1);
    SiteAdd("overlap", S.overlap, EXPECT_OVERLAP, OVERLAP_STOLEN, 1);

    unsigned got = 0;
    BuildStampRead(&got);
    if (got != S.buildStamp)
    {
        g_off = 1;
        Dlog("build 0x%08X, every offset here came from 0x%08X - the game updated.", got, S.buildStamp);
        Dlog("NOTHING IS HOOKED AND NOTHING IS PATCHED. This is the message, not a crash.");
        Dlog("Layer 2 writes to the game's own instructions, so it refuses outright");
        Dlog("rather than guess. If you know the new addresses, regenerate an_sites.h.");
        SiteVerifyAll();
        DiagFlush();
        return false;
    }

    Dlog("  build 0x%08X - the one every offset was read off", got);
    return true;
}

static bool ModStart(void)
{
    if (g_off) { Dlog("anarchy: disabled - no hooks, no patches"); DiagFlush(); return false; }

    // enabled = 0 HAS TO MEAN NOTHING HAPPENS. It is the first thing anybody
    // tries when they suspect this plugin, so it must not leave two inline
    // splices and an import hook in place while the gates quietly read zero -
    // that turns "I ruled the mod out" into a wrong answer.
    if (!g_enabled)
    {
        g_off = 1;
        RecomputeGates();
        Dlog("anarchy: enabled = 0 - nothing hooked, nothing patched.");
        DiagFlush();
        return false;
    }

    // NOT SiteVerifyAll's count. The site table is shared with skins and
    // trackcrew now, so its total says nothing about us - and it would have had
    // anarchy announcing failures that belong to the picker. Each rule below
    // asks SiteOk about its own site, by name, which is what it always did.

    // The frame hook is sovietkit's now, and it is already installed by the
    // time this runs - the host refuses to start any part without it, because
    // without a frame tick there is no key to undo layer 2 with.

    // ---- the dispatcher flag, which is what makes "every tool" true ----
    //
    // GATED ON THE SETTING, because the setting has to mean something. Leaving
    // the call site rewritten while nothing reads the flag would be pure cost
    // and pure risk: every frame's tool dispatch would still run through us for
    // no benefit at all. Deferred to the first frame rather than done here -
    // see g_wantToolHook.
    if (g_widenToBuildings) g_wantToolHook = 1;
    else Dlog("dispatcher: widen_to_buildings = 0, so the tool dispatcher is left"
              " alone entirely and anarchy covers only the shared drag-a-line tool");

    // ---- layer 1, rule 2 first, because it is the only one that patches nothing ----
    if (g_ignoreSignals && !g_distHooked)
    {
        if (RedirectDistCalls())
        {
            g_distHooked = 1;
            Dlog("signals: %d of the 4 signal call sites now come to us; the other"
                 " 14 callers of C3DCalcDistanceSq are untouched and pay nothing", g_distSites);
        }
        else { g_ignoreSignals = 0; Dlog("signals: rule off, nothing written"); }
    }

    // ---- rules 1 and 3 splice code, and refuse rather than write on a mismatch ----
    if (g_ignoreBuild && !g_collideHooked)
    {
        if (!SiteOk("collide"))
        {
            g_ignoreBuild = 0;
            Dlog("buildings: the bytes at exe+%#x are not what this build expects - rule off", S.collide);
        }
        else if (InstallInlineHook(g_exeBase + S.collide, (void*)h_Collide,
                                   (void**)&o_Collide, EXPECT_COLLIDE, COLLIDE_STOLEN,
                                   "anarchy building collision"))
        {
            g_collideHooked = 1;
            SiteMarkPatched("collide");
            Dlog("buildings: inline hook on exe+%#x installed (%d bytes relocated)",
                 S.collide, COLLIDE_STOLEN);
        }
        else { g_ignoreBuild = 0; Dlog("buildings: the loader refused the hook - rule off, nothing written"); }
    }

    if (g_ignoreOverlap && !g_overlapHooked)
    {
        if (!SiteOk("overlap"))
        {
            g_ignoreOverlap = 0;
            Dlog("overlap: the bytes at exe+%#x are not what this build expects - rule off", S.overlap);
        }
        else if (InstallInlineHook(g_exeBase + S.overlap, (void*)h_Overlap,
                                   (void**)&o_Overlap, EXPECT_OVERLAP, OVERLAP_STOLEN,
                                   "anarchy construction overlap"))
        {
            g_overlapHooked = 1;
            SiteMarkPatched("overlap");
            Dlog("overlap: inline hook on exe+%#x installed (%d bytes relocated)",
                 S.overlap, OVERLAP_STOLEN);
        }
        else { g_ignoreOverlap = 0; Dlog("overlap: the loader refused the hook - rule off, nothing written"); }
    }

    if (g_showIndicator) HudResolve();

    // ---- layer 2: check every byte, write none of them yet ----
    Dlog("checking the %d refusal sites...", AN_SITE_COUNT);
    VerifySites();
    Dlog("  %d of %d match this build; %d do not and will never be written to",
         g_sitesOk, AN_SITE_COUNT, g_sitesDead);
    if (g_trace >= 1) ListRefusals();

    RecomputeGates();
    Dlog("ready.");
    Dlog("  layer 1 (%s): buildings %d, signals %d, overlap %d",
         g_layer1Follows ? "on the key" : "always on",
         g_ignoreBuild && g_collideHooked,
         g_ignoreSignals && g_distHooked,
         g_ignoreOverlap && g_overlapHooked);
    Dlog("  layer 1 scope: exe+%#x..%#x (the drag tool), +%#x..%#x (wires and"
         " poles), +%#x..%#x (pipe segments)%s",
         g_lineLo, g_lineHi, g_wireLo, g_wireHi, g_pipeLo, g_pipeHi,
         g_widenToBuildings ? ", and every tool while anarchy is on" : "");
    Dlog("  layer 2 (on a key):  %d sites ready", g_sitesOk);
    Dlog("  indicator: %s", g_hudDead ? "unavailable" :
         g_showIndicator ? (g_hudReady ? "on - the word appears while anarchy is on" : "resolving")
                         : "turned off in the ini");
    Dlog("  %s+%c turns anarchy on and off; %s+%c turns layer 1 on and off.",
         SkModifierName(g_anModifier), K_ANARCHY ? K_ANARCHY : '?',
         SkModifierName(g_modifier),   K_TOGGLE  ? K_TOGGLE  : '?');

    // A MODE WITH NO EXIT IS NOT A MODE. start_on writes ~120 bytes into the
    // game's code before the player has touched anything; if the toggle key
    // resolved to 0 - `keys.anarchy = 256`, or 0, or a typo the parser now
    // rejects - there is no key that will ever put them back and no other key
    // toggles anarchy. Refuse rather than start there.
    if (g_startOn && !K_ANARCHY)
        Dlog("  start_on = 1 but the anarchy key resolved to 0, so there would be no"
             " way to switch it off again. NOT starting on. Fix [keys] anarchy.");
    else if (g_startOn) SetAnarchy(1);
    else Dlog("  anarchy is OFF. Press the key, then try a power line through a house.");

    DiagFlush();
    return true;
}

