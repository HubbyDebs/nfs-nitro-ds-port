# Proyecto: port a PC de Need for Speed: Nitro (Nintendo DS) con gráficos mejorados

Este archivo es el plan de trabajo y el contexto del proyecto. Léelo entero al empezar cada sesión, trabaja **una fase cada vez** y actualiza `PROGRESS.md` al terminar cada paso.

## Quién es el usuario y cómo hablarle

- Habla español, no es experto en ingeniería inversa. Explica con palabras sencillas qué haces y por qué.
- Le interesa el resultado final: el juego jugable en PC, con mejores gráficos (más vértices en los modelos, texturas más grandes, mejor iluminación y resolución alta).
- Al final de cada fase, para y resume: qué has hecho, qué has comprobado de verdad, qué queda dudoso y qué propones hacer después. No pases a la fase siguiente sin que el usuario lo confirme.

## Reglas de trabajo

1. **Solo uso personal con la ROM propia del usuario.** Nunca metas la ROM, los modelos, las texturas ni el audio originales en el repositorio ni en nada que se vaya a compartir. El port debe cargar los recursos desde la ROM del usuario en tiempo de ejecución. En el repositorio solo va código propio y documentación.
2. **No inventes.** Marca cada afirmación como `VERIFICADO` (visto en el desensamblado, en los datos o ejecutándolo) o `HIPÓTESIS`. Si no sabes algo, di que no lo sabes y propón cómo comprobarlo.
3. **Haz copia de seguridad del proyecto de Ghidra** antes de renombrar o cambiar cosas en masa. Ghidra tiene que estar abierto con el programa cargado para que funcione la conexión.
4. Guarda lo que aprendas en `notes/` (un archivo por tema) y el estado en `PROGRESS.md`. Si algo se queda solo en la conversación, se pierde.
5. Las herramientas propias (extractores, scripts) van en `tools/`. El código del port, en `port/`.
6. Prefiere pasos pequeños que se puedan comprobar: un extractor que saque un modelo y se vea en Blender vale más que mil líneas sin probar.

## Estado actual (ya hecho)

- ROM: `C:\nfs\nitro.nds`. Título `NFSNITRO`, código `BNNE` (NTR-BNNE-USA), maker 69 (Electronic Arts).
- Extraída con `ndstool` en `C:\nfs`: `arm9.bin`, `arm7.bin`, `y9.bin`, `y7.bin`, `banner.bin`, `header.bin`, carpetas `data/` y `overlay/`.
- Datos de la cabecera (VERIFICADO con `ndstool -i`):
  - ARM9: dirección RAM `0x02000000`, entrada `0x02000800`, tamaño de código `0x110998`. El archivo `arm9.bin` mide 1.116.580 bytes (incluye un pie de archivo que `ndstool` detecta).
  - Tabla de overlays del ARM9 en la ROM: offset `0x114A00`, tamaño `0x80` (unos 4 overlays, a confirmar).
  - Tabla de nombres de archivo: offset `0x225200`, tamaño `0xC9`. Tabla FAT: offset `0x225400`, tamaño `0x80`, lo que sugiere unos 16 archivos (HIPÓTESIS: pocos archivos grandes, quizá contenedores propios de EA).
  - Zona segura desencriptada (CRC correcta). El aviso "Segment3 CRC INVALID" se ignora.
  - Cadenas del SDK vistas en `arm9.bin`: `[SDK+NINTENDO:BACKUP]` y `[SDK+Actimagine:Mobiclip SDK V1.0.3]` (librería de vídeo). El ARM9 no parece estar comprimido (HIPÓTESIS fuerte, confirmar al ver código con sentido).
- Ghidra 12.1.4, proyecto `nfs`, con `arm9.bin` importado como Raw Binary, lenguaje `ARM:LE:32:v5t:default`, base `0x02000000`. El código empieza en `0x02000800`.
- NTRGhidra está instalada pero es la versión 12.0.4 y Ghidra la marca como incompatible con la 12.1.4, así que **no cargará la ROM entera**. Los overlays hay que importarlos a mano (ver Fase 2).
- Conexión con Ghidra: el plugin `GhidraMCP` de LaurieWired (servidor en `http://127.0.0.1:8080/`; hay que activar `GhidraMCPPlugin` en `File > Configure > Developer` en el Code Browser). Se registra en Claude Code con `claude mcp add --scope user ghidra -- uvx --from git+https://github.com/LaurieWired/GhidraMCP ghidra-mcp --ghidra-server http://127.0.0.1:8080/`.
- REA (morluto/rea) se descartó: su proveedor de Ghidra en Windows solo admite ejecutables x86-64 PE.
- Sistema: Windows. `ndstool` viene de devkitPro (terminal MSYS2). `claude.exe` está en `C:\Users\jj\.local\bin`.

## Plan por fases

### Fase 0: comprobar el entorno
- Confirmar que las herramientas de `ghidra` responden: nombre del programa, número de funciones, dirección base.
- Comprobar que en `0x02000800` hay código ARM con sentido. Si no, corregir la importación antes de seguir.
- Crear `PROGRESS.md` y las carpetas `notes/`, `tools/`, `port/`.
- **Hecho cuando:** puedes listar funciones y cadenas de Ghidra y has anotado la versión de todo en `PROGRESS.md`.

