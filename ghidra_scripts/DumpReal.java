// Resolve export thunks to their real implementations and decompile them.
// -> ~/dev/re4vr-port/analysis/real_impls.txt
// @category RE4VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class DumpReal extends GhidraScript {
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "/dev/re4vr-port/analysis/real_impls.txt";
        Set<String> targets = new HashSet<>(Arrays.asList(
            "ovrp_Initialize5", "ovrp_PreInitialize3", "ovrp_SetupDistortionWindow3",
            "ovrp_GetInstanceExtensionsVk", "ovrp_GetDeviceExtensionsVk"));
        PrintWriter w = new PrintWriter(out);
        DecompInterface dec = new DecompInterface();
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
                DecompileResults r = dec.decompileFunction(real, 120, monitor);
                DecompiledFunction df = r.getDecompiledFunction();
                w.println(df != null ? df.getC() : "  (decompile failed)");
            } catch (Exception e) { w.println("  (exception)"); }
        }
        w.close();
        println("DumpReal: wrote " + out);
    }
}
