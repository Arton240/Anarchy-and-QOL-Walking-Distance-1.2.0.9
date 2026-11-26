// wk_sites.h - every address the walking part knows, per game build.
//
// PROVENANCE, AND WHAT IS AND IS NOT CHECKED. These come from the TesmioLoader
// `walking` plugin (plugins/walking/walking.cpp, docs/12-walking.md and the port
// notes docs/17 and docs/19 in that repository), not from a disassembly done
// for this repo. Nothing in this repository has a SOVIET64.exe to read them
// off. What protects you is the runtime check in mod_walking.inc: every site is
// compared against the instruction shape, the constant it resolves to AND the
// value stored there before a single byte is written, so a wrong row refuses
// instead of corrupting code.
//
//   v1.1.1.9  PE 0x6A3EB6AD  - the build the rest of sovietkit is read off.
//             Table is docs/17-port-v1.1.1.9.md, "plugins/walking/walking.cpp".
//   v1.2.0.8  PE 0x6B0097B2  - plugins/walking/walking.cpp as shipped.
//   v1.2.0.9  NOT HERE. No executable, no stamp, no addresses. See the ini's
//             [sites] section and tools/walking_scan.py for how to add it.
//
// Cross-check that is possible without the exe: between the two rows every
// address moves by ONE constant per region - 0x189F0 across the five builder
// and collector sites, 0x24600 across the three overlay sites, 0x243DE across
// the save loader's two, 0x47548 across the four .rdata floats. Fourteen
// addresses, four deltas; a transcription slip would show up as an odd one out.
// (This checks that the rows agree with each other, not that either is right.)
//
// WHAT EACH SITE IS. The game spells the walking distance out in four
// functions and there is no shared constant, so there are eight limit sites:
//
//   limitOp        mov dword ptr [rsp+0x7C],480.0f   (or [rbp-0x80] from
//                  v1.2.0.8) - the batch builder, an immediate. The only one
//                  that is overwritten rather than repointed.
//   walkQueued     movss xmm0,[rip+walkConst]  one-building builder, the path a
//                  running game takes
//   walkOverlay1/2 movss xmm0,[rip+walkConst]  the walking-distance button's
//                  highlight, which re-runs the search with its own copy
//   carBuild       movss xmm0,[rip+carConst]   parking builder
//   carOverlay     movss xmm0,[rip+carConst]   parking highlight
//   walkRebuild    movss xmm0,[rip+walkRadiusConst]  collector: which buildings
//   carRebuild     movss xmm0,[rip+carRadiusConst]   to rebuild after a road
//                  changes. A radius, deliberately wider than the limit.
//
// and the save loader's "this save predates the longer distances" test:
//
//   regenCmp       83 3D <disp32> 6E        cmp dword [saveVersion],110
//   regenTest      84 C0 0F 85 ...          test al,al / jnz
//   saveVersion    the .data slot regenCmp resolves to

#ifndef WK_SITES_H
#define WK_SITES_H

#define WK_VANILLA_WALK   480.0f
#define WK_VANILLA_CAR    2500.0f
#define WK_VANILLA_WALK_R 530.0f
#define WK_VANILLA_CAR_R  2600.0f

struct WkSites
{
    unsigned    buildStamp;
    const char* label;

    unsigned    limitOp;

    unsigned    walkConst;
    unsigned    carConst;
    unsigned    walkRadiusConst;
    unsigned    carRadiusConst;

    unsigned    walkQueued;
    unsigned    walkOverlay1;
    unsigned    walkOverlay2;
    unsigned    carBuild;
    unsigned    carOverlay;
    unsigned    walkRebuild;
    unsigned    carRebuild;

    unsigned    regenCmp;
    unsigned    regenTest;
    unsigned    saveVersion;
};

static const WkSites WK_PROFILES[] =
{
    {   // v1.1.1.9 - docs/17-port-v1.1.1.9.md
        0x6A3EB6ADu, "v1.1.1.9",
        0x12E2CD,
        0x90AF20, 0x90B104, 0x90AF58, 0x90B108,
        0x12E79F, 0x43F0EA, 0x43F8D5, 0x12F916, 0x440053, 0x12DE97, 0x12F4F2,
        0x438406, 0x438413, 0x9E9C3C
    },
    {   // v1.2.0.8 - walking.cpp 1.3, docs/19-port-v1.2.0.8.md
        0x6B0097B2u, "v1.2.0.8",
        0x146CBD,
        0x952468, 0x95264C, 0x9524A0, 0x952650,
        0x14718F, 0x4636EA, 0x463ED5, 0x148306, 0x464653, 0x146887, 0x147EE2,
        0x45C7E4, 0x45C7F1, 0xA33CFC
    },
};

#define WK_NPROFILES ((int)(sizeof(WK_PROFILES) / sizeof(WK_PROFILES[0])))

#endif // WK_SITES_H
