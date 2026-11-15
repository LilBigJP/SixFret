/* System functions Guitar Hero Live imports that the other runtime files do not answer.
 *
 * Offline contracts: PSN (title user storage, score ranking, word filter, in-game messages,
 * Facebook, WebApi push events) answers "not signed in"; the console's own dialogs
 * (invitations, game custom data) never open; Remote Play, the PlayStation Store icon and
 * the system music player are accepted and have no effect. Every function is written out:
 * nothing here returns success for a call whose result the game would then read.
 *
 * Demonware (Guitar Hero TV) reaches the network through BSD sockets (runtime_socket.c) and
 * libSceHttp, whose requests cannot be sent yet. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define COMMON_DIALOG_NOT_AVAILABLE ((int32_t)0x80B80002)
#define TROPHY_ICON_NOT_FOUND ((int32_t)0x80551624)
#define RTC_INVALID_POINTER ((int32_t)0x80B50002)
#define RTC_INVALID_VALUE ((int32_t)0x80B50003)
#define GUEST_ENETDOWN 50
#define GUEST_EAFNOSUPPORT 47
#define GUEST_ENOSPC 28
#define UNIX_EPOCH_TICK UINT64_C(62135596800000000) /* microseconds from 0001-01-01 to 1970-01-01 */

static _Atomic int32_t next_id=0x4700;
static int32_t new_id(void) { return atomic_fetch_add(&next_id,1); }
static void note_once(_Atomic int *said,const char *text) { if (!atomic_exchange(said,1)) puts(text); }

static ABI int32_t ok(void) { return 0; }
static ABI int32_t object(void) { return new_id(); }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }
/* An asynchronous PSN request is finished at once, with the signed-out result. */
static ABI int32_t poll_signed_out(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }

/* ---- Console dialogs that have no counterpart: they initialize, never open, and report
 * status NONE (0), so the game's dialog pump sees nothing running. ---- */
static ABI int32_t dialog_open(void) { return COMMON_DIALOG_NOT_AVAILABLE; }
static ABI int32_t dialog_status(void) { return 0; }
static ABI int32_t ime_panel_size(const void *param,const void *extended,uint32_t *width,uint32_t *height) {
    (void)param; (void)extended;
    if (!width || !height) return (int32_t)0x80BC0031; /* IME_DIALOG INVALID_ADDRESS */
    *width=793; *height=408; /* the PS4's default panel */
    return 0;
}

/* ---- Remote Play, system music player, PlayStation Store icon ---- */
static ABI int32_t remoteplay_initialize(void *heap,uint64_t size) {
    (void)heap; (void)size; puts("Runtime: Remote Play initialized (not available on PC)"); return 0;
}

/* ---- Network identity ---- */
static ABI int32_t net_mac_address(uint8_t *address,int32_t flags) {
    (void)flags;
    if (!address) return (int32_t)0x80410116; /* NET EINVAL */
    static const uint8_t mac[6]={0x02,0x47,0x48,0x4c,0x50,0x43}; /* locally administered */
    memcpy(address,mac,6); return 0;
}
static ABI int32_t open_ps_id(uint8_t *id) {
    if (!id) return ERR(14);
    static const uint8_t value[16]={'G','H','L','i','v','e','P','C',0,0,0,0,0,0,0,1};
    memcpy(id,value,16); return 0;
}

