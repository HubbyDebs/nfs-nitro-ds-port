# PROGRESS

Última actualización: 2026-10-14

## Entorno

- Carpeta del proyecto: `C:\Users\jj\nfs-nitro-port` (solo código y notas). ROM y datos extraídos: `C:\nfs` (fuera del proyecto).
- Windows 10 LTSC 2021. Python 3.14.7. Ghidra 12.1.4 (proyecto `nfs`, programa `arm9.bin`).
- `ndstool` no está en el PATH de PowerShell (solo en la terminal MSYS2 de devkitPro).

## Fase 0: comprobar el entorno — HECHA (2026-10-13, conexión con Ghidra confirmada: 5185 funciones, base 0x02000000)

Nota: lo de abajo sobre la conexión quedó resuelto al activar el plugin.

- [x] Carpetas `notes/`, `tools/`, `port/` y este archivo.
- [x] Código ARM con sentido en `0x02000800`. VERIFICADO leyendo `arm9.bin` en el offset `0x800`:
      `E3A0C301` (`mov r12,#0x04000000`), `E58CC208` (`str r12,[r12,#0x208]`): es el arranque típico de DS.
      Además el pie de `arm9.bin` tiene la marca `0xDEC00621`.
- [x] Conexión con Ghidra: resuelta al activar `GhidraMCPPlugin` (`File > Configure > Developer`).

## Fase 1: inventario de datos — HECHA (pendiente de confirmación del usuario)

- Ver `notes/data_inventory.md` y `notes/overlays.md`.
- Resumen: todo el juego está en `data/vfs.zip`, un ZIP normal con 4024 archivos en formatos estándar de DS
  (2494 modelos NSBMD con texturas dentro, animaciones, gráficos 2D, scripts Lua 5.0, audio SDAT).
- Desconocido: formato `.hzc` (uno por pista).

## Fase 2: mapa del código — EMPEZADA, BLOQUEADA por Ghidra

El usuario eligió seguir con la Fase 2 (2026-10-13).

- [x] Copia de seguridad del proyecto de Ghidra: `C:\Users\jj\nfs-ghidra-backups\2026-10-13_antes_fase2`
      (hecha con Ghidra abierto; 17 archivos, 42 MB).
- [x] Cadenas clave y sus direcciones: `notes/arm9_map.md` (`tools/arm9_strings.py`).
- [x] Conexión con Ghidra (si se pierde al reiniciar: activar `GhidraMCPPlugin` y `File > Save Tool`).
- [x] Cómo carga el juego un archivo de la ROM: `Vfs_Init` → `Vfs_Open` → `unzOpen` (minizip). 16 funciones
      renombradas en Ghidra. Ver `notes/arm9_map.md`.
- [x] Arranque leído a mano: `_start` `0x02000800` (ARM), `NitroMain` `0x02003000` (Thumb), ITCM/DTCM, BSS.
- [x] Script `tools/ghidra/SetupNfsNitro.java` (copiado a `C:\Users\jj\ghidra_scripts`): crea `_start`, `NitroMain`,
      bloques ITCM/DTCM/BSS/E-S e importa los 4 overlays. SIN PROBAR.
- [x] Script ejecutado y auto-análisis terminado (23:04, 148 s, guardado). 7327 funciones, 14 bloques de memoria.
- [x] `NitroMain` y bucle principal descritos. Clase del sistema de archivos (tabla virtual `0x0210962C`): 31
      funciones renombradas en total. Regla `raw:` y cola de cargas asíncronas.
- [x] Hilo de carga y cadena completa de lectura de un archivo del ZIP (clase `dsPackedFile`, minizip).
- [x] Formato de `vfs.bin` resuelto y probado: `tools/vfs_lookup.py` acierta 4024/4024 archivos.
- [x] Carga de overlays: `FS_LoadOverlay`, cuatro funciones de cambio, al arrancar se cargan el 1 y el 3.
- [x] Primera pista de gráficos: el juego usa NitroSystem G3D estándar.
- [x] Qué overlay es cada cosa (VERIFICADO en el registro de una partida, 2026-10-14): **overlay 1 = menús**,
      **overlay 0 = carrera** (comparten dirección `0x0212A1A0`); el 2 y el 3 se cargan y descargan en cada
      cambio de pantalla (comparten `0x021952A0`).
