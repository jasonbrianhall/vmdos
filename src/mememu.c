/* Emulates the one instruction that touched trapped video memory (the
   EGA/VGA 16-color window at A0000h, whose pages are left unmapped so that
   every access goes through the VGA's latches and write logic). Handles
   what programs use on video memory: MOV, MOVZX/MOVSX, the ALU group,
   TEST, XCHG, INC/DEC, NOT/NEG, shifts/rotates by 1/CL/imm, and the string
   instructions MOVS/STOS/LODS (with REP, all iterations at once), in 16- and
   32-bit code. Memory outside the window is accessed directly (the guest
   and the kernel share the address space). */
#include "kernel.h"

static struct emu_cpu *E;
static int osz, asz;                 /* operand size (2/4; 1 for byte ops), address size (2/4) */
static int seg_ovr;                  /* -1 or 0..5 (ES CS SS DS FS GS) */
static u32 ip;                       /* offset of the next code byte */
static int probe, probe_bad, probe_win;  /* dry run: accesses checked, not done */

enum { ES, CS, SS, DS, FS, GS };

/* The dry run's view of linear a: 0 can't be touched (the guest would
   fault), 1 memory, 2 trapped memory that is emulated. In VMHV through the
   guest's page tables (hv.c); otherwise the guest and the kernel share the
   address space, and only the Voodoo 3's BARs count as trapped (batches
   are run for them alone, see mem_emulate_more). */
static int probe_check(u32 a, int write)
{
    if (hv_active) return hv_emu_check(a, write);
    if (v3_on && v3_window(a)) return 2;
    return page_user(a, write);
}

static u8 fetch8(void)
{
    u32 a = E->seg_base[CS] + (E->code32 ? ip : (ip & 0xFFFF));
    ip++;
    if (probe && probe_check(a, 0) != 1) { probe_bad = 1; return 0x90; }
    return hv_active ? hv_emu_rd8(a) : rd8(a);
}
static u16 fetch16(void) { u16 v = fetch8(); return v | (u16)(fetch8() << 8); }
static u32 fetch32(void) { u32 v = fetch16(); return v | (u32)fetch16() << 16; }

/* ---------------- memory ---------------- */
/* The emulated Voodoo 3's registers or frame buffer (voodoo3.h) behind
   linear a: its address on the card's bus in *p. Its accesses go through
   whole (a dword register write is one write, not four bytes). */
static int v3_at(u32 a, u32 *p)
{
    if (!v3_on) return 0;
    if (hv_active) { if (hv_emu_gpa(a, p)) return 0; }
    else *p = a;
    return v3_window(*p);
}

/* In VMHV the addresses are the guest's linear ones: hv.c walks its page
   tables and finds the window by guest-physical address. */
static u8 mrd8(u32 a)
{
    u32 p;
    if (!probe && v3_at(a, &p)) return (u8)v3_mmio_read(p, 1);
    if (probe) {
        int c = probe_check(a, 0);
        if (c == 2) { probe_win = 1; return 0; }
        if (!c) { probe_bad = 1; return 0; }
    }
    return hv_active ? hv_emu_rd8(a) : vga16_window(a) ? vga16_read(a) : rd8(a);
}
static void mwr8(u32 a, u8 v)
{
    u32 p;
    if (!probe && v3_at(a, &p)) { v3_mmio_write(p, v, 1); return; }
    if (probe) {                                   /* only window writes: no guest page faults to miss */
        if (probe_check(a, 1) == 2) probe_win = 1; else probe_bad = 1;
        return;
    }
    if (hv_active) hv_emu_wr8(a, v);
    else if (vga16_window(a)) vga16_write(a, v);
    else wr8(a, v);
}
static u32 mrd(u32 a, int sz)
{
    u32 p, q;
    if (!probe && sz > 1 && v3_at(a, &p) && v3_at(a + sz - 1, &q) && q - p == (u32)sz - 1) return v3_mmio_read(p, sz);
    u32 v = 0;
    for (int i = 0; i < sz; i++) v |= (u32)mrd8(a + i) << (8 * i);
    return v;
}
static void mwr(u32 a, u32 v, int sz)
{
    u32 p, q;
    if (!probe && sz > 1 && v3_at(a, &p) && v3_at(a + sz - 1, &q) && q - p == (u32)sz - 1) { v3_mmio_write(p, v, sz); return; }
    for (int i = 0; i < sz; i++) mwr8(a + i, (u8)(v >> (8 * i)));
}

