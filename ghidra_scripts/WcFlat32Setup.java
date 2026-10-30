// Before analysis of a flat 32-bit image (Wing Commander III): the entry point, as a function.
// Args: <entry offset, hex>
//@category WC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class WcFlat32Setup extends GhidraScript {

	@Override
	protected void run() throws Exception {
		Address entry = toAddr(Long.parseLong(getScriptArgs()[0], 16));
		currentProgram.getSymbolTable().addExternalEntryPoint(entry);
		disassemble(entry);
		createFunction(entry, "entry");
	}
}
