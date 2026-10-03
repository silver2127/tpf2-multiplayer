#include "density.h"
#include <cstdio>
int main(int argc,char** argv){
    if(argc!=2){fprintf(stderr,"Usage: bigmap-density-restore PATH_TO_BASE_MOD_LUA\n");return 2;}
    std::string why;if(!density::Sync(argv[1],false,why)){fprintf(stderr,"Density restore failed: %s\n",why.c_str());return 1;}return 0;
}
