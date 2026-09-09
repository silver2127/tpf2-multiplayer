// Export every function that has a REAL name (not FUN_xxxxxxxx), with address,
// RVA and demangled signature.
//
// Why this and not class_methods.csv: that file groups by Ghidra *namespace*,
// and namespace recovery on this binary is poor -- 51 classes for 138k
// functions. The RTTI pass, by contrast, recovered 58,624 vftable symbols with
// genuine C++ names. The names are there; it is the class<->method association
// that is missing. Searching the whole symbol table by name finds work that the
// namespace grouping loses.
//
// This is the index the proposal work needs: it turns "there is a type called
// CmdData::BuildProposal" into "here are the addresses of the functions that
// build one".
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class ExportNamedFunctions extends GhidraScript {

    private static String csv(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"")) return "\"" + s.replace("\"", "\"\"") + "\"";
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length > 0) ? args[0] : "C:\\tools\\ghidra_out";
        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[funcs] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        FunctionManager fm = currentProgram.getFunctionManager();

        PrintWriter w = new PrintWriter(new File(d, "named_functions.csv"));
        w.println("name,addr,rva,params,signature");
        int named = 0, total = 0;
        for (Function f : fm.getFunctions(true)) {
            total++;
            String n = f.getName();
            if (n == null || n.startsWith("FUN_") || n.startsWith("thunk_FUN_")) continue;
            long a = f.getEntryPoint().getOffset();
            w.println(csv(f.getName(true)) + "," + Long.toHexString(a) + ","
                    + Long.toHexString(a - base) + "," + f.getParameterCount() + ","
                    + csv(f.getSignature().getPrototypeString()));
            named++;
        }
        w.close();
        println("[funcs] " + named + " named of " + total + " total -> " + outDir
                + "\\named_functions.csv");
    }
}
