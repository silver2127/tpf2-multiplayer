// Which of a function's call sites is on the PLAYER's path?
//
// applyProposal has 24 call sites. A lockstep hook must cancel only the one a
// player's click travels through -- suppressing an internal engine caller would
// corrupt whatever that path was doing. Static xrefs list all 24 without saying
// which carries a click, and a live probe could not answer it either:
// RtlCaptureStackBackTrace returns zero frames from inside a detour because x64
// unwinding is table-driven and the relay is VirtualAlloc'd with no
// RUNTIME_FUNCTION data.
//
// So answer it by reachability instead. Walk UP the call graph from each call
// site and look for an ancestor that touches a UI marker -- the RTTI pass
// recovered real names like UI::StreetBuilder::ProposalDataProduct and
// UI::ConstructionBuilder, and those types only exist on the interactive path.
// A call site with a UI ancestor is a player path; one without is internal.
//
// Function names are useless here (of 76,873 "named" functions, 69,880 are
// Unwind metadata), so markers are matched against the DATA symbols each
// function references, not against its name.
//
// Usage: CallGraphUp.java <outdir> <rva-or-va> <depth> <marker>...
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;

public class CallGraphUp extends GhidraScript {

    private long base;
    private String[] markers;

    /** Any data symbol this function references whose name contains a marker. */
    private String uiMarkerIn(Function f) {
        if (f == null) return null;
        try {
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
                    if (s == null) continue;
                    String n = s.getName(true);
                    if (n == null) continue;
                    for (String m : markers) {
                        if (n.toLowerCase().contains(m.toLowerCase())) {
                            return n.length() > 90 ? n.substring(0, 90) : n;
                        }
                    }
                }
            }
        } catch (Exception e) { /* a function body we cannot walk tells us nothing */ }
        return null;
    }

    private String rva(Function f) {
        return "0x" + Long.toHexString(f.getEntryPoint().getOffset() - base);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) { println("[up] usage: <outdir> <rva> <depth> <marker>..."); return; }
        String outDir = args[0];
        long v = Long.parseLong(args[1].replace("0x", ""), 16);
        int maxDepth = Integer.parseInt(args[2]);
        markers = new String[args.length - 3];
        System.arraycopy(args, 3, markers, 0, markers.length);

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[up] cannot create " + outDir); return; }

        base = currentProgram.getImageBase().getOffset();
        long target = (v >= base) ? v : base + v;
        Address ta = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(target);

        // level 0: the functions that contain a call to the target
        List<Function> sites = new ArrayList<Function>();
        Set<Long> seenSite = new HashSet<Long>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ta);
        while (it.hasNext()) {
            Reference r = it.next();
            if (!r.getReferenceType().isCall()) continue;
            Function cf = getFunctionContaining(r.getFromAddress());
            if (cf == null) continue;
            if (seenSite.add(cf.getEntryPoint().getOffset())) sites.add(cf);
        }

        PrintWriter w = new PrintWriter(new File(d, "callpath_" + Long.toHexString(target - base) + ".txt"));
        w.println("target rva 0x" + Long.toHexString(target - base));
        w.println("distinct calling functions: " + sites.size());
        w.println("markers: " + String.join(", ", markers));
        w.println();

        int uiPaths = 0;
        for (Function site : sites) {
            // BFS upward, remembering how we got there so a hit can be explained
            Map<Long, Function> parent = new HashMap<Long, Function>();
            Map<Long, Integer> depth = new HashMap<Long, Integer>();
            ArrayDeque<Function> q = new ArrayDeque<Function>();
            Set<Long> seen = new HashSet<Long>();
            q.add(site); seen.add(site.getEntryPoint().getOffset());
            depth.put(site.getEntryPoint().getOffset(), 0);

            Function hit = null; String hitSym = null;
            int visited = 0;
            while (!q.isEmpty() && visited < 400) {
                Function f = q.poll(); visited++;
                String m = uiMarkerIn(f);
                if (m != null) { hit = f; hitSym = m; break; }
                int dep = depth.get(f.getEntryPoint().getOffset());
                if (dep >= maxDepth) continue;
                for (Function c : f.getCallingFunctions(monitor)) {
                    long k = c.getEntryPoint().getOffset();
                    if (seen.add(k)) {
                        parent.put(k, f);
                        depth.put(k, dep + 1);
                        q.add(c);
                    }
                }
            }

            w.println("---- call site in " + site.getName() + " rva " + rva(site)
                    + "  (visited " + visited + " ancestors)");
            if (hit != null) {
                uiPaths++;
                w.println("    UI PATH -- marker: " + hitSym);
                // chain from the marked ancestor back down to the call site
                List<String> chain = new ArrayList<String>();
                Function cur = hit;
                while (cur != null) {
                    chain.add(cur.getName() + " " + rva(cur));
                    cur = parent.get(cur.getEntryPoint().getOffset());
                }
                for (int i = chain.size() - 1; i >= 0; i--) {
                    w.println("      " + (i == chain.size() - 1 ? "site: " : "  <- ") + chain.get(i));
                }
            } else {
                w.println("    no UI marker within depth " + maxDepth + " -- looks internal");
            }
            w.println();
        }
        w.println("call sites with a UI ancestor: " + uiPaths + " of " + sites.size());
        w.close();
        println("[up] " + uiPaths + " of " + sites.size() + " calling functions reach a UI marker -> "
                + outDir);
    }
}
