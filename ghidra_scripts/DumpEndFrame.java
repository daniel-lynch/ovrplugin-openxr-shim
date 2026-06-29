// Decompile the real frame-submission path + FFR/SpaceWarp queries to find the
// GPU-sync contract the shim must replicate (the black-frame race).
// -> ~/dev/re4vr-port/analysis/endframe_impls.txt
// @category RE4VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class DumpEndFrame extends GhidraScript {
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "/dev/re4vr-port/analysis/endframe_impls.txt";
        Set<String> targets = new HashSet<>(Arrays.asList(
            "ovrp_EndFrame4", "ovrp_EndFrame3", "ovrp_EndFrame2", "ovrp_EndFrame",
            "ovrp_BeginFrame", "ovrp_GetLayerTextureSpaceWarp",
            "ovrp_GetLayerTextureFoveation", "ovrp_GetTiledMultiResLevel",
            "ovrp_GetTiledMultiResDynamic", "ovrp_SetTiledMultiResLevel",
            "ovrp_GetLayerTexture2"));
        PrintWriter w = new PrintWriter(out);
        DecompInterface dec = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        dec.setOptions(opts);
        dec.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (Function f : fm.getFunctions(true)) {
            if (!targets.contains(f.getName())) continue;
            Function real = f.isThunk() ? f.getThunkedFunction(true) : f;
            w.println("\n/* ===== " + f.getName() + " thunk@" + f.getEntryPoint()
                      + " -> real@" + (real != null ? real.getEntryPoint() : "?")
                      + " ===== */");
            if (real == null) { w.println("  (could not resolve thunk)"); continue; }
            try {
                DecompileResults r = dec.decompileFunction(real, 180, monitor);
                DecompiledFunction df = r.getDecompiledFunction();
                w.println(df != null ? df.getC() : "  (decompile failed)");
            } catch (Exception e) { w.println("  (exception: " + e + ")"); }
        }
        w.close();
        println("DumpEndFrame: wrote " + out);
    }
}
