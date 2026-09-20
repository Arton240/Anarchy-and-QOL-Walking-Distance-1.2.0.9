// tc_sites.h - the one address trackcrew needs, and the bytes it expects there.
//
// Provenance: read statically out of SOVIET64.exe v1.1.1.9, TimeDateStamp
// 0x6A3EB6AD. See TRACKCREW-RE-FINDINGS.md.

#ifndef TC_SITES_H
#define TC_SITES_H

#define TC_BUILD_STAMP 0x6A3EB6ADu

// ---------------------------------------------------------------------------
// 0x6AC6E0 - "may this train proceed?"
//
//     char CanProceed(Vehicle* v)      1 = go, 0 = brake and wait
//
// Its very first act is the game's own Ignore-red-signal check:
//
//     006ac6e6  cmp byte ptr [rcx + 0x5E0], 0
//     006ac6ed  mov rbx, rcx
//     006ac6f0  je  the_real_check
//     006ac6f2  mov al, 1                       <- flag set: proceed, full stop
//     006ac6f9  ret
//
// and only otherwise does it walk the vehicle's path (`v+0x6A0..0x6A8`, current
// index at `v+0x700`) looking at the signal and block state ahead.
//
// Both callers are in the train movement routine 0x6A7520 and have the identical
// shape:
//
//     mov  rcx, rsi          ; rsi = the vehicle
//     call 0x6AC6E0
//     test al, al
//     jne  skip_braking      ; proceeding skips the whole brake/wait block,
//                            ; INCLUDING the following-distance test against
//                            ; the vehicle ahead - which is why one hook covers
//                            ; both "goes through red" and "does not queue
//                            ; behind another track builder".
//
// So returning 1 from here is not a new behaviour we are inventing. It is
// byte-for-byte what happens when a player clicks "Ignore red signal" on that
// train, except that it does not clear itself afterwards (the game zeroes
// +0x5E0 at 0x6A8E9A once the train is through).
#define RVA_CAN_PROCEED 0x6AC6E0
#define CAN_PROCEED_STOLEN 16

// push rbx / sub rsp,0x70 / cmp byte [rcx+0x5E0],0 / mov rbx,rcx
// 2 + 4 + 7 + 3 = 16 bytes, ends on an instruction boundary, no relative
// branches in the stolen range.
static const unsigned char EXPECT_CAN_PROCEED[CAN_PROCEED_STOLEN] =
{
    0x40,0x53,
    0x48,0x83,0xEC,0x70,
    0x80,0xB9,0xE0,0x05,0x00,0x00,0x00,
    0x48,0x8B,0xD9
};

// The vehicle's own ignore-red-signal flag, for reading and for the report.
// We do NOT write it: a hook that answers the question is reversible the moment
// the plugin is switched off, while a byte written into a vehicle persists into
// the save and would leave every track builder permanently signal-blind even
// after the mod is removed.
#define V_OFF_IGNORE_RED 0x5E0

// Where a vehicle's object folder name is expected to live. Treated as a HINT,
// not a fact: the plugin scans for the field at runtime and logs what it found,
// because this offset came from another mod's notes rather than from the
// disassembly above.
#define V_OFF_OBJNAME_HINT 0x200
#define V_OBJNAME_SCAN_MAX 0x800

// The vehicle's type object. The vehicle info window reads a kind enum out of
// [vehicle+0x1708]+0x294 to decide which buttons to show, so +0x1708 is a type
// pointer - and a type is the likeliest place for a model name to live.
#define V_OFF_TYPE 0x1708

