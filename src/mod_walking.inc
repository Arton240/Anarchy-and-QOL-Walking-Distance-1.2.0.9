// mod_walking.inc - how far a citizen will walk. Included into namespace walking.
//
// A building carries a list of the buildings reachable from it on foot
// (`nWalkingBuildingNum` in the script API: building+0xCA8, a vector of
// 0xF0-byte records, the path length found at +0x98). Everything the simulation
// does with walking reads that list - there is no "is this close enough" test
// in the job, shop or service code. So the whole feature is the one number that
// decides what goes into the list when it is built.
//
// The list is built by a path search that carries its ceiling in the query it
// is handed: `query+0x3C`, the longest path the search will accept, read at
// exe+0x5799B5/0x57A278/0x57ABFD (v1.1.1.9) as
//
//     if (limit > 0.0f && node[0x24] > limit) return -1.0f;
//
// so a non-positive limit means NO limit and a positive one is metres.
//
// THE DISTANCE IS WRITTEN OUT IN FOUR FUNCTIONS, with no shared constant, and a
// patch that reaches a strict subset does something half-true:
//
//   builders    clear a building's connection vector and fill it in again.
//               THEIR LIMIT IS THE WALKING DISTANCE. Two for walking (the batch
//               builder the save loader calls, and the one-building builder a
//               running game drains off a queue), one for parking.
//   collectors  after a road changes, decide which buildings need rebuilding
//               and hand them to a builder. Their limit is a search RADIUS, 530
//               and 2600, deliberately a little wider than the limits they
//               serve. Held here at max(limit, vanilla radius): a building
//               900 m from a road that changed has to be in the rebuild set or
//               it never notices the road at all.
//   overlay     what the building window's walking-distance button highlights.
//               It does NOT read the connection vector - it runs the search
//               again with its own copy of the number. Unpatched, citizens
//               walk the new distance while the button draws the old one.
//
// WHY THE INSTRUCTION IS REPOINTED AND THE CONSTANT IS NOT. Seven of the eight
// sites are `movss xmm0,[rip+disp32]` reading the shared .rdata literal pool,
// where 480.0f and 530.0f have dozens of unrelated readers (panel widths, GUI
// coordinates). Writing 1000.0 over the constant would move all of them. The
// displacement of ONE instruction is rewritten instead, to point at a float this
// part owns. The eighth is an imm32 inside a mov and is simply replaced.
//
// WHY A CITY THAT ALREADY EXISTS DOES NOT CHANGE BY ITSELF. Connections are
// stored in the save. The game has the same problem whenever its developers
// raise a figure and ships the fix: the save loader regenerates every walking
// and parking connection when the save's format version is below 110. With
// regen_on_load the compare's imm8 becomes 127 and `test al,al` becomes
// `xor al,al` (same two bytes, always sets ZF), so the game's OWN full
// regeneration runs on every load with whatever limits are installed. Nothing
// is reimplemented here. The save format is unchanged, so a save loads with or
// without this part.
//
// EVERYTHING IS VERIFIED BEFORE ANYTHING IS WRITTEN: the instruction shape, the
// address its displacement resolves to, and the value stored there. All three,
// for every site. A site that does not match is reported and left alone, and a
// group that cannot be applied whole is not applied at all - see ModStart.

#define WALKING_VERSION "1.0"

// Sanity, not balance. The path expander is breadth-first over the road graph,
// so the work grows with the square of the limit and a five-figure distance
// makes placing a building take seconds. Zero is passed through deliberately -
// the engine reads it as "no limit" - but it is not a good idea.
#define WK_LIMIT_MAX 20000.0f

static int   g_on    = 1;
static int   g_regen = 1;
static int   g_probe = 0;
static float g_walk  = 1000.0f;
static float g_car   = WK_VANILLA_CAR;

static int      g_off;          // refused to install; skip everything
static WkSites  S;              // the table in use: built in, or whole from the ini
static const char* g_source = "none";
static float*   g_slot;         // four floats this part owns, within rel32 of the exe

