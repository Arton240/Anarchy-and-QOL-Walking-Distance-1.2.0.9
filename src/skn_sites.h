// skn_sites.h - every address this plugin knows, and nothing else.
//
// Two kinds of number live here and they are NOT interchangeable:
//   * an OBJECT inline in .data   - used as   g_exeBase + rva
//   * a POINTER SLOT in .data     - used as *(void**)(g_exeBase + rva)
// The world is the first kind. The building manager is the second.
// Getting that backwards binds cleanly and does nothing, which is the most
// expensive failure mode in this file.
//
// EVERY ONE OF THESE IS OVERRIDABLE FROM skins.ini [sites], so a game patch
// that moves one is an ini edit, not a rebuild. build_stamp is the gate: on a
// mismatch the plugin logs, switches itself off, and leaves the save alone.
//
// Provenance: all of it read statically out of SOVIET64.exe v1.1.1.9
// (TimeDateStamp 0x6A3EB6AD). See SKINS-RE-FINDINGS.md for the disassembly
// each number came from.

#ifndef SKN_SITES_H
#define SKN_SITES_H

#define SKN_BUILD_STAMP 0x6A3EB6ADu

// ---- globals (OBJECT inline in .data unless marked) ----
#define RVA_WORLD          0x9D4F10   // the world / g_ui object itself
#define RVA_BLDMGR_SLOT    0x9941F0   // POINTER SLOT; manager = *(*(slot) + 0xFF8)
#define BLDMGR_INNER       0x0FF8

// ---- inside the world ----
#define W_OFF_BLD_VEC      0x11B08    // std::vector<C3D_BUILDING*>: begin, +8 end, +0x10 cap
#define W_OFF_DAY          0x590      // int day of year
#define W_OFF_YEAR         0x594      // int year
#define W_OFF_BUILD_SEL    0xD428     // the build menu's selected type descriptor
#define W_OFF_INFO_WIN     0xCAC0     // ptr -> struct whose [0] is the info WINDOW.
                                      // NOT the building: 0.1 assumed that and
                                      // read a "descriptor" of 0x0000000200000002.
                                      // The building is a field inside the
                                      // window, found at runtime - see
                                      // info_building in skins.ini.

// ---- C3D_BUILDING ----
#define B_OFF_TYPEDESC     0x318      // -> type descriptor
#define B_OFF_INSTANCE     0x5F4      // int, index into the type's render instance array
#define B_OFF_BUILT        0x604      // float, >= 1.0 finished
// NOTE: the skin button also writes 0x23 to +0x4A4 - but of the WINDOW, via
// [[world+0xCAC0]], not of the building. 0.1 copied that write onto the
// building and would have put four bytes into an unknown field of every painted
// building. There is no field here to replace it with, and none is needed: the
// renderer re-reads the skin byte every frame.

// ---- type descriptor ----
#define TD_OFF_OBJNAME     0x000      // char[] object name ("3744819159/HOUSE2")
#define TD_OFF_RENDERTYPE  0x410      // int, the argument to C3D_BUILDING::GetType

// ---- C3D_BUILDING::TYPE (what GetType returns) ----
#define TY_OFF_MATERIALS   0x0A8      // C3D_MATERIAL*[] - one per skin
#define TY_OFF_SKINCOUNT   0x2A8      // int
#define TY_OFF_INSTANCES   0x2C0      // -> instance array
#define INSTANCE_STRIDE    0x118      // 280 bytes
#define INST_OFF_SKIN      0x04E      // byte - THE SKIN INDEX

// ---- the picker's two CollisionRay return addresses ----
// (the instruction after `call qword ptr [rip+...]` inside 0x3CB0D0)
#define RVA_PICK_RET_A     0x3CB1DD
#define RVA_PICK_RET_B     0x3CB2EB

// ===========================================================================
//  SELF-VERIFICATION
//
// This plugin patches no game code, so it has no `expect` bytes in the usual
// sense - but it depends on five offsets that live nowhere except in the game's
// own instructions. So it verifies them the only way that actually proves
// anything: by reading back the game's own skin-cycle code and checking that it
// still encodes the offsets we are about to use.
//
// If these three sites match, then +0x5F4, 0x118, +0x4E, +0x2C0, +0x2A8, +0x318
// and +0x410 are all confirmed by the binary itself, not by our notes.
// ===========================================================================