/* ---- Address text (libScePosix); the sockets themselves are in runtime_socket.c. ---- */
static int32_t fail_errno(int32_t e) { *runtime_errno()=e; return -1; }
/* FreeBSD: AF_INET 2 (the game's Demonware build is IPv4 only). */
static ABI int32_t posix_inet_pton(int32_t family,const char *text,uint8_t *out) {
    if (family!=2) return fail_errno(GUEST_EAFNOSUPPORT);
    if (!text || !out) return 0;
    unsigned parts[4]; char end;
    if (sscanf(text,"%3u.%3u.%3u.%3u%c",&parts[0],&parts[1],&parts[2],&parts[3],&end)!=4) return 0;
    for (int i=0;i<4;++i) if (parts[i]>255) return 0;
    for (int i=0;i<4;++i) out[i]=(uint8_t)parts[i];
    return 1;
}
static ABI const char *posix_inet_ntop(int32_t family,const uint8_t *address,char *out,uint32_t size) {
    if (family!=2) { *runtime_errno()=GUEST_EAFNOSUPPORT; return NULL; }
    char text[16];
    if (!address || !out) { *runtime_errno()=14; return NULL; }
    int n=snprintf(text,sizeof text,"%u.%u.%u.%u",address[0],address[1],address[2],address[3]);
    if ((uint32_t)n>=size) { *runtime_errno()=GUEST_ENOSPC; return NULL; }
    memcpy(out,text,(size_t)n+1); return out;
}

/* ---- libSceHttp pieces the other titles never used. Requests still cannot be sent
 * (sceHttpSendRequest in runtime_services.c). ---- */
static ABI int32_t http_unavailable(void) { return HTTP_NETWORK; }

/* ---- AppContent: temporary and download data live in the user folder (/temp0, /download0). ---- */
static ABI int32_t temporary_mount(uint32_t option,char *mount_point) {
    (void)option;
    if (!mount_point) return (int32_t)0x80D90002; /* APP_CONTENT PARAMETER */
    memcpy(mount_point,"/temp0",7);
    puts("Runtime: temporary data mounted at /temp0");
    return 0;
}
static ABI int32_t download_space(const char *mount_point,uint64_t *kilobytes) {
    (void)mount_point;
    if (!kilobytes) return (int32_t)0x80D90002;
    *kilobytes=UINT64_C(8)<<20; /* 8 GiB: more than the title's DOWNLOAD_DATA_SIZE asks for */
    return 0;
}

/* ---- PlayGo: everything is installed, so nothing is left to fetch. ---- */
static ABI int32_t playgo_todo(int32_t handle,void *list,uint32_t entries,uint32_t *out) {
    (void)handle; (void)list; (void)entries;
    if (!out) return (int32_t)0x80B20003; /* PLAYGO BAD_POINTER */
    *out=0; return 0;
}

/* ---- Microphone (party vocals): a port that opens and stays silent. The game's microphone
 * thread (0xcb5100 in 01.08) opens it again at once, without a pause, for as long as opening
 * fails; with an open port whose silent state has a "no device / muted" bit (mask 0x0d) it
 * sleeps 10 ms between looks and never reads samples. ---- */
#define AUDIO_IN_HANDLE 0x4d49
#define AUDIO_IN_SILENT_NO_DEVICE 0x1
static ABI int32_t audio_in_hq_open(int32_t user,int32_t type,int32_t index,uint32_t length,uint32_t frequency,uint32_t format) {
    static _Atomic int said;
    (void)user; (void)type; (void)index; (void)length; (void)frequency; (void)format;
    note_once(&said,"Runtime: microphone port opened (no microphone: it stays silent)");
    return AUDIO_IN_HANDLE;
}
static ABI int32_t audio_in_silent_state(int32_t handle) {
    return handle==AUDIO_IN_HANDLE ? AUDIO_IN_SILENT_NO_DEVICE : AUDIO_IN_NOT_OPENED;
}

/* ---- Trophies: icons are not kept; groups: the game has the base set only. ---- */
static ABI int32_t trophy_icon(void) { return TROPHY_ICON_NOT_FOUND; }
static ABI int32_t trophy_group(void) { return (int32_t)0x8055160C; /* TROPHY GROUP_NOT_FOUND */ }

/* ---- libSceRtc: dates sent by the game's servers ("2015-10-20T12:34:56Z"). A tick is
 * microseconds since 0001-01-01 UTC. ---- */