enum { SLOT_WALK = 0, SLOT_CAR, SLOT_WALK_R, SLOT_CAR_R, SLOT_COUNT };

// What the report says about each site.
enum { ST_UNTOUCHED = 0, ST_PATCHED, ST_REFUSED, ST_SKIPPED };
struct SiteState { const char* label; unsigned rva; int state; };
#define WK_MAX_STATE 16
static SiteState g_st[WK_MAX_STATE];
static int       g_nSt;
static int       g_walkApplied, g_carApplied, g_regenApplied;

static void StateSet(const char* label, unsigned rva, int state)
{
    for (int i = 0; i < g_nSt; i++)
        if (strcmp(g_st[i].label, label) == 0) { g_st[i].state = state; return; }
    if (g_nSt < WK_MAX_STATE) { g_st[g_nSt].label = label; g_st[g_nSt].rva = rva; g_st[g_nSt].state = state; g_nSt++; }
}

static float Larger(float a, float b) { return a > b ? a : b; }

// =========================================================================
//  settings
// =========================================================================
static bool CfgF(const char* sec, const char* key, float* out)
{
    char v[64];
    if (!SkCfgRaw(sec, key, v, sizeof(v))) return false;
    char* end = NULL;
    double d = strtod(v, &end);
    if (end == v || *end || d != d)
    {
        Dlog("[%s] %s = \"%s\" is not a number - using %.0f", sec, key, v, (double)*out);
        return false;
    }
    *out = (float)d;
    return true;
}

static void ReadSettings(void)
{
    g_on    = SkCfgB("walking", "enabled",       g_on != 0);
    g_regen = SkCfgB("walking", "regen_on_load", g_regen != 0);
    g_probe = SkCfgB("walking", "probe",         g_probe != 0);
    CfgF("walking", "distance",     &g_walk);
    CfgF("walking", "car_distance", &g_car);

    // Negative is not "no limit" - zero is. A minus sign is a typo.
    if (g_walk < 0.0f) { Dlog("[walking] distance is negative - using the base game's %.0f", WK_VANILLA_WALK); g_walk = WK_VANILLA_WALK; }
    if (g_car  < 0.0f) { Dlog("[walking] car_distance is negative - using the base game's %.0f", WK_VANILLA_CAR); g_car = WK_VANILLA_CAR; }

    if (g_walk > WK_LIMIT_MAX) { Dlog("[walking] distance %.0f is past the %.0f ceiling - clamped", g_walk, WK_LIMIT_MAX); g_walk = WK_LIMIT_MAX; }
    if (g_car  > WK_LIMIT_MAX) { Dlog("[walking] car_distance %.0f is past the %.0f ceiling - clamped", g_car, WK_LIMIT_MAX); g_car = WK_LIMIT_MAX; }
}

// -------------------------------------------------------------------------
//  [sites]: a game update is an edit here and not a rebuild.
//
//  ALL OR NOTHING. Sixteen keys, every one starting walk_ or regen_ or
//  save_, and either none of them are present (the built-in table for this
//  exe's build stamp is used) or all of them are (the table is taken whole from
//  the ini, and its walk_build_stamp must equal this exe's). A table that is
//  half yours and half compiled in is a table nobody can reason about, and
//  these addresses are the kind of thing that moves together.
// -------------------------------------------------------------------------
struct IniSite { const char* key; unsigned* field; };

static IniSite* IniTable(WkSites* t, int* n)
{
    static IniSite k[16];
    int i = 0;
    k[i++] = { "walk_build_stamp",    &t->buildStamp };
    k[i++] = { "walk_limit_op",       &t->limitOp };
    k[i++] = { "walk_const",          &t->walkConst };
    k[i++] = { "car_const",           &t->carConst };
    k[i++] = { "walk_radius_const",   &t->walkRadiusConst };
    k[i++] = { "car_radius_const",    &t->carRadiusConst };
    k[i++] = { "walk_queued",         &t->walkQueued };
    k[i++] = { "walk_overlay_1",      &t->walkOverlay1 };
    k[i++] = { "walk_overlay_2",      &t->walkOverlay2 };
    k[i++] = { "car_build",           &t->carBuild };
    k[i++] = { "car_overlay",         &t->carOverlay };
    k[i++] = { "walk_rebuild",        &t->walkRebuild };
    k[i++] = { "car_rebuild",         &t->carRebuild };
    k[i++] = { "regen_cmp",           &t->regenCmp };
    k[i++] = { "regen_test",          &t->regenTest };
    k[i++] = { "save_version",        &t->saveVersion };
    *n = i;
    return k;
}

