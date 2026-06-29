// Ghidra headless post-script: dump decompiled ovrp_* functions.
// Signatures for all ovrp_*, full bodies for a core set ->
// ~/dev/re4vr-port/analysis/ovrp_decomp.txt
// @category RE4VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import java.io.PrintWriter;
import java.util.*;

public class DumpOvrp extends GhidraScript {
    public void run() throws Exception {
        Set<String> CORE = new HashSet<>(Arrays.asList(
            "ovrp_PreInitialize3","ovrp_Initialize5","ovrp_Shutdown2","ovrp_Update3",
            "ovrp_BeginFrame4","ovrp_EndFrame4","ovrp_WaitToBeginFrame","ovrp_GetPredictedDisplayTime",
            "ovrp_GetNodePoseState3","ovrp_GetNodePoseStateRaw","ovrp_GetControllerState4",
            "ovrp_SetupLayer","ovrp_CalculateEyeLayerDesc2","ovrp_GetLayerTexture2",
            "ovrp_GetHmdToEyeOffset2","ovrp_GetSystemHeadsetType2","ovrp_SetupDistortionWindow3"
        ));
        String out = System.getProperty("user.home") + "/dev/re4vr-port/analysis/ovrp_decomp.txt";

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();

        List<String> sigs = new ArrayList<>();
        List<String> bodies = new ArrayList<>();
        int count = 0;
        for (Function f : fm.getFunctions(true)) {
            String name = f.getName();
            if (!name.startsWith("ovrp_")) continue;
            count++;
            try {
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                DecompiledFunction df = res.getDecompiledFunction();
                if (df != null) {
                    sigs.add(df.getSignature());
                    if (CORE.contains(name)) {
                        bodies.add("/* ===== " + name + " @ " + f.getEntryPoint()
                                   + " ===== */\n" + df.getC());
                    }
                    continue;
                }
            } catch (Exception e) { /* fall through */ }
            sigs.add("/* (decomp failed) */ " + name + " @ " + f.getEntryPoint());
        }
        Collections.sort(sigs);

        PrintWriter w = new PrintWriter(out);
        w.println("# ovrp_ functions decompiled from libOVRPlugin.so (v1.51 / pkg 19.0.0)");
        w.println("# total ovrp_ functions: " + count + "\n");
        w.println("## ===== SIGNATURES =====");
        for (String s : sigs) w.println(s);
        w.println("\n## ===== CORE FUNCTION BODIES =====\n");
        for (String b : bodies) { w.println(b); w.println(); }
        w.close();

        println("DumpOvrp: wrote " + count + " signatures (" + bodies.size()
                + " core bodies) to " + out);
    }
}
