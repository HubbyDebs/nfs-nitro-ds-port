@echo off
rem Juega con normalidad: cada textura nueva que aparezca en pantalla se guarda en
rem %USERPROFILE%\nfs-nitro-textures\original (solo las que aun no estaban).
rem Cuando termines, ejecuta ampliar_texturas.cmd para ampliarlas con la IA.
set NFS_TEXDUMP=1
call "%~dp0jugar.cmd" %*
