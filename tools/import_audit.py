#!/usr/bin/env python3
"""Compare FalloutNV.exe's import table with the names rt/ actually registers.

Not part of the game build. The recompiled code and the import list are not in
this repository (gen/ is generated from the player's own exe), so this is how
to see which imports would hit stub_missing in rt/main.c.

    python3 tools/import_audit.py
    python3 tools/import_audit.py --exe game/FalloutNV.exe

Matching is the loader's: DLL names ignore case, function names do not.
Ordinals with no name are "#N", the same spelling tools/recomp.py writes.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(ROOT, 'rt')

# Names the recompiler emits inline (tools/recomp.py INLINE_IMPORTS). The
# runtime still registers them, for GetProcAddress and for any call site the
# recompiler did not inline.
INLINE = {
    'InterlockedIncrement', 'InterlockedDecrement', 'InterlockedExchange',
    'InterlockedExchangeAdd', 'InterlockedCompareExchange', 'TlsGetValue',
    'GetCurrentThreadId',
}


_DLL_TOKEN = {'K': 'KERNEL32.dll', 'U': 'USER32.dll'}


def _macro(text):
    """The file's #define E(n), if it builds an ApiEntry. Returns (dll, name_prefix) or None."""
    m = re.search(r'#define\s+E\(n\)\s*\{([^}]+)\}', text)
    if not m:
        return None
    body = m.group(1)
    # {"d3dx9_38.dll", "D3DX" #n, fn}  or  {K, #n, k_##n}
    lit = re.match(r'\s*"([^"]+)"\s*,\s*"([^"]*)"\s*#n', body)
    if lit:
        return lit.group(1), lit.group(2)
    tok = re.match(r'\s*([A-Za-z_]+)\s*,\s*#n', body)
    if tok and tok.group(1) in _DLL_TOKEN:
        return _DLL_TOKEN[tok.group(1)], ''
    return None


def _tables(text):
    """Bodies of `static const ApiEntry name[] = { ... };` in one file."""
    heads = list(re.finditer(r'static const ApiEntry\s+\w+\[\]\s*=\s*\{', text))
    bodies = []
    for h in heads:
        i = h.end()
        depth = 1
        while i < len(text) and depth:
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
            i += 1
        # include a trailing ';' if present; the body is inside the braces
        bodies.append(text[h.end():i - 1])
    return bodies


def registered():
    """(dll, name, source file) from the ApiEntry tables in rt/*.c. Nothing else."""
    out = []
    for fn in sorted(os.listdir(RT)):
        if not fn.endswith('.c'):
            continue
        text = open(os.path.join(RT, fn), encoding='utf-8', errors='replace').read()
        macro = _macro(text)
        for body in _tables(text):
            body = '\n'.join(line for line in body.splitlines() if not line.strip().startswith('#'))
            if macro:
                for name in re.findall(r'\bE\(([A-Za-z0-9_]+)\)', body):
                    out.append((macro[0], macro[1] + name, fn))
            for dll, name in re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,', body):
                out.append((dll, name, fn))
    seen = set()
    uniq = []
    for dll, name, fn in out:
        key = (dll.lower(), name)
        if key in seen:
            continue
        seen.add(key)
        uniq.append((dll, name, fn))
    return uniq


def exe_imports(path):
    import pefile
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    rows = []
    if not hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        return rows
    for desc in pe.DIRECTORY_ENTRY_IMPORT:
        dll = desc.dll.decode('ascii', 'replace')
        for imp in desc.imports:
            if imp.name:
                name = imp.name.decode('ascii', 'replace')
            else:
                name = '#%d' % imp.ordinal
            rows.append((dll, name))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', help='FalloutNV.exe to diff against the registered names')
    args = ap.parse_args()
    regs = registered()
    by = {}
    for dll, name, fn in regs:
        by.setdefault(dll.lower(), []).append((name, fn))
    print('registered imports: %d' % len(regs))
    for dll in sorted(by):
        print('  %s: %d' % (dll, len(by[dll])))
    print('inlined by the recompiler (still registered): %s' % ', '.join(sorted(INLINE)))
    if not args.exe:
        print('no --exe: the IAT is not in this repository. Pass the player\'s FalloutNV.exe to list gaps.')
        return 0
    if not os.path.isfile(args.exe):
        print('not a file: %s' % args.exe, file=sys.stderr)
        return 2
    have = {(dll.lower(), name) for dll, name, _fn in regs}
    imps = exe_imports(args.exe)
    missing = [(d, n) for d, n in imps if (d.lower(), n) not in have]
    print('exe imports: %d' % len(imps))
    print('not registered (stub_missing if called): %d' % len(missing))
    for dll, name in missing:
        print('  %s!%s' % (dll, name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
