"""Regression for metre-indexed alignment blocks published into 2 m caches."""
import ctypes as C
import numpy as np
from pathlib import Path
from test_terrain_cache import Terrain
class Region(C.Structure):
 _fields_=[('data',C.POINTER(C.c_uint16)),('width',C.c_int),('height',C.c_int),('x',C.c_int),('y',C.c_int)]
class Regions(C.Structure):
 _fields_=[('first',C.POINTER(Region)),('last',C.POINTER(Region)),('end',C.POINTER(Region))]
dll=C.CDLL(str(Path(__file__).resolve().parents[1]/'out/tpf2_bigmap.dll'))
pubtype=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(Regions))
fn=dll.BigmapTestTerrainPublish;fn.argtypes=[C.c_void_p,C.POINTER(Regions),pubtype]
obj=C.create_string_buffer(128);t=Terrain.from_buffer(obj,32);t.x=114;t.y=570;t.base=6;t.high=7;t.dx=t.dy=4;t.dz=.05
# Nonrepeating coordinate-dependent world surface; neighboring blocks must agree.
def surface(x,y):return ((x*x+3*y*y+7*x*y+17*x+29*y)%50000).astype(np.uint16)
received=[];errors=[]
@pubtype
def capture(self,items):
 try:
  n=((C.cast(items.contents.last,C.c_void_p).value or 0)-(C.cast(items.contents.first,C.c_void_p).value or 0))//C.sizeof(Region)
  for i in range(n):
   r=items.contents.first[i];a=np.ctypeslib.as_array(r.data,shape=(r.width*r.height,)).copy().reshape(r.height,r.width)
   yy,xx=np.mgrid[r.y:r.y+r.height,r.x:r.x+r.width]
   assert np.array_equal(a,surface(2*xx,2*yy))
   received.append((r.x,r.y,a))
 except Exception as e:errors.append(repr(e))
for x,y,w,h in [(-256,-256,257,257),(0,-256,257,257),(-255,-253,255,253),(1,1,1,1),(-1,-1,1,1),(0,0,1,1),(1,-3,258,260)]:
 yy,xx=np.mgrid[y:y+h,x:x+w];a=surface(xx,yy);old=a.copy();arr=(Region*1)(Region(a.ctypes.data_as(C.POINTER(C.c_uint16)),w,h,x,y));b=C.addressof(arr)
 rs=Regions(C.cast(b,C.POINTER(Region)),C.cast(b+C.sizeof(arr),C.POINTER(Region)),C.cast(b+C.sizeof(arr),C.POINTER(Region)))
 fn(obj,C.byref(rs),capture);assert not errors,errors;assert np.array_equal(a,old)
assert np.array_equal(received[0][2][:,-1],received[1][2][:,0])
# ABI and authoritative-component immutability for the alignment refine bridge.
args=[C.c_void_p,C.POINTER(Terrain),C.c_uint64]+[C.c_int]*4+[C.POINTER(C.c_uint16)]+[C.c_int]*3
rt=C.CFUNCTYPE(None,*args);rf=dll.BigmapTestTerrainRefine;rf.argtypes=args+[rt]
seen=[]
@rt
def refined(terrain,comp,tile,x0,y0,x1,y1,out,stride,dx,dy):seen.append((comp.contents.high,tile,x0,y0,x1,y1,stride,dx,dy))
old=bytes(t);rf(None,C.byref(t),0x1234567800000001,3,5,255,256,None,257,1,2,refined)
assert bytes(t)==old and seen==[(8,0x1234567800000001,3,5,255,256,257,1,2)]
print('PASS: signed region coordinates, partial/empty blocks, independent world-surface samples, neighbor seams, source immutability and refinement ABI')
class Vec(C.Structure):
 _fields_=[('first',C.POINTER(C.c_uint16)),('last',C.POINTER(C.c_uint16)),('end',C.POINTER(C.c_uint16))]
cf=dll.BigmapTestTerrainComparison;cf.argtypes=[C.POINTER(Vec)];cf.restype=C.POINTER(Vec)
yy,xx=np.mgrid[:129,:129];a=surface(xx,yy);begin=a.ctypes.data;end=begin+a.nbytes
v=Vec(*[C.cast(p,C.POINTER(C.c_uint16)) for p in (begin,end,end)])
r=cf(C.byref(v)).contents;result=np.ctypeslib.as_array(r.first,shape=(257*257,)).reshape(257,257)
yy,xx=np.mgrid[:257,:257];sx=xx//2;sy=yy//2;fx=xx%2;fy=yy%2;s=a.astype(np.float64)
want=np.floor(((2-fy)*((2-fx)*s[sy,sx]+fx*s[sy,np.minimum(sx+1,128)])+fy*((2-fx)*s[np.minimum(sy+1,128),sx]+fx*s[np.minimum(sy+1,128),np.minimum(sx+1,128)]))/4+.5).astype(np.uint16)
assert np.array_equal(result,want) and np.array_equal(result[::2,::2],a)
print('PASS: construction comparison temporary 257x257 view, complete independent interpolation oracle')
