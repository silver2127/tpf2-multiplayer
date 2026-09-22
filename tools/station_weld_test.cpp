#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <cstdarg>
#include <vector>
static bool Readable(void* p,size_t n){return p!=nullptr || n==0;}
static void Log(const char*,...){}
#include "../native/src/station_weld.h"
static void i32(uint8_t* p,int v){memcpy(p,&v,4);}
static void ptr(uint8_t* p,void* v){memcpy(p,&v,8);}
static int get(uint8_t* p){int v;memcpy(&v,p,4);return v;}
static void node(uint8_t* p,int id,float x){float pos[3]={x,0,0};memcpy(p,pos,12);i32(p+20,id);}
static void edge(uint8_t* p,int id,int a,int b,bool owned,float tangent){
 i32(p,id);i32(p+4,1);i32(p+8,a);i32(p+12,b);i32(p+0x74,owned?1:0);
 memcpy(p+0x10,&tangent,4);memcpy(p+0x1c,&tangent,4);i32(p+0x70,123);
}
int main(){
 // extra: more station pieces chained off node -5. 0 is the original shape;
 // 100 takes the proposal past the 64-node / 64-segment cap the weld used to
 // refuse (it answered "not ours" and the raw apron was built beside ours).
 for(int extra=0;extra<=100;extra+=100) for(int reverse=0;reverse<2;reverse++) for(int invalid=0;invalid<2;invalid++) {
  const int N=5+extra, M=4+extra;
  uint8_t p[0x220]={},ce[0x8e0]={};
  std::vector<uint8_t> nodes(N*24,0),edges(M*120,0),tags(M*32,0);
  int frozen[]={2,3},cfrozen[]={2,3};
  // Script connector at index 0; three station pieces. Outer template node
  // deliberately lives in the middle, not at the end of the node array.
  node(&nodes[0],-1,10);node(&nodes[24],-2,12);node(&nodes[48],-3,10);
  node(&nodes[72],-4,0);node(&nodes[96],-5,-10);
  edge(&edges[0],-6,reverse?999:-1,reverse?-1:999,false,reverse?-2.f:2.f);
  edge(&edges[120],-7,-4,-5,true,-10);
  edge(&edges[240],-8,-4,-3,true,10);
  edge(&edges[360],-9,-3,-2,true,2);
  for(int k=0;k<extra;k++) {   // -5 -> -6 -> -7 ..., every one station-owned
   node(&nodes[(5+k)*24],-6-k,-20.f-10.f*k);
   edge(&edges[(4+k)*120],-10-k,-5-k,-6-k,true,-10);
  }
  ptr(p,&nodes[0]);ptr(p+8,&nodes[0]+nodes.size());ptr(p+0x18,&edges[0]);ptr(p+0x20,&edges[0]+edges.size());
  ptr(p+0x1f8,ce);ptr(p+0x200,ce+sizeof(ce));i32(ce+0x780,1);
  ptr(p+0x170,frozen);ptr(p+0x178,frozen+2);ptr(ce+0x768,cfrozen);ptr(ce+0x770,cfrozen+2);
  ptr(p+0x1c8,&tags[0]);ptr(p+0x1d0,&tags[0]+tags.size());
  if(invalid)cfrozen[0]=1; // deleting a frozen outer node must refuse unchanged
  std::vector<uint8_t> oldNodes(nodes),oldEdges(edges);
  bool ok=MergeStationEndpoint((uint64_t)p);
  if(invalid){assert(!ok);assert(oldNodes==nodes);assert(oldEdges==edges);continue;}
  assert(ok);assert(get(ce+0x780)==0);assert(frozen[0]==0&&frozen[1]==1&&cfrozen[0]==0&&cfrozen[1]==1);
  assert(get(&nodes[20])==-3&&get(&nodes[44])==-4&&get(&nodes[68])==-5);
  assert(get(&edges[240+8])==(reverse?999:-3));assert(get(&edges[240+12])==(reverse?-3:999));
  assert(get(&edges[240+0x74])==1&&get(&edges[240+0x70])==123);
  assert(get(&edges[0])==-7&&get(&edges[120])==-8&&get(&edges[240])==-9);
  // the vectors shrank by exactly the two dropped nodes and the one dropped segment
  uint64_t ne,se;memcpy(&ne,p+8,8);memcpy(&se,p+0x20,8);
  assert(ne==(uint64_t)(&nodes[0]+(N-2)*24)&&se==(uint64_t)(&edges[0]+(M-1)*120));
  // every extra piece survived the compaction, ids and endpoints intact
  for(int k=0;k<extra;k++) {
   assert(get(&nodes[(3+k)*24+20])==-6-k);
   assert(get(&edges[(3+k)*120])==-10-k&&get(&edges[(3+k)*120+8])==-5-k&&get(&edges[(3+k)*120+12])==-6-k);
  }
 }
 puts("PASS station endpoint weld: both directions, middle node removal, frozen remap, ownership, refusal leaves input unchanged, 105-node/104-segment station (past the old 64 cap)");
}
