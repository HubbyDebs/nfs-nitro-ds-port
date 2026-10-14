# Mapa del código (Fase 2) — EN CURSO

Programa de Ghidra: `arm9.bin`, base `0x02000000`, 5185 funciones tras el primer auto-análisis (2026-10-13).
`VERIFICADO` = visto en los bytes o en el descompilado. Los nombres terminados en `_hyp` en Ghidra son HIPÓTESIS.

## Arranque

- `0x02000800` `_start` (ARM). VERIFICADO (bytes): `mov r12,#0x04000000` / `str r12,[r12,#0x208]`, prepara las pilas,
  copia los bloques *autoload* y salta con `bx r1`.
- **`NitroMain` = `0x02003000`, en modo Thumb.** VERIFICADO: el arranque carga el literal `0x02003001` (bit 0 = Thumb)
  y allí hay `push {r4-r7,lr}` (`f0 b5`). Buena parte del juego es código Thumb.
- **Problema:** Ghidra no tiene función ni en `0x02000800` ni en `0x02003000` (se importó como binario crudo, sin
  punto de entrada). Hasta arreglarlo no se puede descompilar el `main` ni seguir el bucle principal.
  Lo arregla `tools/ghidra/SetupNfsNitro.java`.
- Parámetros del módulo en `0x02000BA0` (VERIFICADO, bytes):
  - lista de autoload `0x02110980`–`0x02110998`; datos de autoload desde `0x0210EDE0`
  - BSS: `0x0210EDE0`–`0x0212A1A0` (acaba justo donde empiezan los overlays 0/1)
  - fin de zona comprimida = 0 → **el ARM9 no está comprimido** (antes era hipótesis)
  - versión del SDK: `0x04027539`
- Autoload (VERIFICADO, bytes en `0x02110980`): 
  - ITCM: `0x01FF8000`, `0x17C0` bytes (en el archivo: `0x0210EDE0`)
  - DTCM: `0x027E0000`, `0x3E0` bytes + `0x780` de BSS (en el archivo: `0x021105A0`)
  - Es decir: el código rápido del juego vive en `0x01FF8000`, no donde aparece en el archivo.

## Sistema de archivos del juego (VERIFICADO en el descompilado)

Cadena: `Vfs_Init` → `Vfs_Open` → archivos de la ROM con el SDK + `unzOpen`.

| Dirección | Nombre en Ghidra | Qué hace |
|---|---|---|
| `0x02085324` | `Vfs_Init` | Llama a `Vfs_Open(this, "vfs.zip", "vfs.dat", "vfs.bin")`. Sin llamadores conocidos todavía |
| `0x020851B8` | `Vfs_Open` | Lee `vfs.dat` como texto (máx. 128 bytes), saca 3 números, crea el índice, lee `vfs.bin` y abre el ZIP |
| `0x0209D804` | `VfsIndex_ctor` | Objeto de `0x2C` bytes con las tablas del índice |
| `0x0203C150` | `unzOpen` | Es **minizip** (la librería de zlib): busca el directorio central, error `-103` = `UNZ_BADZIPFILE`, estructura de `0x88` bytes |
| `0x0203C2EC`, `0x0203C3D0`, `0x0203C428` | `unzlocal_*_hyp` | Auxiliares de minizip |
| `0x02086448` | `RomFile_ctor_open` | Clase envoltorio: inicia un `FSFile` y lo abre por nombre |
| `0x020862AC` | `RomFile_Read` | Lee del archivo |
| `0x020862C8`, `0x02086424` | `RomFile_Close_hyp`, `RomFile_dtor_hyp` | |
| `0x0200D69C`, `0x0200D97C`, `0x0200D9C4`, `0x0200DB18` | `FS_InitFile_hyp`, `FS_OpenFile_hyp`, `FS_CloseFile_hyp`, `FS_ReadFile_hyp` | SDK de Nintendo. HIPÓTESIS fuerte: el `FSFile` ocupa 18 palabras (`0x48` bytes), como en el SDK |
| `0x0208ED00` | `operator_new_hyp` | Reserva de memoria |
| `0x0201F720` | (sin renombrar) | HIPÓTESIS: `sscanf` (formato en `0x021096EC`) |

