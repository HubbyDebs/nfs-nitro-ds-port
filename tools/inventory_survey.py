import sys, struct, zipfile, collections, os

root = sys.argv[1]
z = zipfile.ZipFile(os.path.join(root, 'data/vfs.zip'))
infos = z.infolist()
names = [i.filename for i in infos]

print("== GT500_cop / AudiR8 / bodygraphics(5) / one track")
for i in infos:
    f = i.filename
    if f.startswith(('Graphics/Cars/GT500_cop/', 'Graphics/Cars/AudiR8/', 'Graphics/Tracks/tk1/', 'Scenes/si1/', 'Scripts/config/', 'Graphics/Sky/tk1', 'Graphics/EnvMaps', 'Graphics/Cars/addons')) or f.count('/') <= 1 and not f.startswith('bitmaps'):
        print(f"  {i.file_size:9} {f}")
print([n for n in names if n.startswith('Graphics/Cars/bodygraphics')][:5])
print([n for n in names if n.startswith('bitmaps')][:8])


def nitro_blocks(b):
    magic, bom, ver, size, hsz, nb = struct.unpack_from('<4sHHIHH', b, 0)
    offs = struct.unpack_from(f'<{nb}I', b, 16)
    return magic, [(b[o:o + 4].decode('latin1'), struct.unpack_from('<I', b, o + 4)[0]) for o in offs]


print("== NSBMD block survey")
blk = collections.Counter()
texfmt = collections.Counter()
nmodels = collections.Counter()
notex = 0
for i in infos:
    if not i.filename.lower().endswith('.nsbmd'):
        continue
    b = z.read(i)
    try:
        magic, blocks = nitro_blocks(b)
    except Exception as ex:
        blk['ERR'] += 1
        continue
    blk[(magic, tuple(n for n, _ in blocks))] += 1
    # MDL0 model count
    offs = struct.unpack_from(f'<{len(blocks)}I', b, 16)
    for (n, s), o in zip(blocks, offs):
        if n == 'MDL0':
            nmodels[b[o + 9]] += 1
        if n == 'TEX0':
            # TEX0 header: texture info dict offset at +0x0E (u16), tex count in dict
            texinfo_off = struct.unpack_from('<H', b, o + 0x0E)[0]
            d = o + texinfo_off
            cnt = b[d + 1]
            # dict: header 8 bytes, then (cnt)*4 unknown block + 4 hdr, then data hdr 4 then cnt*8 entries
            p = d + 8 + 4 + cnt * 4  # skip dict hdr(8), unk hdr... approximate per nitro dict layout
            # nitro dict: u8 dummy,u8 cnt,u16 size, then unk block: u16 hdrsize,u16 size,u32 const, cnt*4 ; then data hdr: u16 elemsize,u16 datasize ; cnt*elem ; cnt*16 names
            unk = d + 4
            unk_hsz, unk_sz = struct.unpack_from('<HH', b, unk)
            dat = unk + 8 + cnt * 4
            esz, dsz = struct.unpack_from('<HH', b, dat)
            for k in range(cnt):
                e = dat + 4 + k * esz
                toff, param = struct.unpack_from('<HH', b, e)
                fmt = (param >> 10) & 7
                w = 8 << ((param >> 4) & 7)
                h = 8 << ((param >> 7) & 7)
                texfmt[(fmt, w, h)] += 1
for k, v in blk.most_common():
    print("  ", k, v)
print("  models per MDL0:", dict(nmodels))
FM = {1: 'A3I5', 2: 'pal4', 3: 'pal16', 4: 'pal256', 5: 'tex4x4', 6: 'A5I3', 7: 'direct'}
agg = collections.Counter()
for (f, w, h), v in texfmt.items():
    agg[FM.get(f, f)] += v
print("  tex formats:", dict(agg))
print("  tex sizes:", sorted(((w, h), v) for (w, h), v in collections.Counter({(w, h): 0 for (_, w, h) in texfmt}).items()))
sz = collections.Counter()
for (f, w, h), v in texfmt.items():
    sz[(w, h)] += v
print("  tex sizes:", sorted(sz.items(), key=lambda x: -x[1]))

