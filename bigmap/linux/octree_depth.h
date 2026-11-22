// Experimental octree depth 12/13 for the native Linux Steam build 35924.
// Linux counterpart of bigmap/src/octree_depth12.h; the same ID scheme, the
// same refusals and the same root/depth values. See docs/linux/PORT.md,
// "Octree depth 12/13", for the byte-verified sites and how they were matched.
//
// Depth counts the root. Stock child IDs are 8*parent+1+octant in int32; level
// 10 ends at 0x49249248, so level 11 overflows. Levels 0..10 keep their stock
// IDs. New level-11 nodes take 0x50000000..0x5fffffff and level-12 nodes
// 0x60000000..0x6fffffff from two monotonically increasing process counters
// (never recycled, exhaustion fails fast). The renderer's level decoder reads
// the high nibble plus six for those ranges.
//
// Unlike Windows, the Linux compiler inlined the recursive descent into one
// loop (0x16aa040), so there is no call to detour. The child-ID step of that
// loop is replaced instead: the ID of every child of a level-10-or-deeper node
// is chosen from the PARENT NODE's stored ID, never from the loop's overflowed
// running index. Shallower children keep the stock arithmetic.
// Tests read the arrays below by name (tools/linux/test_octree_depth_elf.py).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace linux_octree {
constexpr uintptr_t ChildSite=0x16aa190, ChildBack=0x16aa08c;
constexpr uintptr_t LevelSite=0x13ec530, LevelBack=0x13ec555;

// Descent loop, child step (only entered with a parent node in rax):
//   lea r13d,[r15+r13*8+1]; movsxd r15,r15d; mov rsi,rax;
//   lea rbx,[rdx+r15*8]; jmp 0x16aa08c
constexpr uint8_t ChildBytes[20]={0x47,0x8d,0x6c,0xef,0x01,0x4d,0x63,0xff,0x48,0x89,0xc6,
    0x4a,0x8d,0x1c,0xfa,0xe9,0xe8,0xfe,0xff,0xff};
// Inlined CalcOctreeLevel(int) in the skip query 0x13ec500, after its
// `test esi,esi; js assert` (the loop's je reads that test's flags):
//   mov r12,rdx; mov r14d,0; mov ecx,1; mov edx,1; je +16; nop;
//   loop: shl edx,3; add r14d,1; add ecx,edx; cmp esi,ecx; jge loop
constexpr uint8_t LevelBytes[37]={0x49,0x89,0xd4,0x41,0xbe,0x00,0x00,0x00,0x00,0xb9,0x01,
    0x00,0x00,0x00,0xba,0x01,0x00,0x00,0x00,0x74,0x10,0x0f,0x1f,0x00,0xc1,0xe2,0x03,0x41,
    0x83,0xc6,0x01,0x01,0xd1,0x39,0xce,0x7d,0xf3};

// Entered by absolute jump from ChildSite. In: rax parent node, r13d running
// index, r15d octant, rdx child array, r9d remaining depth of the child, edi
// the loop's "created" flag (preserved). Dead here: rcx, rdx after use, r10,
// r11, flags. Out: r13d child ID, rsi parent, rbx child slot, as stock.
constexpr size_t ChildStubCounters=0x4d, ChildStubBack=0x89;
constexpr uint8_t ChildStub[145]={
    0x4d,0x63,0xff,                          // 00 movsxd r15,r15d          (stock)
    0x48,0x89,0xc6,                          // 03 mov rsi,rax              (stock)
    0x4a,0x8d,0x1c,0xfa,                     // 06 lea rbx,[rdx+r15*8]      (stock)
    0x8b,0x50,0x08,                          // 0a mov edx,[rax+8]          parent ID
    0x81,0xfa,0x49,0x92,0x24,0x09,           // 0d cmp edx,0x09249249       parent below level 10?
    0x7c,0x67,                               // 13 jl stock
    0x31,0xc9,                               // 15 xor ecx,ecx              bank 0: level 11
    0x81,0xfa,0x48,0x92,0x24,0x49,           // 17 cmp edx,0x49249248
    0x7e,0x15,                               // 1d jle have
    0x81,0xfa,0x00,0x00,0x00,0x50,           // 1f cmp edx,0x50000000
    0x7c,0x60,                               // 25 jl fail
    0x81,0xfa,0xff,0xff,0xff,0x5f,           // 27 cmp edx,0x5fffffff
    0x7f,0x58,                               // 2d jg fail
    0xb9,0x01,0x00,0x00,0x00,                // 2f mov ecx,1                bank 1: level 12
    0x45,0x85,0xc9,                          // 34 have: test r9d,r9d
    0x7e,0x4e,                               // 37 jle fail
    0x48,0x8b,0x13,                          // 39 mov rdx,[rbx]            existing child?
    0x48,0x85,0xd2,                          // 3c test rdx,rdx
    0x74,0x0a,                               // 3f je fresh
    0x44,0x8b,0x6a,0x08,                     // 41 mov r13d,[rdx+8]         keep its ID
    0xff,0x25,0x3e,0x00,0x00,0x00,           // 45 jmp [back]
    0x49,0xba,0,0,0,0,0,0,0,0,               // 4b fresh: movabs r10,counters
    0x41,0xbb,0x01,0x00,0x00,0x00,           // 55 mov r11d,1
    0xf0,0x45,0x0f,0xc1,0x1c,0x8a,           // 5b lock xadd [r10+rcx*4],r11d
    0x41,0x83,0xc3,0x01,                     // 61 add r11d,1               new value
    0xc1,0xe1,0x1c,                          // 65 shl ecx,28
    0x81,0xc1,0x00,0x00,0x00,0x60,           // 68 add ecx,0x60000000       bank limit
    0x41,0x39,0xcb,                          // 6e cmp r11d,ecx
    0x73,0x14,                               // 71 jae fail                 exhausted
    0x45,0x89,0xdd,                          // 73 mov r13d,r11d
    0xff,0x25,0x0d,0x00,0x00,0x00,           // 76 jmp [back]
    0x47,0x8d,0x6c,0xef,0x01,                // 7c stock: lea r13d,[r15+r13*8+1]
    0xff,0x25,0x02,0x00,0x00,0x00,           // 81 jmp [back]
    0x0f,0x0b,                               // 87 fail: ud2 (never publish a bad ID)
    0,0,0,0,0,0,0,0};                        // 89 back

