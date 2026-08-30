// Export the RTTI-recovered class map: class name -> methods -> vftables.
//
// There is no PDB for a commercial release build, so RTTI is the only source of
// real names in this binary, and it is the highest-leverage thing to extract
// first: once a vtable has a class name, every struct embedding it and every
// function taking it stops being anonymous. M1 used exactly this to get from
// 138,112 unnamed functions down to a handful of named hook targets.
//
// Java, not Python, deliberately. PyGhidra needs a pip install, matching
// platform wheels and a launcher flag; the Java script provider is always
// enabled and has no setup at all. This has to be re-runnable months from now.
//
// Output is CSV so it can be grepped and joined without opening Ghidra.
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ExportClassMap extends GhidraScript {

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
        if (!d.isDirectory() && !d.mkdirs()) {
            println("[export] cannot create " + outDir);
            return;
        }

        long base = currentProgram.getImageBase().getOffset();
        FunctionManager fm = currentProgram.getFunctionManager();

        Map<String, List<Function>> classes = new TreeMap<String, List<Function>>();
        int total = 0;
        for (Function f : fm.getFunctions(true)) {
            total++;
            Namespace ns = f.getParentNamespace();
            if (ns == null) continue;
            String name = ns.getName(true);
            if (name == null || name.isEmpty() || "Global".equals(name)) continue;
            List<Function> l = classes.get(name);
            if (l == null) { l = new ArrayList<Function>(); classes.put(name, l); }
            l.add(f);
        }

        PrintWriter w1 = new PrintWriter(new File(d, "classes.csv"));
        w1.println("class,method_count");
        for (Map.Entry<String, List<Function>> e : classes.entrySet()) {
            w1.println(csv(e.getKey()) + "," + e.getValue().size());
        }
        w1.close();

        PrintWriter w2 = new PrintWriter(new File(d, "class_methods.csv"));
        w2.println("class,method,addr,rva,signature");
        for (Map.Entry<String, List<Function>> e : classes.entrySet()) {
            for (Function f : e.getValue()) {
                long a = f.getEntryPoint().getOffset();
                w2.println(csv(e.getKey()) + "," + csv(f.getName()) + ","
                        + Long.toHexString(a) + "," + Long.toHexString(a - base) + ","
                        + csv(f.getSignature().getPrototypeString()));
            }
        }
        w2.close();

        // RTTI / vftable symbols give a class an ADDRESS, which is what lets a
        // raw pointer in a live memory dump be identified as a known type.
        PrintWriter w3 = new PrintWriter(new File(d, "vftables.csv"));
        w3.println("symbol,addr,rva");
        SymbolTable st = currentProgram.getSymbolTable();
        int nrtti = 0;
        for (Symbol s : st.getAllSymbols(true)) {
            String nm = s.getName(true);
            if (nm == null) continue;
            if (nm.contains("vftable") || nm.contains("RTTI")) {
                long a = s.getAddress().getOffset();
                w3.println(csv(nm) + "," + Long.toHexString(a) + "," + Long.toHexString(a - base));
                nrtti++;
            }
        }
        w3.close();

        println("[export] image base    : 0x" + Long.toHexString(base));
        println("[export] functions     : " + total);
        println("[export] named classes : " + classes.size());
        println("[export] rtti symbols  : " + nrtti);
        println("[export] wrote -> " + outDir);
    }
}
