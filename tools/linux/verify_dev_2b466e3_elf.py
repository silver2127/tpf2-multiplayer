#!/usr/bin/env python3
"""Verify the build-35924 VSTOP and OpenGL sites used by this integration."""
from pathlib import Path
import struct
import subprocess
import sys

binary=Path(sys.argv[1]).expanduser()
data=binary.read_bytes()
assert data[:6]==b'\x7fELF\x02\x01'
assert '3a0e156390b0e6f1e372051c24802c8493ae454a' in subprocess.check_output(['readelf','-n',str(binary)],text=True)
offset=struct.unpack_from('<Q',data,32)[0]
size,count=struct.unpack_from('<HH',data,54)
segments=[struct.unpack_from('<IIQQQQQQ',data,offset+i*size) for i in range(count)]
def read(va,n):
    for ty,flags,off,vaddr,_,filesz,memsz,align in segments:
        if ty==1 and vaddr<=va and va+n<=vaddr+filesz:
            return data[off+va-vaddr:off+va-vaddr+n]
    raise AssertionError(hex(va))
def check(va,hexbytes):
    expected=bytes.fromhex(hexbytes)
    assert read(va,len(expected))==expected,hex(va)
check(0x3496531,'e8 3a 76 24 fd') # single SDL swap call
check(0x6ddb70,'ff 25 c2 a0 36 05') # PLT -> 0x5a47c38
assert 'SDL_GL_SwapWindow' in subprocess.check_output(['objdump','-R',str(binary)],text=True).split('0000000005a47c38')[1].splitlines()[0]
check(0x349651b,'48 8b bf d8 00 00 00') # this->window passed in rdi
check(0x15ebf00,'f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55')
check(0x15ebf37,'89 d3 49 89 fc 49 89 f6') # edx entity, rdi return, rsi engine
check(0x15ebf4f,'41 89 cf') # ecx -> r15d (stopped)
check(0x15ebf65,'89 9d 70 f2 ff ff') # entity in payload
check(0x15ebf75,'44 88 bd 74 f2 ff ff c6 45 b8 08') # bool and variant tag
assert read(0x4326a00,100).split(b'\0')[0]==b'Command make_cmd::SetUserStopped(const ecs::Engine&, ecs::Entity, bool)'
for ret,end,want in [(0x1431102,0x1431129,0xd80de86f),(0x1431204,0x143122b,0x27d7f59d)]:
    h=2166136261
    for b in read(ret-5,end-(ret-5)):h=((h^b)*16777619)&0xffffffff
    assert h==want,hex(ret)
    for call,target in [(ret-5,0x15ebf00),(end-5,0x15da840)]:
        raw=read(call,5);assert raw[0]==0xe8 and call+5+struct.unpack('<i',raw[1:])[0]==target
print('PASS: build ID, OpenGL call/PLT/window argument, VSTOP ABI/tag and both guarded factory-to-Add windows')