- [ ] Sin hacer: tabla virtual de `GameApp`; nombrar el SDK en masa.

Total: 54 funciones renombradas en Ghidra (las `_hyp` son hipótesis).

"Hecho cuando" de la Fase 2 (saber cómo arranca el juego y cómo carga un archivo desde la ROM): CUMPLIDO.
Fase 2 HECHA a falta de confirmación del usuario (2026-10-13).

Decisión del usuario (2026-10-13): no tiene Blender todavía; primero quiere ver que el juego se puede portar,
las mejoras gráficas vienen después.

## Fase 3: extracción de modelos y texturas — HECHA (2026-10-14)

- [x] `tools/extract_models.py` + `tools/models/nsbmd.py`: NSBMD → OBJ + MTL + PNG + JSON. El Focus se ve bien en
      Blender (comprobado con `tools/models/blender_preview.py`, Blender 5.2 sin ventana). Formato en
      `notes/model_format.md`.
- [x] **Modelos nuevos en el juego**: el port reconoce cada pieza por la huella de su lista de visualización y puede
      dibujar un sustituto (`reemplazos\*.nmesh`, `tools/models/make_replacement.py`). Prueba de identidad exacta.
      Focus nuevo de prueba (garaje + carrocería subdividida en Blender) instalado: se ve en carrera, con un defecto
      de reflejo en la parte trasera. Guía: `GUIA_MODELOS_BLENDER.md`. Casilla en el menú.
- Dato nuevo (VERIFICADO): en carrera el coche del jugador usa `lod1`, no `lod0`.

## Fase 4: cómo se dibuja el juego — HECHA a falta de confirmación (2026-10-13)

El usuario eligió hacer la Fase 4 antes que la 3 (la Fase 3 queda pendiente).

- [x] `notes/rendering.md`: efectos que piden los 2494 modelos (`tools/survey_rendering.py`), estado global del 3D
      (`Renderer_Init`, `Renderer_RenderFrame`), qué hay que reproducir y qué queda sin resolver.
- [x] 8 funciones más renombradas en Ghidra (62 en total).
- [ ] **Decisión del usuario pendiente:** A (reescritura), B1 (emulador existente + renderizador mejorado) o
      B2 (recompilación estática). Recomendación: B1. Detalle en `notes/rendering.md`.
- Sin resolver: pintura/vinilos en tiempo de ejecución, LOD y visibilidad, partículas, si la niebla se usa de verdad.

## DECISIÓN del usuario (2026-10-13): opción **B2, recompilación estática**

Traducir el código ARM9 (y overlays) a C y sustituir el hardware/SDK de Nintendo por una capa de PC.

Estado del entorno para B2 (VERIFICADO):
- **No hay compilador de C para PC** (ni gcc, ni clang, ni MSVC). Solo los cruzados de devkitPro (ARM).
  El `pacman` de devkitPro no tiene repositorio mingw. Hace falta instalar uno (pendiente de permiso del usuario).
- Python 3.14.7 sin `capstone` (desensamblador). `uv` disponible para instalarlo en un entorno del proyecto.
- `C:\devkitPro\devkitARM\bin\arm-none-eabi-objdump.exe` existe: sirve como referencia para comprobar el desensamblado.
- 149 GB libres en C:.