static int64_t days_from_civil(int64_t y,unsigned m,unsigned d) {
    y-=m<=2;
    const int64_t era=(y>=0 ? y : y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400), doy=(153*(m+(m>2 ? -3 : 9))+2)/5+d-1, doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+(int64_t)doe-719468;
}
static ABI int32_t rtc_parse(uint64_t *tick,const char *text) {
    if (!tick || !text) return RTC_INVALID_POINTER;
    unsigned year,month,day,hour=0,minute=0,second=0; int used=0;
    if (sscanf(text,"%4u-%2u-%2u%n",&year,&month,&day,&used)!=3) return RTC_INVALID_VALUE;
    const char *p=text+used;
    uint64_t micro=0; int64_t offset=0;
    if (*p=='T' || *p=='t' || *p==' ') {
        if (sscanf(++p,"%2u:%2u:%2u%n",&hour,&minute,&second,&used)!=3) return RTC_INVALID_VALUE;
        p+=used;
        if (*p=='.') { uint64_t scale=100000; for (++p;*p>='0' && *p<='9';++p) { micro+=(uint64_t)(*p-'0')*scale; scale/=10; } }
        if (*p=='+' || *p=='-') {
            unsigned oh=0,om=0;
            if (sscanf(p+1,"%2u:%2u",&oh,&om)<1) return RTC_INVALID_VALUE;
            offset=(int64_t)(oh*3600+om*60)*(*p=='+' ? 1 : -1);
        }
    }
    if (!year || month<1 || month>12 || day<1 || day>31 || hour>23 || minute>59 || second>60) return RTC_INVALID_VALUE;
    int64_t seconds=days_from_civil(year,month,day)*86400+(int64_t)hour*3600+minute*60+second-offset;
    *tick=UNIX_EPOCH_TICK+(uint64_t)seconds*1000000+micro;
    return 0;
}

/* ---- libkernel ---- */
static int64_t utc_offset(time_t utc,int *dst) {
    struct tm local,universal;
#ifdef _WIN32
    localtime_s(&local,&utc); gmtime_s(&universal,&utc);
#else
    localtime_r(&utc,&local); gmtime_r(&utc,&universal);
#endif
    if (dst) *dst=local.tm_isdst>0 ? 3600 : 0;
    int64_t days=days_from_civil(local.tm_year+1900,(unsigned)local.tm_mon+1,(unsigned)local.tm_mday)-
                 days_from_civil(universal.tm_year+1900,(unsigned)universal.tm_mon+1,(unsigned)universal.tm_mday);
    return days*86400+(local.tm_hour-universal.tm_hour)*3600+(local.tm_min-universal.tm_min)*60+(local.tm_sec-universal.tm_sec);
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
static ABI int32_t utc_to_local(int64_t utc,int64_t *local,Timesec *st,uint64_t *dst_sec) {
    int dst; int64_t offset=utc_offset((time_t)utc,&dst);
    if (local) *local=utc+offset;
    if (st) { st->t=utc; st->west_sec=(uint32_t)(int32_t)(offset-dst); st->dst_sec=(uint32_t)dst; }
    if (dst_sec) *dst_sec=(uint64_t)dst;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local,int64_t unused,int64_t *utc,int32_t *timezone,int32_t *dst_sec) {
    (void)unused;
    int dst; int64_t offset=utc_offset((time_t)local,&dst); /* the offset at that instant, near enough off a DST change */
    if (utc) *utc=local-offset;
    if (timezone) { timezone[0]=(int32_t)(-(offset-dst)/60); timezone[1]=dst!=0; }
    if (dst_sec) *dst_sec=dst;
    return 0;
}
/* sku.prx is linked into the game image by scripts/link_modules.py and the PS4's own modules are
 * this runtime: loading one by path has nothing left to do. */
static ABI int32_t load_start_module(const char *path,uint64_t args,const void *argp,uint32_t flags,const void *option,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)option;
    printf("Runtime: module %s requested (already part of the image or the runtime)\n",path ? path : "(null)");
    if (result) *result=0;
    return new_id();
}
static ABI int32_t kernel_getdents(int32_t fd,char *buffer,int32_t size) {
    int64_t r=runtime_file_getdents(fd,buffer,(uint64_t)(size<0 ? 0 : size),NULL);
    return r<0 ? ERR(runtime_guest_errno((int)-r)) : (int32_t)r;
}
static ABI int32_t module_info_for_unwind(uint64_t address,int32_t flags,void *info) {
    static _Atomic int said;
    (void)flags; (void)info;
    if (!atomic_exchange(&said,1))
        printf("Runtime: C++ unwind information requested for %#llx (not provided: an exception thrown by the game ends it)\n",
               (unsigned long long)address);
    return ERR(3); /* ESRCH */
}

