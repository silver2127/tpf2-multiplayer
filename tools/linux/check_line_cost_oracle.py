#!/usr/bin/env python3
"""Compare travel-cost and path hashes with original Windows build 35924 instructions.

Usage: check_line_cost_oracle.py /path/to/TransportFever2.exe
Requires x86-64 Linux, Python 3 and g++. Runs isolated arithmetic only.
"""
from pathlib import Path
import ctypes,hashlib,struct,mmap,random,json,sys,subprocess,tempfile
if len(sys.argv) != 2:
 raise SystemExit(__doc__)
binary=Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(binary).hexdigest()=='782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c'
pe=struct.unpack_from('<I',binary,60)[0];sh=pe+24+struct.unpack_from('<H',binary,pe+20)[0]
def read(a,n):
 for j in range(struct.unpack_from('<H',binary,pe+6)[0]):
  _,va,sz,off=struct.unpack_from('<IIII',binary,sh+j*40+8)
  if va<=a and a+n<=va+sz:return binary[off+a-va:off+a-va+n]
 raise ValueError(hex(a))
body=read(0x96f68a,0x96f785-0x96f68a)
# Save SysV nonvolatile registers used by the Windows inline body; supply
# its two input records and private stack slot. No calls, branches or RIP data.
prefix=bytes.fromhex('55415441564881ec90000000893c2489742408668954240c4889e5488d742408')
suffix=bytes.fromhex('4c89d04881c490000000415e415c5dc3')
mem=mmap.mmap(-1,4096,prot=3);mem.write(prefix+body+suffix);addr=ctypes.addressof(ctypes.c_char.from_buffer(mem))
assert ctypes.CDLL(None).mprotect(ctypes.c_void_p(addr),ctypes.c_size_t(4096),5)==0
orig=ctypes.CFUNCTYPE(ctypes.c_uint64,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint16)(addr)
temp=tempfile.TemporaryDirectory(prefix='tpf2mp-line-cost-');work=Path(temp.name)
source=Path(__file__).resolve().parents[2]/'native/linux/src'
wrapper=work/'wrapper.cpp'
wrapper.write_text('#include "windows_person_cost_linux.h"\nextern "C" uint64_t candidate_line(uint32_t p,uint32_t l,uint16_t s){return Tpf2mpWindowsLineCostHash(p,l,s); }\nextern "C" uint64_t candidate_pair(uint32_t a,uint32_t b){return Tpf2mpWindowsPathHash(a,b); }\n')
library=work/'candidate.so'
subprocess.run(['g++','-std=c++17','-O2','-shared','-fPIC','-I',str(source),str(wrapper),str(source/'windows_person_cost_linux.cpp'),'-o',str(library)],check=True)
lib=ctypes.CDLL(str(library));helper=lib.candidate_line;helper.argtypes=[ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint16];helper.restype=ctypes.c_uint64
rng=random.Random(35924);edges=[0,1,20808,25872,0x7fffffff,0x80000000,0xffffffff];stops=[0,1,0x7fff,0x8000,0xffff]
cases=[(p,l,s) for p in edges for l in edges for s in stops]+[(rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(16)) for _ in range(100000)]
for args in cases:
 a,b=orig(*args),helper(*args)
 assert a==b,(args,hex(a),hex(b))
results=[{'kind':'line_cost','passed':True,'cases':len(cases),'body_sha256':hashlib.sha256(body).hexdigest(),'fixtures':[{'args':v,'hash':hex(orig(*v))} for v in [(25872,20808,0),(25872,20808,1),(0,0,0),(0xffffffff,0xffffffff,0xffff),(0x80000000,0x7fffffff,0x8000)]]}]

pair=lib.candidate_pair;pair.argtypes=[ctypes.c_uint32,ctypes.c_uint32];pair.restype=ctypes.c_uint64
pair_cases=[(a,b) for a in edges for b in edges]+[(rng.getrandbits(32),rng.getrandbits(32)) for _ in range(100000)]
for label,start,end,setup,return_value in [
 ('path_revision',0x90ec6d,0x90ed0e,'89fb897d8089b570010000','4889d0'),
 ('path_batch',0x90e98e,0x90ea2d,'89f889f3897d80','4c89c0')]:
 body=read(start,end-start)
 prefix=bytes.fromhex('53554881ec08020000488dac2480000000'+setup)
 suffix=bytes.fromhex(return_value+'4881c4080200005d5bc3')
 page=mmap.mmap(-1,4096,prot=3);page.write(prefix+body+suffix)
 address=ctypes.addressof(ctypes.c_char.from_buffer(page))
 assert ctypes.CDLL(None).mprotect(ctypes.c_void_p(address),ctypes.c_size_t(4096),5)==0
 original=ctypes.CFUNCTYPE(ctypes.c_uint64,ctypes.c_uint32,ctypes.c_uint32)(address)
 for args in pair_cases:
  actual,expected=original(*args),pair(*args)
  assert actual==expected,(label,args,hex(actual),hex(expected))
 results.append({'kind':label,'passed':True,'cases':len(pair_cases),'body_sha256':hashlib.sha256(body).hexdigest(),'fixtures':[{'args':a,'hash':hex(original(*a))} for a in pair_cases[:5]]})

print(json.dumps({'passed':True,'cases':sum(r['cases'] for r in results),'oracles':results},indent=2))
