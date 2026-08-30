// Resolve std::_Func_impl_no_alloc<lambda_HASH,...>::vftable symbols to their
// _Do_call invoke functions (vftable slot +0x10) and decompile them.
// Args: <outDir> <hash1> [<hash2> ...]   -> <outDir>/lambda_<hash>.c
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;
public class ResolveLambdaVtables extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String outDir = a[0];
        new File(outDir).mkdirs();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        SymbolTable st = currentProgram.getSymbolTable();
        for (int i = 1; i < a.length; i++) {
            String hash = a[i];
            Address vft = null;
            for (Symbol s : st.getAllSymbols(true)) {
                String n = s.getName(true);
                if (n.contains(hash) && n.contains("vftable")) { vft = s.getAddress(); println("[vft] " + n + " @ " + vft); break; }
            }
            if (vft == null) { println("[vft] NOT FOUND for " + hash); continue; }
            // MSVC _Func_impl vftable: slot +0x10 turned out to be _Copy in this
            // build. Dump every slot 0x00..0x28 so the real _Do_call is visible.
            for (int slot = 0; slot <= 0x28; slot += 8) {
                long fp = currentProgram.getMemory().getLong(vft.add(slot));
                Address inv = toAddr(fp);
                Function f = getFunctionAt(inv);
                if (f == null) f = createFunction(inv, "lambda_" + hash.substring(0,8) + "_slot" + slot);
                println("[inv] " + hash + " slot+0x" + Integer.toHexString(slot) + " -> " + inv + " " + (f != null ? f.getName() : "?"));
                if (f == null) continue;
                DecompileResults r = dec.decompileFunction(f, 180, monitor);
                PrintWriter w = new PrintWriter(new File(outDir, "lambda_" + hash + "_slot" + Integer.toHexString(slot) + ".c"));
                w.println("/* lambda_" + hash + " slot+0x" + Integer.toHexString(slot) + " invoke=" + inv + " */");
                w.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "/* decompile failed: " + r.getErrorMessage() + " */");
                w.close();
            }
        }
        dec.dispose();
    }
}
