// Before analysis of code the game loaded at runtime (a driver, captured from memory and imported at
// its virtual address): its entry points from the entries file become functions, and "Function
// Start Search" is off, so the rest of the captured bytes (data, whatever followed the driver in
// memory) isn't taken for code.
// Args: <entries file>
//@category WC

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class WcDriverSetup extends GhidraScript {

	@Override
	protected void run() throws Exception {
		setAnalysisOption(currentProgram, "Function Start Search", "false");
		long lo = currentProgram.getMinAddress().getOffset(), hi = currentProgram.getMaxAddress().getOffset();
		Path entries = Path.of(getScriptArgs()[0]);
		int n = 0;
		for (String line : Files.exists(entries) ? Files.readAllLines(entries) : java.util.List.<String>of()) {
			line = line.replaceAll("#.*", "").trim();
			if (line.isEmpty()) {
				continue;
			}
			long a = Long.parseLong(line, 16);
			if (a >= lo && a <= hi) {
				Address at = toAddr(a);
				disassemble(at);
				createFunction(at, null);
				n++;
			}
		}
		println("WcDriverSetup: " + n + " entry points");
	}
}
