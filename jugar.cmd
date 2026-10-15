@echo off
rem Arranca el juego recompilado en una ventana.
rem
rem Al empezar sale un menu: ahi se elige la carpeta del juego (la ROM extraida con ndstool: arm9.bin,
rem header.bin, data\, overlay\), la pantalla y las mejoras graficas. Se guarda todo en opciones.ini, en la
rem carpeta de compilacion, para la proxima vez. Para saltar el menu: set NFS_NOMENU=1
rem Tambien se puede pasar la carpeta del juego como primer argumento: jugar.cmd D:\juegos\nfs
rem
rem Teclas: flechas = cruceta, Z = A, X = B, A = Y, S = X, Q = L, W = R, Intro = Start, Retroceso = Select.
rem Raton (boton izquierdo) sobre la pantalla tactil = lapiz.
rem F1 = cambiar la disposicion de las pantallas.  F2 = activar/desactivar el filtrado de texturas.
rem F3 = luz por pixel si/no.  F4 = subdivision de los coches: 0, 1 o 2 niveles.
rem F5 = niebla si/no.  F6 = sombras si/no.  F7 = brillo y reflejos de los coches si/no.
rem F8 = resplandor (bloom) si/no.  F9 = dibujar el 3D con la tarjeta grafica o con el procesador.
rem F11 = pantalla completa.  F12 = guardar un diagnostico (captura y datos) en la carpeta del juego.

set BUILD=%NFS_BUILD%
if "%BUILD%"=="" set BUILD=%USERPROFILE%\nfs-nitro-build\game
cd /d "%BUILD%"
nfs_boot.exe %1 > ultima_ejecucion.log 2>&1
echo El juego ha terminado. El registro esta en %BUILD%\ultima_ejecucion.log
pause
