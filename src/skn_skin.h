// skn_skin.h - reading and writing the one byte this whole plugin exists for.
//
// THE CHAIN, and why every link is re-derived rather than cached:
//
//     building + 0x318            -> type descriptor        (shared, freed on world close)
//     descriptor + 0x410          -> int render type id     (stable for the run)
//     GetType(manager, typeId)    -> C3D_BUILDING::TYPE*    (engine-owned, ask every time)
//     TYPE + 0x2C0                -> instance array         (reallocated as buildings appear)
//     building + 0x5F4            -> int instance index     (reassigned when buildings die)
//     instances[idx*0x118] + 0x4E -> the skin byte
//
// The only things ever cached are the render type id (an int) and, inside a
// single walk, a descriptor->TYPE* table that is discarded when the walk ends.
// Nothing survives a frame except integers.

#ifndef SKN_SKIN_H
#define SKN_SKIN_H

typedef void* (*GetTypeFn)(void* mgr, int typeId);
static GetTypeFn o_GetType;

static BYTE* World(void) { return g_exeBase + S.rvaWorld; }

// The manager is a POINTER SLOT, and the object it leads to is rebuilt with the
// world, so this is asked for every time rather than held.
// GUARDED HERE, not at one call site. Both dereferences are through addresses
// the ini can change, and the callers (Frame, SelectedBuilding) are covered only
// by the host's per-part __try - which unwinds the whole skins frame body,
// including all nine hotkeys, among them the undo key. A bad bldmgr_slot should
// cost a null return, not the ability to undo a mass apply.
static void* BuildingMgr(void)
{
    void** slot = (void**)(g_exeBase + S.rvaMgrSlot);
    void* mgr = NULL;
    TSM_TRY
    {
        if (!slot || !*slot) return NULL;
        BYTE* outer = (BYTE*)*slot;
        mgr = *(void**)(outer + S.mgrInner);
    }
    TSM_EXCEPT("skins building manager") { return NULL; }
    return mgr;
}

// ---------------------------------------------------------------------------
// A resolved handle on one building's skin. Everything downstream works from
// this, so all the bounds checks live in one place and cannot be skipped.
struct SknRef
{
    BYTE* byte;      // the skin byte itself
    int   count;     // how many skins this type has
    int   typeId;    // render type id
    int   cur;       // current value, already clamped
};

static bool SknResolveByType(void* mgr, int typeId, int instIdx, SknRef* out)
{
    out->byte = NULL; out->count = 0; out->typeId = typeId; out->cur = 0;

    if (!mgr || !o_GetType) return false;
    if (instIdx < 0 || instIdx > SKN_MAX_INSTANCE) return false;

    void* type = NULL;
    TSM_TRY { type = o_GetType(mgr, typeId); }
    TSM_EXCEPT("skins GetType") { return false; }
    if (!type) return false;

    BYTE* t = (BYTE*)type;
    int   n = 0;
    BYTE* inst = NULL;

    TSM_TRY
    {
        n    = *(int*)(t + S.tySkinCount);
        inst = *(BYTE**)(t + S.tyInstances);
    }
    TSM_EXCEPT("skins read TYPE") { return false; }

    // The game's own button refuses to appear below 2, and anything above the
    // engine's own limit means we are reading the wrong object.
    if (n < 1 || n > SKN_MAX_SKINS) return false;
    if (!inst) return false;

    BYTE* b = inst + (size_t)instIdx * S.instStride + S.instSkin;

    // ReadablePtr AS WELL AS THE __try, and it is not redundant.
    //
    // The instance array's length is not exposed anywhere we can read, so
    // SKN_MAX_INSTANCE is a sanity bound and not a real one: 200,000 * 0x118 is
    // 56 MB past `inst`. The __try below proves the byte is READABLE; it cannot
    // prove it is inside the array, and this same file says exactly that about a
    // different case two screens down - "the memory is committed and readable,
    // it is simply somebody else's". ReadablePtr does not close that gap either,
    // but it turns "somewhere in the process" into "somewhere committed", which
    // is what stops a wild instIdx from a recycled building pointer becoming a
    // one-byte write into an unrelated allocation.
    if (!ReadablePtr(b, 1)) return false;

    int cur = 0;
    TSM_TRY { cur = *b; }
    TSM_EXCEPT("skins read skin byte") { return false; }

    if (cur < 0 || cur >= n) cur = 0;     // the load path clamps the same way

    out->byte = b; out->count = n; out->cur = cur;
    return true;
}

