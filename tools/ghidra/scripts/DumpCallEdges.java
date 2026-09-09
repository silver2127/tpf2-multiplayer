// Dump the whole DIRECT call graph as an edge list, once.
//
// Direct calls are only part of the story in this engine -- boost::signals2,
// std::function, packaged_task and virtual dispatch all break the chain, and two
// earlier static walks (depth 4 and 8) found 0 of 22 targets because of exactly
// that. So this is NOT a way to answer "who ultimately triggers X".
//
// What it IS good for: the last hop. Command factories in make_command.cpp are
// plain free functions, and the UI component that builds a command calls one
// directly. Joining these edges against func2src.csv (function -> __FILE__ from
// assert strings) turns "something calls factory 0x9dca00" into
// "vehiclestore.cpp calls factory 0x9dca00", which names the action.
//
// Dumping every edge once beats a per-question Ghidra run, because the project
// is locked to a single process and queries cannot be parallelised.
//
// Usage: DumpCallEdges.java <outdir>
// Output: call_edges.csv -- caller_rva,callee_rva,site_rva
//
//@category TpF2
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

public class DumpCallEdges extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length > 0) ? args[0] : "C:\\tools\\ghidra_out";
        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[edges] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        BufferedWriter w = new BufferedWriter(new FileWriter(new File(d, "call_edges.csv")), 1 << 20);
        w.write("caller_rva,callee_rva,site_rva\n");

        long n = 0, nf = 0;
        FunctionIterator fit = currentProgram.getFunctionManager().getFunctions(true);
        while (fit.hasNext()) {
            if (monitor.isCancelled()) break;
            Function f = fit.next();
            nf++;
            String cr = Long.toHexString(f.getEntryPoint().getOffset() - base);
            InstructionIterator iit =
                currentProgram.getListing().getInstructions(f.getBody(), true);
            while (iit.hasNext()) {
                Instruction ins = iit.next();
                for (Reference r : ins.getReferencesFrom()) {
                    if (!r.getReferenceType().isCall()) continue;
                    Address ta = r.getToAddress();
                    if (!ta.isMemoryAddress()) continue;
                    w.write(cr);
                    w.write(',');
                    w.write(Long.toHexString(ta.getOffset() - base));
                    w.write(',');
                    w.write(Long.toHexString(ins.getAddress().getOffset() - base));
                    w.write('\n');
                    n++;
                }
            }
        }
        w.close();
        println("[edges] " + nf + " functions, " + n + " call edges -> "
                + outDir + "\\call_edges.csv");
    }
}