// Entered by absolute jump from LevelSite with esi >= 0 (the stock js already
// ran). Compact IDs decode as (id>>28)+6; everything else runs the stock bytes
// after re-creating the flags their je reads. ecx/edx are dead after the loop.
constexpr size_t LevelStubBack=0x49;
constexpr uint8_t LevelStub[81]={
    0x81,0xfe,0x00,0x00,0x00,0x50,           // 00 cmp esi,0x50000000
    0x72,0x14,                               // 06 jb stock
    0x49,0x89,0xd4,                          // 08 mov r12,rdx              (stock)
    0x41,0x89,0xf6,                          // 0b mov r14d,esi
    0x41,0xc1,0xee,0x1c,                     // 0e shr r14d,28
    0x41,0x83,0xc6,0x06,                     // 12 add r14d,6               5 -> 11, 6 -> 12
    0xff,0x25,0x2d,0x00,0x00,0x00,           // 16 jmp [back]
    0x85,0xf6,                               // 1c stock: test esi,esi
    0x49,0x89,0xd4,0x41,0xbe,0x00,0x00,0x00,0x00,0xb9,0x01,0x00,0x00,0x00,0xba,0x01,0x00,
    0x00,0x00,0x74,0x10,0x0f,0x1f,0x00,0xc1,0xe2,0x03,0x41,0x83,0xc6,0x01,0x01,0xd1,0x39,
    0xce,0x7d,0xf3,                          // 1e LevelBytes, unchanged (relative branches stay inside)
    0xff,0x25,0x00,0x00,0x00,0x00,           // 43 jmp [back]
    0,0,0,0,0,0,0,0};                        // 49 back
static_assert(sizeof(ChildBytes)>=14 && sizeof(LevelBytes)>=14,"absolute jump fits");

constexpr uint32_t CounterStart[2]={0x4fffffff,0x5fffffff};
inline bool ValidDepth(int depth){return depth>=11 && depth<=13;}
inline int EdgeTiles(int depth){return depth==13?2048:depth==12?1024:512;}
// Same root as Windows: +-2^(depth+5) m, so 128 m leaves at every depth.
inline float RootHalfExtent(int depth){return float(uint32_t(1)<<(depth+5));}

inline void AbsoluteJump(uint8_t* p,uintptr_t dest){
    p[0]=0xff;p[1]=0x25;std::memset(p+2,0,4);std::memcpy(p+6,&dest,8);
}
inline size_t BuildChildStub(uint8_t* dst,uintptr_t back,uint32_t* counters){
    const uint64_t at=uintptr_t(counters);
    std::memcpy(dst,ChildStub,sizeof(ChildStub));
    std::memcpy(dst+ChildStubCounters,&at,8);std::memcpy(dst+ChildStubBack,&back,8);
    return sizeof(ChildStub);
}
inline size_t BuildLevelStub(uint8_t* dst,uintptr_t back){
    std::memcpy(dst,LevelStub,sizeof(LevelStub));std::memcpy(dst+LevelStubBack,&back,8);
    return sizeof(LevelStub);
}
// Site bytes: an absolute jump to the stub, NOP padding (never executed).
inline void SitePatch(uint8_t* out,size_t len,uintptr_t stub){
    std::memset(out,0x90,len);AbsoluteJump(out,stub);
}
}
