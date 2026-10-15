# Recompilación estática (opción B2)

Decisión del usuario del 2026-10-13. Aquí va cómo funciona el traductor, qué se ha probado y qué falta.

## Herramientas instaladas (2026-10-13, con permiso del usuario)

- Compilador portátil **llvm-mingw 20261006** (clang 23.1.3) en `C:\Users\jj\nfs-nitro-tools\`. No toca el sistema;
  se quita borrando la carpeta.
- Entorno de Python del proyecto en `.venv` (creado con `uv`) con `capstone` 5.0.7. De momento el traductor no lo
  usa: decodifica las instrucciones él mismo.
- Carpeta de compilación: `C:\Users\jj\nfs-nitro-build\` (fuera del proyecto).

**Importante (regla 1):** el C que genera el traductor es el código del juego traducido. No va en el repositorio ni
se comparte. Solo se guarda el traductor (`tools/recomp/`) y la capa de ejecución (`port/recomp/`), que son propios.

## Piezas

| Archivo | Qué es |
|---|---|
| `tools/ghidra/ExportFunctions.java` | Exporta de Ghidra dónde empieza y acaba cada función y si es ARM o Thumb |
| `port/recomp/functions.csv` | Esa lista: 7327 funciones (solo direcciones y nombres) |
| `tools/recomp/recomp.py` | El traductor ARM/Thumb → C |
| `port/recomp/rt.h`, `rt.c` | Capa de ejecución: registros, banderas, memoria de la consola, llamadas indirectas |
| `port/recomp/test_vfs.c` | Prueba del hito 2 |
| `tools/recomp/make_vfs_cases.py` | Genera los casos de esa prueba |

## Cómo traduce

- Cada función del juego se convierte en una función C `bloque_direccion(Cpu *c)` (p. ej. `ram_0209d758`).
- Cada instrucción es una etiqueta `L_direccion:` con su efecto sobre los registros `c->r[]`, las banderas y la
  memoria. Los saltos dentro de la función son `goto`; las llamadas (`bl`) son llamadas C directas.
- La memoria de la consola es un bloque de 4 MB en el PC; las direcciones del juego se usan tal cual.
- Volver de una función (`bx lr`, `pop {pc}`...) es un `return` de C. Se comprueba al vuelo que el destino coincide
  con la dirección de retorno con la que se entró; si no, se trata como salto a otra función.
- Saltos calculados (tablas de `switch`): la función lleva un despachador con todas sus direcciones.
- Llamadas indirectas (punteros a función, tablas virtuales de C++): se buscan al vuelo en una tabla
  dirección → función (`call_indirect`).
- El traductor decodifica ARMv5TE y Thumb-1 por su cuenta (sin librerías).

## Resultados

**Hito 2 — VERIFICADO (2026-10-13).** `VfsIndex_FindFile` del juego y las 3 funciones que llama (199 instrucciones),
traducidas a C y compiladas para Windows, devuelven el archivo correcto en **4024 de 4024** casos y dejan la pila
como estaba. Orden:

```
.venv\Scripts\python.exe -I tools\recomp\recomp.py --rom C:\nfs --functions port\recomp\functions.csv --out C:\Users\jj\nfs-nitro-build --roots 0x0209d758
.venv\Scripts\python.exe -I tools\recomp\make_vfs_cases.py C:\nfs C:\Users\jj\nfs-nitro-build\vfs_cases.txt
clang -O1 -w -Iport\recomp -IC:\Users\jj\nfs-nitro-build port\recomp\test_vfs.c port\recomp\rt.c C:\Users\jj\nfs-nitro-build\recomp_funcs.c -o test_vfs.exe
test_vfs.exe C:\nfs C:\Users\jj\nfs-nitro-build\vfs_cases.txt
```

**Traducción completa — VERIFICADO que se genera (falta comprobar que se ejecuta bien).**
7321 funciones, 536.105 instrucciones, en 3 segundos. Sale un archivo C de 60 MB.
- Solo **4 instrucciones sin traducir**: 2 multiplicaciones DSP, 1 `ldm/stm` con registros de usuario, 1 retorno de
  excepción. (Las dos últimas son propias del sistema: cambio de hilo / interrupciones.)
- **987 llamadas a direcciones fijas sin función conocida**: van a overlays (dirección ambigua), a código que Ghidra
  no marcó como función o a funciones del otro modo (ARM/Thumb). Se resuelven al vuelo o fallan con un mensaje.

**Hito 3 — VERIFICADO (2026-10-14).** El juego entero, traducido y compilado para Windows, arranca desde su punto
de entrada original (`0x02000800`) y **su propio código abre `vfs.zip`**. Salida real del programa:

```
[fs] abrir "vfs.zip" -> ok
[fs] abrir "vfs.dat" -> ok
[fs] abrir "vfs.bin" -> ok
[fs] abrir "vfs.zip" -> ok
[hito 3] unzOpen("vfs.zip") del juego devuelve 021bb438 -> ZIP ABIERTO
[fs] abrir "buildname.bin" -> ok
PARADA: el juego pide esperar una interrupcion (cp15 halt): hito 4
```

Hasta ahí se ejecutan, traducidos: el arranque (`crt0`), la copia a ITCM/DTCM, los constructores estáticos de C++,
`OS_Init` y el resto de la inicialización del SDK, la creación del montón, `FileSys_Create`, `Vfs_Init`/`Vfs_Open`
y minizip. El programa se para cuando todos los hilos del juego están dormidos esperando una interrupción (el
"latido" de pantalla): eso ya es el hito 4.

Cómo compilar y arrancar (desde la carpeta del proyecto):

```
.venv\Scripts\python.exe tools\recomp\build.py      (traduce y compila: ~6 min la primera vez, ~11 s después)
C:\Users\jj\nfs-nitro-build\game\nfs_boot.exe C:\nfs
sh tools/recomp/run_loop.sh                          (arranca y, si falta una función, la traduce y repite)
```

## Qué hubo que construir para el hito 3

| Pieza | Archivo | Qué hace |
|---|---|---|
| Descubridor de funciones | `tools/recomp/discover.py` | Ghidra no marca las funciones a las que solo se llega por puntero (constructores, métodos virtuales). Las busca en los datos y las recorre con capstone: **2.320 funciones más**. También lee `missing_calls.txt`, donde el programa apunta las direcciones que pidió y no existían |
| Compilación en paralelo | `tools/recomp/build.py` | Un archivo C por tramo de 32 KB de direcciones (73 archivos); solo recompila los que cambian |
| Modos del procesador | `port/recomp/rt.c` | Registros propios de cada modo (SVC, IRQ, sistema), CP15, BIOS mínima (Div, CpuSet, CRC16...) |
| Registros de hardware | `port/recomp/io.c` | Almacén genérico + contador de línea de pantalla, divisor y raíz por hardware, saludo con el ARM7 (IPCSYNC), DMA, memorias de vídeo como bloques planos |
| Sustitutos del SDK | `port/recomp/hle.c` | `FS_OpenFile`/`FS_ReadFile`/`FS_CloseFile` leen de `C:\nfs\data`; `FS_LoadOverlay`/`FS_UnloadOverlay` cargan el overlay y ejecutan sus constructores (sin probar aún) |
| Hilos del juego | `port/recomp/threads.c` | Cada hilo del SDK corre en una fibra de Windows. `OS_SaveContext`/`OS_LoadContext` se enlazan con `setjmp`/`longjmp` |
| Arranque | `port/recomp/boot.c` | Carga `arm9.bin` y la cabecera, prepara la zona compartida con el ARM7 y salta al punto de entrada |

Cosas aprendidas (VERIFICADO):
- `__builtin_setjmp`/`__builtin_longjmp` de clang **fallan** en Windows x64 (se cae incluso en un programa de 20
  líneas). El `setjmp`/`longjmp` normal funciona, también entre fibras.
- `OSContext` del SDK: `+0x00` cpsr, `+0x04..+0x3C` r0-r14, `+0x40` pc+4, `+0x44` sp del modo SVC.
  `OS_SaveContext` = `0x02008504`, `OS_LoadContext` = `0x02008550`, replanificador = `0x02007DE8`.
- El SDK espera en `0x027FFF8C` (zona compartida) la máscara de servicios que el ARM7 tiene listos.
- Nombres confirmados: `FS_OpenFile` `0x0200D97C`, `FS_SeekFile` `0x0200DB28` (campos `+0x24` inicio, `+0x28` fin,
  `+0x2C` posición), `FS_CloseFile` `0x0200D9C4`, `FS_ReadFile` `0x0200DB18`.

## Hito 4, parte 1: interrupciones — VERIFICADO (2026-10-14)

El juego ya corre **400 fotogramas** seguidos: el latido de pantalla (VBlank) se entrega, los hilos se despiertan y
duermen, y `NitroMain` llega hasta la inicialización del objeto principal del juego (`GameApp_Init_hyp`), que carga
y cambia overlays tal como se veía en Ghidra:

```
[overlay] cargado 1 ... cargado 3 ... descargado 3, cargado 2, descargado 2, cargado 3
[overlay] descargado 1, cargado 0 ... descargado 0, cargado 1 ...
[arm7] mensaje del juego: servicio 6 (TOUCHPANEL), dato 3000000
PARADA: limite de 400 fotogramas alcanzado (NFS_FRAMES)
```

Se queda esperando la respuesta del servicio de pantalla táctil del ARM7, que aún no se simula.

Piezas nuevas:

| Pieza | Archivo | Qué hace |
|---|---|---|
| Tiempo e interrupciones | `port/recomp/irq.c` | El tiempo avanza por "saltos hacia atrás" ejecutados (60.000 por tramo, 4 tramos por fotograma) y cuando el juego se para a esperar. Entrega la interrupción ejecutando el `OS_IrqHandler` original (`0x01FF8000`) y hace a mano lo de `OS_IrqHandler_ThreadSwitch` (despertar hilos y llamar al replanificador `0x02007DE8`) |
| ARM7 simulado | `port/recomp/arm7.c` | Cola de mensajes PXI (registros `0x04000184/188` y `0x04100000`, interrupción 18). Contesta "bien" a las órdenes de la memoria de guardado (servicio 11). El resto solo se apunta |
| DMA | `port/recomp/io.c` | Copias de memoria por hardware, hechas al instante |
| Trazas | `recomp.py --trace`, variable `NFS_TRACE=0x...,0x...` | Imprime argumentos y resultado de las funciones elegidas |
| Volcado | variable `NFS_DUMP=1` | Al parar, guarda la memoria principal en `ram_dump.bin` (no compartir) |
| Límite | variable `NFS_FRAMES=n` | Para tras n fotogramas (por defecto 600) |

**Fallos del traductor encontrados y corregidos** (los tres rompían la descompresión de zlib, que devolvía 0 bytes):

1. **`bl` usado como salto largo.** El compilador original (Thumb) usa `bl` para saltar lejos dentro de la misma
   función. Se traducía como llamada. Ahora, si el destino está dentro de la propia función, es un salto.
2. **Tablas de `switch` que Ghidra no resolvió.** El cuerpo de la función quedaba cortado (a `inflate` le faltaba el
   80 %). `discover.py` reconoce ahora el patrón Thumb (`add rX,pc / ldrh / lsls / asrs / add pc,rX`) y el ARM
   (`addls pc,pc,rX,lsl #2`), lee la tabla y añade el código que faltaba: 11 funciones completadas.
