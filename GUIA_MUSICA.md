# Guía: música en alta calidad

La música del juego viene en la ROM con muy poca calidad: **mono, 22.767 Hz y comprimida a 4 bits** (ADPCM).
Por eso suena con un siseo de fondo y sin agudos, aunque el port la reproduzca perfecta. No se puede "mejorar"
a partir de esos datos: la información que falta no está en ellos.

Lo que sí se puede hacer es que el port toque **tus propios archivos** de esas canciones (comprados o sacados
de tus CD) en lugar de las de la ROM. El port no descarga nada ni incluye ninguna canción.

## Cómo se hace

1. Crea la carpeta `%USERPROFILE%\nfs-nitro-music` (por ejemplo `C:\Users\TuNombre\nfs-nitro-music`).
2. Copia ahí las canciones que tengas, en **MP3, M4A/AAC, FLAC, WAV o WMA** (las abre Windows).
3. Ponles de nombre el **nombre corto** de la tabla (por ejemplo `MajorLazar.mp3`), o déjalas con un nombre que
   lleve **artista y título** (por ejemplo `Major Lazer - Hold The Line.flac`).
4. Juega. Cuando el juego empieza una canción, suena unos segundos la original mientras se carga la tuya y
   luego pasa a la tuya con un fundido.

| Nombre corto | Artista | Canción |
|---|---|---|
| `BloodyBeetroots` | The Bloody Beetroots (con Cool Kids) | Awesome |
| `CodeRoad` | Danko Jones | Code Of The Road |
| `CrystalMethod` | The Crystal Method (con LMFAO) | Sine Language |
| `DizzieRascal` | Dizzee Rascal & Armand Van Helden | Bonkers |
| `Drumagik` | Drumagick | Make It Rock |
| `Evil9` | Evil 9 (con El-P) | All The Cash (Alex Metric Remix) |
| `GayBlades` | The Gay Blades | O Shot (Dmerit Remix) |
| `KOS` | k-os | Fun |
| `MajorLazar` | Major Lazer (con Mr. Lexx & Santigold) | Hold The Line |
| `NotThinkingStraight` | Pint Shot Riot | Not Thinking Straight |
| `OyeVaya` | Earl Greyhound | Oye Vaya |
| `Riase` | Rise Against | Kotov Syndrome |
| `RootsManuva` | Roots Manuva | Buff Nuff |
| `TakingBackSunday` | Taking Back Sunday | Lonely Lonely |
| `TwoFingers` | Two Fingers (con Sway) | Jewels And Gems |
| `WhatNow` | Alex Metric | What Now |

(Los nombres cortos son los que usa el juego por dentro, con sus faltas: `MajorLazar`, `DizzieRascal`, `Riase`.)

## Qué hace el port con ellas

- **Sigue al juego**: sabe qué canción suena y en qué punto exacto (compara lo que sale con la original), y
  copia lo que el juego haga con la música: bajarla en la pausa, el volumen de las opciones, fundidos.
- **Busca dónde cae cada trozo** de la original en tu archivo. Si tu versión empieza antes o después (silencio
  al principio), lo corrige solo. Si es otra versión distinta (más larga, otro remix) y un trozo no se
  encuentra, en ese trozo suena la original.
- **Iguala el volumen** de tu archivo al de la original, para que no suene de golpe más fuerte.
- Suena en **estéreo** y a más calidad. Ojo: en carrera el juego pasa todo el sonido por un efecto de la consola
  (graba la mezcla y la vuelve a tocar a 32.728 Hz), así que en carrera los agudos más altos se recortan un poco;
  en los menús suena entera.

## Si algo no va

- Para que lo escriba todo en la consola: `set NFS_MUS_LOG=1` antes de lanzar el juego. Al empezar sale
  `[musica] N canciones sustitutas en ...`, y al cargar cada una el desfase y cuántos trozos ha encontrado.
- Si un archivo no se abre ("formato que Windows no sabe abrir"), conviértelo a FLAC, MP3 o WAV.
- Para quitarla: `set NFS_MUSIC=0`, o vacía la carpeta. Otra carpeta: `set NFS_MUSIC_DIR=D:\mi\carpeta`.
