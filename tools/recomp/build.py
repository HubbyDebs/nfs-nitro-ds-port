"""Traduce el juego a C y lo compila en paralelo.

Uso: .venv\\Scripts\\python.exe tools/recomp/build.py [--rom C:\\nfs] [--build C:\\Users\\jj\\nfs-nitro-build\\game]

Solo recompila los trozos que han cambiado. El resultado (nfs_boot.exe) y todo el C generado
quedan en la carpeta de compilacion, fuera del proyecto.
"""
import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
RT = os.path.join(PROJECT, 'port', 'recomp')
def find_clang():
    """clang de llvm-mingw: NFS_CLANG, o el primero en ~/nfs-nitro-tools/llvm-mingw-*, o el del PATH"""
    if os.environ.get('NFS_CLANG'):
        return os.environ['NFS_CLANG']
    found = sorted(glob.glob(os.path.expanduser(r'~\nfs-nitro-tools\llvm-mingw-*\bin\clang.exe')))
    return found[-1] if found else (shutil.which('clang') or 'clang')


CLANG = find_clang()

# Funciones del juego/SDK con sustituto nativo en port/recomp/hle.c
HLE = [
    0x0200d97c,  # FS_OpenFile
    0x0200d884,  # FS_ConvertPathToFileID
    0x0200d90c,  # FS_OpenFileFast
    0x0200db18,  # FS_ReadFile
    0x0200d808,  # FSi_ReadFileCore (FS_ReadFileAsync)
    0x0200d9c4,  # FS_CloseFile
    0x0200d218,  # FSi_SendCommand (aviso si se usa)
    0x0200e3b0,  # FS_LoadOverlay
    0x0200e404,  # FS_UnloadOverlay
    0x0203c150,  # unzOpen (solo para anunciar el resultado)
    0x02085670,  # FileSys_OpenFile del juego (solo para apuntar que archivos pide)
    'ov1:0x0212c3c4',  # arranque de una pantalla de presentacion: se saltan los videos
    0x02008550,  # OS_LoadContext (cambio de hilo, threads.c)
    0x0202a2b0,  # longjmp de la libreria de C (threads.c)
    0x02034ee0,  # luaG_errormsg: se deja correr, pero antes se imprime el error de Lua
]
# Funciones tipo setjmp: el punto de reanudacion se pone en quien las llama (ver threads.c)
SETJMP = ['0x02008504=rt_ctx_slot',  # OS_SaveContext
          '0x0202a2a4=rt_jmp_slot']  # setjmp de la libreria de C (lo usa Lua para sus errores)
# Depuracion: funciones cuyas llamadas se imprimen (variable de entorno NFS_TRACE=0x...,0x...)
TRACE = [int(x, 16) for x in os.environ.get('NFS_TRACE', '').split(',') if x]
RUNTIME = ['rt.c', 'io.c', 'hle.c', 'threads.c', 'irq.c', 'arm7.c', 'interp.c', 'video.c', 'gx.c', 'win.c', 'pool.c', 'menu.c', 'snd.c', 'music.c', 'boot.c']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=r'C:\nfs')
    ap.add_argument('--build', default=os.path.expanduser(r'~\nfs-nitro-build\game'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    args = ap.parse_args()

    t0 = time.time()
    ghidra_csv = os.path.join(RT, 'functions.csv')
    extra_csv = os.path.join(RT, 'functions_extra.csv')
    os.makedirs(args.build, exist_ok=True)
    subprocess.check_call([sys.executable, os.path.join(HERE, 'discover.py'), '--rom', args.rom,
                           '--functions', ghidra_csv, '--out', extra_csv,
                           '--missing', os.path.join(args.build, 'missing_calls.txt')])
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'recomp.py'), '--rom', args.rom,
                           # primero las correcciones/descubrimientos: completan funciones de Ghidra
                           '--functions', extra_csv + ',' + ghidra_csv, '--out', args.build,
                           '--hle', ','.join(a if isinstance(a, str) else hex(a) for a in HLE),
                           '--setjmp', ','.join(SETJMP),
                           '--trace', ','.join(hex(a) for a in TRACE)]
                          + (['--abi-check'] if os.environ.get('NFS_ABICHECK') else []))

    # recomp_funcs.h no cuenta: solo anade prototipos, y un trozo cuyo texto no cambia no necesita los nuevos
    newest_dep = os.path.getmtime(os.path.join(RT, 'rt.h'))
    runtime_dep = max(newest_dep, os.path.getmtime(os.path.join(RT, 'sys.h')))
    jobs = []
    for src in sorted(glob.glob(os.path.join(args.build, 'rc_*.c'))) + [os.path.join(RT, n) for n in RUNTIME]:
        obj = os.path.join(args.build, os.path.splitext(os.path.basename(src))[0] + '.o')
        dep = runtime_dep if os.path.dirname(src) == RT else newest_dep
        if os.path.basename(src) == 'gx.c':  # incluye gpu.inc.c
            dep = max(dep, os.path.getmtime(os.path.join(RT, 'gpu.inc.c')))
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), dep):
            jobs.append((src, obj))

    def compile_one(job):
        src, obj = job
        # El codigo traducido va con -O1 (son 80 MB de C); la capa propia, con -O2
        opt = ['-O1']
        if os.path.dirname(src) == RT:
            opt = ['-O2']
            if os.path.basename(src) in ('gx.c', 'video.c', 'win.c'):  # dibujado: al maximo, con AVX2
                opt = ['-O3', '-ffast-math', '-mavx2', '-mfma']
        r = subprocess.run([CLANG] + opt + ['-w', '-c', '-I' + RT, '-I' + args.build, src, '-o', obj],
                           capture_output=True, text=True)
        return src, r.returncode, r.stderr

    failed = False
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for src, code, err in pool.map(compile_one, jobs):
            if code:
                failed = True
                print(f"ERROR en {os.path.basename(src)}:\n{err[:3000]}")
    if failed:
        sys.exit(1)

    objs = sorted(glob.glob(os.path.join(args.build, '*.o')))
    exe = os.path.join(args.build, 'nfs_boot.exe')
    subprocess.check_call([CLANG, '-o', exe] + objs + ['-lgdi32', '-luser32', '-lopengl32', '-lshell32', '-lole32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lm'])
    print(f"compilados {len(jobs)} archivos, enlazado {exe} en {time.time() - t0:.0f} s")


if __name__ == '__main__':
    main()
