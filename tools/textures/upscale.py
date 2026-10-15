"""Amplia con IA (Real-ESRGAN) las texturas volcadas por el juego y las empaqueta para que el
port las cargue en lugar de las originales.

Carpetas (todas del usuario, fuera del proyecto; por defecto C:\\Users\\jj\\nfs-nitro-textures):
  original\\  PNG volcados por el juego (se generan jugando con NFS_TEXDUMP=1)
  x4\\        PNG ampliados x4 (se pueden retocar a mano)
  pack\\      lo que carga el juego: un .tex por textura (cabecera NTX1 + ancho + alto + pixeles BGRA)

Uso: .venv\\Scripts\\python.exe tools\\textures\\upscale.py [--dir CARPETA] [--model realesrgan-x4plus]
                                                           [--car-model realesrgan-x4plus-anime]
                                                           [--redo]   (rehacer aunque ya existan)
                                                           [--pack-only]  (solo reempaquetar x4 -> pack)

Detalles para que el resultado no tenga defectos:
 - Las texturas que se repiten en mosaico se amplian con un margen copiado del lado opuesto, y luego
   se recorta: asi no aparece una costura en los bordes.
 - Los texeles transparentes no tienen color util: antes de ampliar se rellenan con el color de sus
   vecinos opacos, para que no manchen los bordes de vallas, arboles, etc.
 - La transparencia se amplia aparte (interpolacion suave) y se vuelve a juntar.
 - Las texturas de coches (coches.txt, lo apunta el juego) van con el modelo "anime": en logos, letras
   y bordes de la carroceria queda mas limpio que el general (comparado a ojo, 2026-10-14).
"""
import argparse
import os
import shutil
import struct
import subprocess

import numpy as np
from PIL import Image

ESRGAN = os.environ.get('NFS_ESRGAN', os.path.expanduser(r'~\nfs-nitro-tools\realesrgan\realesrgan-ncnn-vulkan.exe'))
PAD = 8
SCALE = 4


def fill_transparent(rgb, alpha):
    """Rellena el color de los texeles transparentes con el de sus vecinos opacos."""
    known = alpha > 0
    if known.all() or not known.any():
        return rgb
    rgb = rgb.astype(np.float32)
    for _ in range(max(rgb.shape[0], rgb.shape[1])):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        new = ~known & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][..., None]
        known = known | new
    return rgb.astype(np.uint8)


def prepare(src, dst):
    im = np.array(Image.open(src).convert('RGBA'))
    rgb = fill_transparent(im[..., :3], im[..., 3])
    rgb = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='wrap')
    Image.fromarray(rgb, 'RGB').save(dst)


def finish(original, upscaled, dst):
    orig = Image.open(original).convert('RGBA')
    w, h = orig.size
    up = Image.open(upscaled).convert('RGB')
    up = up.crop((PAD * SCALE, PAD * SCALE, PAD * SCALE + w * SCALE, PAD * SCALE + h * SCALE))
    alpha = orig.getchannel('A')
    if alpha.getextrema() != (255, 255):
        alpha = alpha.resize((w * SCALE, h * SCALE), Image.BICUBIC)
    else:
        alpha = Image.new('L', (w * SCALE, h * SCALE), 255)
    up.putalpha(alpha)
    up.save(dst)


def pack(src, dst):
    im = np.array(Image.open(src).convert('RGBA'))
    h, w = im.shape[:2]
    bgra = im[..., [2, 1, 0, 3]]  # en memoria el juego usa 0xAARRGGBB
    with open(dst, 'wb') as f:
        f.write(struct.pack('<4sII', b'NTX1', w, h))
        f.write(bgra.tobytes())


