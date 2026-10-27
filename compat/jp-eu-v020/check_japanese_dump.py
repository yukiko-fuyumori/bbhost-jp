#!/usr/bin/env python3
"""Read-only compatibility check for Bloodborne JP Old Hunters data in BBHost.

Usage: py -3 check_japanese_dump.py "C:\\Games\\CUSA03014" "C:\\Games\\CUSA03014\\eboot.elf"
No game data is written, copied, or patched.
"""
import argparse
import hashlib
from pathlib import Path
import struct

BBHOST_EBOOT = "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7"

def entries(p):
    b = p.read_bytes()
    if len(b) < 20 or b[:4] != b'\0PSF':
        raise ValueError('Not a valid PSF/SFO header')
    _, _, key_off, val_off, count = struct.unpack_from('<IIIII', b)
    if count > (len(b)-20)//16 or not 20 <= key_off < len(b) or not 20 <= val_off < len(b):
        raise ValueError('Invalid SFO table')
    fields={}
    for i in range(count):
        keyidx, fmt, actual, reserved, dataoff = struct.unpack_from('<HHIII', b, 20+16*i)
        k = key_off+keyidx; v = val_off+dataoff
        if k >= len(b) or v > len(b) or actual > reserved or reserved > len(b)-v:
            continue
        kend=b.find(b'\0', k)
        if kend < 0 or kend>len(b):
            continue
        name=b[k:kend].decode('ascii','replace')
        raw=b[v:v+actual]
        if fmt==0x0404 and len(raw)>=4:
            fields[name]=struct.unpack_from('<I',raw)[0]
        else:
            fields[name]=raw.rstrip(b'\0').decode('utf-8','replace')
    return fields

def main():
    pa=argparse.ArgumentParser(description=__doc__)
    pa.add_argument('game_dir',type=Path)
    pa.add_argument('eboot',type=Path)
    a=pa.parse_args()
    sfo=a.game_dir / 'sce_sys' / 'param.sfo'
    x=entries(sfo)
    print('SFO:',sfo)
    for field in ('TITLE_ID','CONTENT_ID','APP_VER','USER_DEFINED_PARAM_1'):
        print(f'{field}: {x.get(field, "not present")}')
    h=hashlib.sha256()
    with a.eboot.open('rb') as f:
        for block in iter(lambda:f.read(4*1024*1024),b''):h.update(block)
    print('eboot SHA256:',h.hexdigest())
    compatible=x.get('TITLE_ID')=='CUSA03014' and x.get('USER_DEFINED_PARAM_1')==0 and h.hexdigest()==BBHOST_EBOOT
    print('Compatible with JP-native variant:', 'YES (preflight only)' if compatible else 'NO (not this exact supported configuration)')
    if not compatible:raise SystemExit(2)

if __name__=='__main__':main()