// ---- ASKING THE GAME INSTEAD OF GUESSING -----------------------------------
//
// Every name list is a guess. `$TRAINGROUP_TRACKBUILDER` in a train's
// script.ini is not: it is how the game itself is told which vehicles are track
// builders, and it is the only thing that is equally true of the base game's
// eleven, the DLC's, and a workshop train nobody has ever seen.
//
// The parser interns that keyword into an INDEX - "trackbuilder" is looked up
// in a vector of 0x44-byte feature records hanging off the world object - and
// stores the index in a plain vector<int> on the vehicle TYPE. The game's own
// test is exe+0x3E9420, and it is small enough to quote in full:
//
//     bool HasFeature(ignored, TYPE* t, int idx)
//     {
//         if (!t || idx < 0) return false;
//         int* b = *(int**)(t + 0x96B8);
//         int* e = *(int**)(t + 0x96C0);
//         for (; b != e; ++b) if (*b == idx) return true;
//         return false;
//     }
//
// rcx is not touched. So there is nothing to call - two pointer reads and a
// walk of a handful of ints, which we can do ourselves under a guard.
//
// The index comes from the same place: exe+0x25C7B0 walks the vector at
// world+0x127E8..+0x127F0, stride 0x44, comparing the record's string at
// offset 0, and returns the position. We resolve "trackbuilder" once.
#define W_OFF_FEATURES     0x127E8   // vector<FeatureRec> begin on the world
#define W_OFF_FEATURES_END 0x127F0   // ...and end
#define FEATURE_STRIDE     0x44      // name is at offset 0 of each record
#define T_OFF_GROUPS       0x96B8    // vector<int> begin on the vehicle type
#define T_OFF_GROUPS_END   0x96C0    // ...and end
#define FEATURE_TRACKBUILDER "trackbuilder"

// The world object is the same global skins calls the world; both of these are
// verified by the bytes below rather than trusted.
#define RVA_WORLD_OBJ 0x9D4F10

// The two functions whose instructions ENCODE the four offsets above. We never
// call or patch them - reading them back is how "our notes match this binary"
// stops being a claim and starts being a check.
#define RVA_VERIFY_HASFEATURE 0x3E9420
#define VERIFY_HASFEATURE_LEN 27
static const unsigned char EXPECT_HASFEATURE[VERIFY_HASFEATURE_LEN] =
{
    0x48,0x85,0xD2, 0x74,0x4A, 0x45,0x85,0xC0, 0x78,0x45,
    0x48,0x8B,0x82,0xC0,0x96,0x00,0x00,          // mov rax,[rdx+0x96C0]
    0x45,0x33,0xC9,
    0x48,0x2B,0x82,0xB8,0x96,0x00,0x00           // sub rax,[rdx+0x96B8]
};

#define RVA_VERIFY_FEATIDX 0x25C7B0
#define VERIFY_FEATIDX_LEN 27
static const unsigned char EXPECT_FEATIDX[VERIFY_FEATIDX_LEN] =
{
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x7C,0x24,0x10,
    0x4C,0x8B,0x81,0xF0,0x27,0x01,0x00,          // mov r8,[rcx+0x127F0]
    0x48,0x8B,0xDA,
    0x4C,0x2B,0x81,0xE8,0x27,0x01,0x00           // sub r8,[rcx+0x127E8]
};

