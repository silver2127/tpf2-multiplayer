// For each RVA given, print the instruction boundaries of the first 48 bytes
// and flag instructions that cannot be relocated into a trampoline: anything
// with a RIP-relative memory reference or a call/jump. A hook steals the
// first N bytes and re-executes them elsewhere, so N must be an instruction
// boundary with no flagged instruction before it. hook.cpp supports
// N in {14,15,17,18,19,20,21}.
//
// Usage: PrologueBoundaries.java <outfile> <rva-hex> [<rva-hex> ...]
//@category TpF2
import java.io.File;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

public class PrologueBoundaries extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("[pb] usage: <outfile> <rva> ..."); return; }
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter w = new PrintWriter(new File(args[0]));
        for (int i = 1; i < args.length; i++) {
            long rva = Long.parseLong(args[i].replace("0x", ""), 16);
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(base + rva);
            w.println("FUNC " + args[i]);
            Instruction ins = getInstructionAt(a);
            if (ins == null) { w.println("  (no instruction at entry -- not disassembled?)"); continue; }
            int off = 0;
            while (ins != null && off < 48) {
                boolean flagged = false;
                String why = "";
                for (Reference r : ins.getReferencesFrom()) {
                    RefType t = r.getReferenceType();
                    if (t.isFlow() && !t.isFallthrough()) { flagged = true; why = "flow"; }
                    if (t.isData() && r.getToAddress().isMemoryAddress() && !r.isStackReference()) { flagged = true; why = "rip-rel"; }
                }
                w.println(String.format("  +%02d len=%d end=%02d %s%s", off, ins.getLength(),
                    off + ins.getLength(), ins.toString(), flagged ? "   <-- " + why : ""));
                off += ins.getLength();
                ins = ins.getNext();
            }
        }
        w.close();
        println("[pb] wrote " + args[0]);
    }
}
