// Decompile a list of functions and record how each uses its arguments, so a
// struct layout can be derived from real field accesses.
//
// Input file: one target per line, "<rva-or-va> <label>", '#' comments allowed.
//   9e76e0 applyProposal
//
// Per target this writes:
//   <label>.c           decompiled C
//   <label>.fields.txt  offsets touched off each pointer-ish variable
//
// The field list is the point. A decompiler renders an unknown struct as
// *(int *)(param_1 + 0xe8), and it is that 0xe8 -- cross-checked against the
// live proposal bytes in docs/re/GROUND_TRUTH_applyProposal.md -- that turns a
// guess into a layout. Anything the disassembly implies but the captured bytes
// contradict is wrong, however clean the C looks. Three confident-but-wrong
// diagnoses in one day is the reason that check is not optional.
//
//@category TpF2
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class DecompileTargets extends GhidraScript {

    private static final Pattern FIELD =
        Pattern.compile("\\(([A-Za-z_][A-Za-z0-9_]*)\\s*\\+\\s*(0x[0-9a-fA-F]+|\\d+)\\)");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { println("[decomp] usage: <targets file> [outdir] [timeout]"); return; }
        String targetsFile = args[0];
        String outDir  = (args.length > 1) ? args[1] : "C:\\tools\\ghidra_out\\decomp";
        int timeout    = (args.length > 2) ? Integer.parseInt(args[2]) : 180;

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[decomp] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        FunctionManager fm = currentProgram.getFunctionManager();

        List<String[]> targets = new ArrayList<String[]>();
        BufferedReader br = new BufferedReader(new FileReader(targetsFile));
        String line;
        while ((line = br.readLine()) != null) {
            int h = line.indexOf('#');
            if (h >= 0) line = line.substring(0, h);
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] p = line.split("\\s+");
            targets.add(new String[] { p[0], (p.length > 1) ? p[1] : p[0] });
        }
        br.close();

        DecompInterface ifc = new DecompInterface();
        ifc.setOptions(new DecompileOptions());
        ifc.openProgram(currentProgram);

        int ok = 0;
        for (String[] t : targets) {
            long v = Long.parseLong(t[0].replace("0x", ""), 16);
            // accept either an RVA or a full VA
            long addr = (v >= base) ? v : base + v;
            String label = t[1].replace('/', '_').replace(':', '_');
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr);
            Function fn = fm.getFunctionContaining(a);
            if (fn == null) {
                println("[decomp] NO FUNCTION at 0x" + Long.toHexString(addr) + " (" + label
                        + ") -- address moved on this build, or not code");
                continue;
            }
            DecompileResults res = ifc.decompileFunction(fn, timeout, monitor);
            if (res == null || !res.decompileCompleted()) {
                println("[decomp] FAILED " + label + ": "
                        + (res == null ? "null" : res.getErrorMessage()));
                continue;
            }
            String c = res.getDecompiledFunction().getC();

            PrintWriter w = new PrintWriter(new File(d, label + ".c"));
            w.println("/* " + label + "  addr=0x" + Long.toHexString(addr)
                    + " rva=0x" + Long.toHexString(addr - base));
            w.println("   " + fn.getSignature().getPrototypeString() + " */");
            w.println();
            w.print(c);
            w.close();

            Map<String, Set<Long>> fields = new TreeMap<String, Set<Long>>();
            Matcher m = FIELD.matcher(c);
            while (m.find()) {
                String var = m.group(1);
                String off = m.group(2);
                long o = off.startsWith("0x") ? Long.parseLong(off.substring(2), 16)
                                              : Long.parseLong(off);
                Set<Long> s = fields.get(var);
                if (s == null) { s = new TreeSet<Long>(); fields.put(var, s); }
                s.add(o);
            }
            PrintWriter wf = new PrintWriter(new File(d, label + ".fields.txt"));
            wf.println(label + "  addr=0x" + Long.toHexString(addr)
                    + " rva=0x" + Long.toHexString(addr - base));
            wf.println(fn.getSignature().getPrototypeString());
            wf.println();
            for (Map.Entry<String, Set<Long>> e : fields.entrySet()) {
                StringBuilder sb = new StringBuilder();
                sb.append(e.getKey()).append(": ");
                for (Long o : e.getValue()) sb.append("0x").append(Long.toHexString(o)).append(" ");
                wf.println(sb.toString().trim());
            }
            wf.close();

            ok++;
            println("[decomp] " + label + "  0x" + Long.toHexString(addr)
                    + "  vars_with_fields=" + fields.size()
                    + "  ret=" + fn.getReturnType().getName());
        }
        ifc.dispose();
        println("[decomp] " + ok + "/" + targets.size() + " decompiled -> " + outDir);
    }
}
