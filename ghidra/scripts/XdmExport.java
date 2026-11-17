// Post-analysis: dump decompiled C for every function, plus an index.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;

public class XdmExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outC = args.length > 0 ? args[0] : "/tmp/xdm_decomp.c";
        String outI = args.length > 1 ? args[1] : "/tmp/xdm_index.txt";

        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);

        PrintWriter c = new PrintWriter(outC);
        PrintWriter idx = new PrintWriter(outI);
        int ok = 0, fail = 0;

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            idx.printf("%s  %s  size=%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
            DecompileResults r = di.decompileFunction(f, 45, monitor);
            if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                c.println("/* ===== " + f.getName() + " @ " + f.getEntryPoint() + " ===== */");
                c.println(r.getDecompiledFunction().getC());
                ok++;
            } else {
                fail++;
            }
        }
        c.close(); idx.close();
        println("decompiled " + ok + " functions, " + fail + " failed");
    }
}
