#!/usr/bin/env python3
"""Static recompiler: 32-bit x86 PE (FalloutNV.exe 1.4.0.525) -> C.

Usage: recomp.py --exe FalloutNV.exe --units fnv_decomp/build/units.json --out gen/

The player's own executable is the input; nothing derived from it is meant to be
committed or shared.  Output: gen/funcs_NNN.c, gen/table.c, gen/image.c (section
layout + import list for the loader; the code bytes are not copied, the loader
maps the player's exe at run time).
"""
import argparse, json, os, re, sys, struct, multiprocessing as mp
import pefile, capstone
from capstone import x86 as X

BASE = 0x400000
EXPECT_PROBES_OK = 12

# ----------------------------------------------------------------------------- image

class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        assert self.base == BASE
        self.mem = pe.get_memory_mapped_image()
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.sections = []
        for s in pe.sections:
            self.sections.append((s.Name.rstrip(b'\0').decode(), self.base + s.VirtualAddress, s.Misc_VirtualSize,
                                  s.PointerToRawData, s.SizeOfRawData, s.Characteristics))
        t = [s for s in self.sections if s[0] == '.text'][0]
        self.text_lo, self.text_hi = t[1], t[1] + t[2]
        self.relocs = set()
        for blk in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type == 3:
                    self.relocs.add(self.base + e.rva)
        self.imports = []  # (iat address, dll, name or '#ord')
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                self.imports.append((i.address, d.dll.decode(), i.name.decode() if i.name else '#%d' % i.ordinal))
        self.tls_cb = []
        if hasattr(pe, 'DIRECTORY_ENTRY_TLS'):
            t = pe.DIRECTORY_ENTRY_TLS.struct
            a = t.AddressOfCallBacks
            while a and self.u32(a):
                self.tls_cb.append(self.u32(a)); a += 4
            self.tls = (t.StartAddressOfRawData, t.EndAddressOfRawData, t.AddressOfIndex, t.SizeOfZeroFill)
        else:
            self.tls = None

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from('<I', self.mem, o)[0]

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def code(self, a, n=16):
        o = a - self.base
        return self.mem[o:o + n]


def check_image(img, check_path):
    import hashlib
    spec = json.load(open(check_path))
    ok = 0
    for address, size, digest in spec['probes']:
        a = int(address, 16) - BASE
        ok += hashlib.sha256(img.mem[a:a + size]).hexdigest() == digest
    return ok, len(spec['probes'])

# ----------------------------------------------------------------------------- pass 1: discovery

JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg',
       'jecxz', 'jcxz', 'loop', 'loope', 'loopne'}
RE_TABLE = re.compile(r'^dword ptr \[(?:e[a-z]{2}\*4 \+ )?(0x[0-9a-f]+)\]$')
RE_IMM = re.compile(r'^0x[0-9a-f]+$')


class Discover:
    def __init__(self, img, known):
        self.img = img
        self.known = known  # start -> size from units.json
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.skipdata = False
        self.starts = set()
        self.funcs = {}  # start -> dict(insns=[addr...], labels=set, tables={addr: [targets]}, info={addr:(size,mn,op)})
        self.cache = {}

    def dec(self, a):
        r = self.cache.get(a)
        if r is None:
            r = None
            for i in self.md.disasm_lite(self.img.code(a, 16), a, 1):
                r = (i[1], i[2], i[3])
            self.cache[a] = r if r else (0, '(bad)', '')
            r = self.cache[a]
        return r

    def table(self, tab, fstart):
        out = []
        lo, hi = fstart, fstart + self.known.get(fstart, 1 << 30)
        a = tab
        while a in self.img.relocs:
            t = self.img.u32(a)
            if not self.img.in_text(t) or (fstart in self.known and not (lo <= t < hi)):
                break
            out.append(t)
            a += 4
        return out

    def explore(self, f):
        insns = {}
        labels = set()
        tables = {}
        calls = []
        work = [f]
        while work:
            a = work.pop()
            while a not in insns:
                if a != f and a in self.starts:
                    break  # fell into another function: tail call, decided at emission
                size, mn, op = self.dec(a)
                insns[a] = (size, mn, op)
                if size == 0:
                    break
                nxt = a + size
                if mn in JCC:
                    t = int(op, 16)
                    if self.img.in_text(t):
                        labels.add(t)
                        if not (t != f and t in self.starts):
                            work.append(t)
                elif mn == 'jmp':
                    if RE_IMM.match(op):
                        t = int(op, 16)
                        if self.img.in_text(t) and not (t != f and t in self.starts):
                            labels.add(t); work.append(t)
                    else:
                        m = RE_TABLE.match(op)
                        if m:
                            tab = int(m.group(1), 16)
                            ts = self.table(tab, f)
                            if ts:
                                tables[a] = ts
                                for t in ts:
                                    if not (t != f and t in self.starts):
                                        labels.add(t); work.append(t)
                    break
                elif mn in ('ret', 'retf', 'int3', 'hlt', '(bad)', 'iretd', 'ud2'):
                    break
                elif mn == 'call':
                    if RE_IMM.match(op):
                        t = int(op, 16)
                        if self.img.in_text(t):
                            calls.append(t)
                a = nxt
        return insns, labels, tables, calls

    def run(self, seeds):
        self.starts = set(seeds)
        queue = sorted(self.starts)
        while queue:
            f = queue.pop()
            if f in self.funcs:
                continue
            insns, labels, tables, calls = self.explore(f)
            self.funcs[f] = dict(insns=insns, labels=labels, tables=tables)
            for t in calls:
                if t not in self.starts:
                    self.starts.add(t)
                    queue.append(t)
        return self.funcs