// ---------------------------------------------------------------------------
// THE ONE-WAY SEMAPHORE, AND WHY CAN-PROCEED WAS NEVER GOING TO BE ENOUGH
//
// A track builder that has laid track past a set of one-way signals pointing
// the wrong way does not sit at a red light. It never gets a route at all: the
// rail pathfinder treats a one-way semaphore as a directed edge and refuses to
// expand through it, so the search fails before any signal is ever looked at.
// Hooking "may I proceed?" cannot help with that, because nobody asks.
//
// A semaphore's DIRECTION lives at +0x14 of the semaphore object, which hangs
// off a rail node at +0xB8. The values are not a guess - exe+0x41A510 is the
// handler for clicking a placed semaphore, and it cycles the field itself:
//
//     0041a768  mov dword ptr [rax + 0x14], 0      ; two-way
//     0041a7f0  mov dword ptr [rax + 0x14], 1      ; one-way, one sense
//     0041a80e  mov dword ptr [rax + 0x14], 2      ; one-way, the other
//
// and 10 (0xA) is a waypoint, which never restricts anything.
//
// FOUR PLACES READ IT FOR DIRECTION. Two in the A* edge-cost function
// (exe+0x57A1A0, reached from the rail search at exe+0x57DBC0), two in the
// path builder that turns a found route into a drivable path (exe+0x583D70):
//
//     0057a44f  cmp dword ptr [rcx + 0x14], 1
//     0057a453  jne skip                       ; <- patched to jmp
//     0057a455  cmp dl, 1
//     0057a458  je  impassable                 ; returns -1.0f
//     0057a465  cmp dword ptr [rcx + 0x14], 2
//     0057a469  jne ok                         ; <- patched to jmp
//
//     0058450c  cmp edx, 0xa
//     0058450f  je  ok                         ; <- patched to jmp
//     005847e8  cmp r8d, 0xa
//     005847ec  je  ok                         ; <- patched to jmp
//
// WHAT THE FOUR EDITS MEAN, EXACTLY. Each one is a conditional jump turned into
// the unconditional jump to the SAME target - one byte, 0x74/0x75 -> 0xEB, no
// instruction boundary moved. And the target is not a new path: it is the path
// the game already takes when the semaphore is two-way. Trace direction 0
// through all four tests and it lands exactly where the patched code lands.
//
// So this is not "signals are ignored". It is precisely: every one-way
// semaphore is seen as a two-way semaphore, by the router only. Red is still
// red, blocks are still blocks, and the movement code is untouched - that is
// what the can-proceed hook above is for.
//
// IT IS NOT SCOPED TO TRACK BUILDERS, and it cannot be. The search context is a
// stack local built by whichever caller wanted a route (exe+0x582400 has 82 of
// them, road and ship and air among them); there is no vehicle pointer at a
// fixed offset to test. While these bytes are patched, EVERY train may plan a
// route the wrong way down a one-way section. That is why it is off by default
// and lives on a key.
#define RVA_ONEWAY_PF1 0x57A44Fu
#define ONEWAY_PF1_LEN 8
#define ONEWAY_PF1_AT  4
static const unsigned char EXPECT_ONEWAY_PF1[ONEWAY_PF1_LEN] =
{ 0x83,0x79,0x14,0x01, 0x75,0x05, 0x80,0xFA };

#define RVA_ONEWAY_PF2 0x57A465u
#define ONEWAY_PF2_LEN 8
#define ONEWAY_PF2_AT  4
static const unsigned char EXPECT_ONEWAY_PF2[ONEWAY_PF2_LEN] =
{ 0x83,0x79,0x14,0x02, 0x75,0x37, 0x84,0xD2 };

#define RVA_ONEWAY_PB1 0x58450Cu
#define ONEWAY_PB1_LEN 7
#define ONEWAY_PB1_AT  3
static const unsigned char EXPECT_ONEWAY_PB1[ONEWAY_PB1_LEN] =
{ 0x83,0xFA,0x0A, 0x74,0x72, 0x48,0x8B };

#define RVA_ONEWAY_PB2 0x5847E8u
#define ONEWAY_PB2_LEN 8
#define ONEWAY_PB2_AT  4
static const unsigned char EXPECT_ONEWAY_PB2[ONEWAY_PB2_LEN] =
{ 0x41,0x83,0xF8,0x0A, 0x74,0x16, 0x48,0x8B };

#define ONEWAY_PATCH_BYTE 0xEB

struct TcSites
{
    unsigned buildStamp;
    unsigned canProceed;
    unsigned vIgnoreRed;
    unsigned vNameHint;
    unsigned vNameScanMax;
    unsigned vTypeOff;
    unsigned oneWay[4];
};

static TcSites S =
{
    TC_BUILD_STAMP,
    RVA_CAN_PROCEED,
    V_OFF_IGNORE_RED,
    V_OFF_OBJNAME_HINT,
    V_OBJNAME_SCAN_MAX,
    V_OFF_TYPE,
    { RVA_ONEWAY_PF1, RVA_ONEWAY_PF2, RVA_ONEWAY_PB1, RVA_ONEWAY_PB2 }
};

#endif // TC_SITES_H