// exe+0x6FB24F - the heart of the spray-can button:
//     movsxd rax, dword ptr [r12 + 0x5F4]      the instance index
//     imul   rcx, rax, 0x118                   the stride
//     mov    rax, qword ptr [rbx + 0x2C0]      the instance array
//     inc    byte ptr [rcx + rax + 0x4E]       THE SKIN BYTE
#define RVA_VERIFY_CYCLE 0x6FB24F
#define VERIFY_CYCLE_LEN 26
static const unsigned char EXPECT_CYCLE[VERIFY_CYCLE_LEN] =
{
    0x49,0x63,0x84,0x24,0xF4,0x05,0x00,0x00,
    0x48,0x69,0xC8,0x18,0x01,0x00,0x00,
    0x48,0x8B,0x83,0xC0,0x02,0x00,0x00,
    0xFE,0x44,0x01,0x4E
};

// exe+0x6FB284 - cmp ecx, dword ptr [rbx + 0x2A8]   the skin count
#define RVA_VERIFY_COUNT 0x6FB284
#define VERIFY_COUNT_LEN 6
static const unsigned char EXPECT_COUNT[VERIFY_COUNT_LEN] =
{ 0x3B,0x8B,0xA8,0x02,0x00,0x00 };

// exe+0x6FB07C - mov rax,[r12+0x318] / mov edx,[rax+0x410]
//                the type descriptor, and the render type id inside it
#define RVA_VERIFY_TYPE 0x6FB07C
#define VERIFY_TYPE_LEN 14
static const unsigned char EXPECT_TYPE[VERIFY_TYPE_LEN] =
{
    0x49,0x8B,0x84,0x24,0x18,0x03,0x00,0x00,
    0x8B,0x90,0x10,0x04,0x00,0x00
};

// exe+0x3CB1D7 and exe+0x3CB2E5 - the picker's two `call [CollisionRay]`.
// The return addresses we test against are these + 6, so verifying the call
// itself is what proves the return address is still the right one.
#define RVA_VERIFY_PICK_A 0x3CB1D7
#define RVA_VERIFY_PICK_B 0x3CB2E5
#define VERIFY_PICK_LEN   6
static const unsigned char EXPECT_PICK_A[VERIFY_PICK_LEN] = { 0xFF,0x15,0xE3,0x1D,0x4A,0x00 };
static const unsigned char EXPECT_PICK_B[VERIFY_PICK_LEN] = { 0xFF,0x15,0xF5,0x1C,0x4A,0x00 };

// ---- symbols resolved by name, never by RVA ----
#define SYM_TERRAIN_RENDER "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z"
#define SYM_COLLISION_RAY  "?CollisionRay@C3D_BUILDING@@QEAA_NVC3DVECTOR3@@0PEAH1PEAV2@2_N3333@Z"
#define SYM_GET_TYPE       "?GetType@C3D_BUILDING@@QEAAPEAUTYPE@1@H@Z"

// Sanity bounds. An offset that lands in a neighbouring heap object full of
// plausible bytes passes every sample check and is then written to forever, so
// nothing configurable goes unbounded.
#define SKN_MAX_BUILDINGS  500000
#define SKN_MAX_SKINS      64        // engine limit is 16; 64 is the refuse-above
#define SKN_MAX_INSTANCE   200000   // a sanity bound, not a real one: the length
                                   // of the instance array is not exposed, so
                                   // this only catches a wildly wrong index

struct SknSites
{
    unsigned buildStamp;
    unsigned rvaWorld, rvaMgrSlot, mgrInner;
    unsigned wBldVec, wDay, wYear, wBuildSel, wInfoWin;
    unsigned bTypeDesc, bInstance, bBuilt;
    unsigned tdObjName, tdRenderType;
    unsigned tyMaterials, tySkinCount, tyInstances, instStride, instSkin;
    unsigned pickRetA, pickRetB;
};

static SknSites S =
{
    SKN_BUILD_STAMP,
    RVA_WORLD, RVA_BLDMGR_SLOT, BLDMGR_INNER,
    W_OFF_BLD_VEC, W_OFF_DAY, W_OFF_YEAR, W_OFF_BUILD_SEL, W_OFF_INFO_WIN,
    B_OFF_TYPEDESC, B_OFF_INSTANCE, B_OFF_BUILT,
    TD_OFF_OBJNAME, TD_OFF_RENDERTYPE,
    TY_OFF_MATERIALS, TY_OFF_SKINCOUNT, TY_OFF_INSTANCES, INSTANCE_STRIDE, INST_OFF_SKIN,
    RVA_PICK_RET_A, RVA_PICK_RET_B
};

#endif // SKN_SITES_H
