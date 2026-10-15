@echo off
rem Instalador del port de Need for Speed: Nitro (DS) para PC.
rem Pide TU ROM del juego (.nds, version de EE. UU.), descarga las herramientas necesarias, traduce el juego
rem a C en tu PC y crea el programa (nfs_boot.exe) con un acceso directo en el escritorio.
rem El port no incluye nada del juego: todo sale de tu propia copia.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\instalar.ps1" %*
pause
