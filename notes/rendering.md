# Cómo dibuja el juego (Fase 4)

Fecha: 2026-10-13. `VERIFICADO` = contado en los datos con `tools/survey_rendering.py` o leído en el descompilado.
Lo demás va marcado como `HIPÓTESIS`.

## Resumen en una frase

El juego usa el 3D de la DS de forma **sencilla y estándar**: modelos NSBMD dibujados con la librería NitroSystem G3D,
pistas con la luz ya "pintada" en los vértices, una sola luz para los coches, brillo y reflejo falso en la carrocería,
niebla opcional, y nada de sombreado toon ni bordes.

## Qué piden los modelos (VERIFICADO, los 2494 `.nsbmd`)

Totales: 1.056.001 vértices, 348.856 polígonos (95.985 triángulos y 252.871 cuadriláteros), 19.144 materiales,
20.224 mallas. Las 20.224 listas de visualización se leen enteras sin ningún comando desconocido.

| Aspecto | Resultado |
|---|---|
| Geometría | Tiras de quads (39.076), tiras de triángulos (24.986), quads (17.056), triángulos (6.462) |
| Estructura | 2423 modelos tienen 1 solo nodo; 71 tienen 3 o más. Casi nada de jerarquías |
| Iluminación | 17.408 materiales **sin luces** (color de vértice); 1.736 con **la luz 0** y nada más. Nunca luces 1-3 |
| Dónde hay luz | Coches: 809 de 879 materiales. Pistas: 898 de 18.071 |
| Mallas | 17.515 con color+textura, 1.308 con normal+textura, 747 solo textura |
| Modo de polígono | 18.844 normal (*modulate*), 299 *toon/highlight*, 1 *shadow* (en `paintFX`) |
| Coordenadas de textura | 18.995 normales; 113 generadas a partir de la normal (**mapa de entorno**, reflejo falso); 36 con matriz |
| Caras | 15.732 solo cara delantera; 3.412 a dos caras |
| Transparencia | Ningún material tiene alfa < 31 ni "color 0 transparente". La transparencia sale de las texturas A3I5/A5I3 (195 texturas) |
| Niebla por material | Desactivada en los 19.144 materiales del archivo (ver abajo: el motor la puede activar al dibujar) |
| Brillo especular con tabla | No se pide en ningún material |

Efectos especiales, dónde están (VERIFICADO):
- *toon/highlight*: 296 materiales de coche (`lodglass`, `glass`, materiales de rueda `*_wheel_PNT`) y 2 de pista.
- Mapa de entorno: 112 materiales de coche (`tex_rgb_mat`, presente en los 4 niveles de detalle de cada coche).
  Las texturas de reflejo están en `Graphics/EnvMaps/` (`ref64`, `window`, `windowHi`, `windowHi4`).

Tamaño de los modelos: un coche en su mejor nivel de detalle (`lod0`) tiene de media 449 polígonos y como máximo
1.125. El trozo de pista más grande tiene 4.380. Es geometría muy ligera para un PC.

## Estado global del 3D (VERIFICADO en el descompilado)

Funciones (nombradas en Ghidra): `Renderer_Init` `0x02049410`, `Renderer_RenderFrame` `0x0204808C` (se llama una vez
por vuelta del bucle principal), `Renderer_SetClearColor` `0x020454E4`.

Registro `DISP3DCNT` (`0x04000060`):
- `Renderer_Init`: activa el bit 1 → el modo *toon/highlight* funciona como **highlight** (brillo), no como toon.
  Activa el bit 4 (**antialiasing**), apaga el bit 5 (**sin marcado de bordes**), activa el bit 3 (mezcla alfa).
- `Renderer_RenderFrame`, cada fotograma: activa prueba de alfa (bit 2) con referencia 0 (`0x04000340 = 0`), mezcla
  alfa (bit 3), y vuelve a apagar el marcado de bordes (máscara `0xCFDF`).
- Lee los bits 12 y 13 (desbordamiento de líneas / de polígonos) y lleva la cuenta: el juego vigila si se pasa del
  límite de polígonos de la DS.

Niebla:
- `G3X_SetFog` (`0x02006AE0`) se llama cada fotograma con un interruptor guardado en el renderizador (`+0x864`),
  la pendiente (`+0x85C`) y el desplazamiento. El color sale de tres valores en coma fija (`+0x868/86C/870`).
  Es decir: **la niebla existe y se configura por escena**.
- `G3X_SetFogTable` (`0x02006B98`): tabla lineal 0, 4, 8 ... 124 (32 pasos).
- HIPÓTESIS: como los materiales del archivo traen la niebla apagada, el motor la enciende al dibujar los modelos que
  toca (NitroSystem permite cambiarlo en memoria). Sin comprobar.

Luces:
- `Renderer_Init` define las 4 luces (dirección y color) con valores por defecto mediante llamadas virtuales, y una
  luz ambiente. Los modelos solo usan la luz 0.