Orden de `vfs.bin` según `Vfs_Open` (VERIFICADO): cabecera en `obj+0x24`, luego tabla de `n1` elementos, tabla de `n2`
elementos y `n3` bytes de nombres, con `n1, n2, n3` = los números de `vfs.dat` (4277, 4024, 29822).
Corrige la nota de `data_inventory.md`: los 8 bytes "sobrantes" son una **cabecera al principio**, así que los
registros empiezan en el offset 8.

Consecuencia para el port: el juego usa zlib/minizip estándar, así que el port puede abrir `vfs.zip` con cualquier
librería ZIP normal.

## `NitroMain` y bucle principal (VERIFICADO en el descompilado, 2026-10-13)

`NitroMain` = `0x02003000`–`0x020034C3` (Thumb). Orden:

1. Comprueba si la tapa está cerrada (`0x027FFFA8 & 0x8000`) y, si lo está, duerme (`PM_GoSleepMode_hyp`).
2. Inicialización del SDK (funciones aún sin nombrar: `0x02008A84`, `0x02005608`, `0x0200560C`, `0x020094D4`,
   `0x020096F0`, `0x02007EB0`, `0x02010698`, `0x020057C0`...). HIPÓTESIS: `OS_Init`, `GX_Init`, etc.
   Toca registros reales: `0x04000304` (encendido de pantallas), `0x04001000` (pantalla inferior), `0x04000208` (IME=1).
3. Registra la rutina de VBlank: `FUN_020075B4(1, 0x01FF831D)` → la rutina está en la **ITCM** (`0x01FF831C`, Thumb).
4. `Heap_Init` (`0x020866FC`): crea el montón de memoria con la arena del SDK.
5. `FileSys_Create` (`0x020857FC`): crea el objeto del sistema de archivos (global en `0x0211F170`) y su hilo.
6. Llama a `FileSys->vtable[3](1)` = `Vfs_Init(this, 1)` → abre `vfs.zip`.
7. Crea una docena de objetos globales (gestores) con `operator_new_hyp`. Sin identificar todavía.
8. `GameApp_GetInstance_hyp` (`0x020BE858`): objeto principal de 200 bytes; se llama a su `vtable[0]` (init).
9. **Bucle infinito**, una vuelta por fotograma:
   - gestiona tapa cerrada / reposo,
   - `GameApp->vtable[1](dt)` (HIPÓTESIS: actualizar) y `GameApp->vtable[3]()` (HIPÓTESIS: ¿dibujar?),
   - `FileSys_Update_wakeWorker_hyp` (despierta el hilo de carga si hay peticiones),
   - `FUN_0206E6B8` sobre el objeto de `FUN_0206F640` (máquina de estados; HIPÓTESIS: red/multijugador o guardado),
   - dos llamadas virtuales finales (HIPÓTESIS: presentar el fotograma y esperar al VBlank).

El juego está escrito en C++ con muchas clases y llamadas virtuales: Ghidra no las sigue solo, hay que ir
tabla a tabla (como la de abajo).

## Clase del sistema de archivos (tabla virtual en `0x0210962C`, VERIFICADO)

| Hueco | Dirección | Nombre | Qué hace |
|---|---|---|---|
| 1 | `0x0208555C` | `FileSys_FileExists` | ¿Existe el archivo? |
| 2 | `0x02085440` | `FileSys_GetFileSize` | Tamaño del archivo |
| 3 | `0x02085324` | `Vfs_Init` | Abre el ZIP |
| 6 | `0x02085024` | `FileSys_QueueLoadByName` | Pone en cola una carga (nombre de hasta 64 letras, búfer, tamaño) y devuelve un nº de petición |
| 7 | `0x02084E20` | `FileSys_QueueLoadFromObject_hyp` | Igual pero con un objeto ya abierto |
| 8 | `0x02084C7C` | `FileSys_GetRequestResult_hyp` | Consulta el estado de una petición |
| 9 | `0x020849C8` | `FileSys_CancelRequest_hyp` | Espera/cancela una petición |