3. **Retornos locales.** En funciones con `bl` internos, un `bx lr` puede volver a una dirección de la misma
   función. Esos retornos pasan ahora por el despachador de la función.

Lección: el bucle "falta una función → la creo" (`missing_calls.txt`) es peligroso cuando la dirección que falta es
en realidad una etiqueta **dentro** de una función: crea una función falsa y el programa sigue con un
comportamiento equivocado en vez de pararse. Hay que mirar cada dirección nueva que caiga dentro de otra función.

Otros datos (VERIFICADO): `FSFile` se copia entero (minizip clona su manejador), así que el archivo del PC se
identifica con un número guardado en `+0x30`. El servicio PXI 11 lo usa la librería CARD: orden 0 + puntero a la
estructura de órdenes (`0x02110FA0`), luego órdenes 2 y 6.

## Hito 4, parte 2: servicios del ARM7 y lógica del juego — EN CURSO (2026-10-14)

**Hasta dónde llega (VERIFICADO, salida real, 1500 fotogramas sin fallos):** el juego carga sus scripts de
configuración Lua (`ui`, `vehicles`, `carthemes`, `progression`, `locked`, `songlist`, `defaulttag`), los textos en
inglés (`strings/Main/.../us.str`), el icono de "cargando", abre el archivo de sonido y **carga la pantalla de
licencia de Nintendo** (`graphics/2dmaps/common/splash/screen/lbnintendo.*`). Sobre el fotograma 300 pide el vídeo
del editor (`movies/publisher.mods`); como se le dice que no existe, se queda ahí esperando.

Servicios del ARM7 (`port/recomp/arm7.c`):
- Pantalla táctil (6): contesta "sin tocar". Protocolo SPI del SDK (HIPÓTESIS que funciona: el juego sigue).
- Memoria de guardado (11, librería CARD): EEPROM de 8 KB real, en `nfs_nitro.sav` junto al ejecutable.
  Órdenes 6 leer / 7 escribir / 8 programar.
- Sonido (7): el juego manda mensajes; no se contesta ni suena nada. De momento no bloquea.

Sustitutos nuevos (`hle.c`): `FS_ConvertPathToFileID` `0x0200D884`, `FS_OpenFileFast` `0x0200D90C`,
`FSi_ReadFileCore` `0x0200D808` (lecturas asíncronas), y un aviso de cada archivo que pide el juego
(`FileSys_OpenFile` `0x02085670`). `threads.c`: `setjmp` `0x0202A2A4` y `longjmp` `0x0202A2B0` del propio juego
(Lua los usa para sus errores) sobre `setjmp`/`longjmp` del PC; `luaG_errormsg` `0x02034EE0` imprime los errores de Lua.

### Prueba instrucción a instrucción contra Unicorn — VERIFICADO

`tools/recomp/fuzz.py` (+ `port/recomp/fuzz_main.c`): coge las instrucciones distintas del juego que no son saltos
(46.316 ARM, de las que se prueban 20.000 al azar, y las 10.816 Thumb), las ejecuta con registros y memoria
aleatorios en Unicorn y en el C traducido, y compara registros, banderas y memoria.

Resultado: **134.539 casos, 73 diferencias**. Las 73 son de "instrucciones" que no existen en este procesador
(`movw`/`movt` de ARMv7) o accesos desalineados: datos que el descubridor tomó por código. Ninguna diferencia en
instrucciones reales. Conclusión: los fallos que quedan están en el control de flujo o en la capa de ejecución,
no en la aritmética ni en los accesos a memoria.

### Más fallos del traductor corregidos (todos de control de flujo)

4. **Retornos.** Antes se comparaba el destino con el `lr` de entrada y, si no coincidía, se "llamaba" al destino.
   Cuando una función seguía en otra (cola compartida), ese `lr` ya no valía: cada retorno creaba una llamada
   anidada de más y la pila del PC se agotaba. Ahora un retorno es un retorno; solo se trata como salto si el
   destino es el inicio de una función (los *thunks* de C++ hacen `push destino; pop {pc}`).
5. **Funciones partidas por Ghidra.** El bucle principal del intérprete Lua estaba dividido en dos "funciones" que
   se saltaban la una a la otra. `discover.py` absorbe ahora en cada función el código cercano al que salta o en
   el que continúa (819 funciones completadas; se duplica código, que es inofensivo).
6. `bx rX` con X distinto de `lr` es un salto indirecto, salvo el retorno antiguo `pop {rX}; bx rX`.

Herramienta nueva: el programa captura sus propios fallos de Windows (acceso inválido, pila agotada) y dice dónde.

### Bloqueos actuales

- **Vídeo Mobiclip.** Con `NFS_VIDEO=1` el juego abre el vídeo y el descodificador (overlay 1, `0x02176884`...) acaba
  usando punteros basura: fallo sin diagnosticar (probablemente código en ensamblador con saltos poco habituales).
  Sin esa variable se hace como si el vídeo no existiera y el juego se queda esperando tras la pantalla de Nintendo.
  Hay que arreglar el descodificador o hacer que el juego dé el vídeo por terminado.

## Hito 4, parte 3: intérprete de reserva y ventana 2D — VERIFICADO (2026-10-14)

**El juego se ve.** Capturas reales del programa (en `C:\Users\jj\nfs-nitro-build\game\shot_*.png`; llevan
gráficos del juego, no van en el proyecto): "Licensed by Nintendo", logotipos de EA y Mobiclip, y la pantalla
**"Profile Name"** con el coche, el teclado táctil y las banderas de idioma. Corre 1500 fotogramas sin fallos.

