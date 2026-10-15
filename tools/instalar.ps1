# Instalador del port de Need for Speed: Nitro (DS) para PC.
#
# Prepara todo en el PC del usuario a partir de SU PROPIA ROM: el juego no viene con el port. Pasos:
#   1. Pide la ROM (.nds) y comprueba que sea la versión de EE. UU. (NTR-BNNE-USA) que traduce el port.
#   2. La extrae (tools\nds_extract.py, igual que ndstool) en %USERPROFILE%\nfs-nitro-game.
#   3. Descarga las herramientas a %USERPROFILE%\nfs-nitro-tools: uv (instala Python y sus librerías) y
#      llvm-mingw (el compilador).
#   4. Traduce el juego a C y lo compila (tools\recomp\build.py): %USERPROFILE%\nfs-nitro-build\game\nfs_boot.exe
#   5. Mejoras que salen de la ROM: coches lejanos y luces (upgrade_lods.py) y, si hay Blender 5, los modelos de
#      garaje en carrera (upgrade_cars.py).
#   6. Deja configurada la carpeta del juego y crea un acceso directo en el escritorio.
#
# Uso: instalar.cmd (o: powershell -ExecutionPolicy Bypass -File tools\instalar.ps1 [ruta\juego.nds])
param([string]$Rom = "", [switch]$SinAccesoDirecto)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$Project = Split-Path -Parent $PSScriptRoot
$Tools = Join-Path $env:USERPROFILE "nfs-nitro-tools"
$GameDir = Join-Path $env:USERPROFILE "nfs-nitro-game"
$BuildDir = Join-Path $env:USERPROFILE "nfs-nitro-build\game"
$LlvmVersion = "20261006"
$Arm9Sha1 = "56481f5929138dd3b81f95557b76c7d4a6a3954a"
$curl = Join-Path $env:SystemRoot "System32\curl.exe" # el de Windows (no otro del PATH)
if (-not (Test-Path $curl)) { $curl = "curl.exe" }

# Descomprimir con el tar de Windows (no con otro que haya en el PATH, como el de devkitPro o Git, que no
# entienden "C:\..."); si no esta (Windows antiguo), con Expand-Archive
function Descomprimir($zip, $destino) {
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path $tar) {
        & $tar -xf $zip -C $destino
        if ($LASTEXITCODE -eq 0) { return }
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $destino -Force
}

function Paso($texto) { Write-Host ""; Write-Host "== $texto" -ForegroundColor Cyan }
function Fallo($texto) {
    Write-Host ""; Write-Host "ERROR: $texto" -ForegroundColor Red
    exit 1
}

Write-Host "Need for Speed: Nitro - instalador del port para PC" -ForegroundColor Yellow
Write-Host "Necesitas tu propia copia del juego (archivo .nds, versión de EE. UU.). El port no la incluye."

# ---- 1. la ROM ----------------------------------------------------------------------------------------
Paso "1/6  Elige tu ROM de Need for Speed: Nitro"
if (-not $Rom) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Elige tu ROM de Need for Speed: Nitro (.nds)"
    $dlg.Filter = "ROM de Nintendo DS (*.nds)|*.nds|Todos los archivos (*.*)|*.*"
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fallo "no se ha elegido ninguna ROM." }
    $Rom = $dlg.FileName
}
if (-not (Test-Path -LiteralPath $Rom)) { Fallo "no existe el archivo $Rom" }
Write-Host "ROM: $Rom"
$head = New-Object byte[] 16
$fs = [System.IO.File]::OpenRead($Rom); [void]$fs.Read($head, 0, 16); $fs.Close()
$code = [System.Text.Encoding]::ASCII.GetString($head, 12, 4)
if ($code -ne "BNNE") {
    Fallo "esta ROM tiene el código '$code'. El port solo funciona con Need for Speed: Nitro de EE. UU. (BNNE)."
}

