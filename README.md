# Need for Speed: Nitro (Nintendo DS) — port a PC

Port no oficial a Windows de **Need for Speed: Nitro** para Nintendo DS, con gráficos mejorados. El código del
juego (ARM9) se **recompila a C** y se ejecuta de forma nativa en el PC. El hardware de la DS (chip 3D, motores
2D, interrupciones, temporizadores, DMA, ARM7) se sustituye por una capa propia, y el 3D se dibuja con la
tarjeta gráfica.

> **Importante:** este repositorio **no contiene nada del juego**: ni la ROM, ni modelos, ni texturas, ni audio,
> ni el código traducido. Para jugar necesitas tu propia copia del juego. El port carga todo desde tu ROM
> extraída, en el momento de jugar. Es un proyecto personal sin ánimo de lucro.

## Instalar y jugar (fácil)

1. Descarga la última versión desde **Releases** (el zip del código) y descomprímela donde quieras.
2. Haz doble clic en **`instalar.cmd`**.
3. Elige **tu ROM** de *Need for Speed: Nitro* (archivo `.nds`, versión de EE. UU., `NTR-BNNE-USA`).
4. Espera: el instalador descarga las herramientas (Python y el compilador, unos 200 MB, solo la primera vez),
   extrae tu ROM, **traduce el juego a C en tu PC** y crea el programa. La primera vez tarda unos 10-20 minutos.
5. Abre el juego con el acceso directo **"Need for Speed Nitro"** del escritorio.

Todo va en tu carpeta de usuario: `nfs-nitro-game` (tu ROM extraída), `nfs-nitro-tools` (herramientas),
`nfs-nitro-build\game` (el programa) y `nfs-nitro-models` (mejoras de los coches). Si tienes **Blender 5**
instalado, el instalador también prepara los modelos de garaje de los coches para la carrera.

> **¿Por qué no hay un `.exe` para descargar?** Porque el programa lleva dentro el código del juego traducido,
> y ese código es de EA. El port solo reparte sus herramientas: el programa se crea en tu PC a partir de tu
> propia copia del juego.

## Qué funciona

- El juego arranca y se juega: menús, tutorial, carreras y partidas guardadas (probado en varias pistas; el
  resto de pistas y modos no se ha probado a fondo).
- **3D con la tarjeta gráfica** (OpenGL 3.3+), a la resolución de la ventana (hasta 2048x1536), con suavizado
  de bordes MSAA. Si la gráfica falla, se dibuja por software.
- Mejoras gráficas, cada una se puede activar o quitar:
  - luz por píxel;
  - subdivisión de los coches (más polígonos, sin grietas);
  - filtrado de texturas con reducciones y filtrado anisotrópico;
  - **texturas ampliadas x4 con IA** (Real-ESRGAN), con un modelo especial para los coches;
  - niebla, sombras y modo de brillo como en la consola;
  - barniz de los coches (reflejo de la escena, Fresnel y brillos);
  - resplandor (bloom).
- **Modelos 3D nuevos**: puedes cambiar un coche por uno hecho en Blender. Las guías explican cómo.
- **Menú de opciones** al arrancar, con perfiles Original, Recomendada y Máxima.

- **Sonido**: secuencias, efectos y música en streaming, como el ARM7 de la consola.
- **Música en alta calidad** con tus propios archivos de las canciones: ver [`GUIA_MUSICA.md`](GUIA_MUSICA.md).
- Los coches lejanos se dibujan con el modelo de carrera en vez de los modelos simplificados de la DS.

Lo que queda está en [`PROGRESS.md`](PROGRESS.md), sección "PENDIENTE / NO PERFECTO".

## Requisitos

- Windows 10 u 11 de 64 bits. Una gráfica con OpenGL 3.3 o superior es recomendable. En portátiles con dos
  gráficas se usa la potente.
- Tu ROM de *Need for Speed: Nitro* (EE. UU., `NTR-BNNE-USA`), extraída con **ndstool** (viene con devkitPro):
  ```
  ndstool -x nitro.nds -9 arm9.bin -7 arm7.bin -y9 y9.bin -y7 y7.bin -d data -y overlay -t banner.bin -h header.bin
  ```
  La carpeta se elige en el menú del principio (botón "Buscar…") y se recuerda.
- Para compilar:
  - [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang para Windows).
  - Python 3 con `capstone`, `numpy` y `pillow`.
- Opcional:
  - Ghidra, para volver a generar la lista de funciones (ya incluida en `port/recomp/functions.csv`).
  - Real-ESRGAN (`realesrgan-ncnn-vulkan`), para ampliar texturas.
  - Blender, para modelos nuevos.

> Por defecto todo va en tu carpeta de usuario: `%USERPROFILE%\nfs-nitro-build` (compilación),
> `nfs-nitro-textures`, `nfs-nitro-models` y `nfs-nitro-tools\llvm-mingw-*` (el compilador, que `build.py` busca
> solo; también vale `NFS_CLANG`).

## Compilar

```
python -m venv .venv
.venv\Scripts\pip install capstone numpy pillow
.venv\Scripts\python.exe tools\recomp\build.py --rom C:\nfs --build C:\Users\TU_USUARIO\nfs-nitro-build\game
```

`build.py` hace tres cosas:
1. Descubre las funciones que Ghidra no marcó.
2. Traduce todo el ARM9 y los overlays a C, en la carpeta de compilación, **fuera del repositorio**.
3. Lo compila en paralelo junto con la capa de ejecución (`port/recomp/*.c`) y genera `nfs_boot.exe`.