Para jugar: `jugar.cmd` (en la raíz del proyecto) o `nfs_boot.exe C:\nfs` desde la carpeta de compilación.
Teclas: flechas = cruceta, Z = A, X = B, A = Y, S = X, Q = L, W = R, Intro = Start, Retroceso = Select; ratón con
botón izquierdo sobre la pantalla de abajo = lápiz.

| Pieza | Archivo | Qué hace |
|---|---|---|
| Intérprete de reserva | `port/recomp/interp.c` | Ejecuta instrucción a instrucción el código sin traducir cuando una llamada indirecta va a él; vuelve al C traducido al retornar. Apunta la dirección en `missing_calls.txt` para traducirla en la siguiente compilación. El juego ya no se para por una función que falte |
| Dibujado 2D | `port/recomp/video.c` | Los dos motores 2D: fondos de teselas (4/8 bits), afines y extendidos (teselas, mapas de bits de 8 bits y de color directo), paletas extendidas de fondo, sprites normales, prioridades, brillo maestro. Capturas PNG |
| Ventana | `port/recomp/win.c` | Ventana Win32 (sin librerías), 60 fotogramas por segundo, teclado y ratón |
| Salto de vídeos | `hle.c`, `hle_ov1_0212c3c4` | La lista de pantallas de presentación (overlay 1; entradas de 16 bytes, `+0xC` = "es vídeo") se recorre con las funciones del juego; las entradas de vídeo se saltan con `0x0212C4E0` |

Comprobaciones:
- El intérprete pasa la misma prueba contra Unicorn que el traductor (136.036 casos; mismas diferencias, todas en
  instrucciones falsas, más 1 caso suelto `add fp, sp, #0x2c` en el que Unicorn devuelve todo ceros: parece fallo
  del comparador, sin aclarar).
- Con el intérprete, una sola ejecución sin recompilar llega al mismo punto (13 trozos interpretados).

Variables nuevas: `NFS_HEADLESS=1` (sin ventana), `NFS_SHOT_EVERY=n` (captura cada n fotogramas),
`NFS_SHOT=archivo.png` (captura ampliada x3 al parar, con el estado de los motores 2D), `NFS_SCALE=n`,
`NFS_STACK=1` (pila de llamadas del juego al pedir un vídeo), `NFS_VIDEO=1` (no saltar vídeos).

Sin hacer en el dibujado 2D: ventanas, mezcla alfa, mosaico, sprites rotados o de mapa de bits, paletas extendidas
de sprites, reparto real de bancos de VRAM (se usa un bloque plano por dirección). El 3D no existe todavía: el
fondo BG0 del motor A (salida del 3D) se deja transparente.

Sin comprobar: la pantalla táctil (formato de la muestra y calibración son hipótesis; hay que probar a escribir
el nombre con el ratón).

## Hito 4, parte 4: 3D — VERIFICADO (2026-10-14). El juego es jugable

**La carrera de tutorial se ve y se conduce.** Capturas reales (`shot_*.png` en la carpeta de compilación): pista,
cielo, edificios y el coche con texturas; HUD "CONTROLS / Press SELECT to continue"; lecciones en la pantalla de
abajo. Con el guion de entrada se pasa el nombre, se avanza con SELECT, se acelera y se gira, y el juego llega a
la lección "DRIFTING". 932 polígonos por fotograma. Sin fallos en 6500 fotogramas.

| Pieza | Archivo | Qué hace |
|---|---|---|
| Chip 3D | `port/recomp/gx.c` | Órdenes de geometría (cola empaquetada `0x04000400` y puertos directos), pila de matrices, recorte, luces por vértice, generación de coordenadas de textura, los 7 formatos de textura, y un dibujado por software con corrección de perspectiva por píxel y búfer Z/W |
| Resolución interna | `NFS_3D_SCALE=n` | El 3D se dibuja a n veces 256x192 y el 2D se amplía encima. Es ya una mejora gráfica real. `jugar.cmd` usa 2 |
| Temporizadores | `io.c` (`io_advance`) | Los 4 temporizadores de la consola, con sus avisos. Sin ellos `OS_Sleep` no volvía |
| Avisos de DMA y de la cola 3D | `io.c` | El envío asíncrono de listas 3D del SDK espera esos avisos |
| Interrupciones inmediatas | `irq.c` (`irq_request_soon`) | Una interrupción recién pedida se entrega en el siguiente punto seguro, no al acabar el tramo de tiempo. Antes el juego iba a ~13 fotogramas por segundo esperando en bucles; ahora va a su ritmo |
| Guion de entrada | `win.c`, `NFS_INPUT` | `touch`, `release`, `key`, `hold`, `unhold`: permite recorrer el juego en pruebas automáticas |
| Sonido mínimo | `arm7.c` | Lleva el contador de listas de órdenes de sonido terminadas (no suena nada), para que el juego no espere |
| Comprobación de registros | `recomp.py --abi-check`, `NFS_ABICHECK=1` | Compilación de depuración: cada función comprueba al volver que conserva r4-r11 y sp. Señaló dos fallos en minutos |

Velocidad medida (sin ventana, 4000 fotogramas, incluida la carrera): escala 1 → 10 s; escala 2 → 29 s
(~10 ms por fotograma 3D: va sobrado para 60 por segundo); escala 3 → 69 s (~25 ms: se queda corto); escala 4 → 103 s.
El dibujado es de un solo hilo y sin optimizar.

### Fallos corregidos en esta parte

7. **Entrada de funciones con código anterior a su inicio.** Cuando una función tenía código en direcciones
   menores que su punto de entrada (por haber absorbido trozos), el C empezaba por ese código en vez de por la
   entrada. Afectaba a las rutinas de coma flotante (suma y resta de `double` comparten código) y a muchas más.
8. **Bloques de salida marcados como funciones.** Ghidra marcó como función el bloque de error de una función
   grande; un `bl` usado como salto largo hacia él se traducía como llamada. El descubridor reconoce ahora los
   fragmentos (deshacen una pila que no han preparado) y los une a su función.
9. **Tablas de datos tomadas por funciones.** El descubridor rechaza ahora los candidatos que no acaban en un
   retorno o empiezan por palabras pequeñas (pasó de 686 a 3225 candidatos descartados).
10. **Botones.** El valor compartido de X/Y (`0x027FFFA8`: bit 10 = X, bit 11 = Y) estaba mal escrito y hacía que
    A, B, Select, Start y media cruceta parecieran siempre sin pulsar.
11. `bx rX` con `lr` cargado a mano justo antes (apuntando a la instrucción siguiente) es una llamada.

Datos nuevos (VERIFICADO): la pantalla táctil funciona con muestras de 16 unidades por píxel y la calibración
puesta en los ajustes de usuario (`0x027FFC80`, `+0x58`); el apodo de la consola aparece como nombre de perfil
por defecto. Tras el nombre, el juego entra directamente en la carrera de tutorial (pista `tut`).

Sin hacer en el 3D: niebla, modo *highlight* (se trata como normal), sombras, antialiasing, orden de los
translúcidos, prueba de caja real (siempre contesta "visible", así que no se descarta nada fuera de pantalla),
captura de pantalla para 3D en las dos pantallas. El dibujado 2D sigue sin mezcla alfa ni sprites rotados.

## Estado tras las pruebas del usuario (2026-10-14)