/* ---------------- registers ---------------- */
static u32 rget(int r, int sz)
{
    if (sz == 1) return r < 4 ? *E->gpr[r] & 0xFF : (*E->gpr[r - 4] >> 8) & 0xFF;
    return sz == 2 ? *E->gpr[r] & 0xFFFF : *E->gpr[r];
}
static void rset(int r, u32 v, int sz)
{
    if (sz == 1) {
        if (r < 4) *E->gpr[r] = (*E->gpr[r] & ~0xFFu) | (v & 0xFF);
        else *E->gpr[r - 4] = (*E->gpr[r - 4] & ~0xFF00u) | (v & 0xFF) << 8;
    } else if (sz == 2) *E->gpr[r] = (*E->gpr[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else *E->gpr[r] = v;
}

/* ---------------- ModRM ---------------- */
static int mod_, reg_, rm_;
static u8 modrm_byte;
static u32 ea, ea_off;                 /* linear address, offset when mod != 3 */

static u32 segbase(int def) { return E->seg_base[seg_ovr >= 0 ? seg_ovr : def]; }

static void modrm(void)
{
    u8 m = fetch8();
    modrm_byte = m;
    mod_ = m >> 6; reg_ = (m >> 3) & 7; rm_ = m & 7;
    if (mod_ == 3) return;
    u32 off = 0;
    int def = DS;
    if (asz == 2) {
        static const signed char b1[8] = { 3, 3, 5, 5, 6, 7, 5, 3 }, b2[8] = { 6, 7, 6, 7, -1, -1, -1, -1 };
        if (mod_ == 0 && rm_ == 6) off = fetch16();
        else {
            off = *E->gpr[(int)b1[rm_]] + (b2[rm_] >= 0 ? *E->gpr[(int)b2[rm_]] : 0);
            if (rm_ == 2 || rm_ == 3 || rm_ == 6) def = SS;
            if (mod_ == 1) off += (u32)(int)(signed char)fetch8();
            else if (mod_ == 2) off += fetch16();
        }
        off &= 0xFFFF;
    } else {
        if (rm_ == 4) {
            u8 sib = fetch8();
            int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && mod_ == 0) off = fetch32();
            else { off = *E->gpr[base]; if (base == 4 || base == 5) def = SS; }
            if (idx != 4) off += *E->gpr[idx] << sc;
        } else if (rm_ == 5 && mod_ == 0) off = fetch32();
        else { off = *E->gpr[rm_]; if (rm_ == 5) def = SS; }
        if (mod_ == 1) off += (u32)(int)(signed char)fetch8();
        else if (mod_ == 2) off += fetch32();
    }
    ea_off = off;
    ea = segbase(def) + off;
}

static u32 rm_get(int sz) { return mod_ == 3 ? rget(rm_, sz) : mrd(ea, sz); }
static void rm_set(u32 v, int sz) { if (mod_ == 3) rset(rm_, v, sz); else mwr(ea, v, sz); }

/* ---------------- flags ---------------- */
#define ARITH (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)

static u32 szmask(int sz) { return sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFFu; }

static void set_szp(u32 r, int sz, u32 *f)
{
    r &= szmask(sz);
    if (!r) *f |= EFL_ZF;
    if (r & (1u << (sz * 8 - 1))) *f |= EFL_SF;
    u8 p = (u8)r; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) *f |= EFL_PF;
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static u32 alu(int op, u32 a, u32 b, int sz)
{
    u32 m = szmask(sz), sign = 1u << (sz * 8 - 1), cin = *E->eflags & EFL_CF, r, f = 0;
    a &= m; b &= m;
    switch (op) {
    case 0: case 2: {
        u32 c = op == 2 ? cin : 0;
        r = (a + b + c) & m;
        if ((u64)a + b + c > m) f |= EFL_CF;
        if ((a ^ r) & (b ^ r) & sign) f |= EFL_OF;
        if ((a ^ b ^ r) & 0x10) f |= EFL_AF;
        break; }
    case 3: case 5: case 7: {
        u32 c = op == 3 ? cin : 0;
        r = (a - b - c) & m;
        if ((u64)b + c > a) f |= EFL_CF;
        if ((a ^ b) & (a ^ r) & sign) f |= EFL_OF;
        if ((a ^ b ^ r) & 0x10) f |= EFL_AF;
        break; }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    default: r = a ^ b; break;
    }
    set_szp(r, sz, &f);
    *E->eflags = (*E->eflags & ~ARITH) | f;
    return r;
}

static u32 incdec(u32 a, int dec, int sz)
{
    u32 cf = *E->eflags & EFL_CF;
    u32 r = alu(dec ? 5 : 0, a, 1, sz);
    *E->eflags = (*E->eflags & ~EFL_CF) | cf;
    return r;
}

/* Shift group (C0/C1/D0-D3): 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SAL 7 SAR */
static u32 shift(int op, u32 a, u32 n, int sz)
{
    int bits = sz * 8;
    u32 m = szmask(sz), sign = 1u << (bits - 1);
    n &= 31;
    if (!n) return a & m;
    a &= m;
    u32 orig = a, f = *E->eflags, cf = f & EFL_CF;
    for (u32 i = 0; i < n; i++) {
        u32 hi = a & sign, lo = a & 1;
        switch (op) {
        case 0: a = ((a << 1) | (hi ? 1 : 0)) & m; cf = hi ? 1 : 0; break;
        case 1: a = (a >> 1) | (lo ? sign : 0); cf = lo; break;
        case 2: a = ((a << 1) | cf) & m; cf = hi ? 1 : 0; break;
        case 3: a = (a >> 1) | (cf ? sign : 0); cf = lo; break;
        case 4: case 6: a = (a << 1) & m; cf = hi ? 1 : 0; break;
        case 5: a >>= 1; cf = lo; break;
        default: a = (a >> 1) | hi; cf = lo; break;
        }
    }
    f &= ~(EFL_CF | EFL_OF);
    if (cf) f |= EFL_CF;
    int msb = (a & sign) != 0;
    if (op == 0 || op == 2 || op == 4 || op == 6) { if (msb ^ (int)cf) f |= EFL_OF; }
    else if (op == 1 || op == 3) { if (msb ^ ((a >> (bits - 2)) & 1)) f |= EFL_OF; }
    else if (op == 5) { if (orig & sign) f |= EFL_OF; }
    if (op >= 4) { f &= ~(EFL_ZF | EFL_SF | EFL_PF); set_szp(a, sz, &f); }
    *E->eflags = f;
    return a;
}

/* ---------------- string instructions ---------------- */
static u32 idx_get(int r) { return asz == 2 ? *E->gpr[r] & 0xFFFF : *E->gpr[r]; }
static void idx_add(int r, int d) { rset(r, idx_get(r) + (u32)d, asz); }

static int string_op(u8 op, int rep)
{
    int sz = (op & 1) ? osz : 1;
    int step = (*E->eflags & EFL_DF) ? -sz : sz;
    u32 n = rep ? idx_get(1) : 1;               /* CX/ECX */
    for (; n; n--) {
        switch (op) {
        case 0xA4: case 0xA5:
            mwr(E->seg_base[ES] + idx_get(7), mrd(segbase(DS) + idx_get(6), sz), sz);
            idx_add(6, step); idx_add(7, step);
            break;
        case 0xAA: case 0xAB:
            mwr(E->seg_base[ES] + idx_get(7), rget(0, sz), sz);
            idx_add(7, step);
            break;
        case 0xAC: case 0xAD:
            rset(0, mrd(segbase(DS) + idx_get(6), sz), sz);
            idx_add(6, step);
            break;
        default: return 0;
        }
        if (rep) rset(1, n - 1, asz);
    }
    return 1;
}

static int cond(int cc)
{
    u32 f = *E->eflags;
    int cf = !!(f & EFL_CF), zf = !!(f & EFL_ZF), sf = !!(f & EFL_SF), of = !!(f & EFL_OF), pf = !!(f & EFL_PF), r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf | zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf | (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---------------- x87 ----------------
   A float stored straight into trapped memory (Glide writing vertices into
   the Voodoo's command FIFO: FSTP DWORD [EDI]). The guest's x87 state is
   live in the CPU (vmdos never switches it), so the instruction itself is
   run here, its memory operand changed to a buffer, and the buffer is then
   written to (or was first read from) the trapped memory. */
static u8 x87_code[16] __attribute__((aligned(16)));
static u8 x87_buf[16] __attribute__((aligned(16)));

/* Bytes of memory operand an x87 op reads (*rd) or writes (*wr); 0: none
   of the forms done here. */
static int x87_size(u8 op, int reg, int *rd, int *wr)
{
    *rd = *wr = 0;
    switch (op) {
    case 0xD8: *rd = 1; return 4;                       /* FADD.. m32real */
    case 0xDA: *rd = 1; return 4;                       /* FIADD.. m32int */
    case 0xDC: *rd = 1; return 8;                       /* FADD.. m64real */
    case 0xDE: *rd = 1; return 2;                       /* FIADD.. m16int */
    case 0xD9:
        if (reg == 0) { *rd = 1; return 4; }            /* FLD m32 */
        if (reg == 2 || reg == 3) { *wr = 1; return 4; }   /* FST(P) m32 */
        if (reg == 5) { *rd = 1; return 2; }            /* FLDCW */
        if (reg == 7) { *wr = 1; return 2; }            /* FNSTCW */
        return 0;
    case 0xDB:
        if (reg == 0) { *rd = 1; return 4; }            /* FILD m32 */
        if (reg >= 1 && reg <= 3) { *wr = 1; return 4; }   /* FISTTP FIST FISTP m32 */
        if (reg == 5) { *rd = 1; return 10; }           /* FLD m80 */
        if (reg == 7) { *wr = 1; return 10; }           /* FSTP m80 */
        return 0;
    case 0xDD:
        if (reg == 0) { *rd = 1; return 8; }            /* FLD m64 */
        if (reg >= 1 && reg <= 3) { *wr = 1; return 8; }   /* FISTTP FST FSTP m64 */
        if (reg == 7) { *wr = 1; return 2; }            /* FNSTSW */
        return 0;
    case 0xDF:
        if (reg == 0) { *rd = 1; return 2; }            /* FILD m16 */
        if (reg >= 1 && reg <= 3) { *wr = 1; return 2; }   /* FISTTP FIST FISTP m16 */
        if (reg == 5) { *rd = 1; return 8; }            /* FILD m64 */
        if (reg == 7) { *wr = 1; return 8; }            /* FISTP m64 */
        return 0;
    }
    return 0;
}

static int x87_op(u8 op)
{
    modrm();
    u8 *c = x87_code;
    if (mod_ == 3) {                                    /* registers only: as it is */
        if (probe) return 1;
        c[0] = op; c[1] = modrm_byte; c[2] = 0xC3;
    } else {
        int rd, wr, n = x87_size(op, reg_, &rd, &wr);
        if (!n) return 0;
        if (rd) for (int i = 0; i < n; i++) x87_buf[i] = mrd8(ea + i);
        else if (probe) mwr8(ea, 0);                    /* the dry run: is it the window */
        if (probe) return 1;
        u32 b = (u32)(uintptr_t)x87_buf;
        c[0] = op; c[1] = (u8)(reg_ << 3 | 5);          /* [disp32]: the buffer */
        c[2] = (u8)b; c[3] = (u8)(b >> 8); c[4] = (u8)(b >> 16); c[5] = (u8)(b >> 24);
        c[6] = 0xC3;
        __asm__ volatile("call *%0" :: "r"(c) : "memory", "cc");
        if (wr) {
            u32 i = 0;
            for (; i + 4 <= (u32)n; i += 4) mwr(ea + i, *(u32 *)(x87_buf + i), 4);
            for (; i + 2 <= (u32)n; i += 2) mwr(ea + i, *(u16 *)(x87_buf + i), 2);
        }
        return 1;
    }
    __asm__ volatile("call *%0" :: "r"(c) : "memory", "cc");
    return 1;
}

/* ---------------- one instruction ---------------- */
int mem_emulate(struct emu_cpu *e)
{
    E = e;
    ip = *e->eip;
    seg_ovr = -1;
    int opsz32 = e->code32, adsz32 = e->code32, rep = 0;
    u8 op;
    for (int n = 0;; n++) {
        if (n > 14) return 0;
        op = fetch8();
        switch (op) {
        case 0x26: seg_ovr = ES; continue;
        case 0x2E: seg_ovr = CS; continue;
        case 0x36: seg_ovr = SS; continue;
        case 0x3E: seg_ovr = DS; continue;
        case 0x64: seg_ovr = FS; continue;
        case 0x65: seg_ovr = GS; continue;
        case 0x66: opsz32 = !e->code32; continue;
        case 0x67: adsz32 = !e->code32; continue;
        case 0xF0: continue;
        case 0xF2: case 0xF3: rep = 1; continue;
        }
        break;
    }
    osz = opsz32 ? 4 : 2;
    asz = adsz32 ? 4 : 2;
    int sz = (op & 1) ? osz : 1;

    if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5)) {   /* ALU AL/eAX,imm */
        u32 r = alu(op >> 3, rget(0, sz), sz == 1 ? fetch8() : osz == 4 ? fetch32() : fetch16(), sz);
        if ((op >> 3) != 7) rset(0, r, sz);
    } else if (op < 0x40 && (op & 7) < 4) {              /* ALU r/m,r  r,r/m */
        int aop = op >> 3;
        modrm();
        if (op & 2) {
            u32 r = alu(aop, rget(reg_, sz), rm_get(sz), sz);
            if (aop != 7) rset(reg_, r, sz);
        } else {
            u32 r = alu(aop, rm_get(sz), rget(reg_, sz), sz);
            if (aop != 7) rm_set(r, sz);
        }
    } else switch (op) {
    case 0x80: case 0x81: case 0x83: {
        modrm();
        u32 imm = op == 0x81 ? (osz == 4 ? fetch32() : fetch16()) : fetch8();
        if (op == 0x83) imm = (u32)(int)(signed char)imm;
        u32 r = alu(reg_, rm_get(sz), imm, sz);
        if (reg_ != 7) rm_set(r, sz);
        break; }
    case 0x84: case 0x85: modrm(); alu(4, rm_get(sz), rget(reg_, sz), sz); break;
    case 0x86: case 0x87: {
        modrm();
        u32 a = rm_get(sz), b = rget(reg_, sz);
        rm_set(b, sz); rset(reg_, a, sz);
        break; }
    case 0x88: case 0x89: modrm(); rm_set(rget(reg_, sz), sz); break;
    case 0x8A: case 0x8B: modrm(); rset(reg_, rm_get(sz), sz); break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        u32 off = asz == 4 ? fetch32() : fetch16();
        u32 a = segbase(DS) + off;
        if (op < 0xA2) rset(0, mrd(a, sz), sz); else mwr(a, rget(0, sz), sz);
        break; }
    /* plain register and control-flow instructions: the rest of a copy loop (mem_emulate_more) */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        rset(op & 7, incdec(rget(op & 7, osz), op >= 0x48, osz), osz);
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        u32 rel = (u32)(int)(signed char)fetch8();
        if (cond(op & 15)) ip += rel;
        break; }
    case 0xEB: { u32 rel = (u32)(int)(signed char)fetch8(); ip += rel; break; }
    case 0xE9: { u32 rel = osz == 4 ? fetch32() : (u32)(int)(short)fetch16(); ip += rel; break; }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        u32 rel = (u32)(int)(signed char)fetch8();
        int take;
        if (op == 0xE3) take = idx_get(1) == 0;
        else {
            rset(1, idx_get(1) - 1, asz);
            take = idx_get(1) != 0;
            if (op == 0xE0) take = take && !(*E->eflags & EFL_ZF);
            if (op == 0xE1) take = take && (*E->eflags & EFL_ZF);
        }
        if (take) ip += rel;
        break; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        rset(op & 7, fetch8(), 1);
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        rset(op & 7, osz == 4 ? fetch32() : fetch16(), osz);
        break;
    case 0xA8: case 0xA9: alu(4, rget(0, sz), sz == 1 ? fetch8() : osz == 4 ? fetch32() : fetch16(), sz); break;
    case 0x8D: modrm(); if (mod_ == 3) return 0; rset(reg_, ea_off, osz); break;
    case 0x90: break;
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        if (!string_op(op, rep)) return 0;
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        if (!x87_op(op)) return 0;
        break;
    case 0xC6: case 0xC7: {
        modrm();
        u32 imm = op == 0xC6 ? fetch8() : osz == 4 ? fetch32() : fetch16();
        rm_set(imm, sz);
        break; }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        modrm();
        u32 cnt = op <= 0xC1 ? fetch8() : op <= 0xD1 ? 1 : rget(1, 1);
        rm_set(shift(reg_, rm_get(sz), cnt, sz), sz);
        break; }
    case 0xF6: case 0xF7:
        modrm();
        switch (reg_) {
        case 0: case 1: alu(4, rm_get(sz), sz == 1 ? fetch8() : osz == 4 ? fetch32() : fetch16(), sz); break;
        case 2: rm_set(~rm_get(sz), sz); break;
        case 3: {
            u32 a = rm_get(sz);
            u32 r = alu(5, 0, a, sz);
            rm_set(r, sz);
            break; }
        default: return 0;
        }
        break;
    case 0xFE: case 0xFF:
        modrm();
        if (reg_ > 1) return 0;
        rm_set(incdec(rm_get(sz), reg_, sz), sz);
        break;
    case 0x0F: {
        u8 op2 = fetch8();
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            int ssz = (op2 & 1) ? 2 : 1;
            modrm();
            u32 v = rm_get(ssz);
            if (op2 >= 0xBE) v = ssz == 1 ? (u32)(int)(signed char)v : (u32)(int)(short)v;
            rset(reg_, v, osz);
            break;
        }
        return 0; }
    default:
        return 0;
    }
    *e->eip = e->code32 ? ip : (ip & 0xFFFF);
    return 1;
}

