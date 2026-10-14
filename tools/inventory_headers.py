import sys, struct, zipfile, collections, os

root = sys.argv[1]


def hexdump(b, base=0):
    for i in range(0, len(b), 16):
        c = b[i:i + 16]
        print(f"{base + i:08X}  {c.hex(' '):<47}  {''.join(chr(x) if 32 <= x < 127 else '.' for x in c)}")


def head(rel, off=0, n=128):
    p = os.path.join(root, rel)
    with open(p, 'rb') as f:
        f.seek(off)
        b = f.read(n)
    print(f"== {rel} @0x{off:X}")
    hexdump(b, off)


head('arm9.bin', 0x800, 64)
head('arm9.bin', os.path.getsize(os.path.join(root, 'arm9.bin')) - 16, 16)
head('data/vfs.bin', 0, 256)
head('data/vfs.zip', 0, 64)
head('data/movies/publisher.mods', 0, 64)
head('data/audio/sound_data.sdat', 0, 64)
head('data/audio/sound_dataNetwork.sdat', 0, 64)
head('data/download.char', 0, 32)
head('data/downloadus.srl', 0, 32)

zp = os.path.join(root, 'data/vfs.zip')
print("is_zipfile:", zipfile.is_zipfile(zp))
try:
    z = zipfile.ZipFile(zp)
    infos = z.infolist()
    print("entries:", len(infos))
    ext = collections.Counter()
    size = collections.Counter()
    comp = collections.Counter()
    dirs = collections.Counter()
    for i in infos:
        e = os.path.splitext(i.filename)[1].lower()
        ext[e] += 1
        size[e] += i.file_size
        comp[i.compress_type] += 1
        dirs[i.filename.rsplit('/', 1)[0] if '/' in i.filename else '.'] += 1
    print("compress types:", dict(comp))
    print("total uncompressed:", sum(i.file_size for i in infos))
    for e, n in ext.most_common():
        print(f"  {e or '(none)':10} {n:6} files {size[e]:12} bytes")
    print("dirs:")
    for d, n in sorted(dirs.items()):
        print(f"  {d:60} {n}")
    # magic per extension
    print("magics:")
    seen = {}
    for i in infos:
        e = os.path.splitext(i.filename)[1].lower()
        if i.file_size == 0 or seen.get(e, 0) >= 2:
            continue
        seen[e] = seen.get(e, 0) + 1
        try:
            with z.open(i) as f:
                b = f.read(16)
            print(f"  {i.filename:60} {i.file_size:9}  {b.hex(' ')}  {''.join(chr(x) if 32 <= x < 127 else '.' for x in b)}")
        except Exception as ex:
            print(f"  {i.filename}: ERROR {ex!r}")
except Exception as ex:
    print("zip error:", repr(ex))
