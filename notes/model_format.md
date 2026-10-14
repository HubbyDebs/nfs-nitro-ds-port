# Modelos 3D: formato, extracción y sustitución en el juego (Fase 3 y modelos nuevos)

Fecha: 2026-10-14. `VERIFICADO` = comprobado con los datos o ejecutando el juego.

## Dónde están

- Todo va dentro de `data/vfs.zip`. Coches: `Graphics/Cars/<coche>/` (43 coches y variantes `_cop`).
- Cada coche (VERIFICADO con el Focus): `lod0..lod3.nsbmd` (cuerpo en 4 niveles de detalle), `wheel_lod0..3.nsbmd`,
  `shadow.nsbmd` (el volumen de sombra), `flame.nsbmd`/`.nsbta` (llamas del nitro).
- **En carrera el coche del jugador usa `lod1`** (y `wheel_lod1`); `lod2`/`lod3` para los coches lejanos.
  `lod0` no se vio en carrera: HIPÓTESIS, es el del garaje. (VERIFICADO con `NFS_DL_LOG`: en una carrera se
  enviaron las piezas de `lod1`, `lod2`, `lod3`, `shadow`, `wheel_lod1` y `wheel_lod2`, ninguna de `lod0`.)
- Focus: `lod0` 870 triángulos con textura de detalles de 256x256; `lod1` 411 triángulos con la misma textura a
  128x128 (mismo dibujo, a la mitad: VERIFICADO comparando las imágenes).

## Formato NSBMD (estándar de NitroSystem) — lo que lee `tools/models/nsbmd.py`

- Cabecera `BMD0`, bloques `MDL0` (modelos) y `TEX0` (texturas y paletas).
- Diccionarios de NitroSystem: `u8 0, u8 n, u16 tamaño`, luego un árbol cuyo tamaño (`u16` en +6) **cuenta desde el
  inicio del diccionario** (VERIFICADO), luego `u16 tamaño de elemento, u16 tamaño de datos`, los elementos y n
  nombres de 16 bytes.
- Modelo: tamaño, desplazamientos a SBC, materiales, formas y matrices; información (`posScale` en +0x1C); nodos.
- SBC: código que dice en qué orden se dibuja cada forma con cada material y nodo (`NODEDESC`, `MAT`, `SHP`…).
- Material: `diffAmb`, `specEmi`, `polyAttr`, `texImageParam`, ancho y alto de su textura (+32, +34). Qué
  textura y paleta lleva cada material sale de dos diccionarios (textura→materiales, paleta→materiales).
- Forma: desplazamiento y tamaño de su **lista de visualización** (las mismas órdenes que la cola del chip 3D:
  `BEGIN`, `TEXCOORD`, `NORMAL`, `COLOR`, `VTX_16/10/XY/XZ/YZ/DIFF`, `END`).
- `TEX0`: diccionarios de texturas (`texImageParam` con desplazamiento, tamaño y formato) y de paletas; datos
  normales, datos 4x4 con su tabla de índices de paleta, y paletas.

## Herramientas

| Herramienta | Qué hace |
|---|---|
| `tools/extract_models.py Graphics/Cars/Focus` | Saca cada `.nsbmd` de la carpeta a OBJ + MTL + PNG (con transparencia) + JSON en `C:\Users\jj\nfs-nitro-models\<carpeta>`. Un objeto por forma, llamado `<forma>__<huella>` |
| `tools/models/blender_preview.py` | Imagen de comprobación con Blender sin abrir su ventana (`blender -b --python ... -- modelo.obj salida.png [- ángulo]`) |
| `tools/models/blender_refine.py` | Versión refinada automática en Blender: suelda, subdivide (Catmull-Clark) las formas indicadas manteniendo vivas las aristas de más de N grados, renombra las formas a las piezas del juego que sustituyen y exporta OBJ |
| `tools/models/make_replacement.py modelo.obj --info carpeta` | Convierte un OBJ en piezas `.nmesh` para el juego, en `C:\Users\jj\nfs-nitro-models\reemplazos` |

Comprobado (VERIFICADO): el Focus extraído se ve bien en Blender (textura en su sitio: logo, faros, tiradores).

## Sustitución en el juego (`port/recomp/gx.c`, `io.c`)