El usuario completó el tutorial y jugó varias carreras: "va súper bien y fluido". Su partida guardada se recarga
(VERIFICADO: al arrancar con su `nfs_nitro.sav` sale el menú principal en español con su perfil). Copia de
seguridad en `C:\Users\jj\nfs-nitro-build\saves\`. **Ojo: las pruebas no deben borrar `game\nfs_nitro.sav`;**
las pruebas automáticas se hacen en `game\flick\` con una copia.

Fallo que vio el usuario y arreglo (VERIFICADO con capturas fotograma a fotograma): al cargar una carrera, la
escena se veía entera durante 5 fotogramas entre la pantalla de carga y el fundido desde blanco. El juego la tapa
con el efecto de brillo por capas (`BLDCNT` `0x50` modos 2 y 3, `BLDY` `0x54`), que no estaba hecho. Añadido en
`video.c` (`fade`, `layer_fx`). La mezcla alfa entre capas (modo 1) sigue sin hacer.

Nuevas variables: `NFS_SHOT_FROM` / `NFS_SHOT_TO` (tramo de fotogramas a capturar con `NFS_SHOT_EVERY`).

## Fase 6 (mejoras gráficas), primera parte — VERIFICADO (2026-10-14)

Decisión del usuario: gráficos mejorados ahora; **el sonido va al final, pero es importante para él** (no es
opcional, solo se pospone).

Hecho:
- **Resolución interna alta por defecto.** La ventana elige sola disposición y tamaño según el monitor
  (`win.c`, `configure`). En el PC del usuario: disposición 2 (pantalla del 3D grande, táctil a media escala al
  lado), pantalla principal x4, 3D a **1024x768**.
- **Filtrado bilineal de texturas** (`gx.c`), con los colores pesados por el alfa para no ensuciar los bordes de las
  texturas recortadas. Se quita con `NFS_FILTER=0` o con F2 en marcha.
- **Suavizado de bordes por supermuestreo** (`NFS_AA=n`): el 3D se dibuja a n veces el tamaño mostrado y se reduce
  promediando. Por defecto solo se activa en ventanas pequeñas (escala ≤ 2).
- **Disposiciones de pantalla** (F1 o `NFS_LAYOUT`): 0 vertical, 1 lado a lado, 2 principal grande + táctil pequeña.
- **Rendimiento del dibujado** (medido en la ventana real, en carrera, 3D a 1024x768 con filtro):

  | | 3D | capas 2D | ventana | total por fotograma |
  |---|---|---|---|---|
  | antes | ~13 ms | ~4 ms | ~2 ms | 19,5 ms (por encima de los 16,7 que hacen falta) |
  | ahora | 5,0 ms | 1,7 ms | 2,1 ms | **10,5 ms de media, 13,6 el peor** |

  Qué lo consiguió: (1) las texturas se descodifican una sola vez y se guardan hasta que el juego escribe en la
  memoria de texturas (antes se descodificaban todas en cada fotograma: era casi la mitad del tiempo);
  (2) dibujado repartido por franjas entre los hilos del PC, con cola de franjas; (3) funciones de borde
  incrementales y filtrado en coma fija; (4) las capas 2D se juntan a tamaño original y se amplían una sola vez;
  (5) `-O3 -ffast-math -mavx2` para `gx.c`, `video.c` y `win.c`.
- `NFS_PERF=1` imprime cada 300 fotogramas el tiempo medio y el peor, con el reparto 3D / capas 2D / ventana.

Pendiente de la fase 6: niebla, modo *highlight* (brillo de cristales y ruedas), sombras; texturas sustitutas de
mayor tamaño; modelos con más polígonos; iluminación por píxel. Y del 2D: mezcla alfa y sprites rotados.

## Fase 6, segunda parte: luz por píxel, subdivisión y texturas con IA — VERIFICADO (2026-10-14)

Decisión del usuario: luz por píxel, modelos con más polígonos y texturas más grandes con IA (Real-ESRGAN).

**Luz por píxel** (`gx.c`, F3, `NFS_PIXEL_LIGHT=0` para quitarla). Los vértices iluminados guardan su normal
(espacio de cámara); cada polígono guarda el estado de luces y material vigente (`LightState`); en cada píxel se
interpola la normal y se evalúa la misma fórmula de la consola (difusa, ambiente, especular con su tabla).

**Subdivisión** (`gx.c`, F4, `NFS_TESS=0/1/2`, por defecto 1; `NFS_TESS_ROUND` = fuerza, por defecto 60 %).
Teselado de Phong sobre los polígonos iluminados (coches, ruedas, objetos). Los polígonos se apuntan durante el
fotograma y se montan al final (`finish_polygons`) para poder:
- usar una normal por **posición** (media de todas las que llegan a ese punto): el empuje es el mismo en todos los
  polígonos que comparten el vértice, así **no se abren grietas** (la primera versión las abría; VERIFICADO con
  capturas comparadas);
- no empujar en las **aristas vivas** (las normales que llegan al punto discrepan): el coche no se deforma.

**Texturas con IA** (Real-ESRGAN `realesrgan-x4plus`, portátil en `C:\Users\jj\nfs-nitro-tools\realesrgan`,
descargado con permiso del usuario; funciona con la gráfica integrada por Vulkan: 117 texturas en 16 s).
- Cada textura se identifica por una huella (FNV-1a de 64 bits) de su contenido descodificado.
- `capturar_texturas.cmd` (o `NFS_TEXDUMP=1`): el juego vuelca cada textura nueva a
  `C:\Users\jj\nfs-nitro-textures\original\<huella>_<ancho>x<alto>.png`.
- `ampliar_texturas.cmd` → `tools/textures/upscale.py`: rellena el color de los texeles transparentes, añade un
  margen copiado del lado opuesto (para que no haya costuras en las que se repiten), amplía x4, recorta, amplía la
  transparencia aparte y guarda el PNG en `x4\` (retocable a mano) y el binario en `pack\` (`NTX1`, ancho, alto,
  píxeles BGRA).
- El juego carga `pack\<huella>.tex` en lugar de la original (`NFS_TEXPACK=0` para no usarlas).
- Hoy hay 117 ampliadas: tutorial y Dubái. **Las demás pistas y coches salen con la textura original hasta que
  se capturen y amplíen.**
- **Mipmaps** para todas las texturas (originales y ampliadas): de lejos no parpadean. El nivel se elige por
  píxel con las derivadas de las coordenadas de textura.

**Rendimiento.** Todo el dibujado (3D por franjas, ampliación de capas 2D, copia a la ventana) va ahora en un
grupo fijo de hilos (`pool.c`). Además: prepasada de profundidad (cada píxel opaco se pinta una vez), descarte
rápido de polígonos enteros dentro o fuera de la vista, y ampliación final con filtro bilineal.
Medido en la carrera de Dubái (la más pesada probada), ventana 1536x768, todo activado:
- 3D a x4 (1024x768): 14,6–16 ms de media, picos de 21–35 ms → justo;
- 3D a x3 (768x576) ampliado con filtro: **12,3 ms de media** → elegido por defecto para ventanas grandes.
Quedan picos aislados de 24–35 ms sin explicar (no son las texturas: se renuevan 1 vez cada 300 fotogramas).

Conclusión para el futuro: el dibujado por software llega a su límite en torno a x3–x4. Para más resolución,
suavizado de bordes real y efectos más caros, el siguiente paso sería dibujar el 3D con la tarjeta gráfica
(OpenGL o Vulkan).

## Qué falta (hito 4 en adelante)

- **Servicios del ARM7**: lo siguiente es la pantalla táctil (servicio 6, el juego espera su respuesta). Después
  vendrán sonido (7; ya manda un mensaje y nadie contesta), encendido (8), reloj (5) y la memoria de guardado de
  verdad (ahora se contesta "bien" sin leer ni escribir nada).
- **Temporizadores**: no hay (solo VBlank). El reloj interno del SDK (`OS_GetTick`, alarmas) los usa.
- El tiempo es aproximado: no depende de instrucciones ejecutadas sino de saltos hacia atrás.
- **Gráficos**: las escrituras a los registros de vídeo se guardan y nada más.
- El descubridor mete también datos que parecen código: 550 instrucciones "sin traducir" son casi todas de esas
  funciones falsas. No molestan si nadie las llama.

## Limitaciones conocidas del traductor

- Que una función se traduzca no significa que esté bien traducida: solo está **ejecutado** el camino de arranque
  (hito 3) y las 4 funciones del hito 2. Falta una prueba instrucción a instrucción contra un emulador de referencia.
- Overlays: la tabla de funciones ya elige el overlay cargado, pero no se ha ejecutado todavía ninguno.
- Código que Ghidra no ha marcado como función no se traduce (hará falta un intérprete de reserva o más análisis).
- Sin hacer: interrupciones, hilos del sistema, BIOS (`swi`), registros de hardware (`io_read`/`io_write` solo avisan).
- Rendimiento del compilador: el archivo único de 60 MB **compila sin errores** (VERIFICADO, `clang -O1`), pero
  tarda 633 segundos y da un objeto de 45 MB. Hay que partirlo en trozos y compilar en paralelo.

## Fase 6.2: niebla, brillo, sombras y efectos 2D — VERIFICADO (2026-10-14)

Primero se midio que pide el juego de verdad (`NFS_FX_LOG=1`, o `=2` para el 2D en cada captura), en una carrera
real de Dubai (pista `ro2`) arrancada desde la partida guardada del usuario:

| Efecto | Que pide el juego (VERIFICADO) |
|---|---|
| Niebla | Encendida en carrera: `DISP3DCNT` = `0x039B` (niebla, desplazamiento de tabla 3), color `0x477F` (melocoton, alfa 0), `FOG_OFFSET` = 300, tabla lineal 0..124. En el menu, apagada. 1.300-5.000 poligonos por fotograma la llevan |
| Brillo (*highlight*) | `DISP3DCNT` bit 1 = 1; 1.000-2.800 poligonos en modo 2 por fotograma (la carroceria del coche, entre otros); tabla de tonos con datos |
| Sombras | 148 poligonos de volumen de sombra por fotograma (mascara con id 0 + sombra con id distinto) |
| 2D en carrera | `BLDCNT` `0x00BF` (brillo con `BLDY` 0, sin segundos objetivos: en la practica nada); 25-29 sprites semitransparentes arriba, 8-12 abajo; 1 sprite rotado arriba y 3-4 abajo; ventana 0 activa en la pantalla de abajo (`WININ` `0x3F`, `WINOUT` `0x37`: fuera de ella no se ve BG3) |
| 2D en menus | `BLDCNT` `0x00FF` con `BLDY` 1 (oscurecer un poco); 63-70 sprites semitransparentes |
| No usados | Ventana de sprites, mosaico, sprites de mapa de bits |

Hecho (`gx.c`, `video.c`, `win.c`):
- **Niebla** como la consola: bufer de atributos por pixel (`abuf`: id del poligono opaco, bandera de niebla,
  marca de sombra); al final de cada franja se pasa la profundidad a 15 bits, se busca en la tabla con
  interpolacion y se mezcla color y alfa hacia la niebla. Al ir por pixel a la resolucion interna, sale suave.
  F5 o `NFS_FOG=0` la quitan. HIPOTESIS: con bufer W, la profundidad de 15 bits es W (coma fija 20.12) / 512, es
  decir `w * 8`; con eso la niebla empieza a ~37 unidades y es total a ~550. Se ve razonable (el cielo y las dunas
  lejanas toman el tono calido), pero falta compararlo con capturas del juego real.
- **Modo brillo/toon**: el rojo del color del vertice (o de la luz por pixel) elige un tono de la tabla
  `0x04000380`; en modo brillo se suma despues de la textura, en modo toon sustituye al color.
  Resultado: la carroceria tiene el brillo claro de la consola (antes salia azul plano y oscuro).
- **Sombras por volumen**: los poligonos de sombra van con los translucidos; la mascara marca los pixeles donde
  falla la prueba de profundidad; la sombra pinta solo en pixeles marcados cuyo id opaco sea distinto, y borra la
  marca. Se ve una franja oscura bajo el coche (con la camara detras queda casi tapada). F6 o `NFS_SHADOWS=0`.
- **Alfa del 3D por pixel**: los translucidos guardan su alfa (antes todo era opaco) y el compositor lo mezcla con
  la capa 2D de detras si es segundo objetivo de `BLDCNT`, como la consola.
- **Mezcla 2D de verdad**: por pixel se guarda la capa de debajo y su color sin mezclar; modo 1 (alfa con
  `BLDALPHA`), sprites semitransparentes (siempre se mezclan si hay segundo objetivo), modos 2/3 de brillo. Si lo
  de debajo es el 3D, la mezcla se hace al ampliar, con el pixel 3D a alta resolucion.
- **Sprites rotados/escalados** (y de doble tamano): la barra de la pantalla de abajo salia descolocada encima
  del marcador; ahora sale en su recuadro.
- **Ventanas 0 y 1** (capas visibles dentro/fuera y si hay efectos).

Coste medido (sin ventana, misma carrera de 4.800 fotogramas, 3D x3, dos pasadas): 17,1-18,3 s antes,
18,4-19,7 s ahora: menos de medio milisegundo por fotograma de media.

Copia del ejecutable anterior: `C:\Users\jj\nfs-nitro-build\nfs_boot_antes_6.2.exe`.
Pruebas: `C:\Users\jj\nfs-nitro-build\game\t62\` (capturas, comparaciones `cmp_*.png`, `diff_hl.png`).

Sin hacer: orden por Y de los translucidos, marcado de bordes y antialiasing de la consola (el juego apaga los
bordes; el suavizado ya lo da la resolucion alta), ventana de sprites, mosaico.

## Menú de opciones al arrancar (2026-10-14) — VERIFICADO

Petición del usuario: elegir las mejoras en un menú antes de jugar, en vez de con las teclas F.

- `port/recomp/menu.c`: ventana de Windows (controles con el aspecto actual del sistema) que sale antes de que
  arranque el juego. Desplegables: disposición de pantallas, tamaño de ventana, resolución del 3D, suavizado,
  subdivisión. Casillas: filtro, texturas con IA, luz por píxel, niebla, sombras, guardar texturas nuevas.
  Botones: Valores recomendados, Salir, Jugar (Intro = Jugar, Esc = Salir).
- Lo elegido se guarda en `opciones.ini` (carpeta del juego, junto a la partida) y se convierte en las mismas
  variables `NFS_*` de siempre, así que `win.c` y `gx.c` no cambian. Una variable puesta desde fuera manda sobre
  el archivo. "Guardar texturas nuevas" no se guarda (vale para una partida; lo marca `capturar_texturas.cmd`).
- No sale con `NFS_HEADLESS` (pruebas automáticas) ni con `NFS_NOMENU=1`. Las teclas F1-F6 siguen funcionando.
- Comprobado: captura de la ventana del menú; con `opciones.ini` de prueba (pantallas una encima de otra, 3D x2)
  y pulsando Jugar, el juego arranca con "disposicion 0, resolucion interna x2" y el archivo se reescribe entero.

## Tirones, sombras y perfiles de calidad (2026-10-14) — VERIFICADO

Lo pidió el usuario tras probar: tirones leves al pasar por los círculos amarillos y al salir un policía; las
sombras bajo el coche no se veían bien; perfiles "original" y "máxima" en el menú.

**Tirones.** Nueva herramienta `NFS_SPIKES=ms`: apunta cada fotograma que tarda más de esos milisegundos, con el
tiempo del 3D, polígonos y texturas descodificadas o buscadas en disco. Lo que se vio (VERIFICADO):
- En plena carrera el juego carga archivos al vuelo: partículas al chocar (`sawhorse`, `hydrant`, `cart`), el
  efecto de pintura (`paintfx2`) y los sprites de los poderes (`powups`). Al subir sus texturas a la VRAM, el
  dibujado **tiraba todas las texturas** y las volvía a descodificar (~80 texturas, 5-7 ms).
- Una textura animada (se reescribe cada fotograma) provocaba eso **en cada fotograma** (3 ms) y además buscaba en
  disco su versión ampliada en cada fotograma (12 búsquedas). Cada huella nueva ocupaba además un hueco de la
  tabla de sustitutas, que con el tiempo se llenaba (cada búsqueda habría recorrido la tabla entera).
Arreglo (`gx.c`, `io.c`):
- La VRAM de texturas y paletas se vigila por páginas de 4 KB (`gx_vram_written`): cada textura descodificada
  sabe qué memoria lee (texeles, bloques 4x4, paleta) y solo se vuelve a descodificar si se escribió ahí.
- Al empezar se hace la lista de `pack\*.tex` (731 hoy): una textura sin sustituta ya no toca el disco.
- Las sustitutas se cargan en un hilo aparte la primera vez que hacen falta; mientras, se ve la original (un
  fotograma o dos).
Resultado en la misma carrera (con las mismas cargas de partículas y pintura): de decenas de fotogramas lentos a
**ninguno** por encima de 13 ms (sin ventana), salvo los de las capturas de la propia prueba.
HIPÓTESIS: el policía es el mismo caso (modelo y texturas nuevos que se cargan en marcha); no se ha podido
provocar con el guion de entrada.

**Sombras** (había dos fallos):
1. La sombra borraba la marca de la máscara al primer intento aunque esa cara estuviera bajo el suelo, así que la
   cara que tenía que pintar ya no la encontraba. El juego dibuja la sombra con las dos caras del volumen
   (máscara: id 0, caras traseras; sombra: id 2, las dos caras, alfa 14, negro). Ahora, como la consola, la marca
   no se borra y lo que evita oscurecer dos veces es la regla de la consola: un translúcido no pinta un píxel que ya
   pintó otro translúcido con el mismo identificador (`tbuf`).
2. Los píxeles opacos de polígonos "translúcidos" (texturas A3I5/A5I3, como las llantas) no guardaban ni su
   profundidad ni su identificador: la sombra (y la niebla) los atravesaba y oscurecía las ruedas. Ahora, como en la
   consola, un píxel opaco cuenta como opaco aunque venga de un polígono translúcido.
Cómo es la sombra del juego (VERIFICADO con un volcado de los polígonos y un dibujo de depuración): la silueta del
coche extruida hacia abajo; con la cámara detrás casi toda queda bajo el coche y se ve como una franja oscura en el
suelo junto a los costados (más al girar). La carrocería (id 2) no se sombrea a sí misma.
Depuración: `NFS_SHADOW_LOG=fotograma` lista los polígonos de sombra de ese fotograma.

**Perfiles en el menú** (`menu.c`): desplegable "Calidad gráfica" con Original (3D x1, sin filtro, sin texturas IA,
luz por vértice, sin subdivisión; niebla y sombras sí, porque la consola las tiene), Recomendada, Máxima (3D x5,
suavizado x2, subdivisión 2, todo encendido) y Personalizada (sale sola al tocar cualquier ajuste). No cambia la
disposición ni el tamaño de la ventana. Se quitó el botón "Valores recomendados".
Coste medido sin ventana en carrera: Recomendada 8,7 ms por fotograma (3D 6,2); Máxima 19,9 ms (3D 17,3, picos de
41): no llega a 60 por segundo con el dibujado por software.

## Texturas de coches con otro modelo de IA (2026-10-14) — VERIFICADO

Petición del usuario: que los coches se vean mejor (más calidad en sus texturas).
- El juego apunta las texturas dibujadas sobre polígonos iluminados en `<carpeta de texturas>\coches.txt`
  (`note_lit_texture` en `gx.c`; una vez cada huella, también entre partidas; `NFS_TEXLIST=0` lo apaga). En una
  carrera salieron 16, todas de coches: carrocerías de cada color, ruedas, hojas de logos y faros de Ford, Nissan
  Cube y Hummer, y el mapa de reflejo.
- Comparados a ojo en esas texturas: `realesrgan-x4plus-anime` deja logos, letras y bordes más limpios y la pintura
  con menos ruido que `realesrgan-x4plus`; `realesr-animevideov3` sale más blando. `upscale.py` usa ahora el
  "anime" para las de `coches.txt` (`--car-model`), rehace las que estaban con el general (copia en `x4_anterior\`)
  y apunta las hechas en `hechas_coches.txt`. Hechas: 14 (2 vistas aún sin capturar).
- En el juego (dos ejecuciones iguales salvo las texturas del coche, con `NFS_TEXTURES`) la mejora es pequeña:
  llantas, tirador y logo algo más nítidos. El coche ocupa unos 300 px de ancho con el 3D a x3. Lo que más puede
  ganar: retoque a mano de las hojas de detalles en Blender (`GUIA_TEXTURAS_BLENDER.md`, apartado 4 bis) y más
  resolución 3D.
- Ojo: el coche de la carrera de prueba cambia entre ejecuciones (Escort verde, Focus azul…). Las comparaciones de
  coche hay que hacerlas cambiando solo la carpeta de texturas (`NFS_TEXTURES`).

## Reflejos y brillo de los coches (2026-10-14) — VERIFICADO

Petición del usuario: "shaders y reflejos" mejorables.

Lo que hace el juego de verdad (VERIFICADO con `NFS_FX_LOG`): **la pintura no lleva reflejo**. Solo los cristales
usan coordenadas de textura generadas a partir de la normal (100-155 polígonos por fotograma, todos con la textura
`window` de 64x64, una esfera oscura). `tex_rgb` (que en notas anteriores parecía un mapa de reflejo) es en realidad
el atlas de la carrocería (paneles desplegados, coloreados por la paleta de la pintura). La carrocería, las llantas y
los cristales van con luz y en modo "brillo" (`polyAttr` modo 2).

Hecho (`gx.c`, F7, `NFS_SHINY=0`, casilla del menú; "Original" lo apaga):
- **Reflejo por píxel** en los polígonos con coordenadas generadas (cristales): la consola las calcula en los
  vértices y las estira por el triángulo; ahora se calculan en cada píxel con la normal del modelo interpolada
  (`Vtx.mx/my/mz`, `Poly.envp`), con la misma fórmula. Va con la luz por píxel (F3).
- **Barniz de los coches** (mejora del port, la consola no lo tiene), en materiales con luz y modo brillo o con
  reflejo: reflejo de un cielo sencillo (horizonte = color de la niebla o del fondo de la pista, más azul hacia
  arriba, suelo oscuro) con Fresnel (0,08 de frente, hasta 0,58 de canto; 60 % en cristales), y brillo especular
  Blinn-Phong con la dirección real de la cámara (nítido h^96 y ancho h^16).
- HIPÓTESIS: se supone la cámara casi horizontal para saber qué es "arriba" en el reflejo (vale para la cámara de
  persecución).
- Coste: inapreciable (3D 6,0-6,2 ms con y sin, en la misma carrera).
- Resultado (capturas comparadas): la pintura deja de verse mate (reflejo en techo y costados, brillos en las
  esquinas) y los cristales pasan de negro plano a un reflejo con destello del sol.

### Segunda vuelta: reflejo de la escena y resplandor (2026-10-14) — VERIFICADO

El usuario no notaba apenas el cambio. Ahora:
- **La pintura refleja la propia escena**: con la dirección reflejada se toma lo que se vio en esa dirección en el
  fotograma anterior (las imágenes 3D se alternan: `fb_bufs`, `draw_state.prev`), cuatro muestras para suavizar.
  Si el reflejo apunta hacia la cámara (lo de detrás no está en pantalla), se queda el cielo sencillo. Así la
  carrocería refleja carretera, vallas y cielo, y el reflejo se mueve al conducir. Es la técnica habitual de
  "reflejo en pantalla", aproximada: no traza rayos.
- Más fuerza: Fresnel 0,15 de frente hasta 0,85 de canto; brillo especular x1,3 (nítido) y 0,18 (ancho).
- **Resplandor (bloom)**, F8, `NFS_BLOOM=0`, casilla del menú: la imagen 3D se reduce a 1/4 quedándose con lo que
  pasa de luminancia 0,84, se difumina (gaussiano de radio 6, separable) y se suma al 70 %. Las tres fases van
  repartidas por filas entre los hilos. Con umbral 0,72 las vallas blancas se quemaban: se subió.
- Coste medido en la misma carrera (3D x3): 6,06 ms sin efectos, 6,96 ms con reflejos y resplandor.

## Fase 6.4: el 3D con la tarjeta gráfica (OpenGL) — VERIFICADO (2026-10-14)

Decisión del usuario: dibujar el 3D con la gráfica. El portátil tiene dos: AMD Radeon 680M (integrada) y NVIDIA
GeForce RTX 4050 (VERIFICADO con un programa de prueba; las dos dan OpenGL 4.6). El ejecutable exporta
`NvOptimusEnablement` y `AmdPowerXpressRequestHighPerformance` para que Windows use la NVIDIA (VERIFICADO: el
registro dice "NVIDIA GeForce RTX 4050 Laptop GPU").

Diseño (`port/recomp/gpu.inc.c`, se incluye al final de `gx.c`):
- Todo lo de antes sigue igual en el procesador (órdenes del juego, matrices, recorte, luz por vértice,
  subdivisión, modelos sustitutos, texturas descodificadas y ampliadas). Solo el **dibujado de píxeles** pasa a
  la gráfica: la lista de polígonos del fotograma se sube en un solo búfer y se dibuja en tandas (polígonos
  seguidos con el mismo estado), con sombreadores que repiten las reglas del dibujado por software: texturas
  (premultiplicadas, con reducciones, filtrado anisotrópico x8), modos modulate/decal/toon/brillo, luz por
  píxel con la fórmula de la consola, reflejo por píxel, barniz de los coches (con el reflejo de la escena
  tomado de la imagen anterior), niebla por fragmento, prueba de alfa, recortes, translúcidos (sus píxeles
  opacos primero, como opacos), sombras por volumen con el búfer de plantilla (bits 0-5 = identificador del
  opaco, bit 7 = marca de la máscara), profundidad W o Z escrita por el sombreador (búfer de 32 bits).
- Suavizado de bordes MSAA (`NFS_MSAA`, por defecto x4). La imagen se resuelve, se le aplica el resplandor en la
  gráfica y se lee de vuelta a `fb` (premultiplicada → normal, por filas en paralelo). Así la mezcla con el 2D
  (`video.c`) y la ventana (`win.c`) no cambian.
- Con la gráfica, la resolución del 3D por defecto es la de la ventana (sin el tope de x3 del software).
- `NFS_GPU=0`, F9 o la casilla del menú: dibujado por software. Si OpenGL falla al arrancar, también.
- `NFS_GPU_LOG=1`: tiempos medios de cada fase cada 300 fotogramas.

Comprobado:
- Misma carrera, mismo fotograma, 3D x3 sin MSAA: diferencia media de 3 sobre 765 por píxel con el software
  (texturas algo más nítidas por el filtrado anisotrópico). Sombras, niebla, transparencias, reflejos y
  resplandor iguales a ojo.
- Ventana real, valores por defecto (3D 1024x768 + MSAA x4, NVIDIA): 12-15 ms por fotograma en carrera (antes,
  por software a 768x576 sin suavizado: 12,3 ms). Sin ventana: x3 6,6 ms de 3D, x6 9,9, x8 ~6 ms de la parte de
  la gráfica tras pasar el resplandor y la conversión a la gráfica/hilos.
- Con todo al máximo (x6, MSAA x8, subdivisión 2) en ventana: 18-22 ms: no llega a 60. Lo que queda es del
  procesador: subdivisión, esperar y leer la imagen, mezclar con el 2D a alta resolución y copiarla a la ventana.

Siguiente paso posible: hacer también en la gráfica la mezcla con el 2D y la presentación en la ventana (sin leer
la imagen de vuelta). Con eso x8 debería ir a 60.

Menú: casilla "Dibujar el 3D con la tarjeta gráfica", desplegable "Suavizado de bordes (gráfica)" (No/x2/x4/x8),
resoluciones x6 y x8; "Máxima" = x6 + MSAA x8 + subdivisión 2; "Original" quita el MSAA.

## Ronda de arreglos (2026-10-14) — VERIFICADO salvo donde se dice

- **Coches lejanos**: `tools/models/upgrade_lods.py`. Las piezas de `lod2`/`lod3` (y `wheel_lod2/3`) de cada coche
  se sustituyen por las de `lod1` con el mismo material (VERIFICADO: comparten textura y nombres de material en el
  Focus). Se escriben como `.nmesh` en `reemplazos\`. Si dos coches tienen la misma pieza lejana con distinto
  sustituto, no se toca. VERIFICADO con `NFS_GPU_LOG` que se sustituyen en carrera.
- **Carpeta del juego en el menú** (`menu.c`, `NFS_ROM` en `opciones.ini`), rutas por defecto en `%USERPROFILE%`.
- **Presentación en la gráfica** (`gpu_present` en `gpu.inc.c`, `video_present_gpu` en `video.c`): `video.c`
  junta las capas 2D de cada motor a 256x192 (`build_engine`) y las sube como texturas; un sombreador repite las
  reglas de `upscale_row` (capas delante/detrás del 3D, mezclas pendientes, alfa del 3D, brillo de capa y brillo
  maestro) y pinta cada pantalla en su rectángulo de la ventana; `SwapBuffers`. Sin lectura de vuelta (salvo
  capturas y pruebas sin ventana). `SetProcessDPIAware` en `boot.c`.
- **Sonido**: ver `notes/sound.md`.
- **Translúcidos con el mismo identificador** (gráfica): bit 6 de la plantilla, que se limpia al cambiar de
  identificador. Misma diferencia con el software que antes en las capturas de prueba (no había solapes).
- **2D**: ventana de sprites, mosaico de fondos de teselas y de sprites, sprites de mapa de bits. 0 píxeles
  distintos en las 9 capturas de prueba (el juego no los usa ahí).


## Segunda ronda tras probar el usuario (2026-10-14)

Quejas del usuario: sonido de carrera malo (el del menú bien), traseras de algunos coches (el Dodge Charger) y el
cielo raro. Captura `Capturaug 2.PNG`.

- **Sonido**: error de unidades. La librería SND trabaja en **décimas de dB** (SNDi_DecibelSquareTable = 400·log10,
  mínimo -723 = -72,3 dB; la envolvente va en esas unidades ×128) y se tomaban como dB: envolventes 10 veces más
  rápidas, sostenidos demasiado altos y el volumen por pista del juego mal escalado. Corregido; además un
  limitador suave (la suma de música, motores y efectos saturaba). Grabación de la carrera: 0 % saturado.
  En carrera el juego cambia tono (+12), volumen (+10) y pan (+9) de las pistas ~100 veces por segundo
  (VERIFICADO con `NFS_SND_COUNT=1`); cada efecto es una secuencia de una nota de duración 0 (hasta que se pare).
- **Traseras**: los materiales `blight` y `rlight` de cada coche son un brillo (textura A5I3 blanca de 32x32 con
  degradado) que el juego pone sobre los pilotos al frenar o ir marcha atrás. A alta resolución eran manchas
  blancas tapando el piloto (en la captura, el Charger frenando). `upgrade_lods.py` marca esas piezas (66, bandera
  16/32 del `.nmesh`) y la gráfica las dibuja como luz sumada, roja de freno y blanca suave de marcha atrás.
  Sin ver todavía en pantalla (en la prueba, al frenar la cámara pasa a mirar de frente).
- **Cielo**: HIPÓTESIS más probable, el resplandor: un cielo claro y uniforme pasaba del umbral y soltaba un halo
  que se comía el horizonte. Ahora solo brilla lo que destaca sobre su bloque de 4x4 (umbral = máx(0,84, media +
  0,10)), en la gráfica y en software. Si el cielo sigue raro, probar F5 (niebla) y F8 (resplandor).
- Texturas del Dodge: con la huella calculada como el port (`tex_hash`) desde los NSBMD, la de detalles del Charger
  no está capturada (no usa versión de IA) y la del destello del Challenger ampliada se ve bien (un destello como
  el original). Lo raro eran los destellos de freno dibujados opacos: ya van como luz sumada.

## Tercera ronda (2026-10-14)

- **Pista blanca** (captura `bug.PNG`): NO es un fallo. Es la pista nevada (`Graphics/Tracks/ast`, con muñecos de
  nieve, colisión de hielo y `ast_frozen.hzc`); su textura de suelo `snw_PNTBW` es casi blanca (media 246/255).
  De paso, la tabla de texturas descodificadas (1.024 huecos, 32 intentos por textura) ya no puede devolver "sin
  textura" al llenarse: echa la que lleve más tiempo sin usarse (`used_at`). En una carrera se usan ~133 huecos,
  así que solo pasaría tras muchas carreras seguidas. `NFS_TEXSTATS=1` escribe la ocupación.
- **Trasera rara del coche blanco** (`trasera dodge.PNG`): es el **Camaro** (no el Dodge). Los modelos `lod1` de
  los coches llevan dos piezas de luz (`blight` freno, `rlight` marcha atrás): un cuadrado con un halo blanco
  (textura blanca con alfa) que tapa casi toda la trasera; el color lo pone el material (gris 205). El juego solo
  las dibuja cuando el rival frena o va marcha atrás (VERIFICADO: 0 veces en 4.700 fotogramas de carrera normal,
  con `NFS_DL_LOG=1`; el `lod0` del jugador no las tiene). Se suavizan: la de marcha atrás, más tenue
  (0,35 en vez de 0,75) y las dos con el borde del halo difuminado (alfa al cuadrado). HIPÓTESIS: el juego cambia
  su color al encenderlas (busca los materiales por nombre: cadenas `blight`/`rlight`/`hlight` en `0x02109458`,
  usadas desde `FUN_0207cef8`); sin comprobar.

## Cuarta ronda (2026-10-15) — copia previa: rama y etiqueta `copia-seguridad-2026-10-15`

- **Pantalla completa** (`win.c`): rectángulos de pantalla de cualquier tamaño (`screen_rects`: x, y, ancho, alto;
  antes escalas enteras). F11 / Alt+Intro / Esc, disposición 3 (solo la pantalla del juego). VERIFICADO con
  capturas de la gráfica a 1920x1080 (juego a 1440x1080 y táctil a 480x360).
- **Texturas lejanas** (`NFS_SHARP`): 1 = mipmaps + anisotrópico x16 + nivel -0,75 (por defecto); 2 = sin mipmaps;
  0 = mipmaps -0,4. VERIFICADO: a x6 las tres dan la misma imagen (las texturas casi nunca se reducen a esa
  resolución); a x1, el modo 1 queda entre los otros en detalle (1,98 frente a 1,88 y 2,08) y cambio entre imágenes.
- **Controles** (`controls.h`, ventana "Controles…" del menú): tecla y control del mando por botón, en
  `opciones.ini` (`NFS_KEY_*`, `NFS_PAD_*`). Mando por XInput (DLL cargada al vuelo). Sin probar con un mando real.
- **2D con bordes suaves** (`NFS_SMOOTH2D`, sombreador de mezcla en `gpu.inc.c`): las capas 2D se amplían con el
  filtro de las calcomanías y el contorno de los sprites sobre el 3D con su cobertura (B-spline). Solo en la gráfica.
- **120 imágenes por segundo** (`NFS_FPS120`, experimental, apagado): por cada imagen del juego se dibuja otra
  intermedia (`gpu_interp_frame`): los polígonos se emparejan con los de la imagen anterior (mismo material y
  vértices, el más parecido entre los cercanos en orden, a menos de 12 píxeles de la consola) y cada vértice se
  pone a medio camino; los vértices compartidos se mueven igual (tabla por posición), así no se abren rendijas.
  Se enseña la intermedia en cuanto está y la actual al empezar el periodo siguiente. VERIFICADO: imagen intermedia
  limpia (sin rendijas ni piezas cruzadas tras emparejar por todos los vértices), el juego sigue a 60 (4.300
  imágenes en 72,3 s; sin la opción, 72,3 s también). LIMITACIÓN medida en el portátil del usuario: el juego ya
  gasta ~11 ms de los 16,7 por imagen y la intermedia añade 3-6 ms, así que solo cabe en el 10-35 % de las imágenes
  (más con resolución baja y sin suavizados). Si la media con intermedia pasa de 15,5 ms, un segundo sin ellas.
- Herramientas de prueba: `NFS_WINSHOT_EVERY/FROM/TO` (captura de lo que pinta la gráfica; `_i` = intermedia),
  `NFS_TEST_OFFSCREEN` (ventana fuera de la vista), `NFS_NOMENU`, `NFS_MENU_SNAPSHOT` (menú y controles en PNG sin
  mostrarlos), `NFS_FPS120_LOG`, `NFS_LODBIAS`.

### Ajustes tras probarlo el usuario (2026-10-15)
- **2D**: la versión suave le parecía borrosa. Ahora (valores por defecto de `u_sm`, `NFS_SM2D=a,b,c,d` para
  probar): transición de borde estrecha (0,45-0,55: menos de un píxel de pantalla), borde desde una diferencia de
  luminancia de 0,08, y zonas lisas nítidas (el píxel más cercano con el paso suavizado, en vez de Catmull-Rom).
  La forma de los bordes es mitad B-spline y mitad bilineal: así no desaparecen los puntos de un solo píxel (el
  indicador de carga). Elegida entre 5 variantes con capturas de la gráfica (menú, pantalla de modo, carrera).
- **120 fps**: "algunas texturas se mueven mucho". En la pista hay tramos iguales seguidos y se emparejaba un tramo
  con el de al lado (el más cercano). Ahora se prueba primero el que toca en el orden de dibujo y se exige que las
  coordenadas de textura coincidan (salvo reflejos). Además la intermedia empieza con la imagen actual de fondo: si
  queda una rendija entre piezas se ve esa imagen y no una línea negra (VERIFICADO en la captura que la tenía).
  Las texturas animadas del juego (flechas de las vallas) cambian a 60 por segundo: no se interpolan.
  `NFS_FPS120_FORCE=1` (pruebas): intermedia siempre.

### Tercera revisión del usuario (2026-10-15)
- **Filtro 2D quitado** del todo (al usuario no le gustó ninguna versión): vuelve el sombreador de mezcla de antes y
  la casilla desaparece del menú.
- **Reflejo de la X**: el barniz mezcla en la pintura lo que hay "encima" en la imagen anterior (reflejo en
  pantalla) y reflejaba el icono de la X que el juego dibuja en 3D sobre el coche. Ahora la gráfica guarda junto a
  cada imagen la distancia de cada píxel (segunda imagen R32F, `o_dep = v_w`, resuelta junto al color) y el
  reflejo solo usa píxeles al menos 1,6 veces más lejos que el punto de la pintura: cielo y edificios sí; el
  icono, los muñecos del techo y el propio coche no. Sin probar con la X real (el guion no la hace salir); sin
  errores de OpenGL.
- **Ventanas del Corolla / Renault** (`bug ventanas…PNG`): al subdividir la carrocería, Catmull-Clark redondeaba
  los huecos de las ventanas y la chapa tapaba las esquinas del cristal (que no se subdivide): picos de color.
  `blender_batch.py` fija ahora los vértices de los bordes abiertos (pliegue de vértice 1) y
  `boundary_smooth = PRESERVE_CORNERS`. VERIFICADO en Blender (huecos idénticos al original) y en carrera con el
  Corolla. Regenerado con `upgrade_cars.py`.

### 120 fps: píxeles que "se movían" (2026-10-15)
Medido con `flicker.py` (fuera del proyecto): píxeles donde la imagen real anterior y la actual casi no cambian
(<6) pero la intermedia se aparta (>20), en la zona del coche (que la cámara sigue). Antes 0,293 %; ahora 0,029 %
(una intermedia sin mover nada da 0,043 %). Cambios:
- Las piezas ambiguas (con reflejo, o con la misma coordenada de textura en todos sus vértices: cristales, piezas
  sin textura) solo se emparejan si se han movido menos de 4 píxeles; antes cogían otro triángulo del mismo
  cristal y tapaban el marco. Probado y descartado: elegir el más cercano (0,71-0,74 %, desordena el emparejado).
- Las piezas emparejadas usan su propio movimiento; la tabla de vértices compartidos solo para las que no tienen
  pareja (con ella para todas: 0,29 %; sin ella: 0,20 %).
- La intermedia refleja en la pintura la misma imagen anterior que la imagen real, y la luz y los reflejos usan
  normales y posiciones a medio camino.
Herramientas: `NFS_FPS120_DEBUG=fotograma`, `NFS_FPS120_LOGALL`.

### La X que blanquea la escena (2026-10-15, sin resolver del todo)
- Captura del usuario (`problema X…PNG`): con la X encima del coche, todo el 3D sale blanqueado (carretera y coches).
- Arreglado de paso (comportamiento de la consola que faltaba): el efecto de brillo del 3D y el propio 3D respetan
  las ventanas (WININ/WINOUT y la ventana de sprites) por píxel, en la gráfica (alfa de `ids`) y en el procesador.
  Si el juego hace la X con una ventana, ya solo se aclara dentro. En la carrera de prueba el juego no usa
  ventanas (BLDCNT 00bf/00ff, EVY 0-1, sin sprites ventana): no se pudo comprobar con la X.
- **F12**: guarda `diagnostico_NNNNN.txt` (registros de los dos motores 2D, DISP3DCNT, tabla de tonos, niebla,
  y los polígonos del siguiente dibujado por atributos y textura) y `diagnostico_NNNNN.png` en la carpeta del juego.
  En carrera normal DISP3DCNT=049b (resaltado activado: los polígonos en modo 2 suman la tabla de tonos, gris
  oscuro). HIPÓTESIS: con la X el juego cambia esa tabla o el modo; falta el diagnóstico del usuario.
  `NFS_DIAG_AT=fotograma` lo simula en pruebas.
- **Causa encontrada (VERIFICADO)**: no era luz ni reflejo. El juego blanquea toda la pantalla con el brillo maestro
  (MASTER_BRIGHT, modo "aclarar") en los dos motores: al empezar la carrera hace un fundido desde blanco de 16 a 0
  en unas 47 imágenes (`NFS_BRIGHT_LOG=1` apunta las escrituras), y por lo visto también en otros momentos (la X).
  La consola lo muestra igual: el port era fiel. Opción nueva `NFS_NO_WHITE_FLASH` (casilla del menú, activada):
  con el 3D en el motor principal (carrera) no se aplica el aclarado del brillo maestro. Fundidos a negro y menús,
  sin cambios. VERIFICADO con la captura de la gráfica en "Pulsa el Botón A".
