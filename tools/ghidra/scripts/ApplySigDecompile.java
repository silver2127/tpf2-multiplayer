// Give a function a real prototype, then decompile it.
//
// Every decompilation in this project has come back as
// `undefined FUN_xxxxxxxx(void)` -- Ghidra recovered no signatures from this
// stripped release build, and the known-good buyVehicle_factory control is
// equally unsigned, so it is the baseline rather than a quirk.
//
// That matters specifically for make_cmd::BuildProposal. It returns a 0x38-byte
// Command BY VALUE, which MSVC x64 implements with a hidden return-slot pointer
// in rcx. With no prototype the decompiler does not model that pointer, so the
// writes that populate the Command are not rendered as stores into anything --
// extracting field offsets from it found exactly zero. Declare the return type
// and the same code renders as `*(T *)(retslot + 0xNN) = ...`, which is the
// layout we are after.
//
// Usage: ApplySigDecompile.java <outdir> <rva> <label> <retStructBytes> [<timeout>]
//   retStructBytes = 0 to leave the return type alone.
//
//@category TpF2
import java.io.File;
import java.io.PrintWriter;
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
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplySigDecompile extends GhidraScript {

    private static final Pattern FIELD =
        Pattern.compile("\\(([A-Za-z_][A-Za-z0-9_]*)\\s*\\+\\s*(0x[0-9a-fA-F]+|\\d+)\\)");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) {
            println("[sig] usage: <outdir> <rva> <label> <retStructBytes> [timeout]");
            return;
        }
        String outDir = args[0];
        long v = Long.parseLong(args[1].replace("0x", ""), 16);
        String label = args[2];
        int retBytes = Integer.parseInt(args[3]);
        int timeout = (args.length > 4) ? Integer.parseInt(args[4]) : 180;

        File d = new File(outDir);
        if (!d.isDirectory() && !d.mkdirs()) { println("[sig] cannot create " + outDir); return; }

        long base = currentProgram.getImageBase().getOffset();
        long addr = (v >= base) ? v : base + v;
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addr);
        Function fn = currentProgram.getFunctionManager().getFunctionContaining(a);
        if (fn == null) { println("[sig] no function at " + Long.toHexString(addr)); return; }

        println("[sig] " + label + " before: " + fn.getSignature().getPrototypeString());

        if (retBytes > 0) {
            // An opaque struct of the right SIZE is enough. The decompiler only
            // needs to know the return is too large for a register, which is
            // what triggers the hidden-pointer convention; naming the members is
            // the thing we are trying to learn, not something to assert here.
            String tname = "Command_" + retBytes;
            DataType existing = currentProgram.getDataTypeManager().getDataType("/" + tname);
            DataType cmd = existing;
            if (cmd == null) {
                StructureDataType s = new StructureDataType(tname, retBytes);
                cmd = currentProgram.getDataTypeManager()
                        .addDataType(s, DataTypeConflictHandler.DEFAULT_HANDLER);
            }
            fn.setReturnType(cmd, SourceType.USER_DEFINED);
            try {
                fn.setCallingConvention("__fastcall");
            } catch (Exception e) {
                println("[sig] could not set calling convention: " + e.getMessage());
            }
            println("[sig] " + label + " after : " + fn.getSignature().getPrototypeString());
        }

        // Declare N pointer parameters.
        //
        // Declaring ONLY a struct return is actively harmful: it tells the
        // decompiler the function takes nothing else, so every call site renders
        // with a single argument and the real inputs in rdx/r8/r9 vanish from the
        // output. That is how a factory that obviously consumes build data came
        // to look like it consumed nothing. With a hidden return pointer in rcx,
        // the real arguments start at rdx, so declare them explicitly.
        int nParams = (args.length > 5) ? Integer.parseInt(args[5]) : 0;
        if (nParams > 0) {
            DataType pv = currentProgram.getDataTypeManager().getDataType("/void *");
            if (pv == null) pv = new ghidra.program.model.data.PointerDataType();
            java.util.List<ghidra.program.model.listing.Variable> ps =
                new java.util.ArrayList<ghidra.program.model.listing.Variable>();
            for (int i = 0; i < nParams; i++) {
                ps.add(new ghidra.program.model.listing.ParameterImpl(
                        "a" + (i + 1), pv, currentProgram));
            }
            try {
                fn.replaceParameters(ps,
                        Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS,
                        true, SourceType.USER_DEFINED);
                println("[sig] " + label + " params: " + fn.getSignature().getPrototypeString());
            } catch (Exception e) {
                println("[sig] could not set parameters: " + e.getMessage());
            }
        }

        DecompInterface ifc = new DecompInterface();
        ifc.setOptions(new DecompileOptions());
        ifc.openProgram(currentProgram);
        DecompileResults res = ifc.decompileFunction(fn, timeout, monitor);
        if (res == null || !res.decompileCompleted()) {
            println("[sig] decompile FAILED: " + (res == null ? "null" : res.getErrorMessage()));
            ifc.dispose();
            return;
        }
        String c = res.getDecompiledFunction().getC();
        PrintWriter w = new PrintWriter(new File(d, label + ".sig.c"));
        w.println("/* " + label + " rva=0x" + Long.toHexString(addr - base));
        w.println("   " + fn.getSignature().getPrototypeString() + " */");
        w.println();
        w.print(c);
        w.close();

        // field offsets per variable, same extraction as DecompileTargets
        Map<String, Set<Long>> fields = new TreeMap<String, Set<Long>>();
        Matcher m = FIELD.matcher(c);
        while (m.find()) {
            String var = m.group(1), off = m.group(2);
            long o = off.startsWith("0x") ? Long.parseLong(off.substring(2), 16)
                                          : Long.parseLong(off);
            Set<Long> s = fields.get(var);
            if (s == null) { s = new TreeSet<Long>(); fields.put(var, s); }
            s.add(o);
        }
        PrintWriter wf = new PrintWriter(new File(d, label + ".sig.fields.txt"));
        wf.println(label + " rva=0x" + Long.toHexString(addr - base));
        wf.println(fn.getSignature().getPrototypeString());
        wf.println();
        for (Map.Entry<String, Set<Long>> e : fields.entrySet()) {
            StringBuilder sb = new StringBuilder(e.getKey() + ": ");
            for (Long o : e.getValue()) sb.append("0x").append(Long.toHexString(o)).append(" ");
            wf.println(sb.toString().trim());
        }
        wf.close();
        ifc.dispose();
        println("[sig] " + label + " -> " + fields.size() + " var(s) with field accesses");
    }
}