# ----------------------------------------------------------------------------- pass 2: emission

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
UT = {1: 'u8', 2: 'u16', 4: 'u32', 8: 'u64'}
ST = {1: 's8', 2: 's16', 4: 's32', 8: 's64'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
LFS = {1: '8', 2: '16', 4: '32'}
CC = {'o': 0, 'no': 1, 'b': 2, 'c': 2, 'nae': 2, 'ae': 3, 'nb': 3, 'nc': 3, 'e': 4, 'z': 4, 'ne': 5, 'nz': 5,
      'be': 6, 'na': 6, 'a': 7, 'nbe': 7, 's': 8, 'ns': 9, 'p': 10, 'pe': 10, 'np': 11, 'po': 11,
      'l': 12, 'nge': 12, 'ge': 13, 'nl': 13, 'le': 14, 'ng': 14, 'g': 15, 'nle': 15}
SYNC_OUT = 'c->r[0]=eax;c->r[1]=ecx;c->r[2]=edx;c->r[3]=ebx;c->r[4]=esp;c->r[5]=ebp;c->r[6]=esi;c->r[7]=edi;'
SYNC_IN = 'eax=c->r[0];ecx=c->r[1];edx=c->r[2];ebx=c->r[3];esp=c->r[4];ebp=c->r[5];esi=c->r[6];edi=c->r[7];'
LF = 'lf_op,lf_a,lf_b,lf_r,lf_ci'
FALL = 'f_all(%s)' % LF


STRING_OPS = {'movsb', 'movsw', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd', 'scasb', 'scasw', 'scasd',
              'cmpsb', 'cmpsw', 'cmpsd'}


class Unsupported(Exception):
    pass


def fn(a):
    return 'f_%08X' % a


# Trivial KERNEL32 functions emitted inline at `call dword ptr [IAT slot]` instead of going through the runtime:
# the game calls them hundreds of thousands of times a second. Each body matches the runtime's own version
# (rt/kernel32.c) and pops its stdcall arguments; `p` etc. are the arguments at [esp] before the call.
INLINE_IMPORTS = {
    'InterlockedIncrement': '{u32 p=M32(esp);eax=__atomic_add_fetch((u32*)HP(p),1,__ATOMIC_SEQ_CST);esp+=4;}',
    'InterlockedDecrement': '{u32 p=M32(esp);eax=__atomic_sub_fetch((u32*)HP(p),1,__ATOMIC_SEQ_CST);esp+=4;}',
    'InterlockedExchange': '{u32 p=M32(esp),v=M32(esp+4);eax=__atomic_exchange_n((u32*)HP(p),v,__ATOMIC_SEQ_CST);esp+=8;}',
    'InterlockedExchangeAdd': '{u32 p=M32(esp),v=M32(esp+4);eax=__atomic_fetch_add((u32*)HP(p),v,__ATOMIC_SEQ_CST);esp+=8;}',
    'InterlockedCompareExchange': '{u32 p=M32(esp),x=M32(esp+4),e=M32(esp+8);'
                                  '__atomic_compare_exchange_n((u32*)HP(p),&e,x,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);'
                                  'eax=e;esp+=12;}',
    'TlsGetValue': '{u32 i=M32(esp);eax=i<64?M32(c->fs_base+0xE10+4*i):0;c->last_error=0;M32(c->fs_base+0x34)=0;esp+=4;}',
    'GetCurrentThreadId': '{eax=M32(c->fs_base+0x24);}',
}


class Emitter:
    def __init__(self, img, starts):
        self.img = img
        self.starts = starts
        self.inline_iat = {a: INLINE_IMPORTS[n] for a, dll, n in img.imports
                           if dll.lower() == 'kernel32.dll' and n in INLINE_IMPORTS}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    # ---- operands
    def reg_r(self, r):
        if r in R32: return r
        if r in R16: return '(u16)%s' % R16[r]
        if r in R8L: return '(u8)%s' % R8L[r]
        if r in R8H: return '(u8)(%s>>8)' % R8H[r]
        raise Unsupported('reg ' + r)

    def reg_w(self, r, v):
        if r in R32: return '%s=(u32)(%s);' % (r, v)
        if r in R16: x = R16[r]; return '%s=(%s&0xffff0000u)|(u16)(%s);' % (x, x, v)
        if r in R8L: x = R8L[r]; return '%s=(%s&0xffffff00u)|(u8)(%s);' % (x, x, v)
        if r in R8H: x = R8H[r]; return '%s=(%s&0xffff00ffu)|((u32)(u8)(%s)<<8);' % (x, x, v)
        raise Unsupported('reg ' + r)

    def ea(self, i, o, noseg=False):
        m = o.mem
        parts = []
        if m.base: parts.append(self.reg_r(i.reg_name(m.base)))
        if m.index:
            ix = self.reg_r(i.reg_name(m.index))
            parts.append(ix if m.scale == 1 else '%s*%d' % (ix, m.scale))
        if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = '+'.join(parts)
        if m.segment and not noseg:
            s = i.reg_name(m.segment)
            if s == 'fs': e = 'c->fs_base+' + e
            elif s == 'gs': raise Unsupported('gs')
        return '(u32)(%s)' % e

    def is_mem(self, o): return o.type == X.X86_OP_MEM
    def is_reg(self, o): return o.type == X.X86_OP_REG
    def is_imm(self, o): return o.type == X.X86_OP_IMM

    def rd(self, i, o, size=None, eav='ea'):
        size = size or o.size
        if o.type == X.X86_OP_REG: return self.reg_r(i.reg_name(o.reg))
        if o.type == X.X86_OP_IMM: return '(%s)0x%xu' % (UT[size], o.imm & ((1 << (8 * size)) - 1))
        if o.type == X.X86_OP_MEM:
            return {1: 'M8', 2: 'M16', 4: 'M32', 8: 'M64'}[size] + '(%s)' % eav
        raise Unsupported('op')

    def wr(self, i, o, v, size=None, eav='ea'):
        size = size or o.size
        if o.type == X.X86_OP_REG: return self.reg_w(i.reg_name(o.reg), v)
        if o.type == X.X86_OP_MEM:
            return {1: 'M8', 2: 'M16', 4: 'M32', 8: 'M64'}[size] + '(%s)=(%s)(%s);' % (eav, UT[size], v)
        raise Unsupported('wr')

    def ea_decl(self, i, ops):
        for o in ops:
            if o.type == X.X86_OP_MEM:
                return 'u32 ea=%s;' % self.ea(i, o)
        return ''

    def setlf(self, kind, size, a='a', b='b', r='r', ci='0'):
        return 'lf_op=LF_%s%s;lf_a=%s;lf_b=%s;lf_r=%s;lf_ci=%s;' % (kind, LFS[size], a, b, r, ci)

    def cond(self, cc):
        return 'f_cond(%d,%s)' % (cc, LF)

    # ---- calls/jumps
    def call_direct(self, t, ret):
        pre = 'esp-=4;M32(esp)=0x%xu;' % ret
        if t in self.starts:
            return pre + SYNC_OUT + '%s(c);' % fn(t) + SYNC_IN
        return pre + SYNC_OUT + 'rt_call_ind(c,0x%xu);' % t + SYNC_IN

    def tail(self, t):
        if t in self.starts:
            return SYNC_OUT + '%s(c);return;' % fn(t)
        return SYNC_OUT + 'rt_call_ind(c,0x%xu);return;' % t

    # ---- main
    def function(self, f, info):
        insns, labels, tables = info['insns'], info['labels'], info['tables']
        out = ['void %s(Ctx *restrict c){u8 *const gm = g_guest_mem;' % fn(f),
               'u32 eax=c->r[0],ecx=c->r[1],edx=c->r[2],ebx=c->r[3],esp=c->r[4],ebp=c->r[5],esi=c->r[6],edi=c->r[7];',
               'int lf_op=LF_EXPL;u32 lf_a=0,lf_b=0,lf_r=c->eflags,lf_ci=0;(void)lf_a;(void)lf_b;(void)lf_ci;']
        addrs = sorted(insns)
        # the entry must come first; emit a goto if the entry is not the lowest address
        if addrs[0] != f:
            out.append('goto L_%X;' % f)
            labels = set(labels) | {f}
        prev_next = None
        for a in addrs:
            size, mn, op = insns[a]
            if prev_next is not None and prev_next != a:
                out.append(self.flow_to(prev_next, f, insns))
            out.append('L_%X:;' % a)
            prev_next = None
            if size == 0:
                out.append('rt_unimpl(c,0x%xu,"undecodable");return;' % a)
                continue
            try:
                i = next(self.md.disasm(self.img.code(a, 16), a, 1))
                code, falls = self.insn(i, f, insns, tables)
            except Unsupported as e:
                code, falls = 'rt_unimpl(c,0x%xu,"%s %s (%s)");return;' % (a, mn, op, str(e).replace('"', "'")), False
            out.append('/*%X %s %s*/{%s}' % (a, mn, op.replace('*/', '* /'), code))
            if falls:
                prev_next = a + size
        if prev_next is not None:
            out.append(self.flow_to(prev_next, f, insns))
        out.append('}')
        body = '\n'.join(out)
        used = set(re.findall(r'goto (L_[0-9A-F]+);', body))
        return re.sub(r'(L_[0-9A-F]+):;', lambda m: m.group(0) if m.group(1) in used else '', body)

    def flow_to(self, t, f, insns):
        if t in insns and not (t != f and t in self.starts):
            return 'goto L_%X;' % t
        return self.tail(t)

    def jump(self, t, f, insns):
        if t in insns and not (t != f and t in self.starts):
            return 'goto L_%X;' % t
        return self.tail(t)

    def insn(self, i, f, insns, tables):
        mn = i.mnemonic
        ops = i.operands
        nxt = i.address + i.size
        lock = i.prefix[0] == 0xf0
        rep = i.prefix[0] in (0xf3, 0xf2)
        if mn.startswith('lock '): mn = mn[5:]; lock = True
        if mn.startswith('repz '): mn = 'repe ' + mn[5:]
        if mn.startswith('repnz '): mn = 'repne ' + mn[6:]
        if (mn.startswith('repe ') or mn.startswith('repne ') or mn.startswith('rep ')) and \
                mn.split(' ', 1)[1] not in STRING_OPS | {'movsd'}:
            mn = mn.split(' ', 1)[1]
        if mn.startswith('rep ') or mn.startswith('repe ') or mn.startswith('repne '):
            pre, mn = mn.split(' ', 1)
            return self.string(i, mn, pre), True
        if mn.startswith('bnd '): mn = mn[4:]
        if mn in STRING_OPS and 'xmm' not in i.op_str:
            return self.string(i, mn, None), True

        # ---- control flow
        if mn == 'jmp':
            o = ops[0]
            if self.is_imm(o):
                return self.jump(o.imm & 0xffffffff, f, insns), False
            tgt = self.rd(i, o) if not self.is_mem(o) else 'M32(%s)' % self.ea(i, o)
            cases = ''
            for t in sorted(set(tables.get(i.address, []))):
                if t in insns and not (t != f and t in self.starts):
                    cases += 'case 0x%xu:goto L_%X;' % (t, t)
            return 'u32 t=%s;switch(t){%sdefault:%s rt_call_ind(c,t);return;}' % (tgt, cases, SYNC_OUT), False
        if mn in JCC:
            t = ops[0].imm & 0xffffffff
            if mn == 'jecxz': cond = 'ecx==0'
            elif mn == 'jcxz': cond = '(u16)ecx==0'
            elif mn == 'loop': return 'ecx--;if(ecx!=0){%s}' % self.jump(t, f, insns), True
            elif mn == 'loope': return 'ecx--;if(ecx!=0&&%s){%s}' % (self.cond(4), self.jump(t, f, insns)), True
            elif mn == 'loopne': return 'ecx--;if(ecx!=0&&%s){%s}' % (self.cond(5), self.jump(t, f, insns)), True
            else: cond = self.cond(CC[mn[1:]])
            return 'if(%s){%s}' % (cond, self.jump(t, f, insns)), True
        if mn == 'call':
            o = ops[0]
            if self.is_imm(o):
                return self.call_direct(o.imm & 0xffffffff, nxt), True
            if self.is_mem(o) and not o.mem.base and not o.mem.index and not o.mem.segment:
                body = self.inline_iat.get(o.mem.disp & 0xffffffff)
                if body: return body, True
            tgt = self.rd(i, o) if not self.is_mem(o) else 'M32(%s)' % self.ea(i, o)
            return 'u32 t=%s;esp-=4;M32(esp)=0x%xu;%srt_call_ind(c,t);%s' % (tgt, nxt, SYNC_OUT, SYNC_IN), True
        if mn == 'ret':
            n = ops[0].imm if ops else 0
            return 'esp+=%d;%sreturn;' % (4 + n, SYNC_OUT), False
        if mn in ('int3', 'int1'):
            return '%srt_int3(c,0x%xu);%s' % (SYNC_OUT, i.address, SYNC_IN), True
        if mn in ('nop', 'pause', 'wait', 'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetch',
                  'prefetchw', 'emms', 'lfence', 'sfence', 'mfence', 'fnop', 'fwait'):
            return '', True

        # ---- integer
        h = getattr(self, 'i_' + mn, None)
        if h:
            return h(i, ops, lock), True
        if mn.startswith('set') and mn[3:] in CC:
            return self.wr(i, ops[0], self.cond(CC[mn[3:]]), 1, self.ea(i, ops[0]) if self.is_mem(ops[0]) else 'ea'), True
        if mn.startswith('cmov') and mn[4:] in CC:
            d = self.ea_decl(i, ops)
            return '%sif(%s){%s}' % (d, self.cond(CC[mn[4:]]), self.wr(i, ops[0], self.rd(i, ops[1]))), True
        if mn.startswith('fcmov'):
            m = {'fcmovb': 2, 'fcmove': 4, 'fcmovbe': 6, 'fcmovu': 10, 'fcmovnb': 3, 'fcmovne': 5, 'fcmovnbe': 7,
                 'fcmovnu': 11}[mn]
            return 'if(%s)ST(0)=ST(%d);' % (self.cond(m), self.streg(i, ops[-1])), True
        if mn.startswith('f') or mn in ('fnstsw', 'fnstcw'):
            return self.x87(i, mn, ops), True
        return self.sse(i, mn, ops), True

    # --- integer instruction handlers
    def binop(self, i, ops, lock, kind, expr, store=True):
        d, s = ops
        size = d.size
        T = UT[size]
        decl = self.ea_decl(i, ops)
        b = self.rd(i, s, size)
        if lock and self.is_mem(d) and store:
            atom = {'+': 'add', '-': 'sub', '&': 'and', '|': 'or', '^': 'xor'}.get(expr)
            if atom and kind in ('ADD', 'SUB', 'LOG'):
                r = 'a%sb' % expr
                return '%s %s b=%s;%s a=__atomic_fetch_%s((%s*)HP(ea),b,__ATOMIC_SEQ_CST);%s r=(%s)(%s);%s' % (
                    decl, T, b, T, atom, T, T, r, self.setlf(kind, size))
        if kind in ('ADC', 'SBB'):
            r = '(%s)(a%sb%sci)' % (T, expr, expr)
            ci = 'ci'
            pre = 'u32 ci=f_cf(%s);' % LF
        else:
            r = '(%s)(a%sb)' % (T, expr)
            ci = '0'
            pre = ''
        code = '%s%s%s a=%s;%s b=%s;%s r=%s;' % (decl, pre, T, self.rd(i, d), T, b, T, r)
        if store:
            code += self.wr(i, d, 'r')
        return code + self.setlf(kind, size, ci=ci)

    def i_add(self, i, o, l): return self.binop(i, o, l, 'ADD', '+')
    def i_sub(self, i, o, l):
        if self.is_reg(o[0]) and self.is_reg(o[1]) and o[0].reg == o[1].reg:
            sz = o[0].size
            return self.wr(i, o[0], '0') + 'lf_op=LF_SUB%s;lf_a=0;lf_b=0;lf_r=0;lf_ci=0;' % LFS[sz]
        return self.binop(i, o, l, 'SUB', '-')
    def i_adc(self, i, o, l): return self.binop(i, o, l, 'ADC', '+')
    def i_sbb(self, i, o, l): return self.binop(i, o, l, 'SBB', '-')
    def i_cmp(self, i, o, l): return self.binop(i, o, l, 'SUB', '-', False)
    def i_and(self, i, o, l): return self.binop(i, o, l, 'LOG', '&')
    def i_or(self, i, o, l): return self.binop(i, o, l, 'LOG', '|')
    def i_xor(self, i, o, l):
        if self.is_reg(o[0]) and self.is_reg(o[1]) and o[0].reg == o[1].reg:
            sz = o[0].size
            return self.wr(i, o[0], '0') + 'lf_op=LF_LOG%s;lf_a=0;lf_b=0;lf_r=0;lf_ci=0;' % LFS[sz]
        return self.binop(i, o, l, 'LOG', '^')
    def i_test(self, i, o, l): return self.binop(i, o, l, 'LOG', '&', False)

    def i_mov(self, i, o, l):
        d, s = o
        if self.is_reg(d) and i.reg_name(d.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            return ''
        if self.is_reg(s) and i.reg_name(s.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            return self.wr(i, d, '0x23', 2 if self.is_mem(d) else d.size, self.ea(i, d) if self.is_mem(d) else 'ea')
        decl = self.ea_decl(i, o)
        return decl + self.wr(i, d, self.rd(i, s, d.size))

    def i_movzx(self, i, o, l):
        d, s = o
        return self.ea_decl(i, o) + self.wr(i, d, '(u32)%s' % self.rd(i, s))

    def i_movsx(self, i, o, l):
        d, s = o
        return self.ea_decl(i, o) + self.wr(i, d, '(u32)(s32)(%s)%s' % (ST[s.size], self.rd(i, s)))

    def i_lea(self, i, o, l):
        return self.wr(i, o[0], self.ea(i, o[1], noseg=True))

    def i_push(self, i, o, l):
        s = o[0]
        if self.is_reg(s) and i.reg_name(s.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            return 'esp-=4;M32(esp)=0x23;'
        if s.size == 2:
            return '%su16 v=%s;esp-=2;M16(esp)=v;' % (self.ea_decl(i, o), self.rd(i, s))
        v = self.rd(i, s, 4) if not self.is_imm(s) else '0x%xu' % (s.imm & 0xffffffff)
        return '%su32 v=%s;esp-=4;M32(esp)=v;' % (self.ea_decl(i, o), v)

    def i_pop(self, i, o, l):
        d = o[0]
        if self.is_reg(d) and i.reg_name(d.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            return 'esp+=4;'
        if d.size == 2:
            return 'u16 v=M16(esp);esp+=2;%s' % self.wr(i, d, 'v', 2, self.ea(i, d) if self.is_mem(d) else 'ea')
        if self.is_mem(d):
            return 'u32 v=M32(esp);esp+=4;M32(%s)=v;' % self.ea(i, d)
        return 'u32 v=M32(esp);esp+=4;%s' % self.wr(i, d, 'v')

    def i_pushal(self, i, o, l):
        return ('u32 t=esp;esp-=32;M32(esp+28)=eax;M32(esp+24)=ecx;M32(esp+20)=edx;M32(esp+16)=ebx;M32(esp+12)=t;'
                'M32(esp+8)=ebp;M32(esp+4)=esi;M32(esp)=edi;')

    def i_popal(self, i, o, l):
        return ('edi=M32(esp);esi=M32(esp+4);ebp=M32(esp+8);ebx=M32(esp+16);edx=M32(esp+20);ecx=M32(esp+24);'
                'eax=M32(esp+28);esp+=32;')

    def i_pushfd(self, i, o, l):
        return 'esp-=4;M32(esp)=%s|0x202u|(c->df?0x400u:0);' % FALL

    def i_popfd(self, i, o, l):
        return 'u32 v=M32(esp);esp+=4;c->df=(v>>10)&1;lf_op=LF_EXPL;lf_r=v;'

    def i_lahf(self, i, o, l):
        return self.reg_w('ah', '(%s&0xd5u)|2u' % FALL)

    def i_sahf(self, i, o, l):
        return 'u32 fl=%s;lf_r=(fl&~0xd5u)|((eax>>8)&0xd5u);lf_op=LF_EXPL;' % FALL

    def i_clc(self, i, o, l): return 'u32 fl=%s;lf_r=fl&~F_CF;lf_op=LF_EXPL;' % FALL
    def i_stc(self, i, o, l): return 'u32 fl=%s;lf_r=fl|F_CF;lf_op=LF_EXPL;' % FALL
    def i_cmc(self, i, o, l): return 'u32 fl=%s;lf_r=fl^F_CF;lf_op=LF_EXPL;' % FALL
    def i_cld(self, i, o, l): return 'c->df=0;'
    def i_std(self, i, o, l): return 'c->df=1;'
    def i_cli(self, i, o, l): return ''
    def i_sti(self, i, o, l): return ''

    def i_xchg(self, i, o, l):
        a, b = o
        if self.is_mem(a) or self.is_mem(b):
            m, r = (a, b) if self.is_mem(a) else (b, a)
            T = UT[m.size]
            return 'u32 ea=%s;%s v=__atomic_exchange_n((%s*)HP(ea),(%s)%s,__ATOMIC_SEQ_CST);%s' % (
                self.ea(i, m), T, T, T, self.rd(i, r), self.wr(i, r, 'v'))
        return 'u32 t=%s;%s%s' % (self.rd(i, a), self.wr(i, a, self.rd(i, b)), self.wr(i, b, 't'))

    def i_xadd(self, i, o, l):
        d, s = o
        T = UT[d.size]
        if self.is_mem(d):
            return ('u32 ea=%s;%s b=%s;%s a=__atomic_fetch_add((%s*)HP(ea),b,__ATOMIC_SEQ_CST);%s r=(%s)(a+b);%s%s'
                    % (self.ea(i, d), T, self.rd(i, s), T, T, T, T, self.wr(i, s, 'a'), self.setlf('ADD', d.size)))
        return '%s a=%s;%s b=%s;%s r=(%s)(a+b);%s%s%s' % (T, self.rd(i, d), T, self.rd(i, s), T, T,
                                                         self.wr(i, s, 'a'), self.wr(i, d, 'r'), self.setlf('ADD', d.size))

    def i_cmpxchg(self, i, o, l):
        d, s = o
        T = UT[d.size]
        acc = {1: '(u8)eax', 2: '(u16)eax', 4: 'eax'}[d.size]
        accw = {1: 'al', 2: 'ax', 4: 'eax'}[d.size]
        if self.is_mem(d):
            return ('u32 ea=%s;%s exp=%s;%s a=exp;%s b=a;__atomic_compare_exchange_n((%s*)HP(ea),&exp,(%s)%s,0,'
                    '__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);a=exp;%s r=(%s)(b-a);%s%s' % (
                        self.ea(i, d), T, acc, T, T, T, T, self.rd(i, s), T, T, self.reg_w(accw, 'a'),
                        'lf_op=LF_SUB%s;lf_a=b;lf_b=a;lf_r=r;lf_ci=0;' % LFS[d.size]))
        return '%s a=%s;%s b=%s;%s r=(%s)(b-a);if(a==b){%s}else{%s}lf_op=LF_SUB%s;lf_a=b;lf_b=a;lf_r=r;lf_ci=0;' % (
            T, self.rd(i, d), T, acc, T, T, self.wr(i, d, self.rd(i, s)), self.reg_w(accw, 'a'), LFS[d.size])

    def incdec(self, i, o, l, kind, e):
        d = o[0]
        T = UT[d.size]
        decl = self.ea_decl(i, o)
        if l and self.is_mem(d):
            return '%su32 ci=f_cf(%s);%s a=__atomic_fetch_%s((%s*)HP(ea),1,__ATOMIC_SEQ_CST);%s r=(%s)(a%s1);%s' % (
                decl, LF, T, 'add' if e == '+' else 'sub', T, T, T, e, self.setlf(kind, d.size, b='1', ci='ci'))
        return '%su32 ci=f_cf(%s);%s a=%s;%s r=(%s)(a%s1);%s%s' % (
            decl, LF, T, self.rd(i, d), T, T, e, self.wr(i, d, 'r'), self.setlf(kind, d.size, b='1', ci='ci'))

    def i_inc(self, i, o, l): return self.incdec(i, o, l, 'INC', '+')
    def i_dec(self, i, o, l): return self.incdec(i, o, l, 'DEC', '-')

    def i_neg(self, i, o, l):
        d = o[0]
        T = UT[d.size]
        return '%s%s b=%s;%s r=(%s)(0-b);%s%s' % (self.ea_decl(i, o), T, self.rd(i, d), T, T, self.wr(i, d, 'r'),
                                                 self.setlf('NEG', d.size, a='0'))

    def i_not(self, i, o, l):
        d = o[0]
        return self.ea_decl(i, o) + self.wr(i, d, '~%s' % self.rd(i, d))

    def i_bswap(self, i, o, l):
        return self.wr(i, o[0], '__builtin_bswap32(%s)' % self.rd(i, o[0]))

    def i_cdq(self, i, o, l): return 'edx=(u32)((s32)eax>>31);'
    def i_cwde(self, i, o, l): return 'eax=(u32)(s32)(s16)eax;'
    def i_cbw(self, i, o, l): return self.reg_w('ax', '(u16)(s16)(s8)eax')
    def i_cwd(self, i, o, l): return self.reg_w('dx', '((s16)eax<0)?0xffff:0')

    def i_leave(self, i, o, l): return 'esp=ebp;ebp=M32(esp);esp+=4;'

    def i_imul(self, i, o, l):
        if len(o) == 1:
            s = o[0]
            sz = s.size
            d = self.ea_decl(i, o)
            if sz == 4:
                return ('%ss64 p=(s64)(s32)eax*(s64)(s32)%s;eax=(u32)p;edx=(u32)((u64)p>>32);'
                        'lf_op=LF_EXPL;lf_r=(p!=(s64)(s32)p)?(F_CF|F_OF):0;' % (d, self.rd(i, s)))
            if sz == 2:
                return ('%ss32 p=(s32)(s16)eax*(s32)(s16)%s;%s%slf_op=LF_EXPL;lf_r=(p!=(s32)(s16)p)?(F_CF|F_OF):0;'
                        % (d, self.rd(i, s), self.reg_w('ax', 'p'), self.reg_w('dx', '(u32)p>>16')))
            return ('%ss32 p=(s32)(s8)eax*(s32)(s8)%s;%slf_op=LF_EXPL;lf_r=(p!=(s32)(s8)p)?(F_CF|F_OF):0;'
                    % (d, self.rd(i, s), self.reg_w('ax', 'p')))
        if len(o) == 2:
            a, b = self.rd(i, o[0]), self.rd(i, o[1], o[0].size)
        else:
            a, b = self.rd(i, o[1]), self.rd(i, o[2], o[0].size)
        sz = o[0].size
        S = ST[sz]
        return '%ss64 p=(s64)(%s)%s*(s64)(%s)%s;%slf_op=LF_EXPL;lf_r=(p!=(s64)(%s)p)?(F_CF|F_OF):0;' % (
            self.ea_decl(i, o), S, a, S, b, self.wr(i, o[0], 'p'), S)

    def i_mul(self, i, o, l):
        s = o[0]
        sz = s.size
        d = self.ea_decl(i, o)
        if sz == 4:
            return ('%su64 p=(u64)eax*(u64)%s;eax=(u32)p;edx=(u32)(p>>32);lf_op=LF_EXPL;lf_r=edx?(F_CF|F_OF):0;'
                    % (d, self.rd(i, s)))
        if sz == 2:
            return '%su32 p=(u32)(u16)eax*(u32)%s;%s%slf_op=LF_EXPL;lf_r=(p>>16)?(F_CF|F_OF):0;' % (
                d, self.rd(i, s), self.reg_w('ax', 'p'), self.reg_w('dx', 'p>>16'))
        return '%su32 p=(u32)(u8)eax*(u32)%s;%slf_op=LF_EXPL;lf_r=(p>>8)?(F_CF|F_OF):0;' % (
            d, self.rd(i, s), self.reg_w('ax', 'p'))

    def i_div(self, i, o, l):
        s = o[0]
        sz = s.size
        d = self.ea_decl(i, o)
        z = 'if(!dv){%srt_unimpl(c,0x%xu,"divide by zero");}' % (SYNC_OUT, i.address)
        if sz == 4:
            return '%su32 dv=%s;%su64 n=((u64)edx<<32)|eax;eax=(u32)(n/dv);edx=(u32)(n%%dv);' % (d, self.rd(i, s), z)
        if sz == 2:
            return '%su32 dv=%s;%su32 n=((edx&0xffff)<<16)|(eax&0xffff);%s%s' % (
                d, self.rd(i, s), z, self.reg_w('ax', 'n/dv'), self.reg_w('dx', 'n%dv'))
        return '%su32 dv=%s;%su32 n=eax&0xffff;%s%s' % (d, self.rd(i, s), z, self.reg_w('al', 'n/dv'),
                                                      self.reg_w('ah', 'n%dv'))

    def i_idiv(self, i, o, l):
        s = o[0]
        sz = s.size
        d = self.ea_decl(i, o)
        z = 'if(!dv){%srt_unimpl(c,0x%xu,"divide by zero");}' % (SYNC_OUT, i.address)
        if sz == 4:
            return ('%ss32 dv=(s32)%s;%ss64 n=(s64)(((u64)edx<<32)|eax);eax=(u32)(s32)(n/dv);edx=(u32)(s32)(n%%dv);'
                    % (d, self.rd(i, s), z))
        if sz == 2:
            return '%ss32 dv=(s16)%s;%ss32 n=(s32)(((edx&0xffff)<<16)|(eax&0xffff));%s%s' % (
                d, self.rd(i, s), z, self.reg_w('ax', 'n/dv'), self.reg_w('dx', 'n%dv'))
        return '%ss32 dv=(s8)%s;%ss32 n=(s16)eax;%s%s' % (d, self.rd(i, s), z, self.reg_w('al', 'n/dv'),
                                                        self.reg_w('ah', 'n%dv'))

    def shift(self, i, o, kind):
        d = o[0]
        sz = d.size
        T = UT[sz]
        bits = sz * 8
        decl = self.ea_decl(i, o)
        if len(o) == 1:
            cnt = '1'
        elif self.is_imm(o[1]):
            cnt = str(o[1].imm & 31)
        else:
            cnt = '(%s&31)' % self.rd(i, o[1])
        if cnt == '0':
            return ''
        a = self.rd(i, d)
        if kind == 'SHL':
            r = '(%s)((u32)a<<n)' % T
        elif kind == 'SHR':
            r = '(%s)((u32)a>>n)' % T
        else:
            r = '(%s)((s32)(%s)a>>n)' % (T, ST[sz])
        body = '%s r=%s;%s%s' % (T, r, self.wr(i, d, 'r'), self.setlf(kind, sz, b='0', ci='n'))
        return '%su32 n=%s;%s a=%s;if(n){%s}' % (decl, cnt, T, a, body)

    def i_shl(self, i, o, l): return self.shift(i, o, 'SHL')
    def i_sal(self, i, o, l): return self.shift(i, o, 'SHL')
    def i_shr(self, i, o, l): return self.shift(i, o, 'SHR')
    def i_sar(self, i, o, l): return self.shift(i, o, 'SAR')

    def rot(self, i, o, kind):
        d = o[0]
        sz = d.size
        bits = sz * 8
        T = UT[sz]
        decl = self.ea_decl(i, o)
        if len(o) == 1: cnt = '1'
        elif self.is_imm(o[1]): cnt = str(o[1].imm & 31)
        else: cnt = '(%s&31)' % self.rd(i, o[1])
        a = self.rd(i, d)
        top = '0x%xu' % (1 << (bits - 1))
        if kind == 'rol':
            body = ('u32 k=n%%%d;%s r=k?(%s)((a<<k)|(a>>(%d-k))):a;u32 cf=r&1;u32 of=((r&%s)!=0)^cf;' % (bits, T, T, bits, top))
        elif kind == 'ror':
            body = ('u32 k=n%%%d;%s r=k?(%s)((a>>k)|(a<<(%d-k))):a;u32 cf=(r&%s)!=0;u32 of=((r>>%d)^(r>>%d))&1;'
                    % (bits, T, T, bits, top, bits - 1, bits - 2))
        elif kind == 'rcl':
            body = ('u32 cf=f_cf(%s);%s r=a;for(u32 k=n%%%d;k;k--){u32 nc=(r&%s)!=0;r=(%s)((r<<1)|cf);cf=nc;}'
                    'u32 of=((r&%s)!=0)^cf;' % (LF, T, bits + 1, top, T, top))
        else:
            body = ('u32 cf=f_cf(%s);u32 of=((a&%s)!=0)^cf;%s r=a;for(u32 k=n%%%d;k;k--){u32 nc=r&1;r=(%s)((r>>1)|(cf?%s:0));cf=nc;}'
                    % (LF, top, T, bits + 1, T, top))
        return ('%su32 n=%s;%s a=%s;if(n){u32 fl=%s;%s%slf_r=(fl&~(F_CF|F_OF))|(cf?F_CF:0)|(of?F_OF:0);lf_op=LF_EXPL;}'
                % (decl, cnt, T, a, FALL, body, self.wr(i, d, 'r')))

    def i_rol(self, i, o, l): return self.rot(i, o, 'rol')
    def i_ror(self, i, o, l): return self.rot(i, o, 'ror')
    def i_rcl(self, i, o, l): return self.rot(i, o, 'rcl')
    def i_rcr(self, i, o, l): return self.rot(i, o, 'rcr')

    def i_shld(self, i, o, l):
        d, s, c_ = o
        if d.size != 4: raise Unsupported('shld16')
        cnt = str(c_.imm & 31) if self.is_imm(c_) else '(%s&31)' % self.rd(i, c_)
        return ('%su32 n=%s;u32 a=%s;u32 b=%s;if(n){u32 r=(a<<n)|(b>>(32-n));%su32 fl=%s;'
                'lf_r=(fl&~(F_CF|F_ZF|F_SF|F_PF))|(((a>>(32-n))&1)?F_CF:0)|(r?0:F_ZF)|((r>>31)?F_SF:0)|'
                '(__builtin_parity(r&0xff)?0:F_PF);lf_op=LF_EXPL;}' % (
                    self.ea_decl(i, o), cnt, self.rd(i, d), self.rd(i, s), self.wr(i, d, 'r'), FALL))

    def i_shrd(self, i, o, l):
        d, s, c_ = o
        if d.size != 4: raise Unsupported('shrd16')
        cnt = str(c_.imm & 31) if self.is_imm(c_) else '(%s&31)' % self.rd(i, c_)
        return ('%su32 n=%s;u32 a=%s;u32 b=%s;if(n){u32 r=(a>>n)|(b<<(32-n));%su32 fl=%s;'
                'lf_r=(fl&~(F_CF|F_ZF|F_SF|F_PF))|(((a>>(n-1))&1)?F_CF:0)|(r?0:F_ZF)|((r>>31)?F_SF:0)|'
                '(__builtin_parity(r&0xff)?0:F_PF);lf_op=LF_EXPL;}' % (
                    self.ea_decl(i, o), cnt, self.rd(i, d), self.rd(i, s), self.wr(i, d, 'r'), FALL))

    def bt(self, i, o, op):
        d, s = o
        if self.is_mem(d) and not self.is_imm(s):
            # bit offset can address outside the dword
            base = self.ea(i, d)
            pre = 's32 bo=(s32)%s;u32 ea=%s+(u32)((bo>>5)*4);u32 n=bo&31;' % (self.rd(i, s), base)
            val = 'M32(ea)'
            wr = 'M32(ea)=v;'
        else:
            pre = self.ea_decl(i, o) + 'u32 n=(%s)&%d;' % (self.rd(i, s), d.size * 8 - 1)
            val = self.rd(i, d)
            wr = self.wr(i, d, 'v')
        body = 'u32 v=%s;u32 cf=(v>>n)&1;' % val
        if op == 'bts': body += 'v|=1u<<n;' + wr
        elif op == 'btr': body += 'v&=~(1u<<n);' + wr
        elif op == 'btc': body += 'v^=1u<<n;' + wr
        return pre + body + 'u32 fl=%s;lf_r=(fl&~F_CF)|cf;lf_op=LF_EXPL;' % FALL

    def i_bt(self, i, o, l): return self.bt(i, o, 'bt')
    def i_bts(self, i, o, l): return self.bt(i, o, 'bts')
    def i_btr(self, i, o, l): return self.bt(i, o, 'btr')
    def i_btc(self, i, o, l): return self.bt(i, o, 'btc')

    def i_bsf(self, i, o, l):
        return ('%su32 v=%s;u32 fl=%s;if(v){%slf_r=fl&~F_ZF;}else lf_r=fl|F_ZF;lf_op=LF_EXPL;'
                % (self.ea_decl(i, o), self.rd(i, o[1]), FALL, self.wr(i, o[0], '__builtin_ctz(v)')))

    def i_bsr(self, i, o, l):
        return ('%su32 v=%s;u32 fl=%s;if(v){%slf_r=fl&~F_ZF;}else lf_r=fl|F_ZF;lf_op=LF_EXPL;'
                % (self.ea_decl(i, o), self.rd(i, o[1]), FALL, self.wr(i, o[0], '31-__builtin_clz(v)')))

    def i_rdtsc(self, i, o, l): return 'u64 t=rt_rdtsc();eax=(u32)t;edx=(u32)(t>>32);'
    def i_cpuid(self, i, o, l): return SYNC_OUT + 'rt_cpuid(c);' + SYNC_IN
    def i_xlatb(self, i, o, l): return self.reg_w('al', 'M8(ebx+(u8)eax)')
    def i_hlt(self, i, o, l): raise Unsupported('hlt')

    def i_enter(self, i, o, l):
        if o[1].imm != 0: raise Unsupported('enter nested')
        return 'esp-=4;M32(esp)=ebp;ebp=esp;esp-=%du;' % o[0].imm

    # --- string instructions
    def string(self, i, mn, pre):
        base = mn.split()[0] if ' ' in mn else mn
        sz = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        T = UT[sz]
        M = {1: 'M8', 2: 'M16', 4: 'M32'}[sz]
        acc = {1: '(u8)eax', 2: '(u16)eax', 4: 'eax'}[sz]
        accw = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        step = 'u32 st=c->df?(u32)-%d:%du;' % (sz, sz)
        op = base[:-1]
        if op == 'movs':
            one = '%s(edi)=%s(esi);esi+=st;edi+=st;' % (M, M)
            if pre:
                fast = ('if(!c->df&&(edi-esi>=ecx*%du)){memmove(HP(edi),HP(esi),(size_t)ecx*%d);esi+=ecx*%d;edi+=ecx*%d;ecx=0;}'
                        % (sz, sz, sz, sz))
                return step + fast + 'while(ecx){%secx--;}' % one
            return step + one
        if op == 'stos':
            one = '%s(edi)=%s;edi+=st;' % (M, acc)
            if pre:
                if sz == 1:
                    fast = 'if(!c->df){memset(HP(edi),(u8)eax,ecx);edi+=ecx;ecx=0;}'
                    return step + fast + 'while(ecx){%secx--;}' % one
                return step + 'while(ecx){%secx--;}' % one
            return step + one
        if op == 'lods':
            one = '%sesi+=st;' % self.reg_w(accw, '%s(esi)' % M)
            if pre: return step + 'while(ecx){%secx--;}' % one
            return step + one
        if op in ('scas', 'cmps'):
            if op == 'scas':
                cmp = '%s a=%s;%s b=%s(edi);edi+=st;' % (T, acc, T, M)
            else:
                cmp = '%s a=%s(esi);%s b=%s(edi);esi+=st;edi+=st;' % (T, M, T, M)
            setf = 'lf_op=LF_SUB%s;lf_a=a;lf_b=b;lf_r=(%s)(a-b);lf_ci=0;' % (LFS[sz], T)
            if not pre:
                return step + cmp + setf
            stop = 'a!=b' if pre in ('repe', 'rep') else 'a==b'
            return step + 'while(ecx){%s%secx--;if(%s)break;}' % (cmp, setf, stop)
        raise Unsupported('string ' + mn)

    # --- x87
    def streg(self, i, o):
        n = i.reg_name(o.reg)
        m = re.match(r'st\((\d)\)', n)
        if m: return int(m.group(1))
        if n == 'st0' or n == 'st': return 0
        raise Unsupported('streg ' + n)

    def fmem(self, i, o):
        return self.ea(i, o)

    def x87(self, i, mn, ops):
        b = [x for x in i.bytes]
        k = 0
        while b[k] in (0x66, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x67, 0xf0):
            k += 1
        op0, modrm = b[k], b[k + 1] if k + 1 < len(b) else 0
        reg = (modrm >> 3) & 7
        mod = modrm >> 6
        rm = modrm & 7
        ARITH = ['+', '*', 'com', 'comp', '-', 'r-', '/', 'r/']

        def arith(dst, src, kind, pop=False):
            if kind in ('com', 'comp'):
                s = 'fcom_set(c,%s,%s);' % (dst, src)
                if kind == 'comp': s += 'fpop(c);'
                return s
            if kind == '+': e = '%s+%s' % (dst, src)
            elif kind == '*': e = '%s*%s' % (dst, src)
            elif kind == '-': e = '%s-%s' % (dst, src)
            elif kind == 'r-': e = '%s-%s' % (src, dst)
            elif kind == '/': e = '%s/%s' % (dst, src)
            else: e = '%s/%s' % (src, dst)
            s = '{double _v=%s;%s=_v;}' % (e, dst)
            if pop: s += 'fpop(c);'
            return s

        if op0 in (0xd8, 0xdc, 0xda, 0xde) and mod != 3:
            ea = self.ea(i, [o for o in ops if self.is_mem(o)][0])
            src = {0xd8: '(double)MF32(%s)', 0xdc: 'MF64(%s)', 0xda: '(double)(s32)M32(%s)',
                   0xde: '(double)(s16)M16(%s)'}[op0] % ea
            return arith('ST(0)', src, ARITH[reg])
        if op0 == 0xd8 and mod == 3:
            return arith('ST(0)', 'ST(%d)' % rm, ARITH[reg])
        if op0 in (0xdc, 0xde) and mod == 3:
            pop = op0 == 0xde
            if op0 == 0xde and reg == 3 and rm == 1:
                return 'fcom_set(c,ST(0),ST(1));fpop(c);fpop(c);'
            if reg in (2, 3):
                return arith('ST(0)', 'ST(%d)' % rm, ARITH[reg]) + ('fpop(c);' if pop and reg == 3 else '')
            # dc/de: reg4 = subr (st(i)=st0-st(i)), reg5 = sub (st(i)=st(i)-st0), reg6 = divr, reg7 = div
            kind = {0: '+', 1: '*', 4: 'r-', 5: '-', 6: 'r/', 7: '/'}[reg]
            return arith('ST(%d)' % rm, 'ST(0)', kind, pop)

        mo = [o for o in ops if self.is_mem(o)]
        ea = self.ea(i, mo[0]) if mo else None
        if mn == 'fld':
            if ea:
                sz = mo[0].size
                v = {4: '(double)MF32(%s)', 8: 'MF64(%s)', 10: 'ld80(%s)'}[sz] % ea
                return 'fpush(c,%s);' % v
            return 'double v=ST(%d);fpush(c,v);' % self.streg(i, ops[0])
        if mn in ('fst', 'fstp'):
            p = 'fpop(c);' if mn == 'fstp' else ''
            if ea:
                sz = mo[0].size
                if sz == 4: return 'MF32(%s)=(float)ST(0);%s' % (ea, p)
                if sz == 8: return 'MF64(%s)=ST(0);%s' % (ea, p)
                return 'st80(%s,ST(0));%s' % (ea, p)
            return 'ST(%d)=ST(0);%s' % (self.streg(i, ops[0]), p)
        if mn == 'fild':
            sz = mo[0].size
            return 'fpush(c,(double)%s);' % {2: '(s16)M16(%s)', 4: '(s32)M32(%s)', 8: '(s64)M64(%s)'}[sz] % ea
        if mn in ('fist', 'fistp', 'fisttp'):
            sz = mo[0].size
            p = 'fpop(c);' if mn != 'fist' else ''
            if mn == 'fisttp':
                v = 'trunc(ST(0))'
                f = {2: 'M16(%s)=(u16)(s16)%s;', 4: 'M32(%s)=(u32)(s32)%s;', 8: 'M64(%s)=(u64)(s64)%s;'}[sz] % (ea, v)
                return f + p
            f = {2: 'M16(%s)=(u16)fist16(c,ST(0));', 4: 'M32(%s)=(u32)fist32(c,ST(0));',
                 8: 'M64(%s)=(u64)fist64(c,ST(0));'}[sz] % ea
            return f + p
        consts = {'fldz': '0.0', 'fld1': '1.0', 'fldpi': 'M_PI', 'fldl2e': 'M_LOG2E', 'fldl2t': '3.321928094887362347870',
                  'fldlg2': '0.301029995663981195214', 'fldln2': 'M_LN2'}
        if mn in consts: return 'fpush(c,%s);' % consts[mn]
        if mn == 'fxch':
            n = self.streg(i, ops[-1]) if ops else 1
            return 'double t=ST(0);ST(0)=ST(%d);ST(%d)=t;' % (n, n)
        if mn == 'fchs': return 'ST(0)=-ST(0);'
        if mn == 'fabs': return 'ST(0)=fabs(ST(0));'
        if mn == 'fsqrt': return 'ST(0)=sqrt(ST(0));'
        if mn == 'fsin': return 'ST(0)=sin(ST(0));c->fsw&=~0x400;'
        if mn == 'fcos': return 'ST(0)=cos(ST(0));c->fsw&=~0x400;'
        if mn == 'fsincos': return 'double v=ST(0);ST(0)=sin(v);fpush(c,cos(v));c->fsw&=~0x400;'
        if mn == 'fptan': return 'ST(0)=tan(ST(0));fpush(c,1.0);c->fsw&=~0x400;'
        if mn == 'fpatan': return 'double v=atan2(ST(1),ST(0));fpop(c);ST(0)=v;'
        if mn == 'frndint': return 'ST(0)=frnd(c,ST(0));'
        if mn == 'fscale': return 'ST(0)=ldexp(ST(0),(int)trunc(ST(1)));'
        if mn == 'f2xm1': return 'ST(0)=exp2(ST(0))-1.0;'
        if mn == 'fyl2x': return 'double v=ST(1)*log2(ST(0));fpop(c);ST(0)=v;'
        if mn == 'fyl2xp1': return 'double v=ST(1)*log2(ST(0)+1.0);fpop(c);ST(0)=v;'
        if mn == 'fprem': return 'ST(0)=fmod(ST(0),ST(1));c->fsw&=~0x400;'
        if mn == 'fprem1': return 'ST(0)=remainder(ST(0),ST(1));c->fsw&=~0x400;'
        if mn == 'fxtract': return 'int e;double m=frexp(ST(0),&e);ST(0)=(double)(e-1);fpush(c,m*2.0);'
        if mn in ('fcom', 'fcomp', 'fucom', 'fucomp'):
            p = 'fpop(c);' if mn.endswith('p') else ''
            if ea:
                v = {4: '(double)MF32(%s)', 8: 'MF64(%s)'}[mo[0].size] % ea
            else:
                v = 'ST(%d)' % (self.streg(i, ops[-1]) if ops else 1)
            return 'fcom_set(c,ST(0),%s);%s' % (v, p)
        if mn in ('fcompp', 'fucompp'): return 'fcom_set(c,ST(0),ST(1));fpop(c);fpop(c);'
        if mn in ('ficom', 'ficomp'):
            v = {2: '(double)(s16)M16(%s)', 4: '(double)(s32)M32(%s)'}[mo[0].size] % ea
            return 'fcom_set(c,ST(0),%s);%s' % (v, 'fpop(c);' if mn == 'ficomp' else '')
        if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip', 'fcompi', 'fucompi'):
            n = self.streg(i, ops[-1])
            p = 'fpop(c);' if 'p' in mn[4:] else ''
            return ('double a=ST(0),b=ST(%d);u32 fl=%s&~(F_CF|F_ZF|F_PF|F_OF|F_SF|F_AF);'
                    'if(a!=a||b!=b)fl|=F_CF|F_ZF|F_PF;else if(a<b)fl|=F_CF;else if(a==b)fl|=F_ZF;'
                    'lf_op=LF_EXPL;lf_r=fl;%s' % (n, FALL, p))
        if mn == 'ftst': return 'fcom_set(c,ST(0),0.0);'
        if mn == 'fxam':
            return ('double v=ST(0);u16 cc;if(isnan(v))cc=0x100;else if(isinf(v))cc=0x500;else if(v==0)cc=0x4000;'
                    'else if(fpclassify(v)==FP_SUBNORMAL)cc=0x4400;else cc=0x400;if(signbit(v))cc|=0x200;'
                    'c->fsw=(c->fsw&~0x4700)|cc;')
        if mn in ('fnstsw', 'fstsw'):
            if ea: return 'M16(%s)=fstsw(c);' % ea
            return self.reg_w('ax', 'fstsw(c)')
        if mn in ('fnstcw', 'fstcw'): return 'M16(%s)=c->fcw;' % ea
        if mn == 'fldcw': return 'c->fcw=M16(%s);' % ea
        if mn in ('fnclex', 'fclex'): return 'c->fsw&=0x7f00;'
        if mn in ('fninit', 'finit'): return 'c->fcw=0x37f;c->fsw=0;c->top=0;'
        if mn == 'ffree': return ''
        if mn == 'fincstp': return 'c->top=(c->top+1)&7;'
        if mn == 'fdecstp': return 'c->top=(c->top-1)&7;'
        if mn in ('fnstenv', 'fstenv'):
            return 'M32(%s)=c->fcw;M32(%s+4)=fstsw(c);M32(%s+8)=0;' % (ea, ea, ea)
        if mn == 'fldenv':
            return 'c->fcw=M16(%s);c->fsw=M16(%s+4);c->top=(c->fsw>>11)&7;' % (ea, ea)
        if mn in ('fnsave', 'fsave'):  # 28-byte environment, then ST(0)..ST(7) as 80-bit values; then finit
            return ('{u32 a=%s;M32(a)=c->fcw;M32(a+4)=fstsw(c);M32(a+8)=c->ftw;M32(a+12)=0;M32(a+16)=0;M32(a+20)=0;'
                    'M32(a+24)=0;for(int k=0;k<8;k++)st80(a+28+10*k,ST(k));c->fcw=0x37f;c->fsw=0;c->top=0;}' % ea)
        if mn == 'frstor':
            return ('{u32 a=%s;c->fcw=M16(a);c->fsw=M16(a+4);c->ftw=M16(a+8);c->top=(c->fsw>>11)&7;'
                    'for(int k=0;k<8;k++)ST(k)=ld80(a+28+10*k);}' % ea)
        raise Unsupported('x87 ' + mn)

    # --- SSE
    def xr(self, i, o):
        n = i.reg_name(o.reg)
        if n.startswith('xmm'): return int(n[3:])
        raise Unsupported('xmmreg ' + n)

    def xsrc(self, i, o, n=16):
        """returns code that declares XMM s from operand"""
        if self.is_reg(o): return 'XMM s=c->xmm[%d];' % self.xr(i, o)
        if n == 16: return 'XMM s;memcpy(&s,HP(%s),16);' % self.ea(i, o)
        return 'XMM s={0};memcpy(&s,HP(%s),%d);' % (self.ea(i, o), n)

    def mmx(self, i, mn, ops):
        def opnd(o):
            if self.is_reg(o):
                n = i.reg_name(o.reg)
                if n.startswith('mm'): return 'c->mm[%d]' % int(n[2:])
                raise Unsupported('mmx operand ' + n)
            if self.is_mem(o): return 'M64(%s)' % self.ea(i, o)
            raise Unsupported('mmx operand')
        d, s_ = ops[0], ops[1]
        if mn == 'movq':
            return '{u64 v=%s;%s=v;}' % (opnd(s_), opnd(d))
        ops_w = {'paddw': 0, 'psubw': 1, 'pcmpgtw': 2}
        if mn in ops_w:
            return '%s=mmx_w(%s,%s,%d);' % (opnd(d), opnd(d), opnd(s_), ops_w[mn])
        bit = {'pand': '&', 'por': '|', 'pxor': '^'}
        if mn in bit:
            return '%s%s=%s;' % (opnd(d), bit[mn], opnd(s_))
        if mn == 'pandn':
            return '%s=~%s&%s;' % (opnd(d), opnd(d), opnd(s_))
        raise Unsupported('mmx ' + mn)

    def sse(self, i, mn, ops):
        if any(self.is_reg(o) and i.reg_name(o.reg).startswith('mm') for o in ops):
            return self.mmx(i, mn, ops)
        d = ops[0] if ops else None
        D = lambda: 'c->xmm[%d]' % self.xr(i, d)
        if mn in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd', 'movntps', 'movntdq', 'movntpd'):
            s = ops[1]
            if self.is_mem(d): return 'memcpy(HP(%s),&c->xmm[%d],16);' % (self.ea(i, d), self.xr(i, s))
            return self.xsrc(i, s) + D() + '=s;'
        if mn == 'movss':
            s = ops[1]
            if self.is_mem(d): return 'MF32(%s)=c->xmm[%d].f[0];' % (self.ea(i, d), self.xr(i, s))
            if self.is_mem(s): return '%s.u[0]=M32(%s);%s.u[1]=%s.u[2]=%s.u[3]=0;' % (D(), self.ea(i, s), D(), D(), D())
            return '%s.u[0]=c->xmm[%d].u[0];' % (D(), self.xr(i, s))
        if mn == 'movsd' and ops and (self.is_reg(d) and i.reg_name(d.reg).startswith('xmm') or
                                      self.is_reg(ops[1]) and i.reg_name(ops[1].reg).startswith('xmm')):
            s = ops[1]
            if self.is_mem(d): return 'M64(%s)=c->xmm[%d].q[0];' % (self.ea(i, d), self.xr(i, s))
            if self.is_mem(s): return '%s.q[0]=M64(%s);%s.q[1]=0;' % (D(), self.ea(i, s), D())
            return '%s.q[0]=c->xmm[%d].q[0];' % (D(), self.xr(i, s))
        if mn == 'movsd':  # string movsd without rep
            return self.string(i, 'movsd', None)
        if mn in ('movlps', 'movlpd'):
            s = ops[1]
            if self.is_mem(d): return 'M64(%s)=c->xmm[%d].q[0];' % (self.ea(i, d), self.xr(i, s))
            return '%s.q[0]=M64(%s);' % (D(), self.ea(i, s))
        if mn in ('movhps', 'movhpd'):
            s = ops[1]
            if self.is_mem(d): return 'M64(%s)=c->xmm[%d].q[1];' % (self.ea(i, d), self.xr(i, s))
            return '%s.q[1]=M64(%s);' % (D(), self.ea(i, s))
        if mn == 'movhlps': return '%s.q[0]=c->xmm[%d].q[1];' % (D(), self.xr(i, ops[1]))
        if mn == 'movlhps': return '%s.q[1]=c->xmm[%d].q[0];' % (D(), self.xr(i, ops[1]))
        if mn == 'movq':
            s = ops[1]
            if self.is_mem(d): return 'M64(%s)=c->xmm[%d].q[0];' % (self.ea(i, d), self.xr(i, s))
            if self.is_mem(s): return '%s.q[0]=M64(%s);%s.q[1]=0;' % (D(), self.ea(i, s), D())
            return '%s.q[0]=c->xmm[%d].q[0];%s.q[1]=0;' % (D(), self.xr(i, s), D())
        if mn == 'movd':
            s = ops[1]
            if self.is_reg(d) and i.reg_name(d.reg).startswith('xmm'):
                v = self.rd(i, s, 4) if not self.is_mem(s) else 'M32(%s)' % self.ea(i, s)
                return 'u32 v=%s;%s.q[0]=v;%s.q[1]=0;' % (v, D(), D())
            if self.is_mem(d): return 'M32(%s)=c->xmm[%d].u[0];' % (self.ea(i, d), self.xr(i, s))
            return self.wr(i, d, 'c->xmm[%d].u[0]' % self.xr(i, s))
        ps = {'addps': '+', 'subps': '-', 'mulps': '*', 'divps': '/'}
        if mn in ps:
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.f[k]=%s.f[k]%ss.f[k];' % (D(), D(), ps[mn])
        ss = {'addss': '+', 'subss': '-', 'mulss': '*', 'divss': '/'}
        if mn in ss:
            return self.xsrc(i, ops[1], 4) + '%s.f[0]=%s.f[0]%ss.f[0];' % (D(), D(), ss[mn])
        fn1 = {'sqrt': 'sqrtf', 'rsqrt': 'rsqrt_f', 'rcp': 'rcp_f'}
        for k, f in fn1.items():
            if mn == k + 'ps':
                return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.f[k]=%s(s.f[k]);' % (D(), f)
            if mn == k + 'ss':
                return self.xsrc(i, ops[1], 4) + '%s.f[0]=%s(s.f[0]);' % (D(), f)
        mm = {'maxps': 'maxf_x86', 'minps': 'minf_x86'}
        if mn in mm:
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.f[k]=%s(%s.f[k],s.f[k]);' % (D(), mm[mn], D())
        mm = {'maxss': 'maxf_x86', 'minss': 'minf_x86'}
        if mn in mm:
            return self.xsrc(i, ops[1], 4) + '%s.f[0]=%s(%s.f[0],s.f[0]);' % (D(), mm[mn], D())
        bit = {'andps': '&', 'orps': '|', 'xorps': '^', 'andpd': '&', 'orpd': '|', 'xorpd': '^', 'pand': '&',
               'por': '|', 'pxor': '^'}
        if mn in bit:
            if self.is_reg(ops[1]) and ops[1].reg == d.reg and mn in ('xorps', 'pxor', 'xorpd'):
                return '%s.q[0]=0;%s.q[1]=0;' % (D(), D())
            return self.xsrc(i, ops[1]) + '%s.q[0]%s=s.q[0];%s.q[1]%s=s.q[1];' % (D(), bit[mn], D(), bit[mn])
        if mn in ('andnps', 'andnpd', 'pandn'):
            return self.xsrc(i, ops[1]) + '%s.q[0]=~%s.q[0]&s.q[0];%s.q[1]=~%s.q[1]&s.q[1];' % (D(), D(), D(), D())
        preds = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}
        m = re.match(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(ps|ss)$', mn)
        if m:
            p = preds[m.group(1)] if m.group(1) else (ops[2].imm & 7)
            if m.group(2) == 'ps':
                return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.u[k]=cmpf(%d,%s.f[k],s.f[k]);' % (D(), p, D())
            return self.xsrc(i, ops[1], 4) + '%s.u[0]=cmpf(%d,%s.f[0],s.f[0]);' % (D(), p, D())
        if mn in ('comiss', 'ucomiss'):
            return (self.xsrc(i, ops[1], 4) + 'float a=%s.f[0],b=s.f[0];u32 fl=0;'
                    'if(a!=a||b!=b)fl=F_CF|F_ZF|F_PF;else if(a<b)fl=F_CF;else if(a==b)fl=F_ZF;lf_op=LF_EXPL;lf_r=fl;'
                    % D())
        if mn in ('comisd', 'ucomisd'):
            return (self.xsrc(i, ops[1], 8) + 'double a=%s.d[0],b=s.d[0];u32 fl=0;'
                    'if(a!=a||b!=b)fl=F_CF|F_ZF|F_PF;else if(a<b)fl=F_CF;else if(a==b)fl=F_ZF;lf_op=LF_EXPL;lf_r=fl;'
                    % D())
        if mn == 'shufps':
            imm = ops[2].imm
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;%s.f[0]=dd.f[%d];%s.f[1]=dd.f[%d];%s.f[2]=s.f[%d];%s.f[3]=s.f[%d];' % (
                D(), D(), imm & 3, D(), (imm >> 2) & 3, D(), (imm >> 4) & 3, D(), (imm >> 6) & 3)
        if mn == 'pshufd':
            imm = ops[2].imm
            return self.xsrc(i, ops[1]) + ''.join('%s.u[%d]=s.u[%d];' % (D(), k, (imm >> (2 * k)) & 3) for k in range(4))
        if mn == 'unpcklps':
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;%s.f[0]=dd.f[0];%s.f[1]=s.f[0];%s.f[2]=dd.f[1];%s.f[3]=s.f[1];' % (
                D(), D(), D(), D(), D())
        if mn == 'unpckhps':
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;%s.f[0]=dd.f[2];%s.f[1]=s.f[2];%s.f[2]=dd.f[3];%s.f[3]=s.f[3];' % (
                D(), D(), D(), D(), D())
        if mn == 'punpcklwd':
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;for(int k=0;k<4;k++){%s.w[2*k]=dd.w[k];%s.w[2*k+1]=s.w[k];}' % (D(), D(), D())
        if mn == 'punpckhwd':
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;for(int k=0;k<4;k++){%s.w[2*k]=dd.w[4+k];%s.w[2*k+1]=s.w[4+k];}' % (D(), D(), D())
        if mn == 'movmskps':
            s = self.xr(i, ops[1])
            return self.wr(i, d, '((c->xmm[{0}].u[0]>>31)|((c->xmm[{0}].u[1]>>31)<<1)|((c->xmm[{0}].u[2]>>31)<<2)|'
                                 '((c->xmm[{0}].u[3]>>31)<<3))'.format(s))
        if mn == 'cvtsi2ss':
            s = ops[1]
            v = self.rd(i, s, 4) if not self.is_mem(s) else 'M32(%s)' % self.ea(i, s)
            return '%s.f[0]=(float)(s32)%s;' % (D(), v)
        if mn == 'cvtsi2sd':
            s = ops[1]
            v = self.rd(i, s, 4) if not self.is_mem(s) else 'M32(%s)' % self.ea(i, s)
            return '%s.d[0]=(double)(s32)%s;' % (D(), v)
        if mn in ('cvttss2si', 'cvtss2si'):
            return self.xsrc(i, ops[1], 4) + self.wr(i, d, 'cvt_f2i(c,s.f[0],%d)' % (mn == 'cvttss2si'))
        if mn in ('cvttsd2si', 'cvtsd2si'):
            return self.xsrc(i, ops[1], 8) + self.wr(i, d, 'cvt_d2i(c,s.d[0],%d)' % (mn == 'cvttsd2si'))
        if mn == 'cvtss2sd': return self.xsrc(i, ops[1], 4) + '%s.d[0]=(double)s.f[0];' % D()
        if mn == 'cvtsd2ss': return self.xsrc(i, ops[1], 8) + '%s.f[0]=(float)s.d[0];' % D()
        if mn == 'cvtdq2ps': return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.f[k]=(float)s.s[k];' % D()
        if mn in ('cvttps2dq', 'cvtps2dq'):
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<4;k++)%s.s[k]=cvt_f2i(c,s.f[k],%d);' % (D(), mn == 'cvttps2dq')
        if mn == 'ldmxcsr': return 'c->mxcsr=M32(%s);' % self.ea(i, d)
        if mn == 'stmxcsr': return 'M32(%s)=c->mxcsr;' % self.ea(i, d)
        pint = {'paddb': ('b', '+'), 'paddw': ('w', '+'), 'paddd': ('u', '+'), 'paddq': ('q', '+'),
                'psubb': ('b', '-'), 'psubw': ('w', '-'), 'psubd': ('u', '-'), 'psubq': ('q', '-')}
        if mn in pint:
            f, e = pint[mn]
            n = {'b': 16, 'w': 8, 'u': 4, 'q': 2}[f]
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<%d;k++)%s.%s[k]%s=s.%s[k];' % (n, D(), f, e, f)
        if mn == 'paddusw':
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<8;k++){u32 v=(u32)%s.w[k]+s.w[k];%s.w[k]=v>0xffff?0xffff:v;}' % (D(), D())
        if mn == 'pavgw':
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<8;k++)%s.w[k]=(u16)(((u32)%s.w[k]+s.w[k]+1)>>1);' % (D(), D())
        if mn == 'pcmpgtw':
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<8;k++)%s.w[k]=%s.sw[k]>s.sw[k]?0xffff:0;' % (D(), D())
        sh = {'psrld': ('u', '>>', 32), 'pslld': ('u', '<<', 32), 'psrlq': ('q', '>>', 64), 'psllq': ('q', '<<', 64),
              'psrlw': ('w', '>>', 16), 'psllw': ('w', '<<', 16)}
        if mn in sh and self.is_imm(ops[1]):
            f, e, bits = sh[mn]
            n = ops[1].imm
            cnt = {'u': 4, 'q': 2, 'w': 8}[f]
            if n >= bits: return '%s.q[0]=0;%s.q[1]=0;' % (D(), D())
            return 'for(int k=0;k<%d;k++)%s.%s[k]%s=%d;' % (cnt, D(), f, e, n)
        if mn in sh:  # count in the low 64 bits of an xmm register or a 128-bit memory operand
            f, e, bits = sh[mn]
            cnt = {'u': 4, 'q': 2, 'w': 8}[f]
            return self.xsrc(i, ops[1]) + ('if(s.q[0]>=%d){%s.q[0]=0;%s.q[1]=0;}else{int n=(int)s.q[0];'
                                           'for(int k=0;k<%d;k++)%s.%s[k]%s=n;}' % (bits, D(), D(), cnt, D(), f, e))
        pd = {'addpd': '+', 'subpd': '-', 'mulpd': '*', 'divpd': '/'}
        if mn in pd:
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<2;k++)%s.d[k]=%s.d[k]%ss.d[k];' % (D(), D(), pd[mn])
        sd = {'addsd': '+', 'subsd': '-', 'mulsd': '*', 'divsd': '/'}
        if mn in sd:
            return self.xsrc(i, ops[1], 8) + '%s.d[0]=%s.d[0]%ss.d[0];' % (D(), D(), sd[mn])
        if mn == 'sqrtsd': return self.xsrc(i, ops[1], 8) + '%s.d[0]=sqrt(s.d[0]);' % D()
        if mn == 'sqrtpd': return self.xsrc(i, ops[1]) + '%s.d[0]=sqrt(s.d[0]);%s.d[1]=sqrt(s.d[1]);' % (D(), D())
        if mn in ('maxsd', 'minsd'):
            e = '>' if mn == 'maxsd' else '<'
            return self.xsrc(i, ops[1], 8) + '%s.d[0]=%s.d[0]%ss.d[0]?%s.d[0]:s.d[0];' % (D(), D(), e, D())
        m = re.match(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(pd|sd)$', mn)
        if m:
            p = preds[m.group(1)] if m.group(1) else (ops[2].imm & 7)
            n = 2 if m.group(2) == 'pd' else 1
            return (self.xsrc(i, ops[1], 16 if n == 2 else 8) +
                    'for(int k=0;k<%d;k++){double a=%s.d[k],b=s.d[k];int r;switch(%d){case 0:r=a==b;break;case 1:r=a<b;break;'
                    'case 2:r=a<=b;break;case 3:r=a!=a||b!=b;break;case 4:r=!(a==b);break;case 5:r=!(a<b);break;'
                    'case 6:r=!(a<=b);break;default:r=a==a&&b==b;}%s.q[k]=r?~0ULL:0;}' % (n, D(), p, D()))
        if mn == 'unpcklpd': return self.xsrc(i, ops[1]) + '%s.q[1]=s.q[0];' % D()
        if mn == 'unpckhpd': return self.xsrc(i, ops[1]) + '%s.q[0]=%s.q[1];%s.q[1]=s.q[1];' % (D(), D(), D())
        if mn == 'shufpd':
            imm = ops[2].imm
            return self.xsrc(i, ops[1]) + 'XMM dd=%s;%s.q[0]=dd.q[%d];%s.q[1]=s.q[%d];' % (D(), D(), imm & 1, D(), (imm >> 1) & 1)
        if mn == 'cvtdq2pd': return self.xsrc(i, ops[1], 8) + '%s.d[0]=(double)s.s[0];%s.d[1]=(double)s.s[1];' % (D(), D())
        if mn == 'cvtps2pd': return self.xsrc(i, ops[1], 8) + 'float a=s.f[0],b=s.f[1];%s.d[0]=a;%s.d[1]=b;' % (D(), D())
        if mn == 'cvtpd2ps': return self.xsrc(i, ops[1]) + 'double a=s.d[0],b=s.d[1];%s.f[0]=(float)a;%s.f[1]=(float)b;%s.q[1]=0;' % (D(), D(), D())
        if mn == 'pextrw':
            return self.wr(i, d, 'c->xmm[%d].w[%d]' % (self.xr(i, ops[1]), ops[2].imm & 7))
        if mn == 'pinsrw':
            s = ops[1]
            v = self.rd(i, s, 4) if self.is_reg(s) else 'M16(%s)' % self.ea(i, s)
            return '%s.w[%d]=(u16)%s;' % (D(), ops[2].imm & 7, v)
        if mn in ('pcmpeqd', 'pcmpeqw', 'pcmpeqb', 'pcmpgtd', 'pcmpgtb'):
            f, n = {'d': ('u', 4), 'w': ('w', 8), 'b': ('b', 16)}[mn[-1]]
            sf = {'u': 's', 'w': 'sw', 'b': 'b'}[f]
            e = '==' if 'eq' in mn else '>'
            if 'gt' in mn and f == 'b': raise Unsupported('pcmpgtb')
            fld = f if e == '==' else sf
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<%d;k++)%s.%s[k]=%s.%s[k]%ss.%s[k]?-1:0;' % (n, D(), f, D(), fld, e, fld)
        if mn == 'pmovmskb':
            s = self.xr(i, ops[1])
            return 'u32 m=0;for(int k=0;k<16;k++)m|=(u32)(c->xmm[%d].b[k]>>7)<<k;%s' % (s, self.wr(i, d, 'm'))
        if mn == 'pmaxsw':
            return self.xsrc(i, ops[1]) + 'for(int k=0;k<8;k++)if(s.sw[k]>%s.sw[k])%s.sw[k]=s.sw[k];' % (D(), D())
        raise Unsupported('sse ' + mn)


