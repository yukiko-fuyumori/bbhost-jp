#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
typedef struct {
 uint32_t version,size; void(*log)(const char*,...);const char*(*config)(const char*);
 const char*(*data_dir)(void);const char*(*mods_dir)(void);
 int(*register_hle)(const char*,void*); const char*(*eboot_sha256)(void);
 int(*eboot_is_109)(void); uint64_t(*guest_addr)(uint64_t);
 int(*read)(uint64_t,void*,size_t); int(*write)(uint64_t,const void*,size_t);
 int(*patch)(uint64_t,const void*,const void*,size_t);void* hook;void* reserve[28];
 int(*game_file)(const char*,const void**,size_t*);
 int(*overlay_file)(const char*,const char*,const void*,size_t);
} API;
static unsigned char sfo[256];
static int patches,overlays,allowed=1;
static char mode[8]="JP";
static void logger(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);puts("");va_end(a);}
static const char* config(const char *key){assert(!strcmp(key,"region_switch.region"));return mode;}
static const char*sha(void){return allowed?"941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7":"unexpected";}
static int is109(void){return 1;}
static int r(uint64_t a,void *d,size_t n){(void)a;memset(d,0xcc,n);return 0;}
static int p(uint64_t a,const void *exp,const void *data,size_t n){(void)a;assert(n==6&&((const unsigned char*)exp)[0]==0xcc&&((const unsigned char*)data)[0]==0xb8);patches++;return 0;}
static int game_file(const char *path,const void **ptr,size_t *n){assert(!strcmp(path,"/sce_sys/param.sfo"));*ptr=sfo;*n=256;return 0;}
static int overlay(const char *name,const char *path,const void *data,size_t n){(void)name;(void)path;(void)data;(void)n;overlays++;return 0;}
static void w16(size_t o,uint16_t v){sfo[o]=(uint8_t)v;sfo[o+1]=v>>8;}
static void w32(size_t o,uint32_t v){for(unsigned i=0;i<4;i++)sfo[o+i]=(uint8_t)(v>>(8*i));}
static void make_sfo(void){
 memset(sfo,0,sizeof(sfo));
 w32(0,0x46535000u);w32(8,52);w32(12,128);w32(16,2);
 w16(20,0);w16(22,0x0404);w32(24,4);w32(28,4);w32(32,0);
 w16(36,21);w16(38,0x0204);w32(40,10);w32(44,16);w32(48,16);
 memcpy(sfo+52,"USER_DEFINED_PARAM_1",21);
 memcpy(sfo+73,"TITLE_ID",9);
 w32(128,0);memcpy(sfo+144,"CUSA03014",10);
}
int main(int argc,char**argv){
 assert(argc==2);make_sfo();
 if(!strcmp(argv[1],"wrong-title"))memcpy(sfo+144,"CUSA03173",10);
 if(!strcmp(argv[1],"wrong-region"))w32(128,13);
 if(!strcmp(argv[1],"wrong-eboot"))allowed=0;
 if(!strcmp(argv[1],"off"))strcpy(mode,"OFF");
 void *lib=dlopen("./region_switch.so",RTLD_NOW);assert(lib);
 int(*init)(const API*)=dlsym(lib,"bb_plugin_init");
 int(*img)(const API*)=dlsym(lib,"bb_plugin_image");
 assert(init&&img);
 API a={0};a.version=12;a.size=sizeof(a);a.log=logger;a.config=config;
 a.eboot_sha256=sha;a.eboot_is_109=is109;a.read=r;a.patch=p;a.game_file=game_file;a.overlay_file=overlay;
 assert(init(&a)==0);
 int status=img(&a);
 if(!strcmp(argv[1],"success")) {
   assert(!status && patches==12 && overlays==0 && getenv("BBHOST_JP_EU_ACTIVE") && !strcmp(getenv("BBHOST_JP_EU_ACTIVE"),"1"));
 } else if(!strcmp(argv[1],"off")){
   assert(!status && patches==0 && overlays==0 && !getenv("BBHOST_JP_EU_ACTIVE"));
 } else {
   assert(status!=0 && patches==0 && overlays==0 && !getenv("BBHOST_JP_EU_ACTIVE"));
 }
 printf("PASS %s status=%d patched=%d overlay=%d\n",argv[1],status,patches,overlays);
}
