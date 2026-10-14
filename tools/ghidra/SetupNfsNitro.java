// Prepara el programa arm9.bin de NFS Nitro (DS) para analizarlo:
//  1. Marca el punto de entrada (ARM) y NitroMain (Thumb) y crea sus funciones.
//  2. Crea los bloques ITCM y DTCM con los datos que el arranque copia alli (autoload),
//     el resto del BSS y la zona de registros de E/S.
//  3. Importa los overlays de y9.bin, cada uno en su propio espacio "overlay" de Ghidra
//     (ov0, ov1...), porque varios comparten la misma direccion de RAM.
// Se puede ejecutar mas de una vez: lo que ya existe se salta.
//@category NFSNitro

import java.io.ByteArrayInputStream;
import java.io.File;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class SetupNfsNitro extends GhidraScript {

	// Valores leidos de arm9.bin (ver notes/arm9_map.md)
	private static final long ENTRY = 0x02000800L;
	private static final long NITRO_MAIN = 0x02003000L;
	private static final long AUTOLOAD_SRC = 0x0210EDE0L;
	private static final long ITCM = 0x01FF8000L;
	private static final int ITCM_SIZE = 0x17C0;
	private static final long DTCM = 0x027E0000L;
	private static final int DTCM_SIZE = 0x3E0;
	private static final int DTCM_BSS = 0x780;
	private static final long BSS_END = 0x0212A1A0L;

	private static final int OVERLAY_ENTRY_SIZE = 32;

	private Memory mem;

	@Override
	public void run() throws Exception {
		mem = currentProgram.getMemory();

		copyBlock("itcm", ITCM, AUTOLOAD_SRC, ITCM_SIZE, true);
		copyBlock("dtcm", DTCM, AUTOLOAD_SRC + ITCM_SIZE, DTCM_SIZE, false);
		emptyBlock("dtcm_bss", DTCM + DTCM_SIZE, DTCM_BSS);
		long ramEnd = mem.getBlock(toAddr(ENTRY)).getEnd().getOffset() + 1;
		emptyBlock("bss", ramEnd, BSS_END - ramEnd);
		emptyBlock("io", 0x04000000L, 0x00100000L);

		entry(ENTRY, "_start", false);
		entry(NITRO_MAIN, "NitroMain", true);

		File dir = askDirectory("Carpeta de la ROM extraida (la que contiene y9.bin)", "Elegir");
		importOverlays(dir);

		println("Hecho. Ahora: Analysis > Auto Analyze 'arm9.bin' (con las opciones por defecto).");
	}

	private void copyBlock(String name, long dst, long src, int size, boolean exec)
			throws Exception {
		if (mem.getBlock(name) != null) {
			println(name + ": ya existe, se salta");
			return;
		}
		byte[] data = new byte[size];
		mem.getBytes(toAddr(src), data);
		MemoryBlock block = mem.createInitializedBlock(name, toAddr(dst),
			new ByteArrayInputStream(data), size, monitor, false);
		block.setPermissions(true, true, exec);
		println(String.format("%s: %s - %s", name, block.getStart(), block.getEnd()));
	}

	private void emptyBlock(String name, long start, long size) throws Exception {
		if (mem.getBlock(name) != null) {
			println(name + ": ya existe, se salta");
			return;
		}
		MemoryBlock block = mem.createUninitializedBlock(name, toAddr(start), size, false);
		block.setPermissions(true, true, false);
		println(String.format("%s: %s - %s", name, block.getStart(), block.getEnd()));
	}

	private void entry(long offset, String name, boolean thumb) throws Exception {
		Address addr = toAddr(offset);
		if (getFunctionAt(addr) == null) {
			if (getInstructionAt(addr) == null) {
				Register tmode = currentProgram.getProgramContext().getRegister("TMode");
				currentProgram.getProgramContext().setValue(tmode, addr, addr,
					thumb ? BigInteger.ONE : BigInteger.ZERO);
				disassemble(addr);
			}
			createFunction(addr, name);
		}
		if (getFunctionAt(addr) != null) {
			getFunctionAt(addr).setName(name, SourceType.USER_DEFINED);
			addEntryPoint(addr);
			println(name + ": " + addr);
		}
		else {
			println(name + ": NO se pudo crear la funcion en " + addr);
		}
	}

	private void importOverlays(File dir) throws Exception {
		byte[] y9 = Files.readAllBytes(new File(dir, "y9.bin").toPath());
		ByteBuffer table = ByteBuffer.wrap(y9).order(ByteOrder.LITTLE_ENDIAN);

		for (int i = 0; i < y9.length / OVERLAY_ENTRY_SIZE; i++) {
			int id = table.getInt(i * OVERLAY_ENTRY_SIZE);
			long ram = table.getInt(i * OVERLAY_ENTRY_SIZE + 4) & 0xffffffffL;
			int size = table.getInt(i * OVERLAY_ENTRY_SIZE + 8);
			int bss = table.getInt(i * OVERLAY_ENTRY_SIZE + 12);
			String name = "ov" + id;

			if (mem.getBlock(name) != null) {
				println(name + ": ya existe, se salta");
				continue;
			}
			File file = new File(dir, String.format("overlay/overlay_%04d.bin", id));
			byte[] data = Files.readAllBytes(file.toPath());
			if (data.length != size) {
				println(name + ": el archivo mide " + data.length + " y y9.bin dice " + size +
					" (comprimido?), se salta");
				continue;
			}

			MemoryBlock block = mem.createInitializedBlock(name, toAddr(ram),
				new ByteArrayInputStream(data), size, monitor, true);
			block.setPermissions(true, true, true);
			block.setSourceName(file.getName());
			println(String.format("%s: %s - %s", name, block.getStart(), block.getEnd()));

			if (bss > 0) {
				try {
					Address bssStart = block.getEnd().add(1);
					MemoryBlock bssBlock =
						mem.createUninitializedBlock(name + "_bss", bssStart, bss, false);
					bssBlock.setPermissions(true, true, false);
					println(String.format("%s_bss: %s - %s", name, bssBlock.getStart(),
						bssBlock.getEnd()));
				}
				catch (Exception e) {
					println(name + "_bss: no se pudo crear (" + e.getMessage() + ")");
				}
			}
		}
	}
}
