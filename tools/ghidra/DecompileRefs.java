// Headless Ghidra script: decompile the functions that match queries.
//
//   analyzeHeadless <projdir> <proj> -process <binary> -noanalysis \
//       -scriptPath tools/ghidra -postScript DecompileRefs.java <outdir> <query>...
//
// Queries:
//   str:<text>   functions referencing a defined string (ASCII or UTF-16) containing <text>
//   fn:<text>    functions whose name (including namespace) contains <text>
//   addr:<hex>   the function containing that address (image-relative or absolute)
//
// Writes one <function>_<address>.c per function plus index.txt (query -> files).
// Output describes the analysed binary: keep it in research/private/ (CLAUDE.md Rule 0/1).

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;

public class DecompileRefs extends GhidraScript {
    private DecompInterface decomp;
    private File outDir;
    private final Set<Address> done = new LinkedHashSet<>();

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DecompileRefs.java <outdir> <query>...");
            return;
        }
        outDir = new File(args[0]);
        outDir.mkdirs();
        decomp = new DecompInterface();
        decomp.setOptions(new DecompileOptions());
        decomp.openProgram(currentProgram);

        Map<String, Set<String>> index = new LinkedHashMap<>();
        for (int i = 1; i < args.length; i++) {
            String q = args[i];
            Set<Function> fns = new LinkedHashSet<>();
            if (q.startsWith("str:")) fns.addAll(byString(q.substring(4)));
            else if (q.startsWith("fn:")) fns.addAll(byName(q.substring(3)));
            else if (q.startsWith("addr:")) {
                long v = Long.parseUnsignedLong(q.substring(5).replace("0x", ""), 16);
                Address base = currentProgram.getImageBase();
                Address a = v < base.getOffset() ? base.add(v) : base.getNewAddress(v);
                Function f = getFunctionContaining(a);
                if (f != null) fns.add(f);
            }
            Set<String> files = new LinkedHashSet<>();
            for (Function f : fns) files.add(write(f));
            index.put(q, files);
            println(q + " -> " + files.size() + " function(s)");
        }
        try (PrintWriter w = new PrintWriter(new File(outDir, "index.txt"))) {
            for (Map.Entry<String, Set<String>> e : index.entrySet()) {
                w.println(e.getKey());
                for (String f : e.getValue()) w.println("    " + f);
            }
        }
        decomp.dispose();
    }

    private Set<Function> byString(String needle) {
        Set<Function> out = new LinkedHashSet<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            Object v = d.getValue();
            if (v == null || !v.toString().contains(needle)) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) out.add(f);
            }
        }
        return out;
    }

    private Set<Function> byName(String needle) {
        Set<Function> out = new LinkedHashSet<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (f.getName(true).contains(needle)) out.add(f);
        }
        return out;
    }

    private String write(Function f) throws Exception {
        String name = f.getName(true).replaceAll("[^A-Za-z0-9_]+", "_");
        if (name.length() > 80) name = name.substring(0, 80);
        String file = name + "_" + f.getEntryPoint().toString() + ".c";
        if (!done.add(f.getEntryPoint())) return file;
        DecompileResults res = decomp.decompileFunction(f, 120, monitor);
        try (PrintWriter w = new PrintWriter(new File(outDir, file))) {
            w.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
            if (res != null && res.decompileCompleted()) w.println(res.getDecompiledFunction().getC());
            else w.println("// decompilation failed: " + (res == null ? "null" : res.getErrorMessage()));
        }
        return file;
    }
}
