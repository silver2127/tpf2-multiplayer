// Dump the CONTENTS of RTTI-recovered vftables: every virtual method slot of
// every class whose name matches a substring.
//
// Why this and not a call graph: the engine dispatches player actions through
// virtual UI tool objects (UI::StreetBuilder, UI::ConstructionBuilder,
// UI::Bulldozer, ...). getCallingFunctions() cannot see a virtual call, so a
// static walk finds nothing. But RTTI recovered 58,624 vftable symbols with real
// C++ names, and a vftable is a plain array of function pointers -- reading it
// converts "this class exists" into "here are its methods, at these RVAs",
// which is exactly what the decompiler needs as input.
//
// Slot order is stable within a class, so the same slot index across sibling
// classes (e.g. every *BulldozerAction) is the same virtual method.
//
// Usage: DumpVtables.java <outdir> <substring> [<substring> ...]
// Output: vtable_dump.csv -- class,vft_rva,slot,target_rva,target_name,is_func
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class DumpVtables extends GhidraScript {

    private static String csv(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"")) return "\"" + s.replace("\"", "\"\"") + "\"";
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("[vt] usage: <outdir> <substring>..."); return; }
        String outDir = args[0];
        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[vt] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        Memory mem = currentProgram.getMemory();
        SymbolTable st = currentProgram.getSymbolTable();

        PrintWriter w = new PrintWriter(new File(d, "vtable_dump.csv"));
        w.println("class,vft_rva,slot,target_rva,target_name,is_func");

        int classes = 0, slots = 0;
        for (Symbol s : st.getAllSymbols(true)) {
            String nm = s.getName(true);
            if (nm == null || !nm.endsWith("::vftable")) continue;
            boolean hit = false;
            for (int i = 1; i < args.length; i++) {
                if (nm.toLowerCase().contains(args[i].toLowerCase())) { hit = true; break; }
            }
            if (!hit) continue;
            classes++;
            String cls = nm.substring(0, nm.length() - "::vftable".length());
            Address a = s.getAddress();

            // Walk pointer slots until one does not point into executable memory.
            for (int slot = 0; slot < 256; slot++) {
                Address pa = a.add((long) slot * 8);
                long ptr;
                try { ptr = mem.getLong(pa); } catch (Exception e) { break; }
                if (ptr == 0) break;
                Address ta;
                try {
                    ta = currentProgram.getAddressFactory()
                            .getDefaultAddressSpace().getAddress(ptr);
                } catch (Exception e) { break; }
                if (!mem.contains(ta)) break;
                if (!mem.getBlock(ta).isExecute()) break;
                // A vftable that runs into the NEXT vftable's meta_ptr stops here.
                if (slot > 0) {
                    Symbol[] at = st.getSymbols(pa);
                    boolean boundary = false;
                    for (Symbol x : at) {
                        String xn = x.getName(true);
                        if (xn != null && (xn.endsWith("::vftable")
                                || xn.endsWith("::vftable_meta_ptr"))) { boundary = true; break; }
                    }
                    if (boundary) break;
                }
                Function fn = getFunctionAt(ta);
                String tn = (fn != null) ? fn.getName() : "";
                if (tn.isEmpty()) {
                    Symbol[] ts = st.getSymbols(ta);
                    if (ts.length > 0) tn = ts[0].getName(true);
                }
                w.println(csv(cls) + "," + Long.toHexString(a.getOffset() - base) + ","
                        + slot + "," + Long.toHexString(ptr - base) + ","
                        + csv(tn) + "," + (fn != null));
                slots++;
            }
        }
        w.close();
        println("[vt] " + classes + " vftable(s), " + slots + " slot(s) -> "
                + outDir + "\\vtable_dump.csv");
    }
}