static const RuntimeExport exports[]={
    {"sceRemoteplayInitialize",remoteplay_initialize}, {"sceRemoteplayProhibit",ok},
    {"sceSystemServiceReenableMusicPlayer",ok},
    {"sceNpCommerceShowPsStoreIcon",ok}, {"sceNpCommerceHidePsStoreIcon",ok},
    {"sceNetCtlInit",ok}, {"sceNetCtlTerm",ok}, {"sceNetResolverAbort",ok},
    {"sceNetGetMacAddress",net_mac_address}, {"sceKernelGetOpenPsId",open_ps_id},
    {"inet_pton",posix_inet_pton}, {"inet_ntop",posix_inet_ntop},
    {"sceHttpCreateConnection",object}, {"sceHttpCreateRequest2",object},
    {"sceHttpsSetSslCallback",ok}, {"sceHttpAbortRequest",ok}, {"sceHttpRemoveRequestHeader",ok},
    {"sceHttpSetChunkedTransferEnabled",ok},
    {"sceHttpUriParse",http_unavailable}, {"sceHttpUriEscape",http_unavailable},
    {"sceAppContentTemporaryDataMount2",temporary_mount}, {"sceAppContentTemporaryDataUnmount",ok},
    {"sceAppContentDownloadDataGetAvailableSpaceKb",download_space}, {"sceAppContentDownloadDataFormat",ok},
    {"scePlayGoPrefetch",ok}, {"scePlayGoGetToDoList",playgo_todo},
    {"sceAudioInHqOpen",audio_in_hq_open}, {"sceAudioInGetSilentState",audio_in_silent_state},
    {"sceImeDialogGetPanelSizeExtended",ime_panel_size},
    {"sceGameCustomDataDialogInitialize",ok}, {"sceGameCustomDataDialogOpen",dialog_open},
    {"sceGameCustomDataDialogUpdateStatus",dialog_status}, {"sceGameCustomDataDialogGetResult",dialog_open},
    {"sceGameCustomDataDialogTerminate",ok},
    {"sceInvitationDialogInitialize",ok}, {"sceInvitationDialogOpen",dialog_open},
    {"sceInvitationDialogUpdateStatus",dialog_status}, {"sceInvitationDialogGetResult",dialog_open},
    {"sceInvitationDialogTerminate",ok},
    {"sceNpSnsFacebookDialogInitialize",ok}, {"sceNpSnsFacebookDialogOpen",dialog_open},
    {"sceNpSnsFacebookDialogUpdateStatus",dialog_status}, {"sceNpSnsFacebookDialogGetResult",dialog_open},
    {"sceNpSnsFacebookDialogTerminate",ok},
    {"sceNpProfileDialogClose",ok},
    {"sceNpSnsFacebookCreateRequest",object}, {"sceNpSnsFacebookGetAccessToken",signed_out},
    {"sceNpSnsFacebookDeleteRequest",ok},
    {"sceNpAuthCreateRequest",object}, {"sceNpLookupCreateRequest",object},
    {"sceNpGetAccountId",signed_out},
    {"sceNpInGameMessageInitialize",ok}, {"sceNpInGameMessageTerminate",ok},
    {"sceNpInGameMessageCreateHandle",object}, {"sceNpInGameMessageDeleteHandle",ok},
    {"sceNpInGameMessagePrepare",signed_out}, {"sceNpInGameMessageSendData",signed_out},
    {"sceNpMatching2SetUserInfo",signed_out}, {"sceNpMatching2RegisterRoomMessageCallback",ok},
    {"sceNpMatching2SendRoomMessage",signed_out}, {"sceNpMatching2SendRoomChatMessage",signed_out},
    {"sceNpMatching2GetRoomDataInternal",signed_out}, {"sceNpMatching2GetRoomMemberDataInternal",signed_out},
    {"sceNpScoreRecordScoreAsync",signed_out}, {"sceNpScoreGetRankingByNpIdAsync",signed_out},
    {"sceNpScoreGetRankingByRangeAsync",signed_out}, {"sceNpScoreGetFriendsRankingAsync",signed_out},
    {"sceNpScoreRecordGameDataAsync",signed_out}, {"sceNpScoreGetGameDataAsync",signed_out},
    {"sceNpScorePollAsync",poll_signed_out},
    {"sceNpTusCreateNpTitleCtx",object}, {"sceNpTusDeleteNpTitleCtx",ok},
    {"sceNpTusCreateRequest",object}, {"sceNpTusDeleteRequest",ok},
    {"sceNpTssGetData",signed_out},
    {"sceNpTusSetMultiSlotVariable",signed_out}, {"sceNpTusSetMultiSlotVariableVUser",signed_out},
    {"sceNpTusGetMultiSlotVariable",signed_out}, {"sceNpTusGetMultiSlotVariableVUser",signed_out},
    {"sceNpTusAddAndGetVariable",signed_out}, {"sceNpTusAddAndGetVariableVUser",signed_out},
    {"sceNpTusSetData",signed_out}, {"sceNpTusSetDataVUser",signed_out},
    {"sceNpTusGetData",signed_out}, {"sceNpTusGetDataVUser",signed_out},
    {"sceNpTusDeleteMultiSlotData",signed_out}, {"sceNpTusDeleteMultiSlotDataVUser",signed_out},
    {"sceNpBandwidthTestInitStart",signed_out}, {"sceNpBandwidthTestGetStatus",signed_out},
    {"sceNpBandwidthTestShutdown",signed_out},
    {"sceNpWordFilterCreateTitleCtx",object}, {"sceNpWordFilterDeleteTitleCtx",ok},
    {"sceNpWordFilterCreateRequest",object}, {"sceNpWordFilterCreateAsyncRequest",object},
    {"sceNpWordFilterDeleteRequest",ok}, {"sceNpWordFilterAbortRequest",ok},
    {"sceNpWordFilterCensorComment",signed_out}, {"sceNpWordFilterSanitizeComment",signed_out},
    {"sceNpWordFilterPollAsync",poll_signed_out},
    {"sceNpWebApiCreateHandle",object}, {"sceNpWebApiDeleteHandle",ok},
    {"sceNpWebApiSendRequest2",signed_out},
    {"sceNpWebApiCreateServicePushEventFilter",signed_out}, {"sceNpWebApiDeleteServicePushEventFilter",ok},
    {"sceNpWebApiRegisterServicePushEventCallback",signed_out}, {"sceNpWebApiUnregisterServicePushEventCallback",ok},
    {"sceNpTrophyGetTrophyIcon",trophy_icon}, {"sceNpTrophyGetGroupIcon",trophy_icon},
    {"sceNpTrophyGetGameIcon",trophy_icon}, {"sceNpTrophyGetGroupInfo",trophy_group},
    {"sceRtcParseRFC3339",rtc_parse}, {"sceRtcParseDateTime",rtc_parse},
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelSync",ok},
    {"sceKernelGetdents",kernel_getdents},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
};
uintptr_t runtime_ghl_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