Instalado con permiso del usuario (2026-10-13): llvm-mingw portátil en `C:\Users\jj\nfs-nitro-tools\`, `.venv` con
capstone. Carpeta de compilación `C:\Users\jj\nfs-nitro-build\` (el C generado NO va en el proyecto). Ver `notes/recomp.md`.

Hitos propuestos (cada uno comprobable):
1. [x] Lista de funciones desde Ghidra → `port/recomp/functions.csv` (7327 funciones).
2. [x] Traductor ARM/Thumb → C (`tools/recomp/recomp.py`) y prueba: `VfsIndex_FindFile` traducida acierta
   **4024/4024**. Además el juego entero se traduce: 7321 funciones, 536.105 instrucciones, solo 4 sin traducir.
3. [x] **HECHO (2026-10-14).** El juego traducido (9.641 funciones con las 2.320 descubiertas) arranca en el PC desde
   su punto de entrada y su propio código abre `vfs.zip` (`unzOpen` devuelve un manejador válido). Los hilos del
   juego ya funcionan (fibras). Se para cuando todos los hilos esperan una interrupción. Ver `notes/recomp.md`.
4. Capa de hardware. EN CURSO:
   - [x] 4.1 Interrupciones y VBlank (2026-10-14): el juego corre 400 fotogramas, ejecuta `GameApp_Init_hyp` entero
     con sus cambios de overlay y lee archivos comprimidos del ZIP. 3 fallos del traductor corregidos (ver
     `notes/recomp.md`).
   - [~] 4.2 Servicios del ARM7 (2026-10-14): pantalla táctil y memoria de guardado real (`nfs_nitro.sav`) hechas;
     sonido sin contestar (no bloquea). El juego ejecuta sus scripts Lua, carga textos y la pantalla de licencia de
     Nintendo, y corre 1500 fotogramas sin fallos. **Parado en el vídeo del editor** (`publisher.mods`): se salta y
     el juego se queda esperando. Traductor comprobado contra Unicorn: 134.539 casos, 0 fallos en instrucciones
     reales. 3 fallos más de control de flujo corregidos (ver `notes/recomp.md`).
   - [x] 4.3a Intérprete de reserva (`interp.c`) y ventana con las dos pantallas en 2D (`video.c`, `win.c`)
     (2026-10-14). **El juego se ve**: Nintendo, EA, Mobiclip y la pantalla "Profile Name". Vídeos saltados.
     Se arranca con `jugar.cmd`.
   - [x] 4.3b Pantalla táctil y botones comprobados con un guion de entrada (2026-10-14).
   - [x] 4.3c **3D hecho (2026-10-14): la carrera de tutorial se ve y se conduce.** Chip 3D por software en
     `gx.c` con resolución interna configurable (`NFS_3D_SCALE`), temporizadores, avisos de DMA. 5 fallos más
     del traductor/descubridor corregidos. Detalle en `notes/recomp.md`.
   - [x] Probado por el usuario (2026-10-14): tutorial completo y varias carreras, fluido. Guardado comprobado.
     Arreglado el destello de la escena al cargar carreras (efecto de brillo por capas).
     **Hito 4 cumplido: el juego es jugable.**
   - [x] Menú principal y guardado (VERIFICADO, 2026-10-14): con la partida del usuario el juego arranca, carga
     `nfs_nitro.sav`, va al menú, corre una carrera en Dubái (`ro2`), muestra resultados y vuelve al menú.
   - [ ] 4.4 Pendiente: probar el resto de pistas y modos; 91 direcciones en `missing_calls.txt` y 2 trozos
     interpretados en carrera (`01ff9581`, `01ff9724`) que no dan fallos visibles.
5. Sonido (depende del procesador ARM7, que es otro binario). APLAZADO al final por decisión del usuario
   (2026-10-14), pero **es importante para él**: no se descarta. El guardado ya funciona.
6. Renderizador mejorado y enganches por modelo. EN CURSO:
   - [x] 6.1 (2026-10-14) 3D a 1024x768 por defecto, filtrado bilineal, suavizado opcional (`NFS_AA`),
     disposiciones de pantalla (F1), y dibujado optimizado: de 19,5 a 10,5 ms por fotograma en carrera.
   - [x] 6.2 (2026-10-14) **Niebla, modo brillo (carrocería, cristales, ruedas), sombras por volumen, alfa del 3D
     por pixel, mezcla alfa 2D, sprites semitransparentes, sprites rotados y ventanas.** Antes se midió qué usa el
     juego de verdad (todo eso sí; mosaico, ventana de sprites y sprites de mapa de bits no). F5 niebla, F6
     sombras. Coste: menos de 0,5 ms por fotograma. Duda: la escala de profundidad de la niebla es HIPÓTESIS
     (falta compararla con capturas del juego real). Detalle en `notes/recomp.md`, apartado "Fase 6.2".
   - [x] 6.3 (2026-10-14) Luz por píxel, subdivisión sin grietas, texturas x4 con IA (Real-ESRGAN; hay 731
     ampliadas, todas las capturadas hasta ahora; las de pistas no visitadas con `capturar_texturas.cmd` +
     `ampliar_texturas.cmd`), mipmaps, grupo de hilos.
     Por defecto 3D a x3 ampliado con filtro: 12,3 ms por fotograma en la pista más pesada probada.
   - [x] Menú de opciones al arrancar (2026-10-14): `menu.c`, guarda en `opciones.ini`. Ver `notes/recomp.md`.
   - [x] (2026-10-14) Perfiles Original / Recomendada / Máxima en el menú; tirones al cargar cosas en carrera
     arreglados (texturas por páginas de VRAM, sustitutas en un hilo aparte); sombras corregidas (marca de la
     máscara y píxeles opacos de polígonos translúcidos). Ver `notes/recomp.md`, "Tirones, sombras y perfiles".
   - [x] Guía para retocar texturas a mano en Blender: `GUIA_TEXTURAS_BLENDER.md` (2026-10-14).
   - [x] Reflejos y brillo de los coches (2026-10-14): reflejo por píxel en cristales y barniz con reflejo del cielo,
     Fresnel y brillos en la pintura (F7); la pintura refleja la propia escena; resplandor (bloom, F8). Coste
     total: ~0,9 ms por fotograma. Ver `notes/recomp.md`.
   - [x] 6.4 (2026-10-14) **3D con la tarjeta gráfica** (OpenGL, NVIDIA RTX 4050): mismo resultado que el software,
     con MSAA x4 y 3D a 1024x768 por defecto a 12-15 ms. F9 cambia gráfica/procesador. Ver `notes/recomp.md`.

Decisiones del usuario (2026-10-14):
- Multijugador inalámbrico y "Descarga DS" (overlay 2, `downloadus.srl`): FUERA del port.
- Vídeo del editor (`movies/publisher.mods`, Mobiclip): FUERA. Hay que hacer que el juego lo dé por terminado.
- Sonido: se deja para después de tener carreras en 3D.
- Capturas de referencia del juego real (de un vídeo de YouTube o de emulador): el usuario las pasará más
  adelante, en `C:\Users\jj\nfs-nitro-ref` u otra carpeta que indique.
- Ghidra reanalizado con el buscador agresivo (7407 funciones, `functions.csv` reexportado a las 2:28). Ya se
  compila con esa lista (`build.py` la lee siempre; ejecutables de las 5:16 en adelante).

Riesgos conocidos: llamadas indirectas (tablas virtuales) a código que Ghidra no ha marcado como función; overlays
que comparten dirección; hilos del sistema; el sonido vive en el ARM7; no existe herramienta hecha para DS.

## Siguiente (antiguo, sustituido por la decisión de arriba)

- Propuesta: Fase 3 sin Blender (extractor que saque un coche a OBJ/glTF + PNG, comprobable con cualquier visor)
  o saltar a la Fase 4 (cómo dibuja el juego) para llegar antes a la decisión reescritura / emulación.

## PENDIENTE / NO PERFECTO (actualizado 2026-10-14, tras la ronda de arreglos)

Hecho en esta ronda (detalle en `notes/recomp.md`, "Ronda de arreglos", y en `notes/sound.md`):
- **Coches lejanos** (captura `bug parte trasera coches.PNG`): el juego usa de lejos modelos muy simples
  (`lod2`/`lod3`, ruedas de 20 triángulos). `tools/models/upgrade_lods.py` hace que se dibuje en su lugar el
  modelo de carrera (`lod1`) de los 41 coches (292 piezas; 7 compartidas entre coches se dejan). VERIFICADO que
  el juego las sustituye en carrera (6-9 piezas por fotograma); no se pudo hacer una captura con rivales cerca.
- **Rutas fijas**: el menú pide la carpeta del juego ("Buscar…") y comprueba que tenga `arm9.bin`; texturas,
  modelos, compilación y herramientas usan la carpeta del usuario (`%USERPROFILE%`); `build.py` encuentra solo
  llvm-mingw.
- **Rendimiento**: la gráfica mezcla el 2D con el 3D y pinta la ventana (sin leer la imagen de vuelta). Además la
  ventana ya no la estira Windows (escala de pantalla 125 %): más nítida y más grande. En el PC del usuario:
  ventana 1792x960, 3D 1280x960 + MSAA x4, 13,5-15 ms por fotograma (antes 12-15 ms a 1024x768).
- **Sonido** (`snd.c`): secuencias SSEQ con sus instrumentos, canales PCM/ADPCM/cuadrada/ruido, música en
  streaming con alarmas, salida por waveOut. Comprobado con grabaciones (no oído).
- Gráfica: regla de los translúcidos con el mismo identificador. 2D: ventana de sprites, mosaico, sprites de
  mapa de bits (sin cambios en las pantallas probadas: 0 píxeles distintos).

Tercera ronda (2026-10-14; detalle en `notes/recomp.md` y `notes/sound.md`, "Tercera ronda"):
- **Pista blanca**: es la pista nevada (no es un fallo). La tabla de texturas ya no se puede quedar sin sitio.
- **Trasera del coche blanco**: era el Camaro; los halos de las luces de freno / marcha atrás tapaban la trasera.
  Ahora son más tenues y con el borde difuminado.
- **Ruido y tirones de la música**: el sonido se calculaba a 59,83 fotogramas/s con el port a 60: el búfer se
  llenaba y se tiraban trozos. Arreglado (ritmo a 60 + ajuste automático de la salida, 48 kHz).
- **Música en alta calidad**: la de la ROM es mono, 22,7 kHz y 4 bits. El port puede tocar los archivos del
  usuario en su lugar, sincronizados con el juego (`GUIA_MUSICA.md`).

Lo que queda:
1. **Escuchar el sonido** (el usuario): volumen, tono, cortes, y la música sustituta con canciones de verdad
   (probada solo con archivos hechos de la original). Sin hacer: efecto de reverberación por captura,
   barrido de tono (E3) y portamento.
2. **Probar el resto del juego jugando** (otras pistas, modos, policía): con el guion automático solo se llega a la
   primera carrera.
3. **Distancia de la niebla** y **reflejo de la pintura** (supone la cámara casi horizontal): siguen siendo
   HIPÓTESIS; hacen falta capturas del juego real para comparar.
4. **Focus nuevo de prueba**: el paragolpes trasero se ve arrugado (es cómo ilumina el juego esa forma; casilla
   "Modelos 3D nuevos" para quitarlo junto con los coches lejanos mejorados).
5. Diferencia pequeña gráfica/software: la niebla se aplica a cada fragmento, no al píxel final.
6. Fase 2 suelta: tabla virtual de `GameApp`, nombrar el SDK en masa en Ghidra.

Guías para el usuario: `GUIA_TEXTURAS_BLENDER.md`, `GUIA_MODELOS_BLENDER.md`. Notas técnicas en `notes/`.