def upscale_group(names, model, d_orig, d_x4, tmp_in, tmp_out):
    """Amplia una lista de texturas con un modelo y deja los PNG en x4."""
    for d in (tmp_in, tmp_out):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    for n in names:
        prepare(os.path.join(d_orig, n), os.path.join(tmp_in, n))
    print(f"ampliando {len(names)} texturas con {model}...", flush=True)
    subprocess.run([ESRGAN, '-i', tmp_in, '-o', tmp_out, '-n', model, '-s', str(SCALE), '-f', 'png'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for n in names:
        finish(os.path.join(d_orig, n), os.path.join(tmp_out, n), os.path.join(d_x4, n))
    for d in (tmp_in, tmp_out):
        shutil.rmtree(d, ignore_errors=True)


def read_list(path):
    if not os.path.exists(path):
        return set()
    with open(path, encoding='ascii', errors='ignore') as f:
        return {line.strip() + '.png' for line in f if line.strip()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=os.environ.get('NFS_TEXTURES', os.path.expanduser(r'~\nfs-nitro-textures')))
    ap.add_argument('--model', default='realesrgan-x4plus')
    ap.add_argument('--car-model', default='realesrgan-x4plus-anime',
                    help='modelo para las texturas de coches (lista coches.txt que apunta el juego)')
    ap.add_argument('--redo', action='store_true')
    ap.add_argument('--pack-only', action='store_true')
    args = ap.parse_args()
    d_orig, d_x4, d_pack = (os.path.join(args.dir, n) for n in ('original', 'x4', 'pack'))
    d_old = os.path.join(args.dir, 'x4_anterior')
    tmp_in, tmp_out = os.path.join(args.dir, 'tmp_in'), os.path.join(args.dir, 'tmp_out')
    for d in (d_x4, d_pack):
        os.makedirs(d, exist_ok=True)

    # Texturas de coches: las apunta el juego en coches.txt (las que se dibujan con luz). Se amplian
    # con otro modelo, que da bordes y letras mas limpios. hechas_coches.txt dice cuales ya lo estan.
    cars = read_list(os.path.join(args.dir, 'coches.txt'))
    done_list = os.path.join(args.dir, 'hechas_coches.txt')
    cars_done = read_list(done_list)

    names = sorted(n for n in os.listdir(d_orig) if n.endswith('.png'))
    car_todo = [n for n in names if n in cars and (args.redo or n not in cars_done)]
    todo = [n for n in names if n not in cars and (args.redo or not os.path.exists(os.path.join(d_x4, n)))]
    redone = 0
    if not args.pack_only:
        if todo:
            upscale_group(todo, args.model, d_orig, d_x4, tmp_in, tmp_out)
        if car_todo:
            # Las de coche que ya estaban ampliadas con el modelo general se rehacen; la version anterior
            # se guarda en x4_anterior por si estaba retocada a mano.
            os.makedirs(d_old, exist_ok=True)
            for n in car_todo:
                if os.path.exists(os.path.join(d_x4, n)):
                    shutil.copy2(os.path.join(d_x4, n), os.path.join(d_old, n))
                    redone += 1
            upscale_group(car_todo, args.car_model, d_orig, d_x4, tmp_in, tmp_out)
            with open(done_list, 'a', encoding='ascii') as f:
                for n in car_todo:
                    if n not in cars_done:
                        f.write(n[:-4] + '\n')

    packed = 0
    for n in sorted(os.listdir(d_x4)):
        if not n.endswith('.png'):
            continue
        src, dst = os.path.join(d_x4, n), os.path.join(d_pack, n[:-4] + '.tex')
        if args.redo or not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
            pack(src, dst)
            packed += 1
    amp = 0 if args.pack_only else len(todo) + len(car_todo)
    print(f"originales: {len(names)}, de coches conocidas: {len(cars)}, ampliadas ahora: {amp} "
          f"({0 if args.pack_only else len(car_todo)} de coches, {redone} rehechas; copia en x4_anterior), "
          f"empaquetadas ahora: {packed}")
    missing = len([c for c in cars if c not in set(names)])
    if missing:
        print(f"aviso: {missing} texturas de coches vistas en el juego no estan capturadas; "
              f"juega con capturar_texturas.cmd para tenerlas")


if __name__ == '__main__':
    main()
