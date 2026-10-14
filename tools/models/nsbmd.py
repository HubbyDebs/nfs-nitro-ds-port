"""Lector de modelos NSBMD (NitroSystem G3D de Nintendo DS): geometria, materiales y texturas.

Lo que sabe leer (formato estandar BMD0, ver notes/model_format.md):
  - MDL0: modelos, nodos (huesos) con su posicion/giro/escala, materiales, formas (listas de
    visualizacion de la DS) y el "codigo de estructura" (SBC) que dice que forma se dibuja con que
    material y con que matriz.
  - TEX0: texturas y paletas, en los 7 formatos de la DS.
Las listas de visualizacion se convierten a triangulos con posicion, normal, color y coordenadas de
textura. Cada forma guarda ademas la huella de su lista (dl_hash): es la misma que calcula el port
cuando el juego la envia al chip 3D, y sirve para sustituirla por un modelo nuevo.
"""
import struct

import numpy as np

# ---- utilidades -------------------------------------------------------------------------------


def u8(d, o):
    return d[o]


def u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def s32(d, o):
    return struct.unpack_from('<i', d, o)[0]


def fx32(d, o):
    return s32(d, o) / 4096.0


def fx16(v):
    return (v - 0x10000 if v & 0x8000 else v) / 4096.0


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def fnv1a64(data):
    """Huella de una lista de visualizacion (igual que dl_hash en port/recomp/gx.c)."""
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def read_dict(d, o):
    """Diccionario de NitroSystem: devuelve [(nombre, offset_de_sus_datos, tam_elemento)]."""
    assert d[o] == 0, 'diccionario mal leido'
    n = d[o + 1]
    tree_size = u16(d, o + 6)
    info = o + tree_size  # el tamano cuenta desde el inicio del diccionario (VERIFICADO con los datos)
    elem_size, data_size = u16(d, info), u16(d, info + 2)
    names = info + data_size
    out = []
    for i in range(n):
        name = d[names + i * 16:names + i * 16 + 16].split(b'\0')[0].decode('latin-1')
        out.append((name, info + 4 + i * elem_size, elem_size))
    return out


# ---- texturas --------------------------------------------------------------------------------


def rgb555(c):
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))