Reglas de nombres (VERIFICADO en `FileSys_GetFileSize`):
- Si el nombre empieza por **`raw:`** (cadena en `0x0210971A`), se lee directamente de la ROM, sin pasar por el ZIP.
- Si no, el nombre se pasa a minúsculas, se cambia `/` por `\` y se busca en el ZIP (`Vfs_GetSizeInZip_hyp` `0x020860A8`).
- Las cargas son **asíncronas**: hay una cola protegida con un candado (`Mutex_Lock_hyp`/`Mutex_Unlock_hyp`) y un
  hilo aparte que hace la lectura real.

### Cadena completa de lectura de un archivo (VERIFICADO en el descompilado)

1. Alguien llama a `FileSys_QueueLoadByName(nombre, búfer, tamaño)` → devuelve nº de petición.
2. El bucle principal llama a `FileSys_Update_wakeWorker_hyp`, que manda un mensaje a la cola `0x02125C6C`.
3. `FileSys_WorkerThreadLoop` (`0x020845F4`, hueco 14 de la tabla) espera el mensaje, saca la petición y llama a
   `FileSys_OpenFile` (`0x02085670`, hueco 0).
4. `FileSys_OpenFile` normaliza el nombre y crea:
   - un `RomFile` (`RomFile_ctor_open`) si el nombre empezaba por `raw:`, o
   - un **`dsPackedFile`** (nombre real de la clase, cadena en `0x02109748`; tabla virtual `0x02109728`).
5. `dsPackedFile_ctor_open` (`0x020861C8`): `Vfs_FindFile` → `VfsIndex_FindFile` (`0x0209D758`) busca el nombre en el
   índice de `vfs.bin`, obtiene la posición en el directorio del ZIP y llama a `unzGoToFilePos_hyp` y
   `unzOpenCurrentFile_hyp`.
6. `dsPackedFile_Read` (`0x02086020`) → `unzReadCurrentFile_hyp` (`0x0203BA44`), en trozos si está en el hilo de carga.
7. El hilo marca la petición con estado 3 (bien) o 4 (error); `FileSys_GetRequestResult_hyp` lo consulta.

### Formato de `vfs.bin` (VERIFICADO: `tools/vfs_lookup.py` lo reimplementa y acierta 4024 de 4024 archivos)

- Cabecera de 8 bytes con forma de nodo; el `u16` en el offset 4 es el primer nodo de la raíz.
- 4277 nodos de 8 bytes: `u16` offset del nombre, `u16` hermano siguiente, `u16` primer hijo, `u16` nº de archivo
  (`0xFFFF` = no hay). Es un árbol de carpetas: se parte la ruta por `\` y se busca cada trozo entre los hermanos.
- 4024 valores `u32`: posición de cada archivo dentro del directorio central del ZIP.
- 29822 bytes de nombres en minúsculas terminados en cero.

## Overlays: quién los carga (VERIFICADO en el descompilado)

- `FS_LoadOverlay` = `0x0200E3B0`, `FS_UnloadOverlay` = `0x0200E404`, `FS_LoadOverlayInfo` = `0x0200DF9C` (SDK).
- El objeto del juego guarda en `+0x38` qué overlays están cargados: bit 0 = overlay **1**, bit 1 = overlay **0**,
  bit 2 = overlay 2, bit 3 = overlay 3.
- `GameApp_Init_hyp` (`0x020B3094`) carga al arrancar los overlays **1 y 3**. Durante el arranque cambia un momento
  3→2 y vuelve a 3 para crear objetos, y crea un objeto con código del overlay 1 (`0x02161E78`).
- Cambios: `GameApp_SwitchToOverlay0` (`0x020B27D0`), `...1` (`0x020B2764`), `...2` (`0x020B2708`), `...3` (`0x020B26AC`).
  Cada una descarga la pareja y carga el suyo.
- **Corrige una hipótesis:** al arrancar se carga el overlay 1, no el 0. Como el overlay 1 tiene el vídeo del editor y
  también las rutas de pista, no está claro cuál es "menús" y cuál "carrera". Para saberlo hay que ver quién llama a
  `GameApp_SwitchToOverlay0/1`. **Pendiente.**

## Gráficos 3D: primera pista (para la Fase 4)

- El juego usa la librería estándar **NitroSystem G3D** de Nintendo para dibujar los NSBMD. VERIFICADO en
  `0x0209534C`: es el intérprete de comandos de modelo (SBC) para *billboards*, con sus opciones `0x20/0x40/0x60` y
  los comandos de guardar/restaurar matriz `0x13`/`0x14`. Nombres exactos = HIPÓTESIS.
- El envío de comandos a la consola pasa por código en la ITCM (`0x01FF9418`, `0x01FF955C`).
- Funciones que tocan la cola de comandos 3D (`0x04000400`): SDK en `0x020067xx`–`0x02006Cxx` y `0x0200A4xx`;
  librería/juego en `0x020955F4`, `0x0209585C`, `0x02094C6C`, `0x0209AE70`.
- Consecuencia para el port: el dibujado de modelos sigue un formato y una librería conocidos y documentados.

## Sonido

- `audio/sound_data.sdat` (`0x0210BCA4`) se usa en `0x020B3F40` y `0x020B40A8`. Sin analizar: usan muchas llamadas
  virtuales (clases C++), hay que seguirlas con calma.

## Cadenas útiles como puntos de partida (direcciones VERIFICADAS, su uso es HIPÓTESIS)

| Dirección | Dónde | Cadena |
|---|---|---|
| `0x0210514D` | arm9 | `Lua 5.0.2` (intérprete Lua, versión exacta) |
| `0x02107153`, `0x02107162`, `0x0210DBC6` | arm9 | `scripts/%s.lua`, `scenes/%s.dcs`, `scenes/%s.dcsc` |
| `0x02106ADB`.. | arm9 | `Graphics/Cars`, `%s/bodyGraphics/bodyGraphic%d_128.bmp` |
| `0x0210A525` | arm9 | `%s/%s/%s.str`, `%s/tokens.dat` |
| `0x02000BDC` | arm9 | `[SDK+Actimagine:Mobiclip SDK V1.0.3]` |
| `0x02194153`.. | ov1 | `graphics/tracks/%s/texturebatch.nsbmd`, `%s.hzc`, `%s.pvs`, `scenes/%s/%s.dcsc` (carga de pista) |
| `0x02194211`.. | ov1 | `graphics/cars/%s/lod1.nsbmd` ... partículas, pickups |
| `0x0218FD1F` | ov1 | `movies/publisher.mods` |
| `0x02190454`.. | ov0 | `graphics/%s.pvs`, `Tracks/%s/%s`, `Cars/%s/lod%d`, `Cars/%s/shadow` |

## Pendiente

- Hecho: `SetupNfsNitro.java` ejecutado y probado (bloques `itcm`, `dtcm`, `bss`, `io`, `ov0`..`ov3`; 7327 funciones).
- Hecho: hilo de carga y cadena completa de lectura; carga de overlays.
- Qué overlay es menús y cuál carrera (seguir a quien llama a `GameApp_SwitchToOverlay0/1`).
- Tabla virtual de `GameApp`: actualizar / dibujar (estados del juego).
- Nombrar el resto del SDK (`OS_*`, `MI_*`, `GX_*`/`G3_*`, sonido) y de NitroSystem. Se irá haciendo según haga falta.

Herramienta: `tools/find_callers.py` busca por bytes quién llama o apunta a una dirección (sirve para encontrar
tablas virtuales, que Ghidra no enlaza).
