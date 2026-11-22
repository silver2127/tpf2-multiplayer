// Compare lossless predictors/packing using real tile samples, no game writes.
// Modes 0-4: LZ4 over raw/row-delta/planar residual layouts (mode 3 is pager
// format 2). Mode 5: pager format 3 (planar prediction + context rANS).
#define LZ4_HEAPMODE 0
#include "../src/vendor/lz4/lz4.c"
#include "../src/terrain_codec.h"
#include <vector>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <memory>
using Clock=std::chrono::steady_clock;
constexpr size_t N=TerrainCodec::Samples;
static size_t Committed(size_t n){return (n+4095)&~size_t(4095);}
static void encode(const uint16_t* src,std::vector<uint8_t>& bytes,int mode) {
    std::vector<uint16_t> residual(N);
    for(size_t y=0;y<257;++y)for(size_t x=0;x<257;++x) {
        size_t i=y*257+x;
        uint16_t left=x?src[i-1]:0,up=y?src[i-257]:0,ul=x&&y?src[i-258]:0;
        uint16_t d=uint16_t(src[i]-left-(mode==2?up-ul:0));
        residual[i]=mode?uint16_t((d<<1)^uint16_t(int16_t(d)>>15)):d;
    }
    size_t stride=(N+7)/8;
    bytes.assign(mode==4?stride*16:mode==3?((N+1)/2)*4:N*2,0);
    for(size_t i=0;i<N;++i) {
        unsigned d=residual[i];
        if(mode==4)for(unsigned b=0;b<16;++b)bytes[b*stride+i/8]|=((d>>b)&1)<<(i%8);
        else if(mode==3)for(unsigned b=0;b<4;++b)bytes[b*((N+1)/2)+i/2]|=((d>>(b*4))&15)<<((i%2)*4);
        else {bytes[i]=uint8_t(d);bytes[N+i]=uint8_t(d>>8);}
    }
}
static void decode(const std::vector<uint8_t>& bytes,uint16_t* dst,int mode) {
    size_t stride=(N+7)/8;
    for(size_t y=0;y<257;++y)for(size_t x=0;x<257;++x) {
        size_t i=y*257+x;unsigned d=0;
        if(mode==4)for(unsigned b=0;b<16;++b)d|=((bytes[b*stride+i/8]>>(i%8))&1)<<b;
        else if(mode==3)for(unsigned b=0;b<4;++b)d|=((bytes[b*((N+1)/2)+i/2]>>((i%2)*4))&15)<<(b*4);
        else d=bytes[i]|(unsigned(bytes[N+i])<<8);
        if(mode)d=(d>>1)^unsigned(-int(d&1));
        dst[i]=uint16_t(d+(x?dst[i-1]:0)+(mode==2?(y?dst[i-257]:0)-(x&&y?dst[i-258]:0):0));
    }
}
int main(int argc,char** argv) {
    assert(argc==2);FILE* f=nullptr;fopen_s(&f,argv[1],"rb");assert(f);
    std::vector<std::vector<uint16_t>> tiles;std::vector<uint16_t> t(N),out(N);
    while(fread(t.data(),N*2,1,f)==1)tiles.push_back(t);assert(feof(f));fclose(f);assert(!tiles.empty());
    std::unique_ptr<TerrainCodec::EncodeScratch> es(new TerrainCodec::EncodeScratch);
    std::unique_ptr<TerrainCodec::DecodeScratch> ds(new TerrainCodec::DecodeScratch);
    for(int mode=0;mode<6;++mode) {
        size_t total=0,commit=0;double en=0,de=0;
        std::vector<uint8_t> bytes,decoded;std::vector<char> compressed(LZ4_compressBound(int(N*2+16)));
        for(auto& tile:tiles) {
            if(mode==5) {
                auto start=Clock::now();
                size_t n=TerrainCodec::Encode(tile.data(),(uint8_t*)compressed.data(),N*2,*es);assert(n>0);
                en+=std::chrono::duration<double>(Clock::now()-start).count();total+=n;commit+=Committed(n);
                start=Clock::now();
                bool ok=TerrainCodec::Decode((const uint8_t*)compressed.data(),n,out.data(),*ds);
                de+=std::chrono::duration<double>(Clock::now()-start).count();assert(ok&&out==tile);
                continue;
            }
            auto start=Clock::now();encode(tile.data(),bytes,mode);
            int n=LZ4_compress_default((const char*)bytes.data(),compressed.data(),int(bytes.size()),int(compressed.size()));assert(n>0);
            en+=std::chrono::duration<double>(Clock::now()-start).count();total+=n;commit+=Committed(n);
            decoded.resize(bytes.size());start=Clock::now();
            assert(LZ4_decompress_safe(compressed.data(),(char*)decoded.data(),n,int(decoded.size()))==decoded.size());
            decode(decoded,out.data(),mode);de+=std::chrono::duration<double>(Clock::now()-start).count();assert(out==tile);
        }
        printf("mode=%d tiles=%zu ratio=%.6f projected_gib=%.3f page_commit_gib=%.3f encode_us=%.1f decode_us=%.1f exact=1\n",
            mode,tiles.size(),double(total)/(tiles.size()*N*2),double(total)/tiles.size()*64980/(1024.*1024*1024),
            double(commit)/tiles.size()*64980/(1024.*1024*1024),en*1e6/tiles.size(),de*1e6/tiles.size());
    }
}