- Calcula una tabla de brillo (curva de potencia, 128 entradas) y la envía: es la tabla de *shininess* del hardware.

Otros:
- Color de fondo: `G3X_SetClearColor` con color configurable, identificador de polígono `0x3F` y sin niebla.
- Intercambio de búferes (`0x04000540`): bit 1 = búfer de profundidad **W** (activado salvo que una vista diga lo
  contrario), bit 0 = orden manual de los polígonos translúcidos.
- La tabla de toon (`0x04000380`) y la de colores de borde (`0x04000330`) solo las toca la inicialización del SDK.
  HIPÓTESIS: el juego no las usa.

## Cómo está organizado el dibujado (parcial)

- `Renderer_RenderFrame` recorre una lista de **vistas**. Cada una tiene su rectángulo de pantalla (se empaqueta en la
  estructura global de G3D) y su cámara. VERIFICADO.
- Dentro de cada vista recorre objetos con un campo de tipo (1, 2, 3 o 4). Los de tipo 2 se reparten en dos listas
  (una se ordena antes de dibujar) y se dibujan con `0x02096184`. HIPÓTESIS: tipo 1 = modelos de escena con
  visibilidad (PVS), tipo 2 = modelos sueltos opacos/translúcidos, tipo 3 = capas 2D, tipo 4 = partículas.
- **3D en las dos pantallas**: escribe el registro de captura de pantalla (`0x04000064`) y alterna según la paridad
  del fotograma (`+0xC64 & 1`). HIPÓTESIS fuerte: es la técnica habitual de DS para tener 3D arriba y abajo,
  dibujando cada pantalla en fotogramas alternos. En PC esto desaparece: se dibujan las dos siempre.
- El intérprete de modelos es NitroSystem G3D (ver `arm9_map.md`); el envío de comandos pasa por código en la ITCM.

## Qué habría que reproducir en un renderizador de PC

1. Listas de visualización de DS → triángulos (posiciones en coma fija, quads y tiras). Sencillo.
2. Texturas en 5 formatos (4x4 comprimido, paleta 256, A5I3, A3I5, paleta 4), con repetición.
3. Color de vértice × textura (pistas) y una luz direccional difusa + ambiente (coches).
4. *Highlight* en cristales y ruedas, y mapa de entorno en la carrocería.
5. Niebla lineal por distancia, prueba de alfa y mezcla alfa.
6. Animaciones de textura (452 `.nsbta`) y de huesos (35 `.nsbca`).

Nada de esto es difícil en OpenGL moderno, y todo admite mejoras directas (más resolución, filtrado de texturas,
luz por píxel, reflejos y sombras reales).

## Decisión pendiente del usuario: cómo hacer el port

Datos para decidir (VERIFICADO): el código son 1,1 MB de ARM9 + 0,95 MB de overlays, unas 7.300 funciones, C++ con
muchas clases y llamadas virtuales. Los scripts Lua son solo 150 archivos pequeños (configuración y escenas): la
física, la IA y los menús están en código nativo.

- **A. Reescritura:** reconstruir el juego en C++ con motor propio. Máxima libertad gráfica, pero hay que entender y
  rehacer la física, la IA, los menús y el modo carrera. Riesgo alto de que "no se sienta igual". Meses o años.
- **B. Ejecutar el código original y cambiar solo los gráficos:** el juego es jugable desde el primer día y se
  comporta exactamente igual. Dos formas:
  - **B1, sobre un emulador existente** (p. ej. melonDS, de código abierto): se sustituye su dibujado 3D por uno
    mejorado y se enganchan las mejoras. Es lo más rápido.
  - **B2, recompilación estática:** traducir el código ARM a C y sustituir las funciones del SDK de Nintendo por
    versiones de PC. Da un ejecutable nativo "de verdad", pero para DS no hay herramienta hecha: habría que escribirla.
- Punto a favor de B descubierto en esta fase: como el juego dibuja con NitroSystem G3D y sabemos qué archivo es cada
  modelo, las mejoras se pueden enganchar **a nivel de modelo** (cambiar un coche por su versión subdividida, una
  textura por una más grande), no solo subir la resolución.

Recomendación de Claude: **B1** para tener algo jugable y mejorado pronto; B2 o A solo si después se quiere
independencia total del emulador.

## Sin resolver

- Cómo se aplica la pintura y los vinilos del coche en tiempo de ejecución (`bodygraphics/*.bmp`, `paintFX`).
- Selección de nivel de detalle y sistema de visibilidad (`.pvs`).
- Partículas (`Scripts/particles/*.luac`) y capas 2D.
- Qué hace exactamente cada tipo de objeto del renderizador (tabla de arriba es hipótesis).
- ~~Si la niebla se activa de verdad~~: RESUELTO (2026-10-14, VERIFICADO ejecutando): sí, en carrera. Valores en
  `recomp.md`, apartado "Fase 6.2".
