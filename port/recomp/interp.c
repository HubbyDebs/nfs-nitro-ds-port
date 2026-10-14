/* Interprete de reserva: ejecuta instruccion a instruccion el codigo del juego que no esta
 * traducido a C. Entra cuando una llamada indirecta va a una direccion sin funcion traducida,
 * y vuelve al codigo traducido cuando ese codigo retorna.
 *
 * Sigue el mismo modelo que el traductor (tools/recomp/recomp.py): ARMv5TE y Thumb-1, con las
 * mismas ayudas de rt.h. Se comprueba contra Unicorn con tools/recomp/fuzz.py. */
#include <stdio.h>

#include "rt.h"

enum { SEQ, JUMP, CALL, RET };

typedef struct Step {
    int kind;
    uint32_t dest; /* destino si no es SEQ */
    int thumb;     /* modo en el destino */
    uint32_t next; /* direccion de la instruccion siguiente */
} Step;

static int cond_ok(Cpu *c, uint32_t cond) {
    switch (cond) {
    case 0: return c->fz;
    case 1: return !c->fz;
    case 2: return c->fc;
    case 3: return !c->fc;
    case 4: return c->fn;
    case 5: return !c->fn;
    case 6: return c->fv;
    case 7: return !c->fv;
    case 8: return c->fc && !c->fz;
    case 9: return !c->fc || c->fz;
    case 10: return c->fn == c->fv;
    case 11: return c->fn != c->fv;
    case 12: return !c->fz && c->fn == c->fv;
    case 13: return c->fz || c->fn != c->fv;
    default: return 1;
    }
}

