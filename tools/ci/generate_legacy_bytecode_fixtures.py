#!/usr/bin/env python3
"""Regenerate the genuine stock-v2 fixtures; requires an original LuaJIT CLI."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def big_endian(data):
    out = bytearray(data)
    out[4] ^= 1
    def uleb(pos):
        value = shift = 0
        while True:
            byte = data[pos]
            pos += 1
            value |= (byte & 127) << shift
            shift += 7
            if byte < 128:
                return value, pos
    pos = 5
    stripped = bool(data[4] & 2)
    if not stripped:
        size, pos = uleb(pos)
        pos += size
    while True:
        size, pos = uleb(pos)
        if not size:
            break
        end = pos + size
        uv = data[pos + 3]
        pos += 4
        _, pos = uleb(pos)  # GC constants.
        _, pos = uleb(pos)  # Numeric constants.
        bc, pos = uleb(pos)
        debug = lines = 0
        if not stripped:
            debug, pos = uleb(pos)
            if debug:
                _, pos = uleb(pos)
                lines, pos = uleb(pos)
        for _ in range(bc):
            out[pos:pos + 4] = data[pos:pos + 4][::-1]
            pos += 4
        for _ in range(uv):
            out[pos:pos + 2] = data[pos:pos + 2][::-1]
            pos += 2
        if debug:
            width = 1 if lines < 256 else 2 if lines < 65536 else 4
            for i in range(bc):
                slot = end - debug + i * width
                out[slot:slot + width] = data[slot:slot + width][::-1]
        pos = end
    assert pos == len(data)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock_luajit')
    args = parser.parse_args()
    for kind, stripped in [('cells', True), ('cells', False),
                           ('debug', False), ('sharing', True)]:
        source = ROOT / 'tests/fixtures' / f'legacy-bytecode-{kind}.lua'
        name = '@stock-cell-debug.lua' if kind == 'debug' else '@stock-cell-' + kind + '.lua'
        code = ('local f=assert(loadstring(io.read("*a"),"' + name + '"));'
                'io.write(string.dump(f,' + str(stripped).lower() + '))')
        data = subprocess.check_output([args.stock_luajit, '-e', code],
                                       input=source.read_bytes())
        assert data[:4] == b'\x1bLJ\x02', 'requires an original v2 LuaJIT compiler'
        assert data[4] & 8 and not data[4] & 1, 'requires a little-endian FR2 compiler'
        stem = source.with_suffix('').name + ('-stripped' if stripped else '')
        source.with_name(stem + '.bc').write_bytes(data)
        source.with_name(stem + '-be.bc').write_bytes(big_endian(data))
    source = ROOT / 'tests/fixtures/legacy-bytecode-cells.lua'
    code = ('local f=assert(load(io.read("*a"),"@stock-fr1.lua","tW"));'
            'io.write(string.dump(f))')
    data = subprocess.check_output([args.stock_luajit, '-e', code], input=source.read_bytes())
    assert data[:5] == b'\x1bLJ\x02\x00'
    source.with_name('legacy-bytecode-fr1.bc').write_bytes(data)
    # A valid stock frame just above the normalizer's three-scratch-slot limit.
    source = ('local x=1; local function get()return x end; '
              'local function sink(...)return ... end; return sink(' +
              ','.join(map(str, range(243))) + ')')
    code = ('local f=assert(loadstring(io.read("*a"),"@wide-frame.lua"));'
            'assert(require("jit.util").funcinfo(f).stackslots==248);'
            'io.write(string.dump(f,true))')
    data = subprocess.check_output([args.stock_luajit, '-e', code], input=source.encode())
    assert data[:4] == b'\x1bLJ\x02'
    (ROOT / 'tests/fixtures/legacy-bytecode-wide-frame.bc').write_bytes(data)
    for count in (300, 70000):
        source = ('local function owner()\nlocal x=1\nlocal function get()return x end\n' +
                  '\n' * count + 'x=2\nreturn x+nil\nend\n' +
                  'local ok,err=pcall(owner)\n' +
                  "assert(not ok and err:match('wide%-lines.lua:" + str(count+5) + ":'))\n" +
                  'return true\n')
        code = 'io.write(string.dump(assert(loadstring(io.read("*a"),"@wide-lines.lua"))))'
        data = subprocess.check_output([args.stock_luajit, '-e', code], input=source.encode())
        assert data[:4] == b'\x1bLJ\x02'
        stem = ROOT / 'tests/fixtures' / ('legacy-bytecode-lines-' + str(count))
        stem.with_suffix('.bc').write_bytes(data)
        stem.with_name(stem.name+'-be.bc').write_bytes(big_endian(data))

if __name__ == '__main__':
    main()
