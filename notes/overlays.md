# Overlays del ARM9

Leído de `C:\nfs\y9.bin` (128 bytes = 4 entradas de 32 bytes). VERIFICADO: los tamaños coinciden con los archivos.

| Nº | Dirección RAM | Tamaño | BSS | Inicializadores estáticos | Comprimido |
|---|---|---|---|---|---|
| 0 | `0x0212A1A0` | `0x67B80` (424.832) | `0x4E0` | `0x0218ECC0`–`0x0218ED38` | no |
| 1 | `0x0212A1A0` | `0x6AA00` (436.736) | `0x700` | `0x0218FA4C`–`0x0218FAE4` | no |
| 2 | `0x021952A0` | `0x7900` (30.976) | `0x60` | `0x0219C590`–`0x0219C5A4` | no |
| 3 | `0x021952A0` | `0xF280` (62.080) | `0x20` | `0x021A4030`–`0x021A4038` | no |

- 0 y 1 se cargan en la **misma dirección**, así que nunca están a la vez en memoria. Igual 2 y 3. VERIFICADO.
- `0x0212A1A0 + 0x6AA00 + 0x700 = 0x021952A0`: los overlays 2/3 van justo detrás del overlay 1. VERIFICADO (la cuenta).
- En Ghidra no caben los cuatro en un solo espacio de direcciones: hay que usar bloques *overlay* de Ghidra
  o un programa por combinación.
- HIPÓTESIS (por las cadenas de texto): 2 = red / Descarga DS (`NetWaitForServer`, `downloadus.srl`);
  3 = tutorial (`Be/Cell/Tut_*`).
- HIPÓTESIS: 1 = carrera. Contiene las rutas de pista (`texturebatch.nsbmd`, `%s.hzc`, `%s.pvs`), partículas,
  pickups y destructibles. 0 = menús / selección de coche (carga `Cars/%s/lod%d`, `shadow`, `Sky/%s/sky`).
  Falta confirmarlo viendo el código.

## Importación en Ghidra

Script: `tools/ghidra/SetupNfsNitro.java` (también en `C:\Users\jj\ghidra_scripts`). Crea `ov0`..`ov3`
como bloques *overlay* de Ghidra y su BSS detrás, además de arreglar el arranque (ver `arm9_map.md`).
**Aún no se ha ejecutado.**
