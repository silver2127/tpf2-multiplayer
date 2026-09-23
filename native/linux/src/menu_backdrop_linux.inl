// Read the game's own preblurred artwork, never redistribute it or read back a
// live frame. ui.zip uses stored entries. Unsupported archives fail closed.
static unsigned TitleU16(const unsigned char* p) { return p[0] | (unsigned(p[1])<<8); }
static uint32_t TitleU32(const unsigned char* p) { return TitleU16(p) | (uint32_t(TitleU16(p+2))<<16); }
static bool ReadTitleArt(const char* archive,std::vector<unsigned char>& pixels,int& w,int& h)
{
    FILE* f=fopen(archive,"rb"); if(!f) return false;
    bool ok=false; unsigned char head[30];
    while(fread(head,1,30,f)==30 && TitleU32(head)==0x04034b50) {
        const unsigned flags=TitleU16(head+6), method=TitleU16(head+8);
        const uint32_t size=TitleU32(head+18);
        const unsigned names=TitleU16(head+26), extra=TitleU16(head+28);
        if((flags&9) || names>4096) break; // encrypted/data-descriptor entries
        std::string name(names,'\0');
        if((names && fread(&name[0],1,names,f)!=names) || fseeko(f,extra,SEEK_CUR)) break;
        if(name!="menu-bg_transport-fever_blurred.tga") {
            if(fseeko(f,size,SEEK_CUR)) break;
            continue;
        }
        if(method!=0 || size<18 || size>64*1024*1024) break;
        std::vector<unsigned char> tga(size);
        if(fread(tga.data(),1,size,f)!=size) break;
        const unsigned char* t=tga.data();
        w=TitleU16(t+12); h=TitleU16(t+14); const int bpp=t[16]/8;
        const size_t start=18u+t[0];
        if(t[1]!=0 || t[2]!=2 || (bpp!=3 && bpp!=4) || w<2 || h<2 ||
           w>8192 || h>8192 || start+(size_t)w*h*bpp>size) break;
        pixels.resize((size_t)w*h*4);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            int sy=(t[17]&32)?y:h-1-y, sx=(t[17]&16)?w-1-x:x;
            const auto* src=t+start+((size_t)sy*w+sx)*bpp;
            auto* dst=pixels.data()+((size_t)y*w+x)*4;
            memcpy(dst,src,3); dst[3]=255;
        }
        ok=true; break;
    }
    fclose(f); if(!ok) { pixels.clear(); w=h=0; } return ok;
}
static std::vector<unsigned char> g_titleArt, g_titleBackdrop;
static int g_titleArtW=0,g_titleArtH=0,g_titleBgW=0,g_titleBgH=0;
static bool g_titleArtTried=false;
static void PrepareTitleBackdrop(int w,int h)
{
    if(g_titleBgW==w && g_titleBgH==h) return;
    if(!g_titleArtTried) {
        g_titleArtTried=true;
        const std::string path=P().gameDir+"res/textures/ui/ui.zip";
        if(!ReadTitleArt(path.c_str(),g_titleArt,g_titleArtW,g_titleArtH) && g_log)
            g_log("[panel] native title artwork unavailable; using solid fallback\n");
    }
    g_titleBackdrop.resize((size_t)w*h*4); g_titleBgW=w; g_titleBgH=h;
    const double scale=g_titleArt.empty()?1.0:(std::max)(double(w)/g_titleArtW,double(h)/g_titleArtH);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        auto* d=g_titleBackdrop.data()+((size_t)y*w+x)*4;
        if(g_titleArt.empty()) { d[0]=65;d[1]=48;d[2]=30;d[3]=255;continue; }
        int sx=(int)((x-w/2.0)/scale+g_titleArtW/2.0), sy=(int)((y-h/2.0)/scale+g_titleArtH/2.0);
        sx=(std::max)(0,(std::min)(g_titleArtW-1,sx)); sy=(std::max)(0,(std::min)(g_titleArtH-1,sy));
        memcpy(d,g_titleArt.data()+((size_t)sy*g_titleArtW+sx)*4,4);
    }
}
static void PaintTitleBackdrop(void* dst,size_t pitch,int w,int h,int x,int y,int dw,int dh)
{
    PrepareTitleBackdrop(w,h);
    const int radius=(std::max)(1,S(16));
    for(int yy=0;yy<h;++yy) {
        auto* row=(unsigned char*)dst+(size_t)yy*pitch;
        memcpy(row,g_titleBackdrop.data()+(size_t)yy*w*4,(size_t)w*4);
        if(yy<y-radius || yy>=y+dh+radius) continue;
        for(int xx=(std::max)(0,x-radius);xx<(std::min)(w,x+dw+radius);++xx) {
            const int dx=(std::max)((std::max)(x-xx,xx-(x+dw-1)),0);
            const int dy=(std::max)((std::max)(y-yy,yy-(y+dh-1)),0);
            int dist=(std::max)(dx,dy); if(!dist) continue;
            const int shade=80*(radius-dist)/radius;
            for(int c=0;c<3;++c) row[xx*4+c]=(unsigned char)(row[xx*4+c]*(255-shade)/255);
        }
    }
}