print("== other nitro files")
for ext in ('.nsbta', '.nsbca'):
    c = collections.Counter()
    for i in infos:
        if i.filename.lower().endswith(ext):
            m, bl = nitro_blocks(z.read(i))
            c[(m, tuple(n for n, _ in bl))] += 1
    print("  ", ext, dict(c))

print("== BMP headers")
c = collections.Counter()
for i in infos:
    if i.filename.lower().endswith('.bmp'):
        b = z.read(i)[:54]
        w, h, pl, bpp, comp = struct.unpack_from('<iiHHI', b, 18)
        c[(w, h, bpp, comp)] += 1
print("  ", dict(c))

print("== text peeks")
for n in ('Graphics/Tracks/afc/afc.pvs',):
    print(z.read(n)[:600].decode('latin1'))
b = z.read('strings/main/frontend/es.str')
print(b[:200])
b = z.read('Graphics/Tracks/afc/afc.hzc')
print(b[:64].hex(' '))
# strings in a dcsc / luac
import re
for n in ('Scenes/afc/afc.dcsc', [x for x in names if x.startswith('Scripts/config/')][0]):
    b = z.read(n)
    s = [m.group().decode() for m in re.finditer(rb'[ -~]{4,}', b)]
    print(n, len(b), s[:40])

print("== vfs.bin")
vb = open(os.path.join(root, 'data/vfs.bin'), 'rb').read()
N, F, S = 4277, 4024, 29822
print(len(vb), "N*8+S =", N * 8 + S, "rest", len(vb) - N * 8 - S, "rest/F", (len(vb) - N * 8 - S) / F)
for k in range(6):
    print("  ", struct.unpack_from('<HHHH', vb, k * 8))
for k in (N - 2, N - 1):
    print("  ", k, struct.unpack_from('<HHHH', vb, k * 8))
so = N * 8
print(vb[so:so + 300])
print(vb[so + S:so + S + 64].hex(' '))
print(vb[-32:].hex(' '))

print("== SDAT")
for n in ('data/audio/sound_data.sdat', 'data/audio/sound_dataNetwork.sdat'):
    b = open(os.path.join(root, n), 'rb').read()
    symb_o, symb_s, info_o, info_s, fat_o, fat_s, file_o, file_s = struct.unpack_from('<8I', b, 16)
    print(n, "SYMB", hex(symb_o), symb_s, "INFO", hex(info_o), info_s, "FAT", hex(fat_o), fat_s, "FILE", hex(file_o), file_s)
    print("  magic", b[info_o:info_o + 4], b[fat_o:fat_o + 4], "nfiles", struct.unpack_from('<I', b, fat_o + 8)[0])
    kinds = ['SSEQ', 'SSAR', 'BANK', 'SWAR', 'PLAYER', 'GROUP', 'PLAYER2', 'STRM']
    for k, name in enumerate(kinds):
        ro = struct.unpack_from('<I', b, info_o + 8 + k * 4)[0]
        cnt = struct.unpack_from('<I', b, info_o + ro)[0]
        print(f"   {name}: {cnt}")
    fm = collections.Counter()
    nf = struct.unpack_from('<I', b, fat_o + 8)[0]
    for k in range(nf):
        o, s = struct.unpack_from('<II', b, fat_o + 12 + k * 16)
        fm[b[o:o + 4]] += 1
    print("   file magics:", dict(fm))
    if symb_s:
        ro = struct.unpack_from('<I', b, symb_o + 8 + 7 * 4)[0]
        cnt = struct.unpack_from('<I', b, symb_o + ro)[0]
        nm = []
        for k in range(min(cnt, 12)):
            so2 = struct.unpack_from('<I', b, symb_o + ro + 4 + k * 4)[0]
            if so2:
                e = b.index(b'\0', symb_o + so2)
                nm.append(b[symb_o + so2:e].decode('latin1'))
        print("   STRM names:", nm)

print("== overlays: quick string peek")
for k in range(4):
    b = open(os.path.join(root, f'overlay/overlay_{k:04d}.bin'), 'rb').read()
    s = [m.group().decode() for m in re.finditer(rb'[ -~]{8,}', b)]
    print(k, len(s), s[:12])
