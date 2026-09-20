// skn_cfg.h - reading skins.ini.
//
// The host call goes to the ANSI profile API, which brings three behaviours
// that have each cost this project an evening:
//
//   * a UTF-8 BOM stops the first section header matching and EVERY setting in
//     the file silently falls back to its default. Nothing is logged. Nothing
//     looks wrong. Never write one back with PowerShell's -Encoding UTF8.
//   * the whole line after the `=` comes back, inline comment and all.
//   * GetPrivateProfileIntA parses DECIMAL ONLY - it reads "0x9D4F10" as 0 and
//     says nothing. NOTHING IN [sites] MAY GO THROUGH configInt.

#ifndef SKN_CFG_H
#define SKN_CFG_H

// The file is the kit's now: one ini, one [skins] section inside it.
static const char* g_ini = SK_INI;

static void StripInlineComment(char* v)
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

static bool CfgRaw(const char* section, const char* key, char* out, int n)
{
    if (!H->configString(g_ini, section, key, out, n, "")) return false;
    StripInlineComment(out);
    Trim(out);
    return out[0] != 0;
}

// Base 0 so 0x... is read as hex, but a LEADING ZERO ON A DECIMAL must not turn
// it octal: `port = 0590` was silently 376 in an earlier life of this code.
static unsigned __int64 CfgU(const char* section, const char* key, unsigned __int64 fallback)
{
    char v[64];
    if (!CfgRaw(section, key, v, sizeof(v))) return fallback;

    int base = 10;
    const char* p = v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }

    char* end = NULL;
    unsigned __int64 u = _strtoui64(p, &end, base);
    if (end == p)
    {
        Dlog("[%s] %s = \"%s\" is not a number - using %llu", section, key, v, fallback);
        return fallback;
    }
    return u;
}

static int CfgI(const char* section, const char* key, int fallback, int lo, int hi)
{
    char v[64];
    if (!CfgRaw(section, key, v, sizeof(v))) return fallback;

    char* end = NULL;
    long l = strtol(v, &end, 10);
    if (end == v)
    {
        Dlog("[%s] %s = \"%s\" is not a number - using %d", section, key, v, fallback);
        return fallback;
    }
    if (l < lo || l > hi)
    {
        Dlog("[%s] %s = %ld is outside %d..%d - clamped", section, key, l, lo, hi);
        l = (l < lo) ? lo : hi;
    }
    return (int)l;
}

static bool CfgB(const char* section, const char* key, bool fallback)
{
    return CfgI(section, key, fallback ? 1 : 0, 0, 1) != 0;
}

// A hotkey, written as a virtual-key code in hex or decimal. 0 disables it.
static int CfgKey(const char* key, int fallback)
{
    return (int)CfgU("keys", key, (unsigned __int64)fallback) & 0xFF;
}