static bool LoadSites(unsigned got)
{
    WkSites ini;
    memset(&ini, 0, sizeof(ini));
    int n = 0;
    IniSite* tab = IniTable(&ini, &n);

    int have = 0;
    char v[64];
    for (int i = 0; i < n; i++)
        if (SkCfgRaw("sites", tab[i].key, v, sizeof(v))) have++;

    if (have == 0)
    {
        for (int i = 0; i < WK_NPROFILES; i++)
            if (WK_PROFILES[i].buildStamp == got)
            {
                S = WK_PROFILES[i];
                g_source = S.label;
                return true;
            }
        Dlog("  this exe is build 0x%08X and walking has no address table for it.", got);
        Dlog("  Built in: %s. A game update moves every one of these, so nothing is", "v1.1.1.9 (0x6A3EB6AD), v1.2.0.8 (0x6B0097B2)");
        Dlog("  guessed. Nothing was written. To add this build, either run");
        Dlog("  tools/walking_scan.py on its SOVIET64.exe and put the %d walk_/car_/regen_/", n);
        Dlog("  save_ keys it prints into [sites], or send that output to whoever");
        Dlog("  maintains the kit. See the 'new game build' note in sovietkit.ini.");
        return false;
    }

    if (have != n)
    {
        Dlog("  [sites] has %d of the %d walking keys. It is all or none - a table half", have, n);
        Dlog("  yours and half compiled in is one nobody can reason about. Missing:");
        for (int i = 0; i < n; i++)
            if (!SkCfgRaw("sites", tab[i].key, v, sizeof(v))) Dlog("      %s", tab[i].key);
        return false;
    }

    for (int i = 0; i < n; i++)
        *tab[i].field = (unsigned)SkCfgU("sites", tab[i].key, 0);

    if (ini.buildStamp != got)
    {
        Dlog("  [sites] walk_build_stamp is 0x%08X but this exe is 0x%08X - that table", ini.buildStamp, got);
        Dlog("  was read off a different build and is not being used.");
        return false;
    }
    ini.label = "ini [sites]";
    S = ini;
    g_source = S.label;
    return true;
}

// =========================================================================
//  reading and writing the game's code
// =========================================================================
static bool InImage(unsigned rva, size_t len)
{
    return (SIZE_T)rva + len <= g_exeSize && ReadablePtr(g_exeBase + rva, len);
}

static bool WriteCode(void* at, const void* bytes, size_t n, const char* what)
{
    DWORD prot = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &prot))
    {
        Dlog("  walking: VirtualProtect refused at %p (%s) - nothing written there", at, what);
        return false;
    }
    memcpy(at, bytes, n);
    DWORD back = 0;
    VirtualProtect(at, n, prot, &back);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// The batch builder's limit: `mov dword ptr [rsp+disp8], imm32` (C7 44 24 xx,
// four bytes before the immediate) in v1.1.1.9 and `mov dword ptr [rbp+disp8],
// imm32` (C7 45 xx, three) from v1.2.0.8 - the compiler changed which register
// addresses the slot, nothing else. Both shapes are read, and the immediate has
// to be exactly the vanilla 480.0f.
static int LimitOpLen(const BYTE* p)
{
    if (p[0] != 0xC7) return 0;
    if (p[1] == 0x45) return 3;
    if (p[1] == 0x44 && p[2] == 0x24) return 4;
    return 0;
}

