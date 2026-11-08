// @category MK9
// Direct-Ghidra (NOT REA) dump: for each requested address, find/creates (in memory,
// project opened -readOnly so nothing is saved) the containing function, then emits
// entry, cc, stack purge, callers, callees, disassembly with bytes, and decompiled C.
// args: <outTxt> <addrListFile>   address list: one hex VA per line ("c:" prefix = containing site)
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import ghidra.app.util.headless.HeadlessScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;

public class Mk9Pass1Dump extends HeadlessScript {
    private static final String SHA = "D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6";
    StringBuilder out = new StringBuilder();
    void p(String s) { out.append(s).append('\n'); }

    Address findStart(Address site) throws Exception {
        // walk back to the first byte following 0xCC/0x90 padding or a RET, bounded 0x6000 bytes
        Memory mem = currentProgram.getMemory();
        Address a = site;
        for (int i = 0; i < 0x6000; i++) {
            Address prev = a.subtract(1);
            byte b = mem.getByte(prev);
            if ((b & 0xff) == 0xCC) {
                byte c = mem.getByte(a);
                if ((c & 0xff) != 0xCC) return a;
            }
            a = prev;
        }
        return null;
    }

    Function ensureFunction(Address a, boolean containing) throws Exception {
        Function f = containing ? getFunctionContaining(a) : getFunctionAt(a);
        if (f != null) return f;
        if (!containing) {
            Function c = getFunctionContaining(a);
            if (c != null && c.getEntryPoint().equals(a)) return c;
        }
        Address start = containing ? findStart(a) : a;
        if (start == null) return null;
        if (getInstructionAt(start) == null) new DisassembleCommand(start, null, true).applyTo(currentProgram, monitor);
        CreateFunctionCmd cmd = new CreateFunctionCmd(start);
        cmd.applyTo(currentProgram, monitor);
        f = getFunctionAt(start);
        if (f != null) p("NOTE: function created in-memory at " + start + " (not in saved DB)");
        return f;
    }

    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        if (!SHA.equalsIgnoreCase(currentProgram.getExecutableSHA256())) throw new IllegalStateException("SHA mismatch");
        List<String> lines = Files.readAllLines(Path.of(args[1]));
        DecompInterface dec = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        dec.setOptions(opts);
        dec.openProgram(currentProgram);
        p("# Direct Ghidra dump (not REA). program=" + currentProgram.getName() + " lang=" + currentProgram.getLanguageID() + " cspec=" + currentProgram.getCompilerSpec().getCompilerSpecID() + " base=" + currentProgram.getImageBase());
        Set<Address> done = new HashSet<>();
        for (String raw : lines) {
            String l = raw.trim();
            if (l.isEmpty() || l.startsWith("#")) continue;
            boolean containing = l.startsWith("c:");
            if (containing) l = l.substring(2);
            Address a = toAddr(Long.parseLong(l.replace("0x", ""), 16));
            p("\n================================================================");
            p("REQUEST " + l + (containing ? " (containing)" : ""));
            Function f;
            try { f = ensureFunction(a, containing); } catch (Exception e) { p("ERROR " + e); continue; }
            if (f == null) { p("NO FUNCTION"); continue; }
            if (!done.add(f.getEntryPoint())) { p("DUP entry " + f.getEntryPoint()); continue; }
            Address e = f.getEntryPoint();
            p("ENTRY " + e + " RVA 0x" + Long.toHexString(e.subtract(currentProgram.getImageBase())) + " name=" + f.getName() + " cc=" + f.getCallingConventionName() + " purge=" + f.getStackPurgeSize() + " body=" + f.getBody().getMinAddress() + "-" + f.getBody().getMaxAddress() + " size=" + f.getBody().getNumAddresses());
            p("SIG " + f.getPrototypeString(false, false));
            StringBuilder callers = new StringBuilder();
            int n = 0;
            for (Reference r : getReferencesTo(e)) {
                if (n++ > 60) { callers.append(" ..."); break; }
                Function cf = getFunctionContaining(r.getFromAddress());
                callers.append(" ").append(r.getFromAddress()).append("(").append(r.getReferenceType()).append(cf == null ? "" : " in " + cf.getEntryPoint()).append(")");
            }
            p("CALLERS" + callers);
            // disassembly
            Listing lst = currentProgram.getListing();
            InstructionIterator it = lst.getInstructions(f.getBody(), true);
            StringBuilder calls = new StringBuilder();
            StringBuilder asm = new StringBuilder();
            int cnt = 0;
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (cnt++ < 1500) {
                    StringBuilder hb = new StringBuilder();
                    for (byte b : ins.getBytes()) hb.append(String.format("%02x", b & 0xff));
                    asm.append(ins.getAddress()).append("  ").append(String.format("%-20s", hb)).append(" ").append(ins).append('\n');
                }
                if (ins.getFlowType().isCall()) {
                    Address[] fl = ins.getFlows();
                    calls.append(" ").append(ins.getAddress()).append("->").append(fl.length > 0 ? fl[0].toString() : ins.toString());
                }
            }
            p("CALLEES" + calls);
            DecompileResults res = dec.decompileFunction(f, 90, monitor);
            if (res != null && res.decompileCompleted()) p("C:\n" + res.getDecompiledFunction().getC());
            else p("DECOMPILE FAILED " + (res == null ? "" : res.getErrorMessage()));
            p("ASM:\n" + asm);
        }
        dec.dispose();
        Files.writeString(Path.of(args[0]), out.toString(), StandardCharsets.UTF_8);
    }
}
