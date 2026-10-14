@echo off
rem Arranca el juego recompilado en una ventana. Necesita la ROM extraida en C:\nfs
rem (o la carpeta que se pase como primer argumento).
rem
rem Teclas: flechas = cruceta, Z = A, X = B, A = Y, S = X, Q = L, W = R, Intro = Start, Retroceso = Select.
rem Raton (boton izquierdo) sobre la pantalla tactil = lapiz.
rem F1 = cambiar la disposicion de las pantallas.  F2 = activar/desactivar el filtrado de texturas.
rem F3 = luz por pixel si/no.  F4 = subdivision de los coches: 0, 1 o 2 niveles.
rem F5 = niebla si/no.  F6 = sombras si/no.  F7 = brillo y reflejos de los coches si/no.
rem F8 = resplandor (bloom) si/no.  F9 = dibujar el 3D con la tarjeta grafica o con el procesador.
rem
rem Al arrancar sale un menu para elegir la pantalla y las mejoras graficas. Lo elegido se guarda en
rem C:\Users\jj\nfs-nitro-build\game\opciones.ini para la proxima vez. Para saltarlo: set NFS_NOMENU=1

set ROM=%1
if "%ROM%"=="" set ROM=C:\nfs
cd /d C:\Users\jj\nfs-nitro-build\game
nfs_boot.exe %ROM% > ultima_ejecucion.log 2>&1
echo El juego ha terminado. El registro esta en C:\Users\jj\nfs-nitro-build\game\ultima_ejecucion.log
pause