- El juego envía cada forma al chip 3D por DMA. `io.c` calcula la **huella** (FNV-1a de 64 bits) de los bytes de
  cada lista enviada a `0x04000400`; es la misma huella que pone el extractor en los nombres. VERIFICADO: las
  huellas de las formas del Focus coinciden con las que envía el juego.
- Si en `reemplazos\` hay un `<huella>.nmesh`, `gx.c` dibuja esos triángulos en lugar de la lista (solo si la cola
  está entre órdenes completas). Material, textura, pintura y matrices ya los ha puesto el juego: el sustituto
  solo trae geometría.
- `.nmesh`: `NMS1`, banderas, número de vértices; por vértice `int32 x,y,z` (coma fija 20.12 en unidades de la
  lista original), `int16 s,t` (texeles x16), `int16` normal (-511..511), `u16` color de 15 bits. Banderas: 1 manda
  normales, 2 coordenadas de textura, 4 colores (lo mismo que mandaba la pieza original; el cristal, por ejemplo,
  no manda coordenadas y su reflejo usa las que quedaron de la pieza anterior), 8 no subdividir en el port.
- Prueba de identidad (VERIFICADO): convertir `lod1.obj` sin tocar y dibujarlo como sustituto da la misma imagen
  (37 píxeles distintos de 1,3 millones, en bordes de triángulos; con `NFS_TESS=0`).
- Opciones: `NFS_MESHES=0` (o la casilla del menú) los apaga; `NFS_MODELS` cambia la carpeta; `NFS_DL_LOG=1`
  apunta la huella de cada lista nueva que se envía (para saber qué piezas usa el juego en cada momento).
- Límite de polígonos por fotograma subido a 32.768 (antes 16.384) para que quepan modelos más detallados.

## Prueba: Focus nuevo hecho con Blender (VERIFICADO en carrera)

- Geometría del `lod0` (garaje) en lugar de la del `lod1` (carrera): carrocería subdividida en Blender (667 → 3.960
  triángulos, aristas de más de 35° vivas), detalles (171) y cristales (30) sin subdividir. Instalado en
  `reemplazos\` (3 piezas). Archivos: `C:\Users\jj\nfs-nitro-models\Focus\nuevo\`.
- En carrera se ve la carrocería más redonda y los detalles del modelo de garaje (antena, paragolpes trasero).
- **Defecto visible:** en la parte de atrás, el reflejo de la pintura forma vetas y el paragolpes parece arrugado.
  Pasa igual sin subdividir y con las normales originales del `lod0` (variantes probadas), y en Blender la
  geometría se ve bien: viene del efecto de reflejo del juego (coordenadas de textura generadas a partir de la
  normal, sobre una textura de reflejo de 128x128 muy pixelada) aplicado a una forma con más detalle. HIPÓTESIS
  para mejorarlo: suavizar más las normales de la carrocería, o un reflejo propio del port en lugar del de la DS.
- Variantes en `Focus\nuevo\`: `focus_nuevo.obj` (subdividido, el instalado), `focus_nuevo_b.obj` (soldado y
  normales suaves, sin subdividir), `focus_nuevo_c.obj` (`lod0` tal cual con las normales originales).

## Limitaciones conocidas

- Solo formas con una matriz (sin huesos ni mezcla de matrices): los coches lo cumplen.
- El sustituto usa el material del juego: no puede cambiar texturas ni añadir materiales nuevos (para texturas, ver
  `GUIA_TEXTURAS_BLENDER.md`).
- Cada nivel de detalle es una pieza distinta: para el coche del jugador basta con `lod1`.

## Todos los coches de una vez: `tools/models/upgrade_cars.py` (2026-10-14)

Lo mismo que el Focus de prueba, automático para todos: en carrera (`lod1`) y de lejos (`lod2`, `lod3`) se dibuja
la geometría del modelo de garaje (`lod0`), refinada en Blender (`tools/models/blender_batch.py`, una sola sesión).
- **Emparejado de piezas** (VERIFICADO en los datos de los 35 coches con carpeta normal): carrocería por el mismo
  material (`tex_rgb_mat`), detalles por el nombre con un 1 (`cam_det` → `cam_det1`), cristales `glass` →
  `lodglass`. Solo si las dos texturas son la misma imagen: diferencia de color media < 28 o correlación de la
  luminancia > 0,6 (el GT500 tiene el dibujo retocado: 0,69). Las luces no se tocan (el `lod0` no las tiene).
  Ruedas: `wheel_lod0` sustituye a `wheel_lod1/2/3`.
- **F150 se queda original**: su textura de detalles de garaje tiene otra disposición (correlación -0,19). Si
  una pieza de detalles no tiene pareja, el coche entero se deja (no se mezclan piezas viejas y nuevas).
- **Beetle63**: el `lod0` no tiene cristales aparte (van en la carrocería): el `lodglass` se sustituye por una
  pieza vacía (0 triángulos) para que no salgan cristales dobles. Shelby67: dos piezas de detalles en `lod1`;
  la grande lleva todo y la otra queda vacía.
- **Pegatinas tapadas**: los detalles del `lod0` (pilotos, logos) están en el mismo plano que la chapa; en carrera
  el juego dibuja los detalles antes que la carrocería y esta los tapaba. Se separan 1 cm hacia fuera (por sus
  normales) los detalles y los cristales.
- Carrocería de `lod1` subdividida una vez (aristas de más de 35° vivas); de lejos sin subdividir. VERIFICADO en
  carrera: 18.294 polígonos por fotograma como mucho (límite 65.536); Camaro con su logo lateral y la trasera con
  paragolpes y huecos de pilotos (la DS: un bloque liso), Hummer con todos sus detalles.
- Resultado: 379 piezas + 4 vacías para 34 coches; 5 piezas compartidas entre coches se dejan. Tarda ~4 minutos.
- Copia del estado anterior: `<modelos>\reemplazos_original` (para volver atrás, copiarla sobre `reemplazos`).
- Pendiente: los pilotos traseros se ven oscuros cuando la trasera queda a la sombra (iluminación del juego);
  sin comprobar en el resto de pistas.

## Calcomanías de la pintura más suaves (2026-10-14)

- La pintura de cada coche (`tex_rgb`) la compone el juego mientras se juega: color, franjas y la calcomanía del
  tema (`Graphics/Cars/bodygraphics/bodyGraphicN_128.bmp` / `_256.bmp`: máscaras de 1 bit, 34 dibujos) pintada en
  negro. No está en el pack de texturas ampliadas (cambia con cada coche y color), así que se veía en escalera.
- `smooth_texture` (`gx.c`): las piezas de carrocería de los modelos sustitutos llevan la bandera 64 (la pone
  `make_replacement.py` cuando el material es `tex_rgb_mat`) y su textura se amplía x4 una vez: bicúbica
  (Catmull-Rom) en las zonas lisas; en los bordes (diferencia de luminancia > 0,15 entre los 4 texeles más
  cercanos) el color va del más oscuro al más claro según la luminancia interpolada con una B-spline. Elegida
  entre tres variantes probadas en Python sobre una pintura real (`smoothproto.py`, fuera del proyecto): las otras
  dejaban escalones o halos blancos. VERIFICADO en carrera: la silueta del Camaro sale con bordes redondeados.
- Si la pintura tiene sustituta en el pack, se usa esa. `NFS_SMOOTH_DECALS=0` lo desactiva; `NFS_SMOOTH_DUMP=1`
  guarda la original y la ampliada en PNG (depuración).

## Selección de coche y texturas nítidas (2026-10-15)

- El modelo de garaje (`lod0`, el de la selección de coche) también se sustituye por su versión refinada, y
  ninguna pieza de `upgrade_cars.py` se subdivide ya en el port (`--runtime-subdiv no`): la subdivisión al vuelo
  redondeaba la trasera y doblaba los pilotos (captura `bug skyline.PNG`). 480 piezas + 4 vacías.
- Texturas sin mipmaps por defecto (`NFS_SHARP=1`, casilla del menú "Texturas siempre nítidas"): nítidas también
  de lejos, a cambio de que el suelo lejano pueda parpadear. Anisotrópico x16 (antes x8) para cuando se usen.
- La pantalla de "Circuito" con un coche en una plataforma es una imagen 2D fija (no cambia con los modelos).
- Sin comprobar: la pantalla de selección de coche de las capturas (el guion de prueba no pasa por ella).