// The building -> everything form. `td` is optional; pass it if the caller has
// already read it (the walk has), so we do not read it twice.
static bool SknResolve(void* mgr, BYTE* bld, BYTE* td, SknRef* out)
{
    if (!bld) return false;

    int typeId = 0, instIdx = 0;
    TSM_TRY
    {
        if (!td) td = *(BYTE**)(bld + S.bTypeDesc);
        if (!td) return false;
        typeId  = *(int*)(td + S.tdRenderType);
        instIdx = *(int*)(bld + S.bInstance);
    }
    TSM_EXCEPT("skins resolve building") { return false; }

    return SknResolveByType(mgr, typeId, instIdx, out);
}

// THE ONLY WRITE THIS PLUGIN PERFORMS. One byte, clamped to the range the
// game's own button clamps to, and only when it is actually changing - a
// no-op write is still a write racing the render thread reading it.
//
// 0.2 REMOVED A SECOND WRITE THAT WAS WRONG. The spray-can button follows its
// increment with `mov dword ptr [rcx+0x4A4], 0x23`, and 0.1 copied that onto
// the building. It is not on the building: `rcx` there is
// `[[world+0xCAC0]]`, the info WINDOW, and 0x4A4 is a field of the window.
// Writing it through a building pointer put a 4-byte 0x23 into whatever lives
// at building+0x4A4 - an unknown field, in a live structure, on every painted
// building. It never fired in practice because every path that could reach it
// was failing to resolve first, which is the only reason this is a note and
// not an apology.
//
// Nothing is needed in its place: the renderer reads the skin byte every frame,
// so a changed skin shows up on the next one with no refresh call at all.
static bool SknSet(SknRef* r, int want, BYTE* bld)
{
    (void)bld;
    if (!r->byte || r->count < 1) return false;
    if (want < 0) want = 0;
    if (want >= r->count) want = r->count - 1;
    if (want == r->cur) return false;

    TSM_TRY { *r->byte = (BYTE)want; }
    TSM_EXCEPT("skins write skin byte") { return false; }

    r->cur = want;
    return true;
}

// ---------------------------------------------------------------------------
// The per-walk descriptor cache.
//
// LOCAL TO THE WALK THAT USES IT, DELIBERATELY. Descriptors are freed when a
// world closes and a cache that outlived one would answer confidently for an
// address the game has since handed to something else.
//
// Open addressing on the descriptor pointer, SHIFTED BEFORE MASKING: descriptors
// are aligned allocations, so the low bits are always zero and a hash without
// the shift is a linear scan wearing a hat.
#define TD_SLOTS 8192                 // power of two, and twice TD_MAX
#define TD_MAX   4096

struct TdCache
{
    BYTE* td[TD_SLOTS];
    int   typeId[TD_SLOTS];
    int   n;
    int   full;
};

static void TdReset(TdCache* c) { memset(c->td, 0, sizeof(c->td)); c->n = 0; c->full = 0; }

// -1 means "could not read it"; the caller skips rather than guessing.
static int TdTypeId(TdCache* c, BYTE* td)
{
    if (!td) return -1;

    int slot = (int)(((unsigned long long)td >> 4) & (TD_SLOTS - 1));
    while (c->td[slot] && c->td[slot] != td) slot = (slot + 1) & (TD_SLOTS - 1);
    if (c->td[slot]) return c->typeId[slot];

    if (c->n >= TD_MAX)                // a table that can fill completely is a
    {                                  // table whose probe loop can run forever
        if (!c->full) { c->full = 1; Dlog("  more than %d distinct building types in one walk - the rest are skipped", TD_MAX); }
        return -1;
    }

    // A FAULT IS NOT CACHED. -1 means two different things here - "could not
    // read it" and a legitimate render type of -1 - so caching a transient fault
    // as -1 would make every later caller with g_jobTypeId == -1 MATCH this
    // descriptor. Nothing downstream is harmed today (SknResolve re-reads the
    // same field and fails), but that is containment by accident. Read again
    // next time instead.
    int t = -1;
    bool ok = true;
    TSM_TRY { t = *(int*)(td + S.tdRenderType); }
    TSM_EXCEPT("skins read descriptor") { ok = false; }
    if (!ok) return -1;

    c->td[slot] = td; c->typeId[slot] = t; c->n++;
    return t;
}

// ---------------------------------------------------------------------------
// The building vector. begin/end are pointers, so end-begin is already a count.
struct BldVec { BYTE** begin; __int64 n; };

static bool ReadBldVec(BldVec* v)
{
    v->begin = NULL; v->n = 0;

    BYTE* w = World();
    BYTE** b = NULL; BYTE** e = NULL;

    TSM_TRY
    {
        b = *(BYTE***)(w + S.wBldVec);
        e = *(BYTE***)(w + S.wBldVec + 8);
    }
    TSM_EXCEPT("skins read building vector") { return false; }

    if (!b || !e || e < b) return false;
    __int64 n = (e - b);
    if (n < 0 || n > SKN_MAX_BUILDINGS) return false;

    v->begin = b; v->n = n;
    return true;
}

#endif // SKN_SKIN_H
