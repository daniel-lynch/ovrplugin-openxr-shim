// Dump ovrp* struct layouts Ghidra inferred + decompile the Compositor layer
// methods (ground-truth field offsets for ovrpLayerDesc / ovrpLayerSubmit).
// -> ~/dev/re4vr-port/analysis/struct_layouts.txt
// @category RE4VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class DumpStructs extends GhidraScript {
    public void run() throws Exception {
        String out = System.getProperty("user.home")
                     + "/dev/re4vr-port/analysis/struct_layouts.txt";
        PrintWriter w = new PrintWriter(out);

        // 1) every struct/typedef whose name mentions ovrp or Layer
        w.println("## ===== INFERRED STRUCT LAYOUTS (name ~ ovrp|Layer) =====");
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        Iterator<DataType> it = dtm.getAllDataTypes();
        while (it.hasNext()) {
            DataType dt = it.next();
            String nm = dt.getName();
            if (!(nm.toLowerCase().contains("ovrp") || nm.contains("Layer"))) continue;
            if (dt instanceof Structure) {
                Structure s = (Structure) dt;
                w.println("\nstruct " + nm + "  /* size=" + s.getLength()
                          + " (0x" + Integer.toHexString(s.getLength()) + ") */ {");
                for (DataTypeComponent c : s.getComponents()) {
                    w.printf("  +0x%-4x %-24s %s%n", c.getOffset(),
                             c.getDataType().getName(),
                             c.getFieldName() == null ? "" : c.getFieldName());
                }
                w.println("};");
            } else {
                w.println(nm + "  (" + dt.getClass().getSimpleName()
                          + ", len=" + dt.getLength() + ")");
            }
        }

        // 2) decompile the Compositor layer methods for real offsets
        String[] targets = {
            "ImportLayerDesc", "ExportEyeLayerDesc", "CalculateEyeLayerDesc",
            "SetupLayer", "GetLayerTexture", "EndFrame", "SubmitLayer"
        };
        w.println("\n\n## ===== COMPOSITOR METHOD BODIES (field-offset ground truth) =====");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Set<String> seen = new HashSet<>();
        for (Function f : fm.getFunctions(true)) {
            String name = f.getName();
            boolean hit = false;
            for (String t : targets) if (name.contains(t)) { hit = true; break; }
            if (!hit) continue;
            if (!seen.add(f.getEntryPoint().toString())) continue;
            try {
                DecompileResults r = dec.decompileFunction(f, 60, monitor);
                DecompiledFunction df = r.getDecompiledFunction();
                if (df != null) {
                    w.println("\n/* ===== " + name + " @ " + f.getEntryPoint()
                              + " ===== */");
                    w.println(df.getC());
                }
            } catch (Exception e) { /* skip */ }
        }
        w.close();
        println("DumpStructs: wrote " + out);
    }
}
