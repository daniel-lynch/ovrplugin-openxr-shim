// Decompile the real Vulkan-init path: any function that calls the vrapi Vulkan
// creation imports, plus the Vk extension getters and Initialize impls.
// -> ~/dev/re4vr-port/analysis/init_vulkan.txt
// @category RE4VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class DumpInit extends GhidraScript {
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "/dev/re4vr-port/analysis/init_vulkan.txt";
        PrintWriter w = new PrintWriter(out);

        // imports whose callers we want to see
        Set<String> wantCalls = new HashSet<>(Arrays.asList(
            "vrapi_CreateSystemVulkan", "vrapi_CreateSystemVulkan2",
            "vrapi_EnterVrMode", "vrapi_Initialize", "vrapi_GetDeviceExtensionsVulkan",
            "vrapi_GetInstanceExtensionsVulkan"));
        // also decompile functions whose own name matches these
        String[] nameHits = { "InitializeVulkan", "GetInstanceExtensionsVk",
            "GetDeviceExtensionsVk", "CreateSystemVulkan", "InitializeInternal" };

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Set<String> done = new HashSet<>();

        for (Function f : fm.getFunctions(true)) {
            boolean hit = false;
            String nm = f.getName();
            for (String s : nameHits) if (nm.contains(s)) { hit = true; break; }
            if (!hit) {
                try {
                    for (Function callee : f.getCalledFunctions(monitor)) {
                        if (wantCalls.contains(callee.getName())) { hit = true; break; }
                    }
                } catch (Exception e) {}
            }
            if (!hit) continue;
            if (!done.add(f.getEntryPoint().toString())) continue;
            try {
                DecompileResults r = dec.decompileFunction(f, 90, monitor);
                DecompiledFunction df = r.getDecompiledFunction();
                if (df != null) {
                    w.println("\n/* ===== " + nm + " @ " + f.getEntryPoint() + " ===== */");
                    w.println(df.getC());
                }
            } catch (Exception e) {}
        }
        w.close();
        println("DumpInit: wrote " + done.size() + " functions to " + out);
    }
}