# ----------------------------------------------------------------------------- driver

HEADER = ('#include "../rt/x86.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
          '/* generated functions keep the guest base in a local so stores cannot force reloads */\n'
          '#undef GUEST_MEM\n#define GUEST_MEM gm\n')


_W = {}


def worker_init(exe, starts):
    _W['em'] = Emitter(Image(exe), starts)


# Game functions the runtime replaces (rt/hooks.c). The original body is still emitted, as o_XXXXXXXX, and
# f_XXXXXXXX forwards to the runtime's h_XXXXXXXX, which may call it. Keep this list short: each entry is a
# deliberate departure from the original exe.
HOOKS = {
    0x7CBAF0,  # StartMenu init: leave Continue off the main menu while there are no saves
    0x7CFD20,  # re-enables the Continue/Load tiles: tolerate the Continue tile not existing
    0x789820,  # LoadingMenu::Update: no menu (quitting) -> return instead of reading null+0x5BC
    0x78D080,  # UpdateLoadingMenuState from the loading-screen thread once the menu is gone -> skip
    0x7D0BF0,  # Exit Game: close the app right away (PS5: back to the home screen) instead of tearing down
    # native versions of the hottest small maths routines (rt/fastmath.c); same results, no x87 emulation
    0x416870,  # Vec3 constructor
    0x439E90,  # Vec3 operator+
    0x439EF0,  # Vec3 operator-
    0x4A3760,  # float * Vec3
    0x4B4500,  # Matrix3 * Vec3
    0x43F8D0,  # Matrix3 * Matrix3
    0x62C250,  # Transform * Transform
    0x4B6190,  # dot product (plane normal . point)
    0x4B6160,  # Plane::Distance
    0x4B6100,  # bounding sphere against a plane (culling)
    0x49DA80,  # Plane::WhichSide (culling)
}


