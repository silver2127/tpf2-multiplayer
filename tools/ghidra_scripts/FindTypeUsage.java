// Turn a TYPE NAME into FUNCTION ADDRESSES via vftable cross-references.
//
// Needed because the two naming mechanisms in this binary recover different
// things. RTTI gave 58,624 vftable symbols with genuine C++ names
// (construction_builder_util::Proposal, CmdData::BuildProposal, ...), but
// function names were not recovered at all -- of 76,873 "named" functions,
// 69,880 are Unwind metadata and 4,926 are Catch_All handlers. A stripped
// release build has no symbols to demangle. So there is no function called
// "BuildProposal" to look up.
//
// The bridge is a cross-reference. A C++ constructor writes the vftable pointer
// into the object it is constructing, so whatever code REFERENCES a vftable
// address is the constructor (or a place that type-checks/casts). That converts
// "this type exists" into "here is where it is built", which is the address the
// decompiler needs.
//
// Usage: FindTypeUsage.java <outdir> <substring> [<substring> ...]
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class FindTypeUsage extends GhidraScript {

    private static String csv(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"")) return "\"" + s.replace("\"", "\"\"") + "\"";
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("[xref] usage: <outdir> <substring>..."); return; }
        String outDir = args[0];
        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[xref] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        SymbolTable st = currentProgram.getSymbolTable();

        PrintWriter w = new PrintWriter(new File(d, "type_usage.csv"));
        w.println("type_symbol,symbol_addr,ref_from,ref_rva,ref_type,containing_func,func_rva");

        int syms = 0, refs = 0;
        for (Symbol s : st.getAllSymbols(true)) {
            String nm = s.getName(true);
            if (nm == null) continue;
            boolean hit = false;
            for (int i = 1; i < args.length; i++) {
                if (nm.toLowerCase().contains(args[i].toLowerCase())) { hit = true; break; }
            }
            if (!hit) continue;
            syms++;
            Address sa = s.getAddress();
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(sa);
            while (it.hasNext()) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function fn = getFunctionContaining(from);
                long fa = (fn != null) ? fn.getEntryPoint().getOffset() : 0;
                w.println(csv(nm) + "," + Long.toHexString(sa.getOffset()) + ","
                        + Long.toHexString(from.getOffset()) + ","
                        + Long.toHexString(from.getOffset() - base) + ","
                        + r.getReferenceType() + ","
                        + (fn != null ? csv(fn.getName()) : "") + ","
                        + (fa != 0 ? Long.toHexString(fa - base) : ""));
                refs++;
            }
        }
        w.close();
        println("[xref] matched " + syms + " symbol(s), " + refs + " reference(s) -> "
                + outDir + "\\type_usage.csv");
    }
}