static bool VerifyImm(unsigned rva, const char* label)
{
    if (!InImage(rva, 8)) { Dlog("  %-18s rva 0x%X is outside the executable - refusing", label, rva); return false; }
    const BYTE* s = g_exeBase + rva;
    int op = LimitOpLen(s);
    if (!op)
    {
        Dlog("  %-18s rva 0x%X is %02X %02X %02X, not mov dword [rsp/rbp+disp8],imm32 - refusing",
             label, rva, s[0], s[1], s[2]);
        return false;
    }
    float now; memcpy(&now, s + op, sizeof(now));
    if (now != WK_VANILLA_WALK)
    {
        Dlog("  %-18s rva 0x%X holds %.1f, expected %.1f - refusing", label, rva, now, WK_VANILLA_WALK);
        return false;
    }
    return true;
}

static bool VerifyMovss(unsigned rva, unsigned constRva, float vanilla, const char* label)
{
    if (!InImage(rva, 8) || !InImage(constRva, 4))
    { Dlog("  %-18s rva 0x%X or 0x%X is outside the executable - refusing", label, rva, constRva); return false; }

    static const BYTE movssRip[] = { 0xF3, 0x0F, 0x10, 0x05 };
    const BYTE* s = g_exeBase + rva;
    if (memcmp(s, movssRip, sizeof(movssRip)) != 0)
    {
        Dlog("  %-18s rva 0x%X is %02X %02X %02X %02X, not movss xmm0,[rip+disp32] - refusing",
             label, rva, s[0], s[1], s[2], s[3]);
        return false;
    }
    int disp; memcpy(&disp, s + 4, sizeof(disp));
    unsigned resolved = (unsigned)((int)rva + 8 + disp);
    if (resolved != constRva)
    {
        Dlog("  %-18s rva 0x%X reads 0x%X, expected 0x%X - refusing", label, rva, resolved, constRva);
        return false;
    }
    float now; memcpy(&now, g_exeBase + constRva, sizeof(now));
    if (now != vanilla)
    {
        Dlog("  %-18s 0x%X holds %.1f, expected %.1f - refusing", label, constRva, now, vanilla);
        return false;
    }
    return true;
}

static bool VerifyRegen(void)
{
    if (!InImage(S.regenCmp, 7) || !InImage(S.regenTest, 4) || !InImage(S.saveVersion, 4))
    { Dlog("  regen: a site is outside the executable - refusing"); return false; }

    const BYTE* cmp  = g_exeBase + S.regenCmp;
    const BYTE* test = g_exeBase + S.regenTest;
    if (cmp[0] != 0x83 || cmp[1] != 0x3D)
    { Dlog("  regen: rva 0x%X is not cmp dword [rip+disp32],imm8 - refusing", S.regenCmp); return false; }

    int disp; memcpy(&disp, cmp + 2, sizeof(disp));
    unsigned resolved = (unsigned)((int)S.regenCmp + 7 + disp);
    if (resolved != S.saveVersion || cmp[6] != 110)
    {
        Dlog("  regen: rva 0x%X compares 0x%X against %d, expected 0x%X against 110 - refusing",
             S.regenCmp, resolved, cmp[6], S.saveVersion);
        return false;
    }
    static const BYTE testJnz[] = { 0x84, 0xC0, 0x0F, 0x85 };
    if (memcmp(test, testJnz, sizeof(testJnz)) != 0)
    { Dlog("  regen: rva 0x%X is not test al,al / jnz - refusing", S.regenTest); return false; }
    return true;
}

// =========================================================================
//  applying
// =========================================================================
static bool PatchImm(unsigned rva, float value, const char* label)
{
    BYTE* s = g_exeBase + rva;
    int op = LimitOpLen(s);
    if (!WriteCode(s + op, &value, sizeof(value), label)) return false;
    Dlog("  %-18s %.0f -> %.0f  (rva 0x%X, immediate)", label, WK_VANILLA_WALK, value, rva);
    return true;
}

