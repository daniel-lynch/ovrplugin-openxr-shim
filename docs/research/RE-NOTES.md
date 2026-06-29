# Reverse-engineering notes — libOVRPlugin.so

Pinned version: OVRPlugin **1.51** / pkg `ovrplugin-android-universal:19.0.0.449.531`.
Cross-reference header: public **OVRPlugin.cs @ v1.51** (Unity Oculus Integration,
many GitHub mirrors) — gives ovrp_ signatures + [StructLayout] struct layouts.

## RE pipeline (working)
Headless Ghidra 12.1.2, driven by `ghidra_scripts/DumpOvrp.java`:
```
export JAVA_HOME=~/dev/re4vr-port/tools/jdk-21.0.11+10   # portable Temurin JDK
tools/ghidra_12.1.2_PUBLIC/support/analyzeHeadless ghidra_proj re4vr \
  -import dump/apk_libs/lib/arm64-v8a/libOVRPlugin.so \
  -scriptPath ghidra_scripts -postScript DumpOvrp.java -deleteProject
```
Gotchas solved: Ghidra needs a JDK (JREs rejected) -> portable Temurin in tools/.
Ghidra 12 dropped bundled Jython -> scripts must be Java, not .py.
Output: `analysis/ovrp_decomp.txt` (537 ovrp_ sigs, 27 core bodies).

## Key structural finding
Exported ovrp_* are THIN THUNKS: `(*PTR_ovrp_X)()` jumping to the real impl,
which calls internal C++ `OVR::Util::Compositor::*` (partially symbolized).
=> The game (Java System.loadLibrary + dlsym) calls the EXPORTED symbols. So the
   SHIM just re-exports the same ovrp_ symbol names backed by OpenXR and replaces
   libOVRPlugin.so wholesale. We do NOT reverse the internal Compositor C++.
   Decompilation = reference for semantics + struct sizes only.

## ABI confirmed (triangulation: Ghidra shape + v1.51 header types = MATCH)
ovrp_GetNodePoseState3(ovrpStep, int frameIndex, ovrpNode, ovrpPoseStatef* out):
- null out  -> returns 0xfffffc17 = -1001 (ovrpFailure_InvalidParameter)
- not init  -> returns 0xfffffc16 = -1002 (ovrpFailure_NotInitialized)
- success   -> memcpy(out, ..., 0x58) then return 0
- **ovrpPoseStatef = 0x58 = 88 bytes** == public layout:
  ovrpPosef(28) + 4x ovrpVector3f(48) + double Time(8) -> pad 88. EXACT MATCH.
=> Public OVRPlugin.cs v1.51 struct layouts are TRUSTWORTHY for this binary.
   ovrpResult error-code convention confirmed (-1001 invalid, -1002 not-init).

## Layer structs reversed (2026-06-23, analysis/struct_layouts.txt)
Ghidra recovered C++ type NAMES from demangled symbols but empty layouts (no DWARF);
real offsets came from decompiling OVR::Util::Compositor methods.
- Two compositor backends: CompositorVRAPI_OpenGL + CompositorVRAPI_Vulkan (RE4=Vulkan).
- **ovrpLayerDesc = 0x7c (124B)** [VERIFIED]: ImportLayerDesc memset's 0x7c; switch on
  +0x00 = ovrpShape (cases 0,1,2,4,5 overlays; case 3 = EyeFov projection). Three
  historical copy sizes 0x68/0x6c/0x7c = base / +DepthFormat / +MotionVector. Full
  field layout written to header, offsets confirmed (Fov@0x20, VisibleRect@0x40,
  DepthFormat@0x68).
- **ovrpLayerSubmit = 0x130 (304B)** [VERIFIED]: EndFrame4 allocs count*0x130. Header
  fields (LayerId, TextureStage, ViewportRect[2], Pose@0x28, Flags) = public layout;
  per-shape union tail still [TODO] (reserved bytes for now). Stored internally as
  ovrpLayerSubmitUnion in a std::map<int,pair<union,int>>.
Header now has _Static_asserts sizeof(ovrpLayerDesc)==124 && ovrpLayerSubmit==304;
both pass, shim rebuilds (239 symbols).

## Next RE passes (when continuing)
- Dedup the export-thunk vs impl entries in the dump (script polish).
- For each CORE fn: pair Ghidra arg-shape with v1.51 C# sig -> finalized C header
  for the shim (ovrp types + struct layouts).
- Reverse outliers NOT in the public header (e.g. SpaceWarp/ASW property paths the
  binary references, any custom Armature behavior).
- Separately: entitlement path in libovrplatformloader (ovr_* stub) — different lib.
