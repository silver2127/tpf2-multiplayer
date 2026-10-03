#!/usr/bin/env python3
"""Read-only verification of the Linux game's ELF identity and every patch site."""
import json,struct,sys
from pathlib import Path
manifest=json.loads((Path(__file__).resolve().parents[2]/'linux/sites.json').read_text())
b=Path(sys.argv[1]).read_bytes()
assert b[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',b,18)[0]==62
phoff=struct.unpack_from('<Q',b,32)[0];size,count=struct.unpack_from('<HH',b,54)
segments=[];identity=False
for i in range(count):
    kind,flags,offset,va,pa,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',b,phoff+i*size)
    if kind==1:segments.append((va,va+filesz,offset))
    if kind==4:
        end=offset+filesz
        while offset+12<=end:
            namesz,descsz,ntype=struct.unpack_from('<III',b,offset);offset+=12
            name=b[offset:offset+namesz];offset+=(namesz+3)&~3
            desc=b[offset:offset+descsz];offset+=(descsz+3)&~3
            if name==b'GNU\0' and ntype==3:identity=desc.hex()==manifest['build_id']
assert identity,'unsupported game build'
for address,expected in manifest['sites'].items():
    a=int(address,16);raw=bytes.fromhex(expected)
    lo,hi,off=next(s for s in segments if s[0]<=a and a+len(raw)<=s[1])
    assert b[off+a-lo:off+a-lo+len(raw)]==raw,f'patch mismatch at {address}'
print('PASS: GNU build-id and all',len(manifest['sites']),'Linux patch sites')