static bool Repoint(unsigned rva, float vanilla, float* slot, const char* label)
{
    BYTE* s = g_exeBase + rva;
    __int64 rel = (BYTE*)slot - (s + 8);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Dlog("  %-18s the value slot %p is out of rel32 reach of %p - refusing", label, (void*)slot, (void*)s);
        return false;
    }
    int disp = (int)rel;
    if (!WriteCode(s + 4, &disp, sizeof(disp), label)) return false;
    Dlog("  %-18s %.0f -> %.0f  (rva 0x%X now reads %p)", label, vanilla, *slot, rva, (void*)slot);
    return true;
}

static bool PatchRegen(void)
{
    BYTE* cmp  = g_exeBase + S.regenCmp;
    BYTE* test = g_exeBase + S.regenTest;

    // 127 is the largest an imm8 compare can carry. The format version is 124
    // through v1.1.1.13 and 125 in v1.2.0.8. If a patch ever takes it past 127
    // this stops working - and it stops by doing nothing, the right way round.
    const BYTE ver = 127, orig = cmp[6];
    static const BYTE xorAl[] = { 0x30, 0xC0 };

    if (!WriteCode(cmp + 6, &ver, 1, "save-version compare")) return false;
    if (!WriteCode(test, xorAl, sizeof(xorAl), "terrain-name guard"))
    {
        // One half written is a half-open guard. Put the first back.
        WriteCode(cmp + 6, &orig, 1, "save-version compare (undo)");
        return false;
    }
    Dlog("  regen on: every load rebuilds all walking and parking connections");
    return true;
}