/* Desplazamiento por inmediato con las reglas de ARM (cantidad 0 = casos especiales) */
static uint32_t shift_imm(Cpu *c, int type, uint32_t v, uint32_t amt, uint32_t *co) {
    switch (type) {
    case 0:
        if (!amt) return v;
        *co = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1:
        if (!amt) { *co = v >> 31; return 0; }
        *co = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (!amt) { *co = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *co = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (!amt) { *co = v & 1; return (c->fc << 31) | (v >> 1); }
        *co = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}

/* Operacion de datos. Devuelve 1 si escribe el resultado en rd. */
static int alu(Cpu *c, int op, int flags, uint32_t a, uint32_t b, uint32_t carry, uint32_t *res) {
    int test = (op >= 8 && op <= 11);
    uint32_t r;
    int logical = 0;
    switch (op) {
    case 0: case 8: r = a & b; logical = 1; break;
    case 1: case 9: r = a ^ b; logical = 1; break;
    case 12: r = a | b; logical = 1; break;
    case 13: r = b; logical = 1; break;
    case 14: r = a & ~b; logical = 1; break;
    case 15: r = ~b; logical = 1; break;
    case 2: case 10: r = (flags || test) ? add_f(c, a, ~b, 1) : a - b; break;
    case 3: r = flags ? add_f(c, b, ~a, 1) : b - a; break;
    case 4: case 11: r = (flags || test) ? add_f(c, a, b, 0) : a + b; break;
    case 5: r = flags ? add_f(c, a, b, c->fc) : a + b + c->fc; break;
    case 6: r = flags ? add_f(c, a, ~b, c->fc) : a + ~b + c->fc; break;
    default: r = flags ? add_f(c, b, ~a, c->fc) : b + ~a + c->fc; break;
    }
    if (logical && (flags || test)) {
        c->fn = r >> 31;
        c->fz = (r == 0);
        c->fc = carry;
    }
    *res = r;
    return !test;
}

static void jump(Step *s, int kind, uint32_t dest, int thumb) {
    s->kind = kind;
    s->dest = dest & ~1u;
    s->thumb = thumb;
}

/* kind: 'w' palabra, 'b' byte, 'h' media palabra, 'B' byte con signo, 'H' media con signo */
static uint32_t load(char kind, uint32_t ea) {
    switch (kind) {
    case 'w': return mem_r32(ea);
    case 'b': return mem_r8(ea);
    case 'h': return mem_r16(ea);
    case 'B': return (uint32_t)(int32_t)(int8_t)mem_r8(ea);
    default: return (uint32_t)(int32_t)(int16_t)mem_r16(ea);
    }
}

static void store(char kind, uint32_t ea, uint32_t v) {
    if (kind == 'w') mem_w32(ea, v);
    else if (kind == 'b') mem_w8(ea, v);
    else mem_w16(ea, v);
}

/* Transferencia en bloque (ldm/stm/push/pop). */
static void block(Cpu *c, Step *s, uint32_t pc, int is_load, int rn, uint32_t list, int P, int U, int W) {
    int n = __builtin_popcount(list);
    uint32_t base = c->r[rn], ad, final;
    if (U) { ad = P ? base + 4 : base; final = base + 4 * n; }
    else { ad = P ? base - 4 * n : base - 4 * n + 4; final = base - 4 * n; }
    if (is_load) {
        if (W && !(list & (1u << rn))) c->r[rn] = final;
        for (int i = 0; i < 15; i++)
            if (list & (1u << i)) { c->r[i] = mem_r32(ad); ad += 4; }
        if (list & 0x8000) {
            uint32_t t = mem_r32(ad);
            jump(s, RET, t, t & 1);
        }
    } else {
        for (int i = 0; i < 16; i++)
            if (list & (1u << i)) { mem_w32(ad, i == 15 ? pc + 12 : c->r[i]); ad += 4; }
        if (W) c->r[rn] = final;
    }
}

static void step_arm(Cpu *c, uint32_t pc, Step *s) {
    uint32_t w = mem_r32(pc), *r = c->r;
    uint32_t cond = w >> 28;
    s->kind = SEQ;
    s->next = pc + 4;
#define R(n) ((n) == 15 ? pc + 8 : r[n])

    if (cond == 15) {
        if ((w & 0x0E000000) == 0x0A000000) { /* blx inmediato */
            int32_t off = (int32_t)(w << 8) >> 8;
            r[14] = pc + 4;
            jump(s, CALL, pc + 8 + off * 4 + ((w >> 23) & 2), 1);
            return;
        }
        rt_die(c, "interprete: instruccion ARM cond=1111 en %08x (%08x)", pc, w);
    }
    if (!cond_ok(c, cond)) return;

    if ((w & 0x0FFFFFF0) == 0x012FFF10) { /* bx */
        uint32_t t = R(w & 15);
        jump(s, (w & 15) == 14 ? RET : JUMP, t, t & 1);
        return;
    }
    if ((w & 0x0FFFFFF0) == 0x012FFF30) { /* blx registro */
        uint32_t t = R(w & 15);
        r[14] = pc + 4;
        jump(s, CALL, t, t & 1);
        return;
    }
    if ((w & 0x0FFF0FF0) == 0x016F0F10) { r[(w >> 12) & 15] = clz32(R(w & 15)); return; }
    if ((w & 0x0FBF0FFF) == 0x010F0000) { r[(w >> 12) & 15] = cpsr_read(c, (w >> 22) & 1); return; }
    if ((w & 0x0FB0FFF0) == 0x0120F000) { cpsr_write(c, R(w & 15), (w >> 16) & 15, (w >> 22) & 1); return; }
    if ((w & 0x0FB0F000) == 0x0320F000) {
        uint32_t rot = ((w >> 8) & 15) * 2, imm = w & 0xFF;
        cpsr_write(c, rot ? (imm >> rot) | (imm << (32 - rot)) : imm, (w >> 16) & 15, (w >> 22) & 1);
        return;
    }
    if ((w & 0x0FC000F0) == 0x00000090) { /* mul / mla */
        uint32_t res = R(w & 15) * R((w >> 8) & 15);
        if (w & (1 << 21)) res += R((w >> 12) & 15);
        r[(w >> 16) & 15] = res;
        if (w & (1 << 20)) { c->fn = res >> 31; c->fz = (res == 0); }
        return;
    }
    if ((w & 0x0F8000F0) == 0x00800090) { /* multiplicaciones largas */
        int hi = (w >> 16) & 15, lo = (w >> 12) & 15;
        uint64_t res;
        if (w & (1 << 22)) res = (uint64_t)((int64_t)(int32_t)R(w & 15) * (int64_t)(int32_t)R((w >> 8) & 15));
        else res = (uint64_t)R(w & 15) * (uint64_t)R((w >> 8) & 15);
        if (w & (1 << 21)) res += ((uint64_t)r[hi] << 32) | r[lo];
        r[lo] = (uint32_t)res;
        r[hi] = (uint32_t)(res >> 32);
        if (w & (1 << 20)) { c->fn = (uint32_t)(res >> 63); c->fz = (res == 0); }
        return;
    }
    if ((w & 0x0FB00FF0) == 0x01000090) { /* swp */
        uint32_t ea = R((w >> 16) & 15), t;
        if (w & (1 << 22)) { t = mem_r8(ea); mem_w8(ea, R(w & 15)); }
        else { t = mem_r32(ea); mem_w32(ea, R(w & 15)); }
        r[(w >> 12) & 15] = t;
        return;
    }
    if ((w & 0x0F900090) == 0x01000080 || (w & 0x0F9000F0) == 0x01000050 || (w & 0x0FF000F0) == 0x01200070)
        rt_die(c, "interprete: instruccion ARM sin implementar en %08x (%08x)", pc, w);

    uint32_t top = (w >> 25) & 7;
    if ((w & 0x0E000090) == 0x00000090 && (w & 0x60)) { /* medias palabras y bytes con signo */
        int P = (w >> 24) & 1, U = (w >> 23) & 1, I = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        int rn = (w >> 16) & 15, rd = (w >> 12) & 15, sh = (w >> 5) & 3;
        uint32_t off = I ? (((w >> 4) & 0xF0) | (w & 15)) : R(w & 15);
        uint32_t base = R(rn), wb = U ? base + off : base - off, ea = P ? wb : base;
        int writeback = !P || W;
        if (L) {
            uint32_t v = load(sh == 1 ? 'h' : sh == 2 ? 'B' : 'H', ea);
            if (writeback && rn != rd) r[rn] = wb;
            r[rd] = v;
        } else if (sh == 1) {
            store('h', ea, R(rd));
            if (writeback) r[rn] = wb;
        } else if (sh == 2) { /* ldrd */
            uint32_t v0 = mem_r32(ea), v1 = mem_r32(ea + 4);
            if (writeback) r[rn] = wb;
            r[rd] = v0;
            r[rd + 1] = v1;
        } else { /* strd */
            mem_w32(ea, r[rd]);
            mem_w32(ea + 4, r[rd + 1]);
            if (writeback) r[rn] = wb;
        }
        return;
    }
    if (top == 0 || top == 1) { /* operaciones de datos */
        int op = (w >> 21) & 15, flags = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        uint32_t carry = c->fc, b;
        if (w & (1 << 25)) {
            uint32_t rot = ((w >> 8) & 15) * 2, imm = w & 0xFF;
            b = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            if (rot) carry = b >> 31;
        } else if (w & 0x10) {
            b = sh_reg((w >> 5) & 3, R(w & 15), R((w >> 8) & 15), &carry);
        } else {
            b = shift_imm(c, (w >> 5) & 3, R(w & 15), (w >> 7) & 31, &carry);
        }
        uint32_t res;
        if (rd == 15 && flags && !(op >= 8 && op <= 11))
            rt_die(c, "interprete: retorno de excepcion en %08x", pc);
        if (alu(c, op, flags, (op == 13 || op == 15) ? 0 : R(rn), b, carry, &res)) {
            if (rd == 15) jump(s, (op == 13 && (w & 0xFFF) == 0x00E) ? RET : JUMP, res, 0);
            else r[rd] = res;
        }
        return;
    }
    if (top == 2 || top == 3) { /* ldr / str / ldrb / strb */
        int P = (w >> 24) & 1, U = (w >> 23) & 1, B = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        int rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        uint32_t off, dummy = 0;
        if (top == 3) {
            if (w & 0x10) rt_die(c, "interprete: instruccion ARM indefinida en %08x (%08x)", pc, w);
            off = shift_imm(c, (w >> 5) & 3, R(w & 15), (w >> 7) & 31, &dummy);
        } else {
            off = w & 0xFFF;
        }
        uint32_t base = R(rn), wb = U ? base + off : base - off, ea = P ? wb : base;
        int writeback = !P || W;
        if (L) {
            uint32_t v = load(B ? 'b' : 'w', ea);
            if (writeback && rn != rd) r[rn] = wb;
            if (rd == 15) jump(s, rn == 13 ? RET : JUMP, v, v & 1);
            else r[rd] = v;
        } else {
            store(B ? 'b' : 'w', ea, rd == 15 ? pc + 12 : r[rd]);
            if (writeback) r[rn] = wb;
        }
        return;
    }
    if (top == 4) { /* ldm / stm */
        if (w & (1 << 22)) rt_die(c, "interprete: ldm/stm con registros de usuario en %08x", pc);
        block(c, s, pc, (w >> 20) & 1, (w >> 16) & 15, w & 0xFFFF, (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1);
        return;
    }
    if (top == 5) { /* b / bl */
        int32_t off = (int32_t)(w << 8) >> 8;
        if (w & (1 << 24)) {
            r[14] = pc + 4;
            jump(s, CALL, pc + 8 + off * 4, 0);
        } else {
            jump(s, JUMP, pc + 8 + off * 4, 0);
        }
        return;
    }
    if ((w & 0x0F000010) == 0x0E000010) { /* mrc / mcr */
        int crn = (w >> 16) & 15, rd = (w >> 12) & 15, op2 = (w >> 5) & 7, crm = w & 15;
        if (((w >> 8) & 15) != 15) rt_die(c, "interprete: coprocesador distinto de cp15 en %08x", pc);
        if (w & (1 << 20)) r[rd] = cp15_read(c, crn, crm, op2);
        else cp15_write(c, crn, crm, op2, R(rd));
        return;
    }
    if ((w & 0x0F000000) == 0x0F000000) { swi_handler(c, (w >> 16) & 0xFF); return; }
    rt_die(c, "interprete: instruccion ARM no reconocida en %08x (%08x)", pc, w);
#undef R
}

static void step_thumb(Cpu *c, uint32_t pc, Step *s) {
    uint32_t h = mem_r16(pc), *r = c->r, res, carry = c->fc;
    s->kind = SEQ;
    s->next = pc + 2;
#define R(n) ((n) == 15 ? pc + 4 : r[n])
    uint32_t top5 = h >> 11;

    if (top5 == 0x1E) { /* bl / blx de dos mitades */
        uint32_t h2 = mem_r16(pc + 2);
        if ((h2 >> 11) == 0x1F || (h2 >> 11) == 0x1D) {
            int32_t off = (int32_t)((((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)) << 9) >> 9;
            uint32_t target = pc + 4 + off;
            int to_thumb = (h2 >> 12) & 1;
            if (!to_thumb) target &= ~3u;
            s->next = pc + 4;
            r[14] = (pc + 4) | 1;
            jump(s, CALL, target, to_thumb);
            return;
        }
    }
    if (top5 < 3) { /* desplazamiento por inmediato */
        uint32_t b = shift_imm(c, top5, r[(h >> 3) & 7], (h >> 6) & 31, &carry);
        alu(c, 13, 1, 0, b, carry, &res);
        r[h & 7] = res;
        return;
    }
    if (top5 == 3) { /* add/sub de 3 operandos */
        uint32_t b = (h & 0x400) ? ((h >> 6) & 7) : r[(h >> 6) & 7];
        alu(c, (h & 0x200) ? 2 : 4, 1, r[(h >> 3) & 7], b, carry, &res);
        r[h & 7] = res;
        return;
    }
    if ((h >> 13) == 1) { /* mov/cmp/add/sub con inmediato */
        static const int ops[4] = {13, 10, 4, 2};
        int rd = (h >> 8) & 7, op = ops[(h >> 11) & 3];
        if (alu(c, op, 1, r[rd], h & 0xFF, carry, &res)) r[rd] = res;
        return;
    }
    if ((h >> 10) == 0x10) { /* operaciones entre registros bajos */
        int rd = h & 7, rs = (h >> 3) & 7, op = (h >> 6) & 15;
        static const int map[16] = {0, 1, -1, -1, -1, 5, 6, -1, 8, -1, 10, 11, 12, -1, 14, 15};
        if (op == 2 || op == 3 || op == 4 || op == 7) {
            uint32_t b = sh_reg(op == 2 ? 0 : op == 3 ? 1 : op == 4 ? 2 : 3, r[rd], r[rs], &carry);
            alu(c, 13, 1, 0, b, carry, &res);
            r[rd] = res;
        } else if (op == 9) {
            alu(c, 3, 1, r[rs], 0, carry, &res);
            r[rd] = res;
        } else if (op == 13) {
            res = r[rd] * r[rs];
            r[rd] = res;
            c->fn = res >> 31;
            c->fz = (res == 0);
        } else if (alu(c, map[op], 1, r[rd], r[rs], carry, &res)) {
            r[rd] = res;
        }
        return;
    }
    if ((h >> 10) == 0x11) { /* registros altos / bx */
        int op = (h >> 8) & 3, rd = (h & 7) | ((h >> 4) & 8), rs = (h >> 3) & 15;
        if (op == 3) {
            uint32_t t = R(rs);
            if (h & 0x80) {
                r[14] = (pc + 2) | 1;
                jump(s, CALL, t, t & 1);
            } else if (rs == 15) {
                jump(s, JUMP, (pc + 4) & ~3u, 0);
            } else {
                jump(s, rs == 14 ? RET : JUMP, t, t & 1);
            }
            return;
        }
        if (op == 1) { alu(c, 10, 1, R(rd), R(rs), carry, &res); return; }
        res = op == 0 ? R(rd) + R(rs) : R(rs);
        if (rd == 15) jump(s, (op == 2 && rs == 14) ? RET : JUMP, res, 1);
        else r[rd] = res;
        return;
    }
    if (top5 == 9) { r[(h >> 8) & 7] = mem_r32(((pc + 4) & ~3u) + (h & 0xFF) * 4); return; }
    if ((h >> 12) == 5) { /* ldr/str con registro de desplazamiento */
        static const char kinds[8] = {'w', 'h', 'b', 'B', 'w', 'h', 'b', 'H'};
        int sel = (h >> 9) & 7, rd = h & 7;
        uint32_t ea = r[(h >> 3) & 7] + r[(h >> 6) & 7];
        if (sel >= 3) r[rd] = load(kinds[sel], ea);
        else store(kinds[sel], ea, r[rd]);
        return;
    }
    if ((h >> 13) == 3) { /* ldr/str con inmediato */
        int rd = h & 7, byte = (h >> 12) & 1;
        uint32_t ea = r[(h >> 3) & 7] + ((h >> 6) & 31) * (byte ? 1 : 4);
        if (h & 0x800) r[rd] = load(byte ? 'b' : 'w', ea);
        else store(byte ? 'b' : 'w', ea, r[rd]);
        return;
    }
    if ((h >> 12) == 8) { /* ldrh/strh con inmediato */
        uint32_t ea = r[(h >> 3) & 7] + ((h >> 6) & 31) * 2;
        if (h & 0x800) r[h & 7] = mem_r16(ea);
        else mem_w16(ea, r[h & 7]);
        return;
    }
    if ((h >> 12) == 9) { /* ldr/str relativo a sp */
        uint32_t ea = r[13] + (h & 0xFF) * 4;
        if (h & 0x800) r[(h >> 8) & 7] = mem_r32(ea);
        else mem_w32(ea, r[(h >> 8) & 7]);
        return;
    }
    if ((h >> 12) == 10) { /* add rd, pc/sp, #imm */
        r[(h >> 8) & 7] = ((h & 0x800) ? r[13] : ((pc + 4) & ~3u)) + (h & 0xFF) * 4;
        return;
    }
    if ((h >> 8) == 0xB0) { /* add sp, #imm */
        if (h & 0x80) r[13] -= (h & 0x7F) * 4;
        else r[13] += (h & 0x7F) * 4;
        return;
    }
    if ((h & 0xF600) == 0xB400) { /* push / pop */
        uint32_t list = h & 0xFF;
        if (h & 0x800) block(c, s, pc, 1, 13, list | ((h & 0x100) ? 0x8000 : 0), 0, 1, 1);
        else block(c, s, pc, 0, 13, list | ((h & 0x100) ? 0x4000 : 0), 1, 0, 1);
        return;
    }
    if ((h >> 12) == 12) { /* stmia / ldmia */
        block(c, s, pc, (h >> 11) & 1, (h >> 8) & 7, h & 0xFF, 0, 1, 1);
        return;
    }
    if ((h >> 12) == 13) { /* salto condicional / swi */
        uint32_t cond = (h >> 8) & 15;
        if (cond == 15) { swi_handler(c, h & 0xFF); return; }
        if (cond == 14) rt_die(c, "interprete: instruccion Thumb indefinida en %08x", pc);
        if (cond_ok(c, cond)) jump(s, JUMP, pc + 4 + (int8_t)(h & 0xFF) * 2, 1);
        return;
    }
    if (top5 == 0x1C) { /* salto incondicional */
        jump(s, JUMP, pc + 4 + ((int32_t)(h << 21) >> 21) * 2, 1);
        return;
    }
    rt_die(c, "interprete: instruccion Thumb no reconocida en %08x (%04x)", pc, h);
#undef R
}

/* Para las pruebas: ejecuta una sola instruccion. */
void interp_one(Cpu *c, uint32_t pc, int thumb) {
    Step s;
    if (thumb) step_thumb(c, pc, &s); else step_arm(c, pc, &s);
}

/* Direcciones por las que se ha entrado al interprete: se apuntan para traducirlas la proxima vez */
static void note_entry(uint32_t target) {
    static uint32_t seen[512];
    static int count;
    for (int i = 0; i < count; i++)
        if (seen[i] == target) return;
    if (count < 512) seen[count++] = target;
    FILE *f = fopen("missing_calls.txt", "a");
    if (f) {
        fprintf(f, "%08x%c", target, 10);
        fclose(f);
    }
    fprintf(stderr, "[interprete] codigo sin traducir en %08x: se interpreta%c", target, 10);
}

#define MAX_DEPTH 256

void interp_run(Cpu *c, uint32_t target) {
    uint32_t ret = c->r[14];
    uint32_t shadow[MAX_DEPTH]; /* direcciones de retorno de las llamadas hechas dentro del interprete */
    int depth = 0;
    uint32_t pc = target & ~1u;
    int thumb = target & 1;
    note_entry(target);

    for (;;) {
        Step s;
        if (thumb) step_thumb(c, pc, &s); else step_arm(c, pc, &s);
        if (s.kind == SEQ) {
            pc = s.next;
            continue;
        }
        uint32_t key = s.dest | (uint32_t)s.thumb;
        /* "mov lr, pc" seguido de un salto: es una llamada */
        if (s.kind == JUMP && (c->r[14] & ~1u) == s.next) s.kind = CALL;

        if (s.kind == CALL) {
            uint32_t back = c->r[14];
            if (rt_is_function(key)) {
                call_indirect(c, key);
            } else {
                if (depth == MAX_DEPTH) rt_die(c, "interprete: demasiadas llamadas anidadas");
                shadow[depth++] = back;
                pc = s.dest;
                thumb = s.thumb;
                continue;
            }
            pc = back & ~1u;
            thumb = back & 1;
            continue;
        }
        if (s.dest <= pc && !--g_budget) watchdog(c, pc);

        if (depth && ((s.dest ^ shadow[depth - 1]) & ~1u) == 0) { /* vuelve a quien llamo, aqui dentro */
            depth--;
            pc = s.dest;
            thumb = s.thumb;
            continue;
        }
        if (!depth && ((s.dest ^ ret) & ~1u) == 0) return; /* vuelve al codigo traducido */
        if (rt_is_function(key)) { /* salto a una funcion traducida: se ejecuta y se da por retornado */
            call_indirect(c, key);
            if (!depth) return;
            uint32_t back = shadow[--depth];
            pc = back & ~1u;
            thumb = back & 1;
            continue;
        }
        if (s.kind == RET && !depth) return; /* retorno con un lr que ya no valia: basta con volver */
        pc = s.dest;
        thumb = s.thumb;
    }
}
