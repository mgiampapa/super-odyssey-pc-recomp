// Super Mario Odyssey 1.4.1 (main build b424be150a8e7d78): its main exports almost no symbols (44 of 1.0.0's 90,530),
// so the game functions the runtime looks up by name (loader_find_symbol) come from this table instead - found by
// tools/match141.py (vtables through RTTI, code shape, call graph, fuzzy candidates checked by hand: tools/port141.py,
// tools/dis141.py, tools/refs141.py). Absent here, by what was found:
//   - agl::utl::DynamicTextureAllocator::calc: never called by the game, dropped from 1.4.1's build (no function sets
//     its 0x4000 flag); odyssey_120.c needs another way for its extra draws
//   - nn::g3d::SkeletonObj::CalculateWorldMtx: inlined into its callers in 1.4.1 (no candidate near its 4 callers)
//   - sead::system::HaltWithDetail / PrintStringImpl: no candidate (probably dropped with the release build's asserts)
//   - the completed-save builder's and heap diagnostics' names: not ported yet
#pragma once
#include <stdint.h>

typedef struct { const char *name; uint64_t addr; } OdysseyV141Sym;

static const OdysseyV141Sym ODYSSEY_V141_SYMS[] = {
    // 120 FPS (runtime/hle/odyssey_120.c)
    {"_ZN10GameSystem8movementEv", 0x70004e2730ull},                                   // vtable GameSystem slot 4
    {"_ZN19PlayerActorHakoniwa8movementEv", 0x70003c06b0ull},                          // vtable slot 7
    {"_ZN2al15GameFrameworkNx9procDraw_Ev", 0x70006254b0ull},                          // vtable slot 30
    {"_ZNK10StageScene8drawMainEv", 0x7000473270ull},                                  // vtable slot 8
    {"_ZNK20DemoChangeWorldScene8drawMainEv", 0x7000446d80ull},                        // fuzzy 0.47, same size
    {"_ZNK9DemoScene8drawMainEv", 0x7000448390ull},                                   // vtable DemoScene slot 8
    {"_ZN2al13getProjectionEPKNS_10IUseCameraEi", 0x70005b3e10ull},                    // code shape
    {"_ZNK4sead10Projection25getDeviceProjectionMatrixEv", 0x7000967d30ull},           // call graph
    {"_ZN4sead15GameFrameworkNx9procCalc_Ev", 0x70009421a0ull},                        // vtable slot 31
    {"_ZN4sead13ControllerMgr4calcEv", 0x7000964b10ull},                               // vtable slot 22
    {"_ZN2al18updateGraphicsPrevEPNS_5SceneE", 0x700074e940ull},                       // code shape
    {"_ZNK4sead12LookAtCamera14doUpdateMatrixEPNS_8Matrix34IfEE", 0x70009661a0ull},    // vtable slot 4
    {"_ZN3agl3utl23DynamicTextureAllocator9sInstanceE", 0x70014af318ull},              // 30 functions' GOT use agree
                                                                                       // (its GOT slot 0x7001366240)
    // skeletal poses at the half tick (runtime/hle/odyssey_pose.c)
    {"_ZN2al13AnimPlayerSkl11calcSklAnimEv", 0x7000571770ull},                         // call graph
    {"_ZN2nn3g3d11SkeletonObj17CalculateSkeletonEi", 0x70008f41acull},                 // code shape
    // sead
    {"_ZN4sead6system4HaltEv", 0x700097bfa0ull},                                       // code shape
    // the stage (runtime/hle/odyssey_warp.c)
    {"_ZN10StageScene4initERKN2al13SceneInitInfoE", 0x700046e7a0ull},                  // vtable slot 3
    {"_ZN10StageScene7controlEv", 0x7000472af0ull},                                    // vtable slot 7
    // Warp test tool: verified constructor fields/call sites and neighboring wrappers in both builds.
    {"_ZN15ChangeStageInfoC1EPK14GameDataHolderPKcS4_biNS_15SubScenarioTypeE", 0x700021c040ull},
    {"_ZN16GameDataFunction18tryChangeNextStageE20GameDataHolderWriterPK15ChangeStageInfo", 0x70004d2c50ull},
    {"_ZN16GameDataFunction35tryChangeNextStageWithDemoWorldWarpE20GameDataHolderWriterPKc", 0x70004d2d30ull},
    {"_ZN16GameDataFunction19getCurrentStageNameE22GameDataHolderAccessor", 0x70004d2df0ull},
};

// Addresses the runtime uses directly for 1.0.0, and their 1.4.1 counterparts
#define ODYSSEY_V141_ANIMPLAYERSKL_VTABLE 0x70012dbc28ull   // al::AnimPlayerSkl's vtable + 0x10 (1.0.0: 0x7001dd63f8)
#define ODYSSEY_V141_BUFFER_MAP 0x70008fe8b4ull             // nn::gfx::detail::BufferImpl::Map (1.0.0: 0x7000ae6e40)
#define ODYSSEY_V141_MODELKEEPER_CALC 0x70006b9ec0ull       // al::ModelKeeper::calc (1.0.0: 0x700094c428)
#define ODYSSEY_V141_DRAWMAIN_CAMERA 0x7000473354ull        // in StageScene::drawMain (1.0.0: 0x70004ccbc8)
#define ODYSSEY_V141_UPDATE_VIEW_GPU 0x70005e77f0ull        // al::GraphicsSystemInfo::updateViewGpu (1.0.0: 0x7000879c98)
