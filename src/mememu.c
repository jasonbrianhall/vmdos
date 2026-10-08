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

enum { ES, CS, SS, DS, FS, GS };

static u8 fetch8(void)
{
    u32 a = E->seg_base[CS] + (E->code32 ? ip : (ip & 0xFFFF));
    ip++;
    return rd8(a);
}
static u16 fetch16(void) { u16 v = fetch8(); return v | (u16)(fetch8() << 8); }
static u32 fetch32(void) { u32 v = fetch16(); return v | (u32)fetch16() << 16; }

/* ---------------- memory ---------------- */
static u8 mrd8(u32 a) { return vga16_window(a) ? vga16_read(a) : rd8(a); }
static void mwr8(u32 a, u8 v) { if (vga16_window(a)) vga16_write(a, v); else wr8(a, v); }
static u32 mrd(u32 a, int sz)
{
    u32 v = 0;
    for (int i = 0; i < sz; i++) v |= (u32)mrd8(a + i) << (8 * i);
    return v;
}
static void mwr(u32 a, u32 v, int sz) { for (int i = 0; i < sz; i++) mwr8(a + i, (u8)(v >> (8 * i))); }

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
static u32 ea;                         /* linear address when mod != 3 */

static u32 segbase(int def) { return E->seg_base[seg_ovr >= 0 ? seg_ovr : def]; }

static void modrm(void)
{
    u8 m = fetch8();
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

    if (op < 0x40 && (op & 7) < 4) {              /* ALU r/m,r  r,r/m */
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
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        if (!string_op(op, rep)) return 0;
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
