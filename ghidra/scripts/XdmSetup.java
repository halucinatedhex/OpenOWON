// Pre-analysis: give Ghidra the memory map it cannot infer from a raw binary,
// and name everything we already know. Run before auto-analysis so the
// decompiler sees real symbols and does not invent names for peripherals.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class XdmSetup extends GhidraScript {

    private void block(String name, long addr, long len, boolean write) throws Exception {
        Memory mem = currentProgram.getMemory();
        Address a = toAddr(addr);
        if (mem.getBlock(a) != null) return;
        MemoryBlock b = mem.createUninitializedBlock(name, a, len, false);
        b.setRead(true);
        b.setWrite(write);
        b.setExecute(false);
        b.setVolatile(!write ? false : true);
    }

    @Override
    public void run() throws Exception {
        // 48 KB SRAM
        block("SRAM", 0x20000000L, 0xC000L, true);
        // Peripherals the firmware touches. Marked volatile so the decompiler
        // does not fold repeated reads of a status register into one.
        block("APB1", 0x40000000L, 0x8000L, true);
        block("APB2", 0x40010000L, 0x6400L, true);
        block("AHB",  0x40018000L, 0x8000L, true);
        block("SYSFLASH", 0x1FFFF000L, 0x1000L, false);   // option bytes / system memory
        block("PPB",  0xE0000000L, 0x100000L, true);      // NVIC, SCB, DBGMCU
        block("CFGFLASH", 0x0803D800L, 0x2800L, false);   // settings + calibration
        println("memory blocks created");

        // Apply the symbol file produced by this project's own analysis.
        String[] args = getScriptArgs();
        if (args.length < 1) { println("no symbol file given"); return; }
        Pattern p = Pattern.compile("^(0x[0-9A-Fa-f]+)\\s+(\\w+)");
        int n = 0, fn = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = r.readLine()) != null) {
                int h = line.indexOf('#');
                if (h >= 0) line = line.substring(0, h);
                Matcher m = p.matcher(line.trim());
                if (!m.find()) continue;
                long a = Long.decode(m.group(1));
                String name = m.group(2);
                Address addr = toAddr(a);
                if (currentProgram.getMemory().getBlock(addr) == null) continue;
                createLabel(addr, name, true);
                n++;
                // Anything in the code region and named like a routine becomes a function.
                if (a >= 0x08002800L && a < 0x0801F934L && !name.contains("table")
                        && !name.contains("_ram") && !name.contains("charset")) {
                    if (getFunctionAt(addr) == null) {
                        try { createFunction(addr, name); fn++; } catch (Exception e) { }
                    }
                }
            }
        }
        println("applied " + n + " labels, created " + fn + " functions");
    }
}