# ---- 2. herramientas -------------------------------------------------------------------------------------
Paso "2/6  Descargando las herramientas (solo la primera vez)"
New-Item -ItemType Directory -Force -Path $Tools | Out-Null
$uv = Join-Path $Tools "uv\uv.exe"
if (-not (Test-Path $uv)) {
    Write-Host "Descargando uv (instala Python)..."
    $zip = Join-Path $Tools "uv.zip"
    & $curl -L --fail --progress-bar -o $zip "https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-pc-windows-msvc.zip"
    if ($LASTEXITCODE -ne 0) { Fallo "no se pudo descargar uv. Comprueba la conexión a internet." }
    New-Item -ItemType Directory -Force -Path (Join-Path $Tools "uv") | Out-Null
    Descomprimir $zip (Join-Path $Tools "uv")
    Remove-Item $zip
    if (-not (Test-Path $uv)) { Fallo "no se encuentra uv.exe despues de descomprimirlo." }
}
$clang = Get-ChildItem -Path $Tools -Filter "llvm-mingw-*" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName "bin\clang.exe" } | Where-Object { Test-Path $_ } | Select-Object -Last 1
if (-not $clang) {
    Write-Host "Descargando el compilador llvm-mingw (unos 190 MB)..."
    $zip = Join-Path $Tools "llvm-mingw.zip"
    & $curl -L --fail --progress-bar -o $zip "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmVersion/llvm-mingw-$LlvmVersion-ucrt-x86_64.zip"
    if ($LASTEXITCODE -ne 0) { Fallo "no se pudo descargar llvm-mingw. Comprueba la conexión a internet." }
    Write-Host "Descomprimiendo..."
    Descomprimir $zip $Tools
    Remove-Item $zip
    $clang = Join-Path $Tools "llvm-mingw-$LlvmVersion-ucrt-x86_64\bin\clang.exe"
    if (-not (Test-Path $clang)) { Fallo "no se encuentra clang.exe después de descomprimir llvm-mingw." }
}
$env:NFS_CLANG = $clang
$venv = Join-Path $Project ".venv"
$python = Join-Path $venv "Scripts\python.exe"
if (-not (Test-Path $python)) {
    Write-Host "Instalando Python..."
    & $uv venv --python 3.12 $venv
    if ($LASTEXITCODE -ne 0) { Fallo "no se pudo instalar Python." }
}
Write-Host "Instalando las librerías de Python (capstone, numpy, pillow)..."
& $uv pip install --python $python capstone numpy pillow
if ($LASTEXITCODE -ne 0) { Fallo "no se pudieron instalar las librerías de Python." }

# ---- 3. extraer la ROM ----------------------------------------------------------------------------------
Paso "3/6  Extrayendo la ROM en $GameDir"
& $python (Join-Path $Project "tools\nds_extract.py") $Rom $GameDir
if ($LASTEXITCODE -ne 0) { Fallo "no se pudo extraer la ROM." }
$sha = (Get-FileHash -Algorithm SHA1 -LiteralPath (Join-Path $GameDir "arm9.bin")).Hash.ToLower()
if ($sha -ne $Arm9Sha1) {
    Write-Host "AVISO: el código de esta ROM no es exactamente el que traduce el port ($sha)." -ForegroundColor Yellow
    Write-Host "Puede ser otra revisión del juego o una ROM modificada: puede no funcionar." -ForegroundColor Yellow
}

# ---- 4. traducir y compilar ----------------------------------------------------------------------------
Paso "4/6  Traduciendo el juego a C y compilando (la primera vez tarda varios minutos)"
& $python (Join-Path $Project "tools\recomp\build.py") --rom $GameDir --build $BuildDir
if ($LASTEXITCODE -ne 0) { Fallo "la compilación ha fallado. Mira los mensajes de arriba." }
$exe = Join-Path $BuildDir "nfs_boot.exe"
if (-not (Test-Path $exe)) { Fallo "no se ha generado nfs_boot.exe." }

# ---- 5. mejoras que salen de la ROM ----------------------------------------------------------------------
Paso "5/6  Preparando las mejoras de los coches"
& $python (Join-Path $Project "tools\models\upgrade_lods.py") --rom $GameDir
if ($LASTEXITCODE -ne 0) { Write-Host "AVISO: no se pudieron preparar los coches lejanos (el juego funciona igual)." -ForegroundColor Yellow }
$blender = Get-ChildItem "C:\Program Files\Blender Foundation\Blender 5*\blender.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($blender) {
    Write-Host "Blender encontrado: modelos de garaje en carrera (unos minutos)..."
    & $python (Join-Path $Project "tools\models\upgrade_cars.py") --rom $GameDir --blender $blender.FullName
    if ($LASTEXITCODE -ne 0) { Write-Host "AVISO: no se pudieron preparar los modelos (el juego funciona igual)." -ForegroundColor Yellow }
} else {
    Write-Host "Sin Blender 5: los coches usan los modelos de carrera originales (instala Blender y vuelve a ejecutar para mejorarlos)."
}

# ---- 6. configuración y acceso directo -----------------------------------------------------------------
Paso "6/6  Último paso"
$ini = Join-Path $BuildDir "opciones.ini"
$lines = @()
if (Test-Path $ini) { $lines = @(Get-Content $ini | Where-Object { $_ -notmatch "^NFS_ROM=" }) }
if ($lines.Count -eq 0) { $lines = @("# Opciones del port de NFS Nitro (las cambia el menu del principio)") }
$lines = @($lines[0], "NFS_ROM=$GameDir") + @($lines | Select-Object -Skip 1)
Set-Content -Path $ini -Value $lines -Encoding Default
if (-not $SinAccesoDirecto) {
$desk = [Environment]::GetFolderPath("Desktop")
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut((Join-Path $desk "Need for Speed Nitro.lnk"))
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = $BuildDir
$lnk.IconLocation = "$exe,0"
$lnk.Description = "Need for Speed: Nitro (port a PC)"
$lnk.Save()
}

Write-Host ""
Write-Host "Listo. Abre el juego con el acceso directo 'Need for Speed Nitro' del escritorio." -ForegroundColor Green
Write-Host "Programa: $exe"