// ---- [sites] ---------------------------------------------------------------
// Every one of these goes through CfgU, never CfgI, for the hex reason above.
static void LoadSites(void)
{
    S.buildStamp   = (unsigned)CfgU("sites", "build_stamp",    S.buildStamp);
    S.rvaWorld     = (unsigned)CfgU("sites", "world",          S.rvaWorld);
    S.rvaMgrSlot   = (unsigned)CfgU("sites", "bldmgr_slot",    S.rvaMgrSlot);
    S.mgrInner     = (unsigned)CfgU("sites", "bldmgr_inner",   S.mgrInner);

    S.wBldVec      = (unsigned)CfgU("sites", "w_building_vec", S.wBldVec);
    S.wDay         = (unsigned)CfgU("sites", "w_day",          S.wDay);
    S.wYear        = (unsigned)CfgU("sites", "w_year",         S.wYear);
    S.wBuildSel    = (unsigned)CfgU("sites", "w_build_sel",    S.wBuildSel);
    S.wInfoWin     = (unsigned)CfgU("sites", "w_info_window",  S.wInfoWin);

    S.bTypeDesc    = (unsigned)CfgU("sites", "b_typedesc",     S.bTypeDesc);
    S.bInstance    = (unsigned)CfgU("sites", "b_instance",     S.bInstance);
    S.bBuilt       = (unsigned)CfgU("sites", "b_built",        S.bBuilt);

    S.tdObjName    = (unsigned)CfgU("sites", "td_objname",     S.tdObjName);
    S.tdRenderType = (unsigned)CfgU("sites", "td_rendertype",  S.tdRenderType);

    S.tyMaterials  = (unsigned)CfgU("sites", "ty_materials",   S.tyMaterials);
    S.tySkinCount  = (unsigned)CfgU("sites", "ty_skincount",   S.tySkinCount);
    S.tyInstances  = (unsigned)CfgU("sites", "ty_instances",   S.tyInstances);
    S.instStride   = (unsigned)CfgU("sites", "inst_stride",    S.instStride);
    S.instSkin     = (unsigned)CfgU("sites", "inst_skin",      S.instSkin);

    S.pickRetA     = (unsigned)CfgU("sites", "pick_ret_a",     S.pickRetA);
    S.pickRetB     = (unsigned)CfgU("sites", "pick_ret_b",     S.pickRetB);

    // __try catches a bad pointer. It does not catch a correct write to the
    // wrong place, so anything a human can type gets bounded here.
    //
    // ALL OF THEM, NOT TWO OF THEM. This bounded inst_stride and inst_skin and
    // left the other eighteen alone - including b_instance, which is the direct
    // input to the instance index and therefore to the one byte this plugin
    // writes. A structure offset is bounded by a plausible structure size; an
    // RVA is bounded by the executable.
    if (S.instStride < 0x10 || S.instStride > 0x1000)
    { Dlog("  inst_stride %#x is implausible - using %#x", S.instStride, INSTANCE_STRIDE); S.instStride = INSTANCE_STRIDE; }
    if (S.instSkin >= S.instStride)
    { Dlog("  inst_skin %#x is outside the stride - using %#x", S.instSkin, INST_OFF_SKIN); S.instSkin = INST_OFF_SKIN; }

    {
        struct SknLim { const char* name; unsigned* v; unsigned def; };

        // Structure offsets. 0x20000 is far above anything real here (the
        // largest is 0x11B08) and still low enough to catch a missing "0x" or a
        // pasted RVA.
        SknLim off[] = {
            { "bldmgr_inner",   &S.mgrInner,     BLDMGR_INNER },
            { "w_building_vec", &S.wBldVec,      W_OFF_BLD_VEC },
            { "w_day",          &S.wDay,         W_OFF_DAY },
            { "w_year",         &S.wYear,        W_OFF_YEAR },
            { "w_build_sel",    &S.wBuildSel,    W_OFF_BUILD_SEL },
            { "w_info_window",  &S.wInfoWin,     W_OFF_INFO_WIN },
            { "b_typedesc",     &S.bTypeDesc,    B_OFF_TYPEDESC },
            { "b_instance",     &S.bInstance,    B_OFF_INSTANCE },
            { "b_built",        &S.bBuilt,       B_OFF_BUILT },
            { "td_objname",     &S.tdObjName,    TD_OFF_OBJNAME },
            { "td_rendertype",  &S.tdRenderType, TD_OFF_RENDERTYPE },
            { "ty_materials",   &S.tyMaterials,  TY_OFF_MATERIALS },
            { "ty_skincount",   &S.tySkinCount,  TY_OFF_SKINCOUNT },
            { "ty_instances",   &S.tyInstances,  TY_OFF_INSTANCES },
        };
        for (int i = 0; i < (int)(sizeof(off) / sizeof(off[0])); i++)
            if (*off[i].v > 0x20000u)
            {
                Dlog("  [sites] %s = %#x is not a plausible structure offset - using %#x",
                     off[i].name, *off[i].v, off[i].def);
                *off[i].v = off[i].def;
            }

        SknLim rva[] = {
            { "world",       &S.rvaWorld,   RVA_WORLD },
            { "bldmgr_slot", &S.rvaMgrSlot, RVA_BLDMGR_SLOT },
            { "pick_ret_a",  &S.pickRetA,   RVA_PICK_RET_A },
            { "pick_ret_b",  &S.pickRetB,   RVA_PICK_RET_B },
        };
        for (int i = 0; i < (int)(sizeof(rva) / sizeof(rva[0])); i++)
            if ((size_t)*rva[i].v >= (size_t)g_exeSize)
            {
                Dlog("  [sites] %s = %#x is outside the executable - using %#x",
                     rva[i].name, *rva[i].v, rva[i].def);
                *rva[i].v = rva[i].def;
            }
    }
}

// The gate. On a mismatch the plugin says so and does nothing - that is the
// message, not a crash.
static bool BuildStampOk(unsigned* got)
{
    *got = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_exeBase;
    if (!ReadablePtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    if (!ReadablePtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *got = nt->FileHeader.TimeDateStamp;
    return *got == S.buildStamp;
}

#endif // SKN_CFG_H
