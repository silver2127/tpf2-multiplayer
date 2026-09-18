"""Compile the real edge decoder against a paired ownership-tool record.

Requires MSVC Build Tools; artifacts stay in .local-test/road-ownership.
The memory-vector reader is stubbed, not the decoder under test.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'native/src/slice_hook.cpp').read_text(encoding='utf-8')
edge = source[source.index('struct Edge {'):source.index('\n', source.index('struct Edge {'))]
decoder = source[source.index('static int DecodeEdgesVec('):source.index('static int DecodeNodesVec(')]
fixture = bytes.fromhex(
    'ffffffff0000000077ae010078ae0100049b924293e6f24192aa203f69b09e4200000000eb1d0a3e00000000ffffffff00000000000000000000000000000000'
    '0000000000000000000000000f00000000010000000000000200000002000000ffffffff00010000e1b1000001000c00510f000001000000')
assert len(fixture) == 120
out = ROOT / '.local-test/road-ownership'
out.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <cstdint>
#include <cstring>
#include <vector>
#include <cassert>
enum VecRead { VEC_EMPTY, VEC_OK, VEC_BAD };
static uint8_t records[240];
static uint64_t bytes=240;
static VecRead status=VEC_OK;
static VecRead ReadVecAnyEx(uint64_t, uint64_t* begin, uint64_t* span, const char*) {
    *begin=(uint64_t)records; *span=bytes; return status;
}
static void Log(const char*, ...) {}
'''
harness += edge + '\n' + decoder
harness += '\nstatic const uint8_t fixture[]={' + ','.join(map(str, fixture)) + '};\n'
harness += r'''
int main() {
    memcpy(records, fixture, 120); memcpy(records+120, fixture, 120);
    records[120+0x74]=0; // absence must ignore the stale player payload
    std::vector<Edge> edges;
    assert(DecodeEdgesVec(0,&edges,"test")==2);
    assert(edges[0].owner==3921 && edges[1].owner==-1);
    assert(edges[0].node0==110199 && edges[0].node1==110200);
    records[0x74]=0;
    assert(DecodeEdgesVec(0,&edges,"test")==2 && edges[0].owner==-1);
    records[0x74]=2; assert(DecodeEdgesVec(0,&edges,"test")==-1);
    records[0x74]=1; memset(records+0x70,255,4);
    assert(DecodeEdgesVec(0,&edges,"test")==-1);
    bytes=119; assert(DecodeEdgesVec(0,&edges,"test")==-1);
    status=VEC_EMPTY; assert(DecodeEdgesVec(0,&edges,"test")==0 && edges.empty());
}
'''
(out / 'decode_test.cpp').write_text(harness, encoding='utf-8')
(out / 'decode_test.cmd').write_text(
    '@echo off\ncall "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    'cl /nologo /EHsc /W4 decode_test.cpp /Fe:decode_test.exe || exit /b 1\n'
    'decode_test.exe\n', encoding='utf-8')
subprocess.run(['cmd', '/c', 'decode_test.cmd'], cwd=out, check=True)
print('PASS: captured owned/public edge records, absent optional payload, reuse, malformed/empty vectors')
