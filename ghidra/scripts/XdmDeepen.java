// Second pass: Ghidra's auto-analysis only reaches code it can prove is
// reachable. Seed functions three ways, then iterate to a fixed point:
//
//   1. Thumb function pointers in literal pools / tables (odd words that
//      point into the code region) - catches callbacks, menu handlers, ISRs.
//   2. Call targets of every disassembled BL/BLX that has no function yet.
//   3. Thumb prologues in the remaining gaps (push {..,lr} / stmdb sp!).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.FlowType;

public class XdmDeepen extends GhidraScript {
    // Code lives below the font/data blob that starts around 0x08014800.
    static final long LO = 0x08002800L, HI = 0x08014800L;

    private boolean tryMake(long a) {
        Address addr = toAddr(a);
        if (getFunctionAt(addr) != null) return false;
        if (getFunctionContaining(addr) != null && getInstructionAt(addr) != null) return false;
        try {
            if (getInstructionAt(addr) == null) {
                // Thumb mode: set TMode context before disassembly
                setTMode(addr);
                disassemble(addr);
            }
            if (getInstructionAt(addr) == null) return false;
            return createFunction(addr, null) != null;
        } catch (Exception e) {
            return false;
        }
    }

    private void setTMode(Address addr) throws Exception {
        var reg = currentProgram.getProgramContext().getRegister("TMode");
        if (reg != null)
            currentProgram.getProgramContext().setValue(reg, addr, addr, java.math.BigInteger.ONE);
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Address imgLo = currentProgram.getMinAddress(), imgHi = currentProgram.getMaxAddress();
        int ptrs = 0, calls = 0, pro = 0;

        // 1. Thumb function pointers anywhere in the image.
        for (Address a = imgLo; a.add(3).compareTo(imgHi) <= 0; a = a.add(4)) {
            if (monitor.isCancelled()) return;
            long v;
            try { v = mem.getInt(a) & 0xFFFFFFFFL; } catch (Exception e) { break; }
            if ((v & 1) == 0 || v < LO || v >= HI) continue;
            long t = v & ~1L;
            // only plausible function starts: preceded by a return, padding or literal
            if (tryMake(t)) ptrs++;
        }

        // 2 + 3, iterated: new code exposes new calls and new gaps.
        for (int round = 0; round < 8; round++) {
            int before = calls + pro;

            InstructionIterator it = currentProgram.getListing().getInstructions(toAddr(LO), true);
            java.util.List<Long> targets = new java.util.ArrayList<>();
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getAddress().getOffset() >= HI) break;
                FlowType ft = ins.getFlowType();
                if (!ft.isCall()) continue;
                for (Address t : ins.getFlows()) {
                    long o = t.getOffset();
                    if (o >= LO && o < HI && getFunctionAt(t) == null) targets.add(o);
                }
            }
            for (long t : targets) if (tryMake(t)) calls++;

            for (long a = LO; a < HI - 4; a += 2) {
                if (monitor.isCancelled()) return;
                Address addr = toAddr(a);
                if (getInstructionAt(addr) != null) continue;
                if (getFunctionContaining(addr) != null) continue;
                int b0 = mem.getByte(addr) & 0xFF;
                int b1 = mem.getByte(addr.add(1)) & 0xFF;
                // push {...,lr} = B5xx ; push {r4-r7..} = B4xx with r4 set ; stmdb sp! = E92D
                boolean prologue = (b1 == 0xB5) || (b1 == 0xB4 && (b0 & 0xF0) != 0)
                                || (b0 == 0x2D && b1 == 0xE9);
                if (prologue && tryMake(a)) pro++;
            }
            if (calls + pro == before) break;
        }
        println("deepen: " + ptrs + " from pointers, " + calls + " from calls, "
                + pro + " from prologues");
    }
}
