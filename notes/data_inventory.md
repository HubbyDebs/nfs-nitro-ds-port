# Inventario de datos (Fase 1)

Fecha: 2026-10-13. Origen: `C:\nfs` (ROM extraída con `ndstool`).
Scripts usados: `tools/inventory_headers.py`, `tools/inventory_survey.py`, `tools/inventory_textures.py`
(se ejecutan con `python -I tools\<script>.py C:\nfs`).

Cada afirmación va marcada como `VERIFICADO` (leído de los datos con los scripts) o `HIPÓTESIS`.

## Archivos de la ROM

| Archivo | Tamaño | Formato | Estado |
|---|---|---|---|
| `data/vfs.zip` | 20.862.266 | ZIP estándar (Deflate), 4024 archivos, 57.150.627 bytes descomprimidos | VERIFICADO (Python `zipfile` lo abre y lee todo) |
| `data/vfs.bin` | 80.142 | Índice del ZIP para el juego (ver abajo) | parcial |
| `data/vfs.dat` | 19 | Texto: `4277, 4024, 29822` | VERIFICADO |
| `data/audio/sound_data.sdat` | 35.737.728 | SDAT estándar de Nintendo | VERIFICADO |
| `data/audio/sound_dataNetwork.sdat` | 172.192 | SDAT estándar (sonidos del modo descarga) | VERIFICADO |
| `data/movies/publisher.mods` | 95.512 | Vídeo Mobiclip (firma `MODSN3`) | VERIFICADO (firma); contenido sin analizar |
| `data/downloadus.srl`, `downloadeu.srl` | 1.692.360 c/u | ROM hija de DS para "Descarga DS" (cabecera `NFSNITRO` / `BNNE`) | VERIFICADO (cabecera) |
| `data/download.char`, `download.plt` | 512 / 32 | Icono y paleta del juego de descarga | HIPÓTESIS |
| `data/buildname.bin` | 27 | Texto `NFSA_us_DS_2008_12_02_16_07` (fecha de compilación 2-dic-2008) | VERIFICADO |
| `data/sku.bin` | 2 | Texto `us` | VERIFICADO |
| `overlay/overlay_0000..0003.bin` | 424.832 / 436.736 / 30.976 / 62.080 | Código ARM9 adicional, sin comprimir (ver `overlays.md`) | VERIFICADO (tamaños = `y9.bin`) |

Conclusión importante: **no hay contenedor propio de EA que descifrar**. Casi todo el juego está en un ZIP
normal con formatos estándar de Nintendo DS.

## Dentro de `vfs.zip`

| Extensión | Nº | Bytes | Qué es | Estado |
|---|---|---|---|---|
| `.nsbmd` | 2494 | 30.373.464 | Modelos 3D estándar (`BMD0`). Todos tienen 1 modelo (`MDL0`); 449 llevan además texturas dentro (`TEX0`) | VERIFICADO (cabeceras de los 2494) |
| `.nsbta` | 452 | 295.520 | Animación de texturas (`BTA0`/`SRT0`: desplazar/rotar UV) | VERIFICADO |
| `.nsbca` | 35 | 348.444 | Animación de huesos (`BCA0`/`JNT0`), p. ej. edificios que caen | VERIFICADO |
| `.bmp` | 181 | 10.906.018 | BMP de Windows sin comprimir: 78 fondos 256x192 (16 y 24 bits) y 103 vinilos de coche de 8 bits (64, 128 y 256 px) | VERIFICADO |
| `.ncgr` `.nclr` `.nsc` `.ncer` `.nanr` | 169/92/79/90/90 | ~6,2 MB | Gráficos 2D estándar (teselas, paletas, mapas, sprites, animaciones): menús y HUD | VERIFICADO (firmas `RGCN`, `RLCN`, `NCSC`, `RECN`, `RNAN`) |
| `.luac` | 78 | 171.490 | Scripts Lua 5.0 compilados | VERIFICADO (cabecera) |
| `.dcsc` | 72 | 2.025.071 | También Lua 5.0 compilado: escenas de las pistas (`createEntity`, `Waypoint`, `NextNode`, `transformN`...) | VERIFICADO (cabecera y cadenas) |
| `.pvs` | 62 | 625.116 | XML de texto: conjuntos de visibilidad de cada pista (`<Sets>`, `<ViewVolume>`, `<VisibleSet>`) | VERIFICADO |
| `.hzc` | 80 | 5.821.121 | Binario propio, uno por pista. Empieza por `05 01` | Formato **desconocido**. HIPÓTESIS: colisión/altura del suelo |
| `.str` | 48 | 342.877 | Textos por idioma: cadenas con longitud de 16 bits delante, Latin-1 | VERIFICADO (es.str legible) |
| `.dat` (`tokens.dat`) | 2 | 28.004 | Índice de los textos | HIPÓTESIS |

### Carpetas

- `Graphics/Cars/<coche>/`: 43 coches (más variantes `_cop`). Cada uno: `lod0..lod3.nsbmd` (4 niveles de detalle;
  AudiR8: 143 KB, 48 KB, 9 KB, 4 KB), `wheel_lod0..3.nsbmd`, `shadow.nsbmd`, `flame.nsbmd` + `flame.nsbta`.
  `bodygraphics/` tiene los vinilos BMP; `AceFX/`, `addons/` son extras.
