#define _GNU_SOURCE
/* SPDX-License-Identifier: GPL-3.0-or-later
 * BBHost 1.09 EU GOTY: Japanese game region + EU online selectors.
 * One shared source for Linux x86-64 and Windows x64. Experimental.
 * BBHost plugin ABI: https://github.com/droogie/bbhost/blob/master/include/bbhost_plugin.h
 */
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
/* Avoid any CRT dependency in the Windows build. */
#define WINAPI __attribute__((ms_abi))
__declspec(dllimport) int WINAPI SetEnvironmentVariableA(const char *,const char *);
__declspec(dllimport) void * WINAPI GetModuleHandleA(const char *);
__declspec(dllimport) int WINAPI VirtualProtect(void *, size_t, uint32_t, uint32_t *);
#define SETENV(k,v) SetEnvironmentVariableA((k),(v))
#define UNSETENV(k) SetEnvironmentVariableA((k), (const char*)0)
#else
#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#define EXPORT __attribute__((visibility("default")))
#define SETENV(k,v) (setenv((k),(v),1)==0)
#define UNSETENV(k) unsetenv((k))
#endif

/* Exact initial ABI layout. Current BBHost API v12 extends this table. */
typedef struct {
  uint32_t version, size;
  void (*log)(const char *, ...);
  const char *(*config)(const char *);
  const char *(*data_dir)(void);
  const char *(*mods_dir)(void);
  int (*register_hle)(const char *, void *);
  const char *(*eboot_sha256)(void);
  int (*eboot_is_109)(void);
  uint64_t (*guest_addr)(uint64_t);
  int (*read)(uint64_t, void *, size_t);
  int (*write)(uint64_t, const void *, size_t);
  int (*patch)(uint64_t, const void *, const void *, size_t);
  void *hook;
  void *other_v1_to_v8[28];
  int (*game_file)(const char *, const void **, size_t *);
  int (*overlay_file)(const char *, const char *, const void *, size_t);
} BbHostApi;
typedef struct {uint32_t api_version,flags; const char *name,*title,*version,*author,*description;} BbPluginInfo;
typedef struct {uint32_t type; const char *key,*label,*help,*def; double min,max; const char *choices;} BbOption;
_Static_assert(offsetof(BbHostApi,patch)==0x58,"patch API offset");
_Static_assert(offsetof(BbHostApi,game_file)==0x148,"game_file API offset");
_Static_assert(offsetof(BbHostApi,overlay_file)==0x150,"overlay API offset");
#define BB_OPT_CHOICE 4u
#define BB_PLUGIN_OPT_IN 1u
EXPORT const BbPluginInfo bb_plugin_info={
  12,BB_PLUGIN_OPT_IN,"region_switch","Japanese + EU Online","2.0.0-exp",
  "Community experimental port", "Japanese game region, EU online selectors; needs HTTP compatibility in BBHost."
};
EXPORT const BbOption bb_plugin_options[]={
  {BB_OPT_CHOICE,"region","Game region","JP keeps Japanese text and EU online. EU disables this plugin's changes.","JP",0,0,"JP|EU"},
  {0}
};
static const BbHostApi *g_api;
static int g_jp;
static unsigned char g_sfo[65536];
static const unsigned char g_fix[6]={0xb8,0x02,0x00,0x00,0x00,0x90};
static const uint64_t g_sites[12]={
  0x023e450dull,0x023b86b3ull,0x01d5e537ull,0x0227d9e1ull,
  0x022899e6ull,0x0228b1e1ull,0x022917c1ull,0x02292342ull,
  0x02295fe0ull,0x02297161ull,0x022a3e9eull,0x022a4783ull
};
static void cp(void *dst,const void *src,size_t n){unsigned char *d=(unsigned char*)dst;const unsigned char *s=(const unsigned char*)src;for(size_t i=0;i<n;i++)d[i]=s[i];}
static int same(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return 0;return 1;}
static int eq(const char *a,const char *b){if(!a||!b)return 0;while(*a&&*b){if(*a++!=*b++)return 0;}return *a==*b;}
static size_t len(const char *s){size_t n=0;while(s[n])n++;return n;}
static uint16_t rd16(const unsigned char *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t rd32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void wr32(unsigned char *p,uint32_t x){for(int i=0;i<4;i++)p[i]=(unsigned char)(x>>(8*i));}
static int set_jp_sfo(unsigned char *b,size_t n,uint32_t *original){
  static const char key[]="USER_DEFINED_PARAM_1";
  if(n<20 || rd32(b)!=0x46535000u)return 0;
  uint32_t keys=rd32(b+8),data=rd32(b+12),count=rd32(b+16);
  if(keys<20 || keys>=n || data>=n || count>(n-20)/16)return 0;
  for(uint32_t i=0;i<count;i++){
    size_t e=20+(size_t)i*16;
    size_t k=(size_t)keys+rd16(b+e),v=(size_t)data+rd32(b+e+12);
    if(k>=n || n-k<sizeof(key) || v>n || n-v<4)continue;
    if(rd16(b+e+2)!=0x0404 || rd32(b+e+4)<4 || rd32(b+e+8)<4)continue;
    if(!same(b+k,key,sizeof(key)))continue;
    *original=rd32(b+v);
    if(*original!=13)return -1; /* Strictly the user's EU GOTY SFO */
    wr32(b+v,0);
    return 1;
  }
  return 0;
}

/* Key-table record scanner, strictly matching unique initial values. */
struct Range {uintptr_t lo,hi;};
static struct Range ranges[80];
static size_t n_ranges;
static int contains(uintptr_t p,size_t n){if(!p || p+n<p)return 0;for(size_t i=0;i<n_ranges;i++)if(p>=ranges[i].lo && p+n<=ranges[i].hi)return 1;return 0;}
static int named(uintptr_t p,const char *name){size_t n=len(name)+1;return contains(p,n) && same((const void*)p,name,n);}
static int patch_buttons(void);

#if defined(_WIN32)
static int gather_ranges(void){
    unsigned char *base=(unsigned char*)GetModuleHandleA((const char*)0);
    if(!base || rd16(base)!=0x5a4d)return 0;
    uint32_t nt_off=rd32(base+0x3c);
    if(nt_off>=4096 || rd32(base+nt_off)!=0x4550)return 0;
    unsigned char *nt=base+nt_off;
    unsigned count=rd16(nt+6),opt=rd16(nt+20);
    if(count>80 || opt<112)return 0;
    unsigned char *secs=nt+24+opt;
    uint32_t size_image=rd32(nt+0x50);
    if(size_image<4096 || size_image>(256u<<20))return 0;
    for(unsigned i=0;i<count;i++){
      unsigned char *s=secs+i*40;
      uint32_t va=rd32(s+12),vs=rd32(s+8),flags=rd32(s+36);
      if(!(flags&0x40000000u) || vs<32 || va>=size_image || vs>size_image-va)continue;
      ranges[n_ranges++]=(struct Range){(uintptr_t)(base+va),(uintptr_t)(base+va+vs)};
    }
    return n_ranges!=0;
}
static int update_keys(uintptr_t roll,uintptr_t interact){
    uint32_t old1=0,old2=0,unused=0;
    if(!VirtualProtect((void*)roll,4,0x40u,&old1))return 0;
    if(!VirtualProtect((void*)interact,4,0x40u,&old2)){
        VirtualProtect((void*)roll,4,old1,&unused);return 0;
    }
    uint32_t c=0x4000u,o=0x2000u;
    cp((void*)roll,&c,4);cp((void*)interact,&o,4);
    VirtualProtect((void*)interact,4,old2,&unused);
    VirtualProtect((void*)roll,4,old1,&unused);
    return 1;
}
#else
static int phdr_main(struct dl_phdr_info *inf,size_t size,void *ud){
 (void)size;(void)ud;if(inf->dlpi_name&&inf->dlpi_name[0])return 0;
 for(size_t i=0;i<inf->dlpi_phnum && n_ranges<80;i++){
   const ElfW(Phdr)*p=&inf->dlpi_phdr[i];
   if(p->p_type!=PT_LOAD || !(p->p_flags&PF_R) || p->p_memsz<32)continue;
   uintptr_t lo=(uintptr_t)inf->dlpi_addr+p->p_vaddr,hi=lo+p->p_memsz;
   if(hi>lo) ranges[n_ranges++]=(struct Range){lo,hi};
 }
 return 1;
}
static int gather_ranges(void){dl_iterate_phdr(phdr_main,0);return n_ranges!=0;}
static int prot(uintptr_t p){
 FILE *f=fopen("/proc/self/maps","r"); if(!f)return -1;
 char line[512],mode[8];unsigned long long a,b;int found=-1;
 while(fgets(line,sizeof(line),f)) if(sscanf(line,"%llx-%llx %7s",&a,&b,mode)==3 && p>=a && p<b){
   found=(mode[0]=='r'?PROT_READ:0)|(mode[1]=='w'?PROT_WRITE:0)|(mode[2]=='x'?PROT_EXEC:0);break;
 }
 fclose(f);return found;
}
static int update_keys(uintptr_t roll,uintptr_t interact){
 long ps=sysconf(_SC_PAGESIZE);if(ps<=0 || (ps&(ps-1)))return 0;
 uintptr_t a=roll&~((uintptr_t)ps-1),b=interact&~((uintptr_t)ps-1);
 int pa=prot(roll),pb=prot(interact);
 if(pa<0||pb<0 || !(pa&PROT_READ)||!(pb&PROT_READ))return 0;
 if(mprotect((void*)a,(size_t)ps,pa|PROT_WRITE))return 0;
 if(a!=b && mprotect((void*)b,(size_t)ps,pb|PROT_WRITE)){
     mprotect((void*)a,(size_t)ps,pa);return 0;
 }
 uint32_t c=0x4000u,o=0x2000u;cp((void*)roll,&c,4);cp((void*)interact,&o,4);
 if(a!=b)mprotect((void*)b,(size_t)ps,pb);
 mprotect((void*)a,(size_t)ps,pa);return 1;
}
#endif
static int patch_buttons(void){
 n_ranges=0;
 if(!gather_ranges())return 0;
 uintptr_t roll=0,inter=0;int rc=0,ic=0;
 for(size_t j=0;j<n_ranges;j++){
   struct Range s=ranges[j];if(s.hi<=s.lo+32)continue;
   uintptr_t p=(s.lo+7)&~(uintptr_t)7;
   for(;p<=s.hi-32;p+=8){
      uintptr_t id=0,label=0;uint32_t mapping=0;
      cp(&id,(void*)p,8);cp(&label,(void*)(p+8),8);cp(&mapping,(void*)(p+0x18),4);
      if(mapping==0x2000u && named(id,"roll") && named(label,"Dodge / Sprint")){roll=p+0x18;rc++;}
      if(mapping==0x4000u && named(id,"interact") && named(label,"Interact")){inter=p+0x18;ic++;}
   }
 }
 if(rc!=1||ic!=1)return 0;
 return update_keys(roll,inter);
}

EXPORT int bb_plugin_init(const BbHostApi *api){
 if(!api || api->version<9 || api->size<0x158 || !api->log || !api->config || !api->patch){
    if(api && api->log) api->log("region_switch: requires BBHost plugin API >=9");
    return 1;
 }
 g_api=api;
 const char *region=api->config("region_switch.region");
 g_jp=!eq(region,"EU")&&!eq(region,"US");
 if(!g_jp){UNSETENV("BBHOST_JP_EU_ACTIVE");api->log("region_switch: EU mode; no changes");return 0;}
 if(!SETENV("BBHOST_SYSTEM_LANG","JP") || !SETENV("BBHOST_JP_EU_ACTIVE","1")){
     api->log("region_switch: failed to set game language/HTTP compatibility marker");
     return 1;
 }
 api->log("region_switch: JP selected; EU online compatibility enabled automatically");
 return 0;
}
EXPORT int bb_plugin_image(const BbHostApi *api){
 if(api)g_api=api;
 if(!g_api || !g_jp)return 0;
 if(!g_api->eboot_is_109 || !g_api->read || !g_api->game_file || !g_api->overlay_file){
   g_api->log("region_switch: missing BBHost API functions");UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
 }
 if(!g_api->eboot_is_109()){
   g_api->log("region_switch: unsupported eboot; needs Bloodborne 1.09");UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
 }
 const void *file=0;size_t size=0;uint32_t original=0;
 if(g_api->game_file("/sce_sys/param.sfo",&file,&size) || !file || size>sizeof(g_sfo) || size<20){
   g_api->log("region_switch: cannot read original param.sfo");UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
 }
 cp(g_sfo,file,size);
 int sr=set_jp_sfo(g_sfo,size,&original);
 if(sr!=1){
   g_api->log("region_switch: EU GOTY USER_DEFINED_PARAM_1=13 required (found %u, status=%d)",original,sr);
   UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
 }
 /* Runtime patches, identical addresses on Windows and Linux. */
 int written=0,already=0;
 for(size_t i=0;i<12;i++){
   unsigned char old[6];
   if(g_api->read(g_sites[i],old,6)){
     g_api->log("region_switch: site %u unreadable; refusing to continue",(unsigned)i+1);
     UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
   }
   if(same(old,g_fix,6)){already++;continue;}
   int status=g_api->patch(g_sites[i],old,g_fix,6);
   if(status){
     g_api->log("region_switch: site %u patch failed (%d)",(unsigned)i+1,status);
     UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
   }
   written++;
 }
 g_api->log("region_switch: EU online selectors %d written, %d already present (12 total)",written,already);
 if(g_api->overlay_file("region_switch","/sce_sys/param.sfo",g_sfo,size)){
   g_api->log("region_switch: failed to install JP param.sfo overlay");UNSETENV("BBHOST_JP_EU_ACTIVE");return 1;
 }
 g_api->log("region_switch: USER_DEFINED_PARAM_1 13 -> 0, JP overlay ready");
 if(patch_buttons())g_api->log("region_switch: Japanese Cross/Circle keyboard mapping installed");
 else g_api->log("region_switch: keyboard binding layout not matched; mappings unchanged");
 g_api->log("region_switch: EU-online bootstrap requires compatible BBHost HTTP implementation");
 return 0;
}
