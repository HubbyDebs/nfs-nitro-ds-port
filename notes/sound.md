# Sonido (2026-10-14)

`port/recomp/snd.c` hace lo que en la consola hace el programa del ARM7 para el sonido (librería SND del SDK de
Nintendo) y saca el resultado por los altavoces del PC (waveOut, 32.728 Hz, estéreo).

## Cómo llegan las órdenes (VERIFICADO)

El juego manda al ARM7 (mensaje con etiqueta 7, "SOUND") la dirección de una lista de órdenes
`{siguiente, número, 4 argumentos}`. La orden 29 da la zona compartida (`SNDSharedWork`): el ARM7 lleva ahí el
contador de listas terminadas, qué reproductores y canales suenan, las variables y los contadores de ticks.
Órdenes vistas en una partida (menú + carrera): 2/3 preparar y arrancar secuencia, 6/7 parámetros de reproductor y
de pista, 9 canales permitidos, 12/13 arrancar y parar temporizadores, 14 canal PCM suelto, 17 captura, 18
alarma, 20 volumen de canal, 25 selector de salida, 26/27 bloquear canales, 29 zona compartida, 30-33 invalidar /
leer información del controlador.

Formatos deducidos de los datos (VERIFICADO con los valores del juego):
- 14 `SETUP_CHANNEL_PCM`: arg0 = periodo<<16 | canal; arg1 = datos; arg2 = volumen<<24 | divisor<<22 | longitud
  del bucle (palabras); arg3 = formato<<24 | bucle<<26 | pan<<16 | inicio del bucle. La música del menú: canal 6,
  PCM16 a ~22,7 kHz; la de carrera: canales 1 y 3 (izquierda/derecha) a ~32,7 kHz.
- 18 `SETUP_ALARM`: número, primer aviso, periodo (ticks del sistema, 33,5 MHz/64), identificador. Al saltar, el
  ARM7 manda al juego `(id << 8) | número` con la etiqueta 7; así el juego rellena el búfer de la música.
- Banco SBNK en memoria: número de instrumentos en +0x38 y tabla en +0x3C (tipo | desplazamiento<<8); punteros a
  los archivos de ondas en +0x18 (los enlaza el juego). Instrumento: onda, archivo, tecla base, ADSR, pan.

## Qué hace snd.c

- Reproductor de secuencias SSEQ (48 ticks por negra, tempo, 16 reproductores, 32 pistas): notas, esperas, cambio
  de programa, abrir pista, saltos, llamadas, bucles, variables (con prefijos aleatorio / variable / condicional),
  volumen, expresión, pan, transposición, pitch bend, prioridad, notas que esperan o no, ligado, vibrato/trémolo,
  ADSR (con las fórmulas del SDK: ataque multiplicativo, caída y liberación lineales en dB).
- Instrumentos: PCM (un sonido, batería por teclas, zonas por teclas), cuadradas y ruido.
- Canales: PCM8, PCM16, ADPCM IMA, cuadrada, ruido; bucle o una vez; interpolación lineal.
- Alarmas para la música en streaming. Una secuencia que termina deja sonar sus notas hasta el final, como la
  consola (antes se cortaban y los efectos no se oían).
- Sin hacer: el efecto con captura (los canales 1 y 3 repiten lo capturado: se silencian y sale el sonido sin el
  efecto), barrido de tono (orden E3), portamento.

## Comprobado

Sin poder oírlo, con una grabación (`NFS_WAV=archivo.wav`) de menú y carrera:
- hay sonido en el menú (música) y en carrera, con un silencio en la pantalla de carga;
- no es un bucle atascado (correlación ~0 con la longitud del búfer) y la música del menú es tonal (planitud
  espectral 0,13-0,2; ruido = 0,85);
- solo los canales de secuencia (`NFS_SND_ONLY=seq`): suenan en carrera, con paneo izquierda/derecha.
- En ventana, con la salida de audio real, el juego va igual (9 ms por fotograma) y sin fallos.
FALTA: que el usuario lo escuche y diga si suena bien (volumen, tono, cortes).

Opciones: casilla "Sonido" del menú (`NFS_SOUND=0`), `NFS_VOLUME=0..100`, `NFS_SND_LOG=1` (órdenes y notas),
`NFS_WAV=archivo.wav`, `NFS_SND_ONLY=seq|stream`.

## Arreglos tras escucharlo el usuario (2026-10-14)

- **Motor sin cambiar de tono**: cada efecto es una secuencia de una nota y fin; al terminar la partitura se
  liberaba la pista y las órdenes del juego que cambian tono, volumen y pan de la pista (`TRACK_PARAM`, unas 100
  por segundo en carrera) no la encontraban. Ahora la pista se queda (marcada como terminada) hasta que el
  reproductor acaba de sonar. VERIFICADO: 3.998 de 4.000 órdenes aplicadas (antes se perdían).
- **Décimas de dB** (ver `notes/recomp.md`, "Segunda ronda") y limitador suave.
- **Captura y selector de salida**: en carrera y en pausa el juego graba la mezcla con las unidades de captura
  (orden 17), la procesa en el ARM9 con sus alarmas y la reproduce con los canales 1 y 3, y el selector de salida
  (orden 25: izquierda = canal 1, derecha = canal 3, los dos fuera de la mezcla) manda eso a los altavoces. Hecho
  igual: la captura escribe la mezcla en el búfer después de que los canales 1 y 3 lean su muestra (van un búfer
  por detrás, como en la consola). HIPÓTESIS de los bits de la orden 17 (unidad = bit 31, bucle = bit 29) por los
  valores del juego. VERIFICADO que el sonido de carrera sigue llegando por ese camino (niveles normales).
