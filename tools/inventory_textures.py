import sys, struct, zipfile, os, collections

z = zipfile.ZipFile(os.path.join(sys.argv[1], 'data/vfs.zip'))


def blocks(b):
    nb = struct.unpack_from('<H', b, 14)[0]
    return [b[o:o + 4].decode('latin1') for o in struct.unpack_from(f'<{nb}I', b, 16)]


c = collections.Counter()
for i in z.infolist():
    f = i.filename
    if f.lower().endswith('.nsbmd'):
        top = '/'.join(f.split('/')[:2])
        base = f.rsplit('/', 1)[1] if top == 'Graphics/Cars' else ''
        if top == 'Graphics/Cars':
            base = ''.join(ch for ch in base if not ch.isdigit())
        c[(top, base, 'TEX0' in blocks(z.read(i)))] += 1
for k, v in sorted(c.items()):
    print(k, v)
for n in ('Graphics/Cars/AudiR8/lod0.nsbmd', 'Graphics/Tracks/tk1/textureBatch.nsbmd', 'Graphics/Tracks/tk1/DC_1_TAG_1.nsbmd'):
    print(n, blocks(z.read(n)))
b = z.read('Scripts/config/ai.luac')
print(b[:20].hex(' '))