### Fase 1: inventario de los datos del juego
- Listar todo lo de `data/` y `overlay/` con tamaños (`find data -type f -exec ls -l {} \;`).
- Buscar firmas conocidas de DS: `BMD0` (modelo NSBMD), `BTX0` (texturas NSBTX), `BCA0`/`BTA0` (animaciones), `SDAT` (sonido), `NCGR`/`NCLR`/`NSCR`/`NCER` (gráficos 2D). Usa `grep -rla`.
- Si no hay formatos estándar a la vista, mira dentro de los archivos grandes: cabeceras, tablas de offsets, compresión (LZ10/LZ11/LZ77, Huffman, RLE de DS) y documenta el contenedor.
- **Entregable:** `notes/data_inventory.md` con cada archivo, su formato y cómo se organiza por dentro.
- **Hecho cuando:** sabes dónde están los modelos, las texturas, las pistas y el audio, y has comprobado de verdad al menos un bloque de cada tipo.

### Fase 2: mapa del código (ARM9 y overlays)
- Importar a Ghidra los overlays de `overlay/` en las direcciones de RAM que indican `y9.bin` y la tabla de overlays (cada entrada da su dirección de carga y tamaño). Hazlo con un script de Ghidra o a mano, un bloque de memoria por overlay. Anota las direcciones en `notes/overlays.md`.
- Lanzar el análisis automático y reconocer las funciones del SDK de Nintendo (arranque, `OS_*`, `FS_*`/acceso a archivos, `MI_*` copias y DMA, `GX_*`/`G3_*` 3D, sonido). Usa cadenas y patrones conocidos y nombra las funciones.
- Localizar el bucle principal, la carga de overlays y la función que lee archivos de la ROM.
- Buscar las cadenas relacionadas con coches y archivos y seguir sus referencias.
- **Entregable:** `notes/arm9_map.md` con las funciones clave y sus direcciones, y el proyecto de Ghidra guardado con nombres.
- **Hecho cuando:** sabes cómo arranca el juego y cómo carga un archivo desde la ROM.

### Fase 3: extracción de modelos y texturas
- Con lo aprendido en las fases 1 y 2, escribe un extractor en Python (o C++) que convierta los modelos a OBJ o glTF y las texturas a PNG. Si el formato es NSBMD estándar, se puede apoyar en herramientas existentes; si es propio, hay que reconstruirlo.
- Las texturas de DS tienen formatos concretos (paleta de 4, 16 o 256 colores, A3I5, A5I3, comprimido 4x4, color directo). Documenta cuál usa cada una.
- **Entregable:** `tools/extract_models.py` (o similar) y `notes/model_format.md`.
- **Hecho cuando:** un coche entero (modelo y texturas) se abre bien en Blender, con las UV correctas.

### Fase 4: cómo se dibuja el juego
- Estudiar cómo el juego emite comandos 3D a la DS (cola de comandos GX, listas de visualización, matrices, atributos de polígono, iluminación, niebla, borde y sombreado "toon").
- Documentar qué efectos usa realmente, para saber qué hay que reproducir.
- **Entregable:** `notes/rendering.md`.
- **Decisión importante al final de esta fase** (informar al usuario con pros y contras): 
  - **Opción A, reescritura:** reconstruir el juego función a función en C++, con un motor propio. Más limpio y más flexible, pero son meses.
  - **Opción B, capa de emulación con renderizador mejorado:** ejecutar el código ARM9 original y sustituir solo la parte gráfica por un renderizador de PC mejorado. Es mucho más rápida y realista para ver mejores gráficos, pero se queda atada al código original.

### Fase 5: visor de modelos en PC (primer resultado visible)
- Programa en C++ con SDL2 y OpenGL 3.3 (o la alternativa que se acuerde) que cargue un coche extraído y lo muestre a 1080p o más, con antialiasing y cámara orbital.
- **Hecho cuando:** el usuario ve un coche del juego girando en pantalla en su PC.

### Fase 6: mejoras gráficas
Se añaden de menos a más esfuerzo, y cada una se puede probar en el visor:
1. Resolución alta y antialiasing (ya en la Fase 5).
2. Texturas más grandes: reescalado con IA y carga de texturas sustitutas (carpeta aparte del usuario, nunca en el repositorio).
3. Subdivisión de modelos (más vértices): script de Blender con el modificador *Subdivision Surface*, marcando bordes para no redondear las esquinas rectas, recalculando normales y conservando las UV. Aviso al usuario: la subdivisión suaviza las formas, pero no añade detalle que estaba pintado en la textura.
4. Iluminación y sombras modernas, reflejos, bloom.
- **Entregable:** `tools/subdivide_models.py` y los ajustes del renderizador.

### Fase 7: jugabilidad
- Según la decisión de la Fase 4: reconstruir o enlazar la lógica del juego (física del coche, pistas, IA de rivales, menús, entrada de teclado y mando, pantalla táctil sustituida por el ratón).
- Audio: el sonido está en formato SDAT de DS, que se puede reproducir con una librería existente o con un reproductor propio.
- El vídeo (Mobiclip) se puede dejar para el final, sustituirlo o saltarlo.
- **Hecho cuando:** se puede correr una carrera completa.

### Fase 8: pulido y empaquetado
- Opciones de gráficos, resolución, controles, y un cargador que pida la ruta de la ROM del usuario. Nada de recursos originales dentro del paquete.

## Primer paso para empezar

1. Comprobar que la conexión con Ghidra funciona (Fase 0).
2. Hacer el inventario de `data/` y `overlay/` (Fase 1) y pegar al usuario un resumen sencillo.
3. Preguntarle qué quiere hacer a continuación antes de entrar en la Fase 2.
