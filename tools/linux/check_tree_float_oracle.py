#!/usr/bin/env python3
"""Check tree scale/angle draw order against Windows build 35924 instructions.

Usage: check_tree_float_oracle.py /path/to/TransportFever2.exe
Requires x86-64 Linux. Runs isolated float/MT arithmetic, never game imports.
"""
from pathlib import Path
import ctypes as C,mmap,struct,json,hashlib,sys
if len(sys.argv) != 2:raise SystemExit(__doc__)
P=Path(sys.argv[1]);b=P.read_bytes();assert hashlib.sha256(b).hexdigest()=='782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c'
p=struct.unpack_from('<I',b,60)[0];sh=p+24+struct.unpack_from('<H',b,p+20)[0]
def read(a,n):
 for j in range(struct.unpack_from('<H',b,p+6)[0]):
  _,va,sz,off=struct.unpack_from('<IIII',b,sh+j*40+8)
  if va<=a and a+n<=va+sz:return b[off+a-va:off+a-va+n]
 raise ValueError(hex(a))
lo=0x900000;m=mmap.mmap(-1,0x3900000,prot=3);addr=C.addressof(C.c_char.from_buffer(m))
def put(a,v):m[a-lo:a-lo+len(v)]=v
def hx(v):return bytes.fromhex(v)
def jmp(a,b):return b'\xe9'+struct.pack('<i',b-a-5)
for a,n in [(0x918b40,0x299),(0x955170,0x126),(0x95cdd9,0x26),(0x3089b00,48),(0x2f20a90,16)]:put(a,read(a,n))
put(0x41cc1d0,struct.pack('<I',2))
# Elide log/floor setup: IEEE float32 canonical generation uses one u32
# draw. Keep the original arithmetic, MT advancement and both caller calls.
setup=hx('44 0f 29 4c 24 20 b8 01 00 00 00 45 0f 57 c9 0f 57 f6')
setup+=hx('41 b9 00 00 80 4f 66 45 0f 6e c1 41 b9 00 00 80 3f 66 41 0f 6e f9')
put(0x955190,setup+b'\x90'*(0x9551e2-0x955190-len(setup)))
code=hx('55 48 81 ec 80 00 00 00 48 89 74 24 78 48 8d af 60 fb ff ff')
for off,raw in [(0x60,0x3f400000),(0x64,0x3fa00000),(0x68,0),(0x6c,0x40c90fdb)]:code+=hx('c7 44 24')+bytes([off])+struct.pack('<I',raw)
code+=jmp(0x900000+len(code),0x95cdd9);put(0x900000,code)
put(0x95cdff,hx('48 8b 74 24 78 f3 44 0f 11 06 f3 0f 11 46 04 48 81 c4 80 00 00 00 5d c3'))
assert C.CDLL(None).mprotect(C.c_void_p(addr),C.c_size_t(len(m)),5)==0
class MT(C.Structure):_fields_=[('words',C.c_uint32*624),('index',C.c_uint64)]
fn=C.CFUNCTYPE(None,C.POINTER(MT),C.POINTER(C.c_float))(addr)
def state(seed):
 a=MT();a.words[0]=seed
 for i in range(1,624):a.words[i]=(1812433253*(a.words[i-1]^(a.words[i-1]>>30))+i)&0xffffffff
 p=a.words[396]^a.words[623];v=(p<<1)^(0x321161bf if p&0x80000000 else 0);a.words[0]=(a.words[0]&0x80000000)|(v&0x7fffffff);a.index=624;return a
def draw(a):
 if a.index==624:
  for i in range(624):
   v=(a.words[i]&0x80000000)|(a.words[(i+1)%624]&0x7fffffff);a.words[i]=a.words[(i+397)%624]^(v>>1)^(0x9908b0df if v&1 else 0)
  a.index=0
 y=a.words[a.index];a.index+=1;y^=y>>11;y^=(y<<7)&0x9d2c5680;y^=(y<<15)&0xefc60000;y^=y>>18;return y
f=lambda x:C.c_float(x).value
cases=0
for seed in [0,1,5489,0x80000000,0xffffffff]:
 a=state(seed);e=MT.from_buffer_copy(a)
 for i in range(2000):
  first,second=draw(e),draw(e);expected=(f(f(f(first)*2**-32)*.5+.75),f(f(f(second)*2**-32)*f(6.283185307179586)))
  out=(C.c_float*2)();fn(C.byref(a),out)
  assert tuple(out)==expected and bytes(a)==bytes(e),(seed,i,tuple(out),expected,a.index,e.index)
  cases+=1
def untemper(y):
 x=y
 for _ in range(6):x=y^(x>>18)
 y=x
 for _ in range(6):x=y^((x<<15)&0xefc60000)
 y=x
 for _ in range(6):x=y^((x<<7)&0x9d2c5680)
 y=x
 for _ in range(6):x=y^(x>>11)
 return x&0xffffffff
# Include float rounding to exactly 1.0, which native's private unit core
# clamps. The adapter calls the already corrected common core instead.
for first in [0,1,0x80000000,0xffffff80,0xffffffff]:
 for second in [0,1,0x80000000,0xffffff80,0xffffffff]:
  a=state(5489);a.index=0;a.words[0]=untemper(first);a.words[1]=untemper(second)
  e=MT.from_buffer_copy(a);draw(e);draw(e)
  expected=(f(f(f(first)*2**-32)*.5+.75),f(f(f(second)*2**-32)*f(6.283185307179586)))
  out=(C.c_float*2)();fn(C.byref(a),out)
  assert tuple(out)==expected and bytes(a)==bytes(e),(first,second,tuple(out),expected)
  cases+=1
print(json.dumps({'passed':True,'cases':cases,'windows_call_window':'0x95cdd9..0x95cdff','windows_draw_core':'0x9551e2..0x955296','scale_first':True,'full_mt_state_equal':True},indent=2))