- `Graphics/Tracks/<pista>/`: 54 carpetas con código de 3 letras (`tk1`, `ro1`, `us5`...). Muchos trozos pequeños
  (`DC_n_TAG_m.nsbmd`, `DC_gN_PNTBW_m.nsbmd`), un `textureBatch.nsbmd`, un `.hzc` y un `.pvs`.
- `Graphics/Sky/<pista>/sky.nsbmd`, `Graphics/EnvMaps` (reflejos), `Particles`, `destructibles`, `pickups`, `paintFX`, `Selectors`.
- `Graphics/2dMaps`, `Graphics/2dSprites`: menús (`FE`), juego (`BE`), HUD, pantallas de inicio.
- `Scenes/<pista>/*.dcsc`: escena de cada pista. `Scripts/config/*.luac`: `vehicles`, `tracks`, `ai`, `progression`,
  `powerups`, `songlist`, `ui`... `Scripts/particles`, `Scripts/collision`.
- `strings/main` y `strings/download`: textos en 8 idiomas. `bitmaps/`: fondos.

### Texturas (VERIFICADO leyendo los 449 bloques `TEX0`)

- No hay archivos `.nsbtx` sueltos: las texturas van **dentro** de los `.nsbmd`.
- Formatos: comprimido 4x4: 2217 · paleta 256: 163 · A5I3: 142 · A3I5: 53 · paleta 4: 2. Ninguna de paleta 16 ni color directo.
- Tamaños más comunes: 64x64 (1294), 128x128 (373), 32x32 (305). Máximo 256x256 (84) y 512x128 (7).
- Coches: de 173 `lodN.nsbmd`, 85 llevan texturas y 88 no; ruedas 74 sí / 76 no.
  HIPÓTESIS: los niveles de detalle bajos reutilizan las texturas del nivel alto.
- Pistas: solo 70 de 1902 modelos llevan texturas. El `.pvs` nombra `textureBatch`, así que
  (HIPÓTESIS fuerte) los trozos de pista toman sus texturas del `textureBatch.nsbmd` de su carpeta.

### Lua

Cabecera: `1b 4c 75 61 50 01 04 04 04 06 08 09 09 04 76 5e df 01`. Es Lua 5.0, little-endian, y el tamaño de
número es **4 bytes**. El número de prueba se lee como el entero 31415926, así que los números de Lua son
**enteros de 32 bits**, no decimales (VERIFICADO en la cabecera). HIPÓTESIS: valores en coma fija.
Consecuencia: hace falta un descompilador de Lua 5.0 ajustado a números enteros.

## `vfs.bin` (parcial)

> RESUELTO: el formato completo y comprobado está en `arm9_map.md` ("Formato de `vfs.bin`") y en `tools/vfs_lookup.py`.
> CORRECCIÓN (Fase 2, visto en el código de `Vfs_Open`): el archivo empieza con una **cabecera de 8 bytes** y después
> van, en este orden, la tabla de 4277 nodos, la tabla de 4024 archivos y los 29822 bytes de nombres. Los registros
> de 8 bytes empiezan por tanto en el offset 8, no en el 0 como se supone abajo.

- `vfs.dat` da tres números: 4277, 4024, 29822. 4024 = archivos del ZIP (VERIFICADO). 4277 - 4024 = 253.
  HIPÓTESIS: 4277 nodos (archivos + carpetas) y 29822 bytes de nombres.
- El tamaño cuadra: 4277x8 + 4024x4 + 29822 = 80.134, y el archivo mide 80.142 (sobran 8 bytes). VERIFICADO (la cuenta).
- Empieza con registros de 8 bytes (4 campos de 16 bits); el último campo es el número de archivo (0..4023) o `FFFF`.
- Después hay una tabla de números de 32 bits que crecen de ~70 en 70. Una entrada del directorio central del ZIP
  mide 46 bytes + el nombre (24 letras en el primero = 70). HIPÓTESIS fuerte: son posiciones dentro del
  directorio del ZIP, para abrir archivos sin recorrerlo.
- Al final están los nombres en minúsculas terminados en cero (`dc_9d_tag_11.nsbmd`, `n2o_topart9.bmp`...). VERIFICADO.
- Para el port no hace falta: basta con abrir el ZIP por nombre. Solo importa para entender el código del juego (Fase 2).

## Audio (VERIFICADO leyendo los bloques INFO/FAT del SDAT)

- `sound_data.sdat`: 124 archivos: 36 `SSAR` (grupos de efectos), 36 `SBNK` (bancos), 36 `SWAR` (ondas), **16 `STRM`**
  (música en flujo: `STRM_CrystalMethod`, `STRM_DizzieRascal`...). No hay secuencias `SSEQ`.
- `sound_dataNetwork.sdat`: 4 `SSAR`, 4 `SBNK`, 4 `SWAR`.

## Pendiente / dudoso

- Formato `.hzc`: sin analizar. Es la pieza desconocida más importante (seguramente colisión).
- `tokens.dat`, `download.char/.plt`: sin analizar.
- No he abierto todavía ningún modelo en Blender: solo se han leído las cabeceras. Eso es la Fase 3.
