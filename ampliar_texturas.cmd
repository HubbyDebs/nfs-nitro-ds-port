@echo off
rem Amplia x4 con IA (Real-ESRGAN) las texturas capturadas que aun no esten ampliadas y las deja
rem listas para el juego. Las ampliadas en PNG quedan en %USERPROFILE%\nfs-nitro-textures\x4
rem (se pueden retocar a mano; al volver a ejecutar esto se reempaquetan).
cd /d "%~dp0"
.venv\Scripts\python.exe tools\textures\upscale.py
pause
