// Based on the supplied mock; exercise the real SDK and keyboard layout on both OSes.
// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include "bbhost_plugin.h"
#include "host/bindings.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>
using API = BbHostApi;
static const BindingInfo bindings[] = {
 {"roll", "Dodge / Sprint", "回避", 0x2000, "Space", 0},
 {"interact", "Interact", "調べる", 0x4000, "E", 0},
};
static bool active() {
#if defined(_WIN32)
 char value[8] = {};
 return GetEnvironmentVariableA("BBHOST_JP_EU_ACTIVE", value, sizeof(value)) == 1 && value[0] == '1';
#else
 const char* value = getenv("BBHOST_JP_EU_ACTIVE");
 return value && !strcmp(value, "1");
#endif
}
static unsigned char sfo[256];
static int patches,overlays,allowed=1;
#if BBHOST_PERSONAL_EU
static constexpr const char* title="CUSA03173";
static constexpr const char* other_title="CUSA03014";
static constexpr unsigned region=13, expected_overlays=1;
#else
static constexpr const char* title="CUSA03014";
static constexpr const char* other_title="CUSA03173";
static constexpr unsigned region=0, expected_overlays=0;
#endif
static char mode[8]="JP";
static void logger(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);puts("");va_end(a);}
static const char* config(const char *key){assert(!strcmp(key,"region_switch.region"));return mode;}
static const char*sha(void){return allowed?"941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7":"unexpected";}
static int is109(void){return 1;}
static int r(uint64_t a,void *d,size_t n){(void)a;memset(d,0xcc,n);return 0;}
static int p(uint64_t a,const void *exp,const void *data,size_t n){(void)a;assert(n==6&&((const unsigned char*)exp)[0]==0xcc&&((const unsigned char*)data)[0]==0xb8);patches++;return 0;}
static int game_file(const char *path,const void **ptr,size_t *n){assert(!strcmp(path,"/sce_sys/param.sfo"));*ptr=sfo;*n=256;return 0;}
static int overlay(const char *name,const char *path,const void *data,size_t n){assert(!strcmp(name,"region_switch") && !strcmp(path,"/sce_sys/param.sfo") && n==sizeof(sfo));
 const auto* bytes=static_cast<const unsigned char*>(data);
 assert(bytes[128]==0 && bytes[129]==0 && bytes[130]==0 && bytes[131]==0);
 assert(!memcmp(bytes+144,title,10));overlays++;return 0;}
static void w16(size_t o,uint16_t v){sfo[o]=(uint8_t)v;sfo[o+1]=v>>8;}
static void w32(size_t o,uint32_t v){for(unsigned i=0;i<4;i++)sfo[o+i]=(uint8_t)(v>>(8*i));}
static void make_sfo(void){
 memset(sfo,0,sizeof(sfo));
 w32(0,0x46535000u);w32(8,52);w32(12,128);w32(16,2);
 w16(20,0);w16(22,0x0404);w32(24,4);w32(28,4);w32(32,0);
 w16(36,21);w16(38,0x0204);w32(40,10);w32(44,16);w32(48,16);
 memcpy(sfo+52,"USER_DEFINED_PARAM_1",sizeof("USER_DEFINED_PARAM_1"));
 memcpy(sfo+73,"TITLE_ID",9);
 w32(128,region);memcpy(sfo+144,title,10);
}
int main(int argc,char**argv){
 assert(argc==2);make_sfo();
 if(!strcmp(argv[1],"wrong-title"))memcpy(sfo+144,other_title,10);
 if(!strcmp(argv[1],"wrong-region"))w32(128,region==0 ? 13 : 0);
 if(!strcmp(argv[1],"wrong-eboot"))allowed=0;
 if(!strcmp(argv[1],"off"))strcpy(mode,"OFF");
 using Entry = int (*)(const API*);
#if defined(_WIN32)
 HMODULE lib = LoadLibraryA("./region_switch.dll"); assert(lib);
 auto init = reinterpret_cast<Entry>(GetProcAddress(lib,"bb_plugin_init"));
 auto img = reinterpret_cast<Entry>(GetProcAddress(lib,"bb_plugin_image"));
 auto info = reinterpret_cast<const BbPluginInfo*>(GetProcAddress(lib,"bb_plugin_info"));
#else
 void *lib=dlopen("./region_switch.so",RTLD_NOW);assert(lib);
 auto init = reinterpret_cast<Entry>(dlsym(lib,"bb_plugin_init"));
 auto img = reinterpret_cast<Entry>(dlsym(lib,"bb_plugin_image"));
 auto info = static_cast<const BbPluginInfo*>(dlsym(lib,"bb_plugin_info"));
#endif
 assert(info && !(info->flags & BB_PLUGIN_OPT_IN));
 assert(init&&img);
 API a={0};a.version=12;a.size=sizeof(a);a.log=logger;a.config=config;
 a.eboot_sha256=sha;a.eboot_is_109=is109;a.read=r;a.patch=p;a.game_file=game_file;a.overlay_file=overlay;
 assert(init(&a)==0);
 unsigned char before[sizeof(sfo)]; memcpy(before,sfo,sizeof(sfo));
 int status=img(&a);
 assert(!memcmp(before,sfo,sizeof(sfo)));
 if(!strcmp(argv[1],"success")) {
   assert(!status && patches==12 && overlays==expected_overlays && active());
 } else if(!strcmp(argv[1],"off")){
   assert(!status && patches==0 && overlays==0 && !active());
 } else {
   assert(status!=0 && patches==0 && overlays==0 && !active());
 }
 const volatile BindingInfo* mapped = bindings;
 bool success = !strcmp(argv[1],"success");
 assert(mapped[0].button == (success ? 0x4000u : 0x2000u));
 assert(mapped[1].button == (success ? 0x2000u : 0x4000u));
 printf("PASS %s status=%d patched=%d overlay=%d\n",argv[1],status,patches,overlays);
}
