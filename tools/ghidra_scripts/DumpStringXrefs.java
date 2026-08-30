// Dump EVERY defined string in the binary together with the functions that
// reference it. This is the corpus the whole action-map depends on.
//
// Why: a stripped release build has no function names (of 76,873 "named"
// functions, 69,880 are Unwind metadata). Assertion text, on the other hand, is
// compiled in verbatim, it spells out field paths and file names, and the
// function referencing an assert string is by definition code that operates on
// that field. Dumping the whole corpus ONCE turns every later question into a
// local grep instead of another serialised Ghidra run -- and the Ghidra project
// is locked to a single process, so runs cannot be parallelised.
//
// Usage: DumpStringXrefs.java <outdir> [minlen]
// Output: strings.csv  -- rva,len,ref_count,ref_func_rvas(space-sep),text
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class DumpStringXrefs extends GhidraScript {

    private static String csv(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"") || s.contains("\n"))
            return "\"" + s.replace("\"", "\"\"").replace("\n", "\\n").replace("\r", "") + "\"";
        return s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length > 0) ? args[0] : "C:\\tools\\ghidra_out";
        int minLen    = (args.length > 1) ? Integer.parseInt(args[1]) : 4;

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[str] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        PrintWriter w = new PrintWriter(new File(d, "strings.csv"));
        w.println("rva,len,nrefs,ref_func_rvas,text");

        int n = 0, withRef = 0;
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            if (monitor.isCancelled()) break;
            Data data = it.next();
            if (!data.hasStringValue()) continue;
            StringDataInstance sdi = StringDataInstance.getStringDataInstance(data);
            String txt = (sdi != null) ? sdi.getStringValue() : null;
            if (txt == null) continue;
            txt = txt.trim();
            if (txt.length() < minLen) continue;

            Address sa = data.getAddress();
            Set<String> funcs = new LinkedHashSet<String>();
            int nrefs = 0;
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(sa);
            while (ri.hasNext()) {
                Reference r = ri.next();
                nrefs++;
                Function fn = getFunctionContaining(r.getFromAddress());
                if (fn != null) funcs.add(Long.toHexString(fn.getEntryPoint().getOffset() - base));
                else funcs.add("@" + Long.toHexString(r.getFromAddress().getOffset() - base));
                if (funcs.size() > 512) break;
            }
            if (nrefs > 0) withRef++;

            StringBuilder sb = new StringBuilder();
            for (String f : funcs) { if (sb.length() > 0) sb.append(' '); sb.append(f); }

            w.println(Long.toHexString(sa.getOffset() - base) + "," + txt.length() + ","
                    + nrefs + "," + csv(sb.toString()) + "," + csv(txt));
            n++;
        }
        w.close();
        println("[str] " + n + " strings (" + withRef + " referenced) -> " + outDir + "\\strings.csv");
    }
}
