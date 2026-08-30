// For a target function, dump every CALL SITE and the instructions immediately
// after it, so we can see whether the caller consumes the return value.
//
// This exists because Ghidra recovered no signature for the function we care
// about -- `undefined FUN_1409e76e0(void)` -- and the known-good
// buyVehicle_factory control came back equally unsigned, so that is the baseline
// analysis quality on this stripped binary rather than a quirk of one function.
// The decompiler cannot tell us; the call sites can.
//
// Why it matters: a lockstep hook must be able to CANCEL a local command --
// intercept, skip the original, and re-issue it later on a shared schedule. If
// the function returns nothing, suppressing it is a straight `ret`. If callers
// read eax/rax afterwards, the relay has to synthesise a return value that
// means "rejected/no-op", and picking the wrong one corrupts the caller.
//
// The heuristic: after the call, does any instruction READ eax/rax before some
// instruction WRITES it? A read-before-write means the value is consumed.
// Reported per site, with the raw instructions so the judgement can be checked
// by eye rather than trusted.
//
// Usage: FindCallSites.java <outdir> <rva-or-va> [<instructions-to-show>]
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindCallSites extends GhidraScript {

    private static boolean mentionsAx(String s) {
        String t = s.toLowerCase();
        return t.contains("rax") || t.contains("eax") || t.contains(" ax") || t.contains(",ax");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) { println("[calls] usage: <outdir> <rva-or-va> [count]"); return; }
        String outDir = args[0];
        long v = Long.parseLong(args[1].replace("0x", ""), 16);
        int show = (args.length > 2) ? Integer.parseInt(args[2]) : 10;

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[calls] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        long target = (v >= base) ? v : base + v;
        Address ta = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(target);
        Listing listing = currentProgram.getListing();

        Function tf = currentProgram.getFunctionManager().getFunctionContaining(ta);
        PrintWriter w = new PrintWriter(new File(d, "callsites_" + Long.toHexString(target - base) + ".txt"));
        w.println("target 0x" + Long.toHexString(target) + " rva 0x" + Long.toHexString(target - base));
        if (tf != null) {
            w.println("function : " + tf.getName());
            w.println("signature: " + tf.getSignature().getPrototypeString());
            w.println("returns  : " + tf.getReturnType().getName());
            w.println("params   : " + tf.getParameterCount());
            w.println("calling  : " + tf.getCallingConventionName());
        }
        w.println();

        int sites = 0, consumed = 0, ignored = 0;
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ta);
        while (it.hasNext()) {
            Reference r = it.next();
            if (!r.getReferenceType().isCall()) continue;
            sites++;
            Address from = r.getFromAddress();
            Function cf = currentProgram.getFunctionManager().getFunctionContaining(from);
            w.println("---- call site " + sites + " at 0x" + Long.toHexString(from.getOffset())
                    + " (rva 0x" + Long.toHexString(from.getOffset() - base) + ")"
                    + (cf != null ? "  in " + cf.getName() : ""));

            Instruction ins = listing.getInstructionAt(from);
            if (ins == null) { w.println("    <no instruction>"); continue; }
            boolean decided = false, isConsumed = false;
            Instruction cur = ins.getNext();
            for (int i = 0; i < show && cur != null; i++) {
                String txt = cur.toString();
                w.println(String.format("    %-12s %s", Long.toHexString(cur.getAddress().getOffset()), txt));
                if (!decided && mentionsAx(txt)) {
                    // a write to ax as the destination kills the value; anything
                    // else that mentions it is a read
                    String mn = cur.getMnemonicString().toLowerCase();
                    boolean writesAx = (mn.equals("mov") || mn.equals("xor") || mn.equals("lea")
                            || mn.equals("movzx") || mn.equals("movsx"))
                            && txt.replaceAll("\\s+", " ").matches("(?i)^\\w+ (r|e)?ax,.*");
                    decided = true;
                    isConsumed = !writesAx;
                }
                cur = cur.getNext();
            }
            if (!decided) {
                ignored++;
                w.println("    => return value NOT touched in the next " + show
                        + " instruction(s) -- looks ignored here");
            } else if (isConsumed) {
                consumed++;
                w.println("    => reads eax/rax after the call -- RETURN VALUE CONSUMED");
            } else {
                ignored++;
                w.println("    => overwrites eax/rax first -- return value discarded here");
            }
            w.println();
        }
        w.println("call sites: " + sites + "  consuming: " + consumed + "  ignoring: " + ignored);
        w.close();
        println("[calls] target rva 0x" + Long.toHexString(target - base)
                + "  sites=" + sites + " consuming=" + consumed + " ignoring=" + ignored
                + " -> " + outDir);
    }
}
