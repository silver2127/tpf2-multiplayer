// Dump a raw array of function pointers at an address.
//
// Written for the command dispatch table apply_command.cpp indexes with the
// std::variant tag: (&PTR_1430b10c0)[cmd.payload[0xb18]]. That table is the
// engine's own switch over every command type, so reading it recovers the apply
// handler for all 37 CmdData types at once -- something no call-graph walk can
// do, because a variant dispatch is an indirect jump through exactly this table.
//
// Usage: DumpPtrTable.java <outdir> <rva-or-va> <count> [label]
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.FlowType;

public class DumpPtrTable extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) { println("[ptr] usage: <outdir> <rva> <count> [label]"); return; }
        String outDir = args[0];
        long v = Long.parseLong(args[1].replace("0x", ""), 16);
        int n = Integer.parseInt(args[2]);
        String label = (args.length > 3) ? args[3] : ("ptrtable_" + args[1]);

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[ptr] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        long addr = (v >= base) ? v : base + v;
        Memory mem = currentProgram.getMemory();
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr);

        PrintWriter w = new PrintWriter(new File(d, label + ".csv"));
        w.println("index,target_rva,is_func,func_entry_rva,first_call_rva");
        for (int i = 0; i < n; i++) {
            long p;
            try { p = mem.getLong(a.add((long) i * 8)); } catch (Exception e) { break; }
            Address t = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(p);
            Function fn = currentProgram.getFunctionManager().getFunctionContaining(t);
            // Table entries are often one-line thunks around the real handler, and
            // Ghidra does not always define them as functions -- so there is no
            // call edge to follow. Walk the instructions instead and take the
            // first call/jump out, which is the handler.
            String first = "";
            Instruction ins = currentProgram.getListing().getInstructionAt(t);
            if (ins == null) {
                disassemble(t);
                ins = currentProgram.getListing().getInstructionAt(t);
            }
            for (int k = 0; k < 24 && ins != null && first.isEmpty(); k++) {
                FlowType ft = ins.getFlowType();
                if (ft.isCall() || ft.isJump()) {
                    Address[] fl = ins.getFlows();
                    if (fl.length > 0) first = Long.toHexString(fl[0].getOffset() - base);
                }
                if (ft.isTerminal()) break;
                ins = ins.getNext();
            }
            w.println(i + "," + Long.toHexString(p - base) + "," + (fn != null) + ","
                    + (fn != null ? Long.toHexString(fn.getEntryPoint().getOffset() - base) : "")
                    + "," + first);
        }
        w.close();
        println("[ptr] " + n + " entries at 0x" + Long.toHexString(addr)
                + " -> " + outDir + "\\" + label + ".csv");
    }
}
