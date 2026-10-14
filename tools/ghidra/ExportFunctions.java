// Exporta la lista de funciones del programa a un CSV para el recompilador:
// bloque de memoria, direccion, tamano, modo (arm/thumb) y nombre.
// No exporta codigo del juego, solo donde empieza y acaba cada funcion.
//@category NFSNitro

import java.io.File;
import java.io.PrintWriter;
import java.math.BigInteger;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

public class ExportFunctions extends GhidraScript {

	@Override
	public void run() throws Exception {
		File out = askFile("Guardar la lista de funciones (functions.csv)", "Guardar");
		Register tmode = currentProgram.getProgramContext().getRegister("TMode");
		int count = 0;

		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("block,address,size,mode,ranges,name");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				monitor.checkCancelled();
				Address entry = f.getEntryPoint();
				MemoryBlock block = currentProgram.getMemory().getBlock(entry);
				BigInteger t = currentProgram.getProgramContext().getValue(tmode, entry, false);
				boolean thumb = t != null && t.intValue() == 1;

				// Una funcion puede tener varios trozos separados: se listan todos como inicio-fin
				StringBuilder ranges = new StringBuilder();
				for (AddressRange r : f.getBody()) {
					if (ranges.length() > 0) {
						ranges.append(' ');
					}
					ranges.append(String.format("%08x-%08x", r.getMinAddress().getOffset(),
						r.getMaxAddress().getOffset()));
				}
				w.println(String.format("%s,%08x,%d,%s,%s,%s",
					block == null ? "?" : block.getName(), entry.getOffset(),
					f.getBody().getNumAddresses(), thumb ? "thumb" : "arm", ranges, f.getName()));
				count++;
			}
		}
		println(count + " funciones exportadas a " + out.getAbsolutePath());
	}
}