/* After an emulated access to the trapped window: the instructions after it
   are most likely the rest of a copy or fill loop (the Windows driver's
   byte-at-a-time planar copies), each of which would fault again. Go on
   emulating them while each one, tried first as a dry run, touches only
   the window (or reads mapped memory) and is something the emulator does;
   stop at anything else, or after 16 instructions without a window access,
   and let the guest run on from there. In VMHV (the guest's page tables
   are walked for every access), and for the Voodoo 3 (a command FIFO or a
   texture written a dword at a time) with the guest's pages checked. */
int mem_emulate_more(struct emu_cpu *e)
{
    if ((!hv_active && !v3_on) || (*e->eflags & EFL_TF)) return 0;
    int n = 0, idle = 0;
    while (n < 4096 && idle < 16) {
        u32 r[8], rip = *e->eip, fl = *e->eflags;
        struct emu_cpu d = *e;
        for (int i = 0; i < 8; i++) { r[i] = *e->gpr[i]; d.gpr[i] = &r[i]; }
        d.eip = &rip; d.eflags = &fl;
        probe = 1; probe_bad = 0; probe_win = 0;
        int ok = mem_emulate(&d);
        probe = 0;
        if (!ok || probe_bad) break;
        if (!mem_emulate(e)) break;
        n++;
        idle = probe_win ? 0 : idle + 1;
    }
    return n;
}