La primera vez tarda unos minutos; después solo recompila lo que cambia.

## Jugar

Ejecuta `jugar.cmd`. Primero sale el menú: elige la carpeta del juego, la calidad gráfica y las opciones.

| Tecla | Botón DS | | Tecla | Función |
|---|---|---|---|---|
| Flechas | Cruceta | | F1 | Disposición de las pantallas |
| Z / X | A / B | | F2 | Filtro de texturas |
| A / S | Y / X | | F3 | Luz por píxel |
| Q / W | L / R | | F4 | Subdivisión de los coches |
| Intro | Start | | F5 / F6 | Niebla / sombras |
| Retroceso | Select | | F7 / F8 | Barniz / resplandor |
| Ratón | Pantalla táctil | | F9 | Gráfica o procesador |
| | | | F11 / Alt+Intro | Pantalla completa (Esc: ventana) |

**Mando** (Xbox o compatible, XInput): gatillo derecho o A = acelerar / aceptar, gatillo izquierdo o B = frenar /
atrás, RB = nitro (R), LB = freno de mano (L), X = Y, Y = X, Start, Back = Select, cruceta o palanca izquierda.
**Icono del programa**: si pones un icono en `%USERPROFILE%\nfs-nitro-build\icono\nfs.ico` (o `NFS_ICON=ruta`),
`build.py` lo mete dentro de `nfs_boot.exe` y lo usan la ventana y el menú. El icono no va en el repositorio.
**Controles**: botón "Controles…" del menú para cambiar teclas y mando.
**120 imágenes por segundo** (experimental): casilla del menú; dibuja una imagen intermedia entre cada dos del
juego. Necesita un monitor de 120 Hz o más y margen de sobra (se apaga sola cuando no da tiempo).
**Pantalla completa**: casilla del menú o F11; con la disposición "Solo la pantalla del juego" la imagen ocupa
todo el alto (1440x1080 en un monitor de 1920x1080; F1 cambia de disposición).

Para que los coches se vean con el modelo de garaje (más detallado) en carrera y de lejos, una vez:
`.venv\Scripts\python.exe tools\models\upgrade_lods.py` y luego `.venv\Scripts\python.exe tools\models\upgrade_cars.py`
(necesita Blender 5.2; tarda unos 4 minutos).

Las opciones se guardan en `opciones.ini`, junto a la partida guardada (`nfs_nitro.sav`).

## Texturas y modelos mejorados

- **Texturas:** `capturar_texturas.cmd` guarda las texturas que aparecen mientras juegas, y
  `ampliar_texturas.cmd` las amplía con IA. Se pueden retocar a mano en Blender: ver
  [`GUIA_TEXTURAS_BLENDER.md`](GUIA_TEXTURAS_BLENDER.md).
- **Modelos:** `tools\extract_models.py` saca un coche a OBJ + PNG, y `tools\models\make_replacement.py`
  mete en el juego el modelo editado. Ver [`GUIA_MODELOS_BLENDER.md`](GUIA_MODELOS_BLENDER.md).

Las texturas y los modelos se quedan en carpetas tuyas, fuera del repositorio.

## Estructura

| Carpeta | Contenido |
|---|---|
| `port/recomp/` | Capa de ejecución: memoria y E/S (`rt.c`, `io.c`), interrupciones (`irq.c`), hilos (`threads.c`), ARM7 simulado (`arm7.c`), sustitutos de funciones del SDK (`hle.c`), intérprete de reserva (`interp.c`), chip 3D (`gx.c`, `gpu.inc.c`), 2D (`video.c`), ventana y teclado (`win.c`), menú (`menu.c`) |
| `tools/recomp/` | Traductor ARM/Thumb → C (`recomp.py`), descubridor de funciones, compilación, pruebas contra un emulador |
| `tools/models/`, `tools/textures/` | Lector de NSBMD, extractor, conversión de modelos, ampliación de texturas |
| `tools/ghidra/` | Scripts de Ghidra (preparar el proyecto, exportar funciones) |
| `notes/` | Todo lo averiguado: formato de los datos, mapa del código, cómo dibuja el juego, el recompilador |
| `PROGRESS.md` | Estado del proyecto, decisiones y lo que queda |

## Cómo funciona (resumen)

1. **Traducción.** Cada función ARM9 se traduce a una función C que trabaja sobre los registros y la memoria de
   una DS virtual. El código que no se pudo traducir lo ejecuta un intérprete. Se comprobó contra un emulador de
   referencia (Unicorn), con 134.539 casos.
2. **Hardware.** Las escrituras del juego en los registros de la DS llegan a la capa del port:
   - las órdenes 3D van a `gx.c`, que transforma, recorta, ilumina y subdivide;
   - la gráfica (`gpu.inc.c`) dibuja los píxeles;
   - `video.c` compone el 2D;
   - el ARM7 se simula a nivel de mensajes (pantalla táctil y memoria de guardado).
3. **Mejoras.** Como el port sabe qué textura y qué modelo dibuja el juego en cada momento, puede cambiar
   texturas (por su huella) y piezas de modelos (por la huella de su lista de visualización) por versiones mejores.

Los detalles, con lo verificado y lo que es hipótesis, están en [`notes/recomp.md`](notes/recomp.md).