// Several sites share a value, and the walking limit alone is spelled out in
// four functions, so each slot is one float and every site points at one.
static bool ModStart(void)
{
    if (g_off) { Dlog("walking: disabled"); DiagFlush(); return false; }

    if (g_probe)
    {
        if (InImage(S.limitOp, 8))
            Dlog("  walking probe: rva 0x%X (batch limit) holds %02X %02X %02X %02X", S.limitOp,
                 g_exeBase[S.limitOp], g_exeBase[S.limitOp + 1], g_exeBase[S.limitOp + 2], g_exeBase[S.limitOp + 3]);
        const struct { const char* l; unsigned r; unsigned c; } p[] = {
            { "walk queued", S.walkQueued, S.walkConst }, { "walk overlay 1", S.walkOverlay1, S.walkConst },
            { "walk overlay 2", S.walkOverlay2, S.walkConst }, { "car build", S.carBuild, S.carConst },
            { "car overlay", S.carOverlay, S.carConst }, { "walk rebuild", S.walkRebuild, S.walkRadiusConst },
            { "car rebuild", S.carRebuild, S.carRadiusConst } };
        for (int i = 0; i < 7; i++)
            if (InImage(p[i].r, 8) && InImage(p[i].c, 4))
            {
                float f; memcpy(&f, g_exeBase + p[i].c, sizeof(f));
                Dlog("  walking probe: %-16s rva 0x%X -> 0x%X = %.1f", p[i].l, p[i].r, p[i].c, f);
            }
    }

    // ---- verify everything first; write nothing yet -------------------------
    const bool okLimit    = VerifyImm(S.limitOp, "walk build batch");
    const bool okQueued   = VerifyMovss(S.walkQueued,   S.walkConst,       WK_VANILLA_WALK,   "walk build queued");
    const bool okOverlay1 = VerifyMovss(S.walkOverlay1, S.walkConst,       WK_VANILLA_WALK,   "walk overlay");
    const bool okOverlay2 = VerifyMovss(S.walkOverlay2, S.walkConst,       WK_VANILLA_WALK,   "walk overlay 2");
    const bool okCarBuild = VerifyMovss(S.carBuild,     S.carConst,        WK_VANILLA_CAR,    "car build");
    const bool okCarOver  = VerifyMovss(S.carOverlay,   S.carConst,        WK_VANILLA_CAR,    "car overlay");
    const bool okWalkReb  = VerifyMovss(S.walkRebuild,  S.walkRadiusConst, WK_VANILLA_WALK_R, "walk rebuild");
    const bool okCarReb   = VerifyMovss(S.carRebuild,   S.carRadiusConst,  WK_VANILLA_CAR_R,  "car rebuild");
    const bool okRegen    = g_regen ? VerifyRegen() : false;

    // A GROUP IS APPLIED WHOLE OR NOT AT ALL. The limit is spelled out in two
    // builders; patching one and not the other gives citizens that walk the new
    // distance from some code paths and the old one from others, and the log
    // would say "patched". The overlay and the radii are cosmetic and
    // best-effort, but only ride along once the builders are in.
    const bool doWalk = okLimit && okQueued;
    const bool doCar  = okCarBuild;
    if (!doWalk) Dlog("  walking: the two walking builders do not both verify, so the walking"
                      " distance is left exactly as the game has it.");
    if (!doCar)  Dlog("  walking: the parking builder does not verify, so the car distance is"
                      " left exactly as the game has it.");
    if (!doWalk && !doCar)
    {
        g_off = 1;
        Dlog("walking: nothing matched this build and NOTHING WAS WRITTEN INTO GAME CODE.");
        DiagFlush();
        return false;
    }

    g_slot = (float*)AllocNear(g_exeBase, 64);
    if (!g_slot)
    {
        g_off = 1;
        Dlog("walking: no allocation within reach of the executable - nothing patched");
        DiagFlush();
        return false;
    }
    g_slot[SLOT_WALK]   = g_walk;
    g_slot[SLOT_CAR]    = g_car;
    g_slot[SLOT_WALK_R] = Larger(g_walk, WK_VANILLA_WALK_R);
    g_slot[SLOT_CAR_R]  = Larger(g_car,  WK_VANILLA_CAR_R);

    StateSet("walk build batch", S.limitOp, ST_UNTOUCHED);

    if (doWalk)
    {
        const bool a = PatchImm(S.limitOp, g_walk, "walk build batch");
        const bool b = Repoint(S.walkQueued, WK_VANILLA_WALK, &g_slot[SLOT_WALK], "walk build queued");
        StateSet("walk build batch",  S.limitOp,    a ? ST_PATCHED : ST_REFUSED);
        StateSet("walk build queued", S.walkQueued, b ? ST_PATCHED : ST_REFUSED);
        g_walkApplied = (a && b) ? 1 : (a || b) ? -1 : 0;
        if (a && !b)
        {
            // The immediate is the one write that can be taken back from here:
            // the repointed displacement's original is not kept.
            const float vanilla = WK_VANILLA_WALK;
            if (WriteCode(g_exeBase + S.limitOp + LimitOpLen(g_exeBase + S.limitOp), &vanilla, sizeof(vanilla), "walk build batch (undo)"))
            { g_walkApplied = 0; StateSet("walk build batch", S.limitOp, ST_REFUSED); }
        }
        if (g_walkApplied < 0)
            Dlog("  walking: ONE of the two walking builders was patched and the other was not."
                 " Citizens will use two different distances. Switch walking off in the ini.");

        if (okOverlay1) { bool r = Repoint(S.walkOverlay1, WK_VANILLA_WALK, &g_slot[SLOT_WALK], "walk overlay");   StateSet("walk overlay",   S.walkOverlay1, r ? ST_PATCHED : ST_REFUSED); }
        else Dlog("  walking: the overlay's first site did not verify - the button may draw the old distance");
        if (okOverlay2) { bool r = Repoint(S.walkOverlay2, WK_VANILLA_WALK, &g_slot[SLOT_WALK], "walk overlay 2"); StateSet("walk overlay 2", S.walkOverlay2, r ? ST_PATCHED : ST_REFUSED); }
        else Dlog("  walking: the overlay's second site did not verify - the button may draw the old distance");
        if (okWalkReb)  { bool r = Repoint(S.walkRebuild,  WK_VANILLA_WALK_R, &g_slot[SLOT_WALK_R], "walk rebuild"); StateSet("walk rebuild", S.walkRebuild, r ? ST_PATCHED : ST_REFUSED); }
        else Dlog("  walking: the rebuild radius did not verify - a road change may not refresh far buildings");
    }
    else
    {
        StateSet("walk build queued", S.walkQueued, ST_SKIPPED);
        StateSet("walk overlay", S.walkOverlay1, ST_SKIPPED);
        StateSet("walk overlay 2", S.walkOverlay2, ST_SKIPPED);
        StateSet("walk rebuild", S.walkRebuild, ST_SKIPPED);
    }

    if (doCar)
    {
        const bool a = Repoint(S.carBuild, WK_VANILLA_CAR, &g_slot[SLOT_CAR], "car build");
        StateSet("car build", S.carBuild, a ? ST_PATCHED : ST_REFUSED);
        g_carApplied = a ? 1 : 0;
        if (okCarOver) { bool r = Repoint(S.carOverlay, WK_VANILLA_CAR, &g_slot[SLOT_CAR], "car overlay"); StateSet("car overlay", S.carOverlay, r ? ST_PATCHED : ST_REFUSED); }
        else Dlog("  walking: the parking overlay did not verify - its button may draw the old distance");
        if (okCarReb)  { bool r = Repoint(S.carRebuild, WK_VANILLA_CAR_R, &g_slot[SLOT_CAR_R], "car rebuild"); StateSet("car rebuild", S.carRebuild, r ? ST_PATCHED : ST_REFUSED); }
        else Dlog("  walking: the parking rebuild radius did not verify");
    }
    else
    {
        StateSet("car build", S.carBuild, ST_SKIPPED);
        StateSet("car overlay", S.carOverlay, ST_SKIPPED);
        StateSet("car rebuild", S.carRebuild, ST_SKIPPED);
    }

    if (g_regen)
    {
        if (okRegen && PatchRegen()) g_regenApplied = 1;
        else Dlog("  regen off: only what is built from now on uses the new limit");
    }

    if (g_walk == 0.0f && doWalk)
        Dlog("  distance 0 means the search is unbounded - expect long pauses when placing buildings");

    Dlog("  walking %.0f m (base game %.0f), cars %.0f m (base game %.0f); a path length"
         " along roads and footpaths, not a straight line",
         g_walk, WK_VANILLA_WALK, g_car, WK_VANILLA_CAR);
    DiagFlush();
    return (g_walkApplied > 0) || (g_carApplied > 0);
}