def write_if_changed(path, text):
    """Leave an identical file untouched so incremental builds skip it."""
    try:
        with open(path) as fh:
            if fh.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w') as fh:
        fh.write(text)


def emit_chunk(args):
    items, out = args
    em = _W['em']
    decls = set()
    bodies = []
    unimpl = 0
    for f, info in items:
        body = em.function(f, info)
        unimpl += body.count('rt_unimpl(c,') - body.count('"divide by zero"')
        if f in HOOKS:
            body = body.replace('void %s(' % fn(f), 'void o_%08X(' % f, 1)
            body += '\nvoid h_%08X(Ctx *restrict c);\nvoid %s(Ctx *restrict c){h_%08X(c);}' % (f, fn(f), f)
        bodies.append(body)
    for f, info in items:
        decls.add(f)
    # declarations of every function referenced
    refs = set(int(x, 16) for b in bodies for x in re.findall(r'\bf_([0-9A-F]{8})\(c\)', b))
    text = [HEADER]
    for r in sorted(refs | decls):
        text.append('void %s(Ctx *restrict c);\n' % fn(r))
    for b in bodies:
        text.append(b + '\n')
    write_if_changed(out, ''.join(text))
    return unimpl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--units', required=True)
    ap.add_argument('--image-check')
    ap.add_argument('--out', required=True)
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()

    img = Image(a.exe)
    if a.image_check:
        ok, n = check_image(img, a.image_check)
        if ok < min(EXPECT_PROBES_OK, n):
            sys.exit('this is not FalloutNV.exe 1.4.0.525 with plain code (%d/%d probes)' % (ok, n))
        print('image check: %d/%d probes match' % (ok, n))

    units = json.load(open(a.units))['units']
    known = {}
    for u in units:
        for f in u['functions']:
            known[int(f['address'], 16)] = f['size']
    seeds = set(known) | {img.entry} | set(img.tls_cb)
    # code referenced by relocations (vtables, function pointers, EH funclets) outside known bodies
    spans = sorted(known.items())
    import bisect
    keys = [s for s, _ in spans]

    def inside(t):
        k = bisect.bisect_right(keys, t) - 1
        return k >= 0 and keys[k] < t < keys[k] + spans[k][1]

    for r in img.relocs:
        t = img.u32(r)
        if img.in_text(t) and t not in known and not inside(t):
            seeds.add(t)
    print('seeds:', len(seeds))
    d = Discover(img, known)
    funcs = d.run(seeds)
    starts = frozenset(funcs)
    ninsn = sum(len(v['insns']) for v in funcs.values())
    print('functions: %d, instructions: %d' % (len(funcs), ninsn))

    os.makedirs(a.out, exist_ok=True)
    order = sorted(funcs)
    chunks = [order[k:k + a.chunk] for k in range(0, len(order), a.chunk)]
    jobs = []
    for n, ch in enumerate(chunks):
        jobs.append(([(f, funcs[f]) for f in ch], os.path.join(a.out, 'funcs_%03d.c' % n)))
    with mp.Pool(a.jobs, initializer=worker_init, initargs=(a.exe, starts)) as pool:
        unimpl = sum(pool.map(emit_chunk, jobs, chunksize=1))
    print('unsupported instruction sites:', unimpl)

    t = ['#include "../rt/x86.h"\n']
    for f in order:
        t.append('void %s(Ctx *restrict c);\n' % fn(f))
    t.append('const u32 g_func_addr[] = {\n%s};\n' % ',\n'.join('0x%xu' % f for f in order))
    t.append('GuestFn const g_func_ptr[] = {\n%s};\n' % ',\n'.join(fn(f) for f in order))
    t.append('const u32 g_func_count = %d;\n' % len(order))
    write_if_changed(os.path.join(a.out, 'table.c'), ''.join(t))
    import io
    fh = io.StringIO()
    if True:
        fh.write('#include "../rt/loader.h"\n')
        fh.write('const ImageSection g_sections[] = {\n')
        for s in img.sections:
            fh.write('  {"%s", 0x%xu, 0x%xu, 0x%xu, 0x%xu, 0x%xu},\n' % s)
        fh.write('};\nconst int g_section_count = %d;\n' % len(img.sections))
        fh.write('const ImageImport g_imports[] = {\n')
        for iat, dll, name in img.imports:
            fh.write('  {0x%xu, "%s", "%s"},\n' % (iat, dll, name))
        fh.write('};\nconst int g_import_count = %d;\n' % len(img.imports))
        fh.write('const u32 g_entry = 0x%xu;\n' % img.entry)
        fh.write('const u32 g_tls_callbacks[] = {%s0};\n' % ''.join('0x%xu,' % x for x in img.tls_cb))
        t = img.tls or (0, 0, 0, 0)
        fh.write('const u32 g_tls_start = 0x%xu, g_tls_end = 0x%xu, g_tls_index_addr = 0x%xu;\n' % t[:3])
        fh.write('const u32 g_image_size = 0x%xu;\n' % img.pe.OPTIONAL_HEADER.SizeOfImage)
        fh.write('const u32 g_stack_reserve = 0x%xu;\n' % img.pe.OPTIONAL_HEADER.SizeOfStackReserve)
    write_if_changed(os.path.join(a.out, 'image.c'), fh.getvalue())


if __name__ == '__main__':
    main()