def decode_texture(fmt, w, h, tex, pal, color0_transparent, idx4x4=None):
    """Devuelve una imagen RGBA (h, w, 4) uint8. tex/pal/idx4x4 son bytes a partir del inicio de la textura."""
    out = np.zeros((h, w, 4), np.uint8)

    def pc(i):
        if 2 * i + 1 >= len(pal):
            return (255, 0, 255)
        return rgb555(pal[2 * i] | (pal[2 * i + 1] << 8))

    for y in range(h):
        for x in range(w):
            a = 255
            if fmt == 1:  # A3I5
                v = tex[y * w + x]
                c = pc(v & 31)
                a = (v >> 5) * 255 // 7
            elif fmt == 6:  # A5I3
                v = tex[y * w + x]
                c = pc(v & 7)
                a = (v >> 3) * 255 // 31
            elif fmt == 2:  # 4 colores
                v = (tex[(y * w + x) // 4] >> ((x & 3) * 2)) & 3
                c = pc(v)
                if v == 0 and color0_transparent:
                    a = 0
            elif fmt == 3:  # 16 colores
                v = (tex[(y * w + x) // 2] >> ((x & 1) * 4)) & 15
                c = pc(v)
                if v == 0 and color0_transparent:
                    a = 0
            elif fmt == 4:  # 256 colores
                v = tex[y * w + x]
                c = pc(v)
                if v == 0 and color0_transparent:
                    a = 0
            elif fmt == 7:  # color directo
                v = tex[(y * w + x) * 2] | (tex[(y * w + x) * 2 + 1] << 8)
                c = rgb555(v)
                a = 255 if v & 0x8000 else 0
            elif fmt == 5:  # 4x4 comprimida
                blk = ((y // 4) * (w // 4) + x // 4)
                bits = (tex[blk * 4 + (y & 3)] >> ((x & 3) * 2)) & 3
                info = idx4x4[blk * 2] | (idx4x4[blk * 2 + 1] << 8)
                base, mode = (info & 0x3FFF) * 2, info >> 14
                c0, c1 = pc(base), pc(base + 1)
                if bits == 0:
                    c = c0
                elif bits == 1:
                    c = c1
                elif mode == 2:
                    c = pc(base + bits)
                elif mode == 0:
                    if bits == 2:
                        c = pc(base + 2)
                    else:
                        c, a = (0, 0, 0), 0
                elif mode == 1:
                    if bits == 2:
                        c = tuple((p + q) // 2 for p, q in zip(c0, c1))
                    else:
                        c, a = (0, 0, 0), 0
                else:
                    c = tuple((p * 5 + q * 3) // 8 for p, q in zip(c0, c1)) if bits == 2 else \
                        tuple((p * 3 + q * 5) // 8 for p, q in zip(c0, c1))
            else:
                c = (255, 0, 255)
            out[y, x] = (c[0], c[1], c[2], a)
    return out


class Tex0:
    def __init__(self, d, o):
        assert d[o:o + 4] == b'TEX0'
        self.d, self.o = d, o
        self.tex_dict_ofs = o + u16(d, o + 0x0E)
        self.tex_data = o + u32(d, o + 0x14)
        self.tex4_dict_ofs = o + u16(d, o + 0x1E)
        self.tex4_data = o + u32(d, o + 0x24)
        self.tex4_idx = o + u32(d, o + 0x28)
        self.pltt_dict_ofs = o + u16(d, o + 0x34)
        self.pltt_data = o + u32(d, o + 0x38)
        self.textures = {}
        for name, e, _ in read_dict(d, self.tex_dict_ofs):
            param = u32(d, e)
            self.textures[name] = param
        self.palettes = {}
        for name, e, _ in read_dict(d, self.pltt_dict_ofs):
            self.palettes[name] = u16(d, e) * 8

    def image(self, tex_name, pltt_name):
        d, param = self.d, self.textures[tex_name]
        fmt = (param >> 26) & 7
        w, h = 8 << ((param >> 20) & 7), 8 << ((param >> 23) & 7)
        ofs = (param & 0xFFFF) * 8
        c0 = (param >> 29) & 1
        bpp = {1: 8, 2: 2, 3: 4, 4: 8, 5: 2, 6: 8, 7: 16}[fmt]
        size = w * h * bpp // 8
        pal = b''
        if fmt != 7 and pltt_name in self.palettes:
            pal = d[self.pltt_data + self.palettes[pltt_name]:]
        if fmt == 5:
            tex = d[self.tex4_data + ofs:self.tex4_data + ofs + size]
            idx = d[self.tex4_idx + ofs // 2:self.tex4_idx + ofs // 2 + size // 2]
            return decode_texture(5, w, h, tex, pal, c0, idx)
        tex = d[self.tex_data + ofs:self.tex_data + ofs + size]
        return decode_texture(fmt, w, h, tex, pal, c0)


# ---- listas de visualizacion -------------------------------------------------------------------
PARAMS = {0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16, 0x19: 12,
          0x1A: 9, 0x1B: 3, 0x1C: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1,
          0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1, 0x34: 32, 0x40: 1, 0x41: 0,
          0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1, 0x00: 0}


def run_display_list(dl):
    """Convierte una lista de visualizacion en triangulos.

    Devuelve (vertices, triangulos, avisos). Cada vertice: dict con pos (en unidades del modelo, sin la
    escala posScale), nrm, uv (en texeles, como la DS), col, mtx (ultima matriz restaurada)."""
    words = struct.unpack_from('<%dI' % (len(dl) // 4), dl)
    verts, tris, notes = [], [], set()
    cur = {'nrm': (0.0, 0.0, 1.0), 'uv': (0.0, 0.0), 'col': (1.0, 1.0, 1.0), 'mtx': 0}
    pos = [0, 0, 0]
    prim, strip = None, []
    i = 0

    def emit_vertex():
        v = dict(cur)
        v['pos'] = tuple(p / 4096.0 for p in pos)
        verts.append(v)
        return len(verts) - 1

    def add_vertex(vi):
        nonlocal strip
        strip.append(vi)
        if prim == 0 and len(strip) == 3:
            tris.append(tuple(strip)); strip = []
        elif prim == 1 and len(strip) == 4:
            a, b, c, d = strip
            tris.append((a, b, c)); tris.append((a, c, d)); strip = []
        elif prim == 2 and len(strip) >= 3:
            n = len(strip)
            a, b, c = strip[-3:]
            tris.append((a, b, c) if n % 2 == 1 else (b, a, c))
        elif prim == 3 and len(strip) >= 4 and len(strip) % 2 == 0:
            a, b, c, d = strip[-4:]
            # tira de cuadrilateros: v0 v1 v3 v2
            tris.append((a, b, d)); tris.append((a, d, c))

    while i < len(words):
        packed = words[i]
        i += 1
        cmds = [(packed >> (8 * k)) & 0xFF for k in range(4)]
        for cmd in cmds:
            n = PARAMS.get(cmd)
            if n is None:
                notes.add('orden desconocida %02x' % cmd)
                n = 0
            p = words[i:i + n]
            i += n
            if cmd == 0x40:
                prim, strip = p[0] & 3, []
            elif cmd == 0x41:
                prim, strip = None, []
            elif cmd == 0x20:
                c = p[0]
                cur['col'] = ((c & 31) / 31, ((c >> 5) & 31) / 31, ((c >> 10) & 31) / 31)
            elif cmd == 0x21:
                c = p[0]
                cur['nrm'] = (sext(c & 0x3FF, 10) / 512, sext((c >> 10) & 0x3FF, 10) / 512, sext((c >> 20) & 0x3FF, 10) / 512)
            elif cmd == 0x22:
                c = p[0]
                cur['uv'] = (sext(c & 0xFFFF, 16) / 16.0, sext(c >> 16, 16) / 16.0)
            elif cmd in (0x23, 0x24, 0x25, 0x26, 0x27, 0x28):
                if cmd == 0x23:
                    pos = [sext(p[0] & 0xFFFF, 16), sext(p[0] >> 16, 16), sext(p[1] & 0xFFFF, 16)]
                elif cmd == 0x24:
                    pos = [sext(p[0] & 0x3FF, 10) << 6, sext((p[0] >> 10) & 0x3FF, 10) << 6, sext((p[0] >> 20) & 0x3FF, 10) << 6]
                elif cmd == 0x25:
                    pos = [sext(p[0] & 0xFFFF, 16), sext(p[0] >> 16, 16), pos[2]]
                elif cmd == 0x26:
                    pos = [sext(p[0] & 0xFFFF, 16), pos[1], sext(p[0] >> 16, 16)]
                elif cmd == 0x27:
                    pos = [pos[0], sext(p[0] & 0xFFFF, 16), sext(p[0] >> 16, 16)]
                else:
                    pos = [pos[0] + sext(p[0] & 0x3FF, 10), pos[1] + sext((p[0] >> 10) & 0x3FF, 10), pos[2] + sext((p[0] >> 20) & 0x3FF, 10)]
                if prim is not None:
                    add_vertex(emit_vertex())
            elif cmd == 0x14:
                cur['mtx'] = p[0] & 31
            elif cmd in (0x00,):
                pass
            else:
                notes.add('orden %02x en la lista' % cmd)
    return verts, tris, notes


def dl_commands(dl):
    """Que ordenes usa una lista (para saber si manda normales, colores o coordenadas de textura)."""
    words = struct.unpack_from('<%dI' % (len(dl) // 4), dl)
    seen, i = set(), 0
    while i < len(words):
        packed = words[i]
        i += 1
        for k in range(4):
            cmd = (packed >> (8 * k)) & 0xFF
            if cmd:
                seen.add(cmd)
            i += PARAMS.get(cmd, 0)
    return seen


# ---- modelo ------------------------------------------------------------------------------------


class Model:
    pass


def parse_sbc(d, o, end):
    """Recorre el codigo de estructura: devuelve [(nodo, material, forma)] en orden de dibujo."""
    draws, mat, node = [], None, 0
    lens = {0x00: 0, 0x01: 0, 0x02: 2, 0x03: 1, 0x04: 1, 0x05: 1, 0x06: 3, 0x07: 1, 0x08: 1, 0x0A: 8,
            0x0B: 0, 0x0C: 2, 0x0D: 2}
    i = o
    while i < end:
        op = d[i]
        cmd, opt = op & 0x1F, op >> 5
        if cmd == 0x01:
            break
        if cmd == 0x09:  # NODEMIX
            num = d[i + 2]
            i += 3 + 3 * num
            continue
        n = lens.get(cmd)
        if n is None:
            raise ValueError('orden SBC desconocida %02x' % op)
        if cmd == 0x02:
            node = d[i + 1]
        elif cmd == 0x04:
            mat = d[i + 1]
        elif cmd == 0x05:
            draws.append((node, mat, d[i + 1]))
        elif cmd == 0x06:
            node = d[i + 1]
            n += {0: 0, 1: 1, 2: 1, 3: 2}[opt]
        elif cmd in (0x07, 0x08):
            n += 1 if opt in (1, 2) else (2 if opt == 3 else 0)
        i += 1 + n
    return draws


def node_matrix(d, e):
    """Matriz 4x4 (columna = posicion) de un nodo a partir de sus datos SRT. Solo lo basico: traslacion,
    giro como matriz completa o pivote, escala."""
    flag = u16(d, e)
    m00 = fx16(u16(d, e + 2))
    p = e + 4
    T = np.eye(4)
    R = np.eye(3)
    S = np.ones(3)
    if not flag & 1:  # traslacion
        T[:3, 3] = [fx32(d, p), fx32(d, p + 4), fx32(d, p + 8)]
        p += 12
    if not flag & 2:  # giro
        if flag & 8:  # pivote
            a, b = fx16(u16(d, p)), fx16(u16(d, p + 2))
            p += 4
            pivot, neg = (flag >> 4) & 15, (flag >> 8) & 15
            # reconstruccion estandar de NitroSystem
            s1 = -1 if neg & 1 else 1
            s2 = -1 if neg & 2 else 1
            s3 = -1 if neg & 4 else 1
            s4 = -1 if neg & 8 else 1
            M = np.zeros(9)
            rows = {0: (4, 5, 7, 8), 1: (3, 5, 6, 8), 2: (3, 4, 6, 7), 3: (1, 2, 7, 8), 4: (0, 2, 6, 8),
                    5: (0, 1, 6, 7), 6: (1, 2, 4, 5), 7: (0, 2, 3, 5), 8: (0, 1, 3, 4)}[pivot]
            M[pivot] = s1
            M[rows[0]], M[rows[1]], M[rows[2]], M[rows[3]] = a, s2 * b, s3 * b, s4 * a
            R = M.reshape(3, 3)
        else:
            vals = [m00] + [fx16(u16(d, p + 2 * k)) for k in range(8)]
            p += 16
            R = np.array(vals).reshape(3, 3)
    if not flag & 4:
        S = np.array([fx32(d, p), fx32(d, p + 4), fx32(d, p + 8)])
    M = np.eye(4)
    M[:3, :3] = R.T * S  # la DS guarda las matrices por filas (los vectores multiplican por la izquierda)
    return T @ M


def load(d):
    assert d[:4] == b'BMD0', 'no es un NSBMD'
    nblocks = u16(d, 14)
    blocks = {d[o:o + 4]: o for o in struct.unpack_from('<%dI' % nblocks, d, 16)}
    tex0 = Tex0(d, blocks[b'TEX0']) if b'TEX0' in blocks else None
    m0 = blocks[b'MDL0']
    models = []
    for name, e, _ in read_dict(d, m0 + 8):
        mo = m0 + u32(d, e)
        m = Model()
        m.name = name
        sbc, mat, shp, end_env = (mo + u32(d, mo + 4), mo + u32(d, mo + 8), mo + u32(d, mo + 12), mo + u32(d, mo + 16))
        info = mo + 0x14
        m.num_nodes, m.num_mats, m.num_shps = d[info + 3], d[info + 4], d[info + 5]
        m.pos_scale = fx32(d, info + 8)
        node_dict = info + 0x2C
        m.nodes = []
        for nname, ne, _ in read_dict(d, node_dict):
            m.nodes.append((nname, node_matrix(d, node_dict + u32(d, ne))))
        m.draws = parse_sbc(d, sbc, mat)
        # materiales
        m.materials = []
        mats = read_dict(d, mat + 4)
        for mname, me, _ in mats:
            md = mat + u32(d, me)
            m.materials.append({'name': mname, 'diff_amb': u32(d, md + 4), 'spec_emi': u32(d, md + 8),
                                'poly_attr': u32(d, md + 12), 'tex_param': u32(d, md + 20),
                                'width': u16(d, md + 32), 'height': u16(d, md + 34), 'tex': None, 'pltt': None})
        for key, field in ((u16(d, mat), 'tex'), (u16(d, mat + 2), 'pltt')):
            for tname, te, _ in read_dict(d, mat + key):
                lst, num = mat + u16(d, te), d[te + 2]
                for k in range(num):
                    m.materials[d[lst + k]][field] = tname
        # formas
        m.shapes = []
        for sname, se, _ in read_dict(d, shp):
            so = shp + u32(d, se)
            dl_ofs, dl_size = so + u32(d, so + 8), u32(d, so + 12)
            dl = d[dl_ofs:dl_ofs + dl_size]
            verts, tris, notes = run_display_list(dl)
            m.shapes.append({'name': sname, 'dl': dl, 'dl_hash': fnv1a64(dl), 'verts': verts, 'tris': tris,
                             'notes': notes, 'flag': u32(d, so + 4), 'cmds': dl_commands(dl)})
        models.append(m)
    return models, tex0