static bool ModInit(void)
{
    ReadSettings();
    Dlog("--- walking %s: how far a citizen will walk ---", WALKING_VERSION);

    if (!g_on)
    {
        g_off = 1;
        Dlog("  [walking] enabled = 0 - nothing hooked, nothing patched.");
        return false;
    }

    unsigned got = 0;
    BuildStampRead(&got);
    if (!LoadSites(got))
    {
        g_off = 1;
        Dlog("  Nothing is patched. This is the message, not a crash.");
        DiagFlush();
        return false;
    }
    Dlog("  build 0x%08X - address table: %s", got, g_source);
    return true;
}

static void Report(void)
{
    Dlog("--- walking %s ---", WALKING_VERSION);
    Dlog("  distance %.0f m, car_distance %.0f m, regen_on_load %d, address table %s",
         g_walk, g_car, g_regen, g_source);
    Dlog("  walking builders %s, parking builder %s, regeneration on load %s",
         g_walkApplied > 0 ? "patched" : g_walkApplied < 0 ? "HALF PATCHED" : "untouched",
         g_carApplied ? "patched" : "untouched", g_regenApplied ? "on" : "off");
    for (int i = 0; i < g_nSt; i++)
        Dlog("    %-18s rva 0x%06X  %s", g_st[i].label, g_st[i].rva,
             g_st[i].state == ST_PATCHED ? "patched" : g_st[i].state == ST_REFUSED ? "REFUSED"
             : g_st[i].state == ST_SKIPPED ? "skipped (its group did not verify)" : "untouched");
}
