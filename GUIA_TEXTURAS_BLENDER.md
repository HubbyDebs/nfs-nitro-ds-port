# Guía: mejorar las texturas del juego con Blender

Esta guía explica de dónde sacar las texturas del juego, cómo retocarlas en Blender y cómo hacer que el juego
use la versión retocada. No hace falta saber programar.

> **Importante:** las texturas salen de tu ROM. Son solo para tu uso personal: no las subas ni las compartas, y no
> las metas en la carpeta del proyecto (`nfs-nitro-port`). Todo esto vive en `C:\Users\jj\nfs-nitro-textures`.

---

## 1. Cómo funciona (en dos frases)

Mientras juegas, el juego puede **guardar cada textura que aparece en pantalla** como una imagen PNG. Si en otra
carpeta encuentra una **versión más grande de esa misma textura**, la usa en su lugar.

Así que el trabajo es: capturar las texturas → retocarlas en Blender → guardarlas con **el mismo nombre** en la
carpeta correcta → empaquetarlas.

---

## 2. Las carpetas

Todo está en `C:\Users\jj\nfs-nitro-textures`:

| Carpeta | Qué hay | ¿Se toca? |
|---|---|---|
| `original\` | Las texturas tal como las saca el juego (pequeñas: 64x64, 32x32, 128x64…). | **No.** Solo se miran. |
| `x4\` | Las mismas, ampliadas 4 veces con IA (Real-ESRGAN). **Aquí es donde se retoca.** | **Sí.** |
| `pack\` | Lo que carga el juego (archivos `.tex`). Se genera solo a partir de `x4\`. | **No.** |

Estado hoy: hay **3.004** texturas capturadas en `original\`, pero solo **731** ampliadas en `x4\`. Antes de
retocar, conviene ampliar las que faltan (paso 3).

### El nombre de cada archivo es su "huella"

Ejemplo: `0265c96e78e94c47_64x64.png`

- `0265c96e78e94c47` es una huella calculada a partir de los píxeles de la textura original. Así reconoce el juego
  cada textura. **Si cambias el nombre, el juego ya no la encuentra.**
- `64x64` es el tamaño **original** (en la consola). En `x4\` la imagen mide el cuádruple (256x256), pero el nombre
  sigue diciendo `64x64`: es correcto, no lo cambies.

---

## 3. Antes de empezar: capturar y ampliar

1. **Capturar** (opcional, si quieres texturas de pistas o coches que aún no has visto): abre
   `C:\Users\jj\nfs-nitro-port\capturar_texturas.cmd` y juega por donde quieras. En el menú sale marcada la
   casilla "Guardar las texturas nuevas". Cada textura nueva va a `original\`.
2. **Ampliar con IA:** abre `C:\Users\jj\nfs-nitro-port\ampliar_texturas.cmd`. Amplía x4 todas las de `original\`
   que aún no estén en `x4\`, y luego lo empaqueta todo. Con unas 2.300 pendientes tardará unos minutos.

Después de esto, cada textura tiene su versión en `x4\` y está lista para retocar.

> **Haz una copia de seguridad de `x4\`** antes de retocar a lo grande (por ejemplo, copiarla a `x4_copia`). Si
> algún día se ejecuta la ampliación con la opción `--redo`, rehace todo `x4\` y se perderían tus retoques.

---

## 4. Encontrar la textura que quieres mejorar

Los nombres no dicen qué es cada cosa, así que hay que buscarlas a ojo:

- En el Explorador de Windows, abre `x4\` y pon **Ver → Iconos muy grandes**. Verás todas en miniatura.
- **Ordena por fecha** (`original\` sirve mejor para esto): las que se capturaron juntas suelen ser de la misma
  pista o del mismo coche.
- **Truco para aislar una pista concreta:** cambia el nombre de `original\` a `original_todas\`, crea una carpeta
  `original\` vacía, juega solo esa pista con `capturar_texturas.cmd`, y en `original\` quedarán **solo** las de
  esa pista. Al terminar, mueve esos archivos a `original_todas\` y devuélvele el nombre `original`.

Cosas que conviene saber:

- **El mismo dibujo con otros colores es otra textura.** Por ejemplo, el mismo coche en rojo y en azul son archivos
  distintos. Si retocas uno, el otro no cambia.
- **Texturas animadas** (agua, pantallas, efectos): el juego las cambia en cada fotograma y cada fotograma tiene
  su propia huella. Salen muchos archivos casi iguales. No merece la pena retocarlas.
- **Pintura y vinilos de los coches:** puede que el juego los combine en marcha. Si retocas uno y no cambia en el
  juego, es por eso.

---

## 4 bis. Texturas de coches

El juego apunta solo qué texturas son de coches: las que se dibujan con luz, que en este juego son casi solo los
coches. Lo hace cada vez que juegas, en `C:\Users\jj\nfs-nitro-textures\coches.txt`: una línea por textura, con el
mismo nombre que el archivo (sin `.png`). Puede colarse algún objeto iluminado de la pista.

- `ampliar_texturas.cmd` amplía esas con un modelo de IA distinto (`realesrgan-x4plus-anime`), que deja más limpios
  los logos, las letras y los bordes de la carrocería. Si una ya estaba ampliada con el modelo general, la rehace y
  guarda la anterior en `x4_anterior\` (por si la habías retocado). Las ya rehechas se apuntan en
  `hechas_coches.txt`.
- **Para tener todos los coches:** juega con `capturar_texturas.cmd` y pasa por el garaje o la selección de coches.
  Cada coche que se ve en 3D queda apuntado y capturado. Cada color de pintura es una textura distinta.
- **Qué merece la pena retocar a mano en Blender:** las hojas de detalles (logos, matrículas, letras como
  "ESCORT RS", faros y pilotos) y las llantas. La IA no puede inventar letras nítidas; a mano sí.
- **Aviso:** en pantalla el coche ocupa pocos píxeles (unos 300 de ancho con el 3D a x3), así que el cambio de
  modelo de IA se nota poco. Se nota más con el 3D a x4 o x5 y con retoques a mano.

## 5. Retocar en Blender (Blender 5.2, ya instalado)

Blender se puede usar como programa de dibujo 2D para estas imágenes.

### Abrir la imagen

1. Abre Blender. Arriba, en las pestañas de espacios de trabajo, elige **Texture Paint**. También vale cambiar
   cualquier panel a **Image Editor**.
2. En el Image Editor: **Image → Open** (`Alt+O`) y elige el PNG de `C:\Users\jj\nfs-nitro-textures\x4\`.
3. Arriba a la izquierda del Image Editor, cambia el modo de **View** a **Paint** para poder pintar.

### Qué se puede hacer

- **Pintar y corregir** con los pinceles **Draw** (pintar), **Soften** (suavizar), **Smear** (difuminar) y
  **Clone** (copiar de una zona a otra: muy útil para arreglar manchas que haya dejado la IA).
- **Añadir detalle a mano:** rejillas, letras más nítidas en carteles, juntas en el asfalto, remaches… La IA amplía,
  pero no inventa detalle con sentido. Aquí es donde un retoque a mano se nota.
- **Transparencia (alfa):** las texturas de vallas, árboles, rejas, etc. tienen zonas transparentes. Para tocarlas,
  usa el modo de fusión del pincel **Erase Alpha** (borrar) o **Add Alpha** (añadir). Las zonas transparentes
  tienen que seguir siéndolo, o se verán cuadrados donde antes había huecos.

### Reglas para que el juego la acepte

1. **El tamaño tiene que ser un múltiplo exacto del original**, el mismo para el ancho y el alto. Para una textura
   `64x64`, valen 256x256 (x4, la normal), 512x512 (x8), 128x128 (x2)…, pero **no** 300x300 ni 256x512. Si quieres
   cambiarlo, usa **Image → Resize**.
   - Recomendado: quedarse en x4. Con x8 cada textura ocupa 4 veces más memoria (hoy las 731 ya ocupan 220 MB).
2. **Las texturas que se repiten** (asfalto, hierba, muros) tienen que seguir **encajando por los bordes**: lo que
   sale por la derecha entra por la izquierda, y lo de arriba con lo de abajo. Si pintas cerca de un borde, haz lo
   mismo en el borde contrario, o se verá una costura en el juego.
3. **No cambies el nombre** del archivo y guárdalo **en la misma carpeta `x4\`**, sobrescribiendo el anterior.
4. **No toques `original\`**: la huella sale de ahí.

### Guardar (importante)

**Image → Save As** (`Shift+Alt+S`) y comprueba a la derecha:

- **File Format:** PNG
- **Color:** **RGBA**. Si eliges RGB, se pierde la transparencia.
- **Color Depth:** 8
- **Save as Render: desmarcado.** Si está marcado, Blender aplica su gestión de color (AgX/Filmic) y la textura
  sale con otros colores.

Guarda con el mismo nombre en `x4\`. Si solo usas **Image → Save** (`Alt+S`) sobre la imagen que abriste,
mantiene el formato y basta.

> Ver la textura puesta en el coche o la pista en 3D dentro de Blender todavía no es posible: haría falta el
> extractor de modelos (Fase 3 del plan, sin hacer). Por ahora se retoca en 2D y se comprueba en el juego.

---

## 6. Pasar los retoques al juego

1. **Empaquetar:** abre `C:\Users\jj\nfs-nitro-port\ampliar_texturas.cmd`. Vuelve a empaquetar solo las de `x4\`
   que hayan cambiado: se fija en la fecha del archivo, así que tus retoques se detectan solos. Si además hay
   capturas nuevas sin ampliar, las amplía.
   - Si solo quieres empaquetar, sin ampliar nada nuevo: en una terminal, dentro de
     `C:\Users\jj\nfs-nitro-port`, ejecuta `.venv\Scripts\python.exe tools\textures\upscale.py --pack-only`
2. **Cierra el juego si estaba abierto y vuelve a abrirlo.** La lista de texturas se lee al arrancar.
3. En el menú del principio, deja marcada **"Texturas ampliadas con IA"**. Ojo: el perfil "Original" la desmarca.

### Si no ves el cambio

- **¿Cambiaste el nombre o lo guardaste en otra carpeta?** Tiene que ser exactamente el mismo nombre, en `x4\`.
- **¿El tamaño es un múltiplo exacto del original?** Si no lo es, el juego la ignora en silencio y usa la original.
- **¿Se empaquetó?** Mira en `pack\` que el `.tex` con ese nombre tenga la fecha de ahora.
- **¿Es una textura animada o un color de coche distinto?** Ver la sección 4.
- **Comparar en marcha:** durante el juego, F2 activa o desactiva el filtro de texturas. No hay tecla para quitar
  solo las texturas ampliadas: para eso, en el menú, desmarca "Texturas ampliadas con IA" y vuelve a entrar.

---

## 7. Resumen rápido

1. `capturar_texturas.cmd` → jugar (solo si faltan texturas).
2. `ampliar_texturas.cmd` → amplía con IA y empaqueta.
3. Copia de seguridad de `x4\`.
4. Abrir el PNG de `x4\` en Blender, retocar, **Save As: PNG, RGBA, 8 bits, sin "Save as Render"**, mismo nombre.
5. `ampliar_texturas.cmd` otra vez → empaqueta los retocados.
6. Abrir el juego con "Texturas ampliadas con IA" marcada.

---

## Datos técnicos (por si los necesita otra persona u otra IA)

- Huella: FNV-1a de 64 bits sobre los píxeles descodificados (0xAARRGGBB) de la textura original, empezando con
  `1469598103934665603 ^ (ancho * 4099 + alto)`. Código: `port/recomp/gx.c`, función `tex_hash`.
- `.tex`: cabecera `NTX1` + ancho + alto (enteros de 32 bits, little endian) + píxeles en orden B, G, R, A por
  bytes. Lo genera `tools/textures/upscale.py` (función `pack`).
- El juego acepta la sustituta si su ancho y su alto son múltiplos del original. La busca por el nombre
  `<huella>_<ancho>x<alto>.tex` en `pack\`, que lista al arrancar, y la carga en un hilo aparte la primera vez que
  aparece. Le hace sus reducciones (mipmaps) y la filtra igual que a las demás.
- Carpeta de texturas distinta: variable de entorno `NFS_TEXTURES`.
