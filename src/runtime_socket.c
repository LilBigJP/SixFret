/* BSD sockets (libScePosix) on the host's network: Guitar Hero Live's Demonware layer opens a
 * UDP socket (bound to port 31000) while it starts and does not go on without one, and Guitar
 * Hero TV talks to its servers through these calls.
 *
 * The guest uses FreeBSD's ABI: sockaddr_in starts with a length byte, fd_set is 1024 bits in
 * 64-bit words, option names and errno values are FreeBSD's. Guest descriptors are 512 and up, so
 * they never meet a file descriptor and still fit an fd_set. Errors reach the game as -1 with
 * its errno set (runtime_errno). SF_SOCKET_TRACE=1 logs every call. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HostSocket;
typedef int HostLength;
#define HOST_INVALID INVALID_SOCKET
#define host_close closesocket
static int host_error(void) { return WSAGetLastError(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int HostSocket;
typedef socklen_t HostLength;
#define HOST_INVALID (-1)
#define host_close close
static int host_error(void) { return errno; }
#endif

enum { FIRST_FD=512, MAX_SOCKETS=128, GUEST_FD_SETSIZE=1024 };
enum { G_EBADF=9, G_EFAULT=14, G_EINVAL=22, G_EMFILE=24, G_EAGAIN=35, G_EINPROGRESS=36, G_ENOTSOCK=38,
       G_EMSGSIZE=40, G_ENOPROTOOPT=42, G_EPROTONOSUPPORT=43, G_EAFNOSUPPORT=47, G_EADDRINUSE=48,
       G_EADDRNOTAVAIL=49, G_ENETDOWN=50, G_ENETUNREACH=51, G_ECONNABORTED=53, G_ECONNRESET=54, G_ENOBUFS=55,
       G_EISCONN=56, G_ENOTCONN=57, G_ETIMEDOUT=60, G_ECONNREFUSED=61, G_EHOSTUNREACH=65 };
enum { G_AF_INET=2, G_SOL_SOCKET=0xffff, G_MSG_PEEK=0x2, G_MSG_DONTWAIT=0x80 };
/* FreeBSD socket options the game may set; the PS4 adds SO_NBIO (non-blocking) and others. */
enum { G_SO_REUSEADDR=0x4, G_SO_KEEPALIVE=0x8, G_SO_BROADCAST=0x20, G_SO_LINGER=0x80, G_SO_REUSEPORT=0x200,
       G_SO_SNDBUF=0x1001, G_SO_RCVBUF=0x1002, G_SO_SNDTIMEO=0x1005, G_SO_RCVTIMEO=0x1006, G_SO_NBIO=0x1200 };

typedef struct { uint8_t length, family; uint16_t port; uint32_t address; uint8_t zero[8]; } GuestSockaddrIn;
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { uint64_t bits[GUEST_FD_SETSIZE/64]; } GuestFdSet;
_Static_assert(sizeof(GuestSockaddrIn)==16 && sizeof(GuestFdSet)==128,"FreeBSD socket layouts");

static struct { int used, nonblocking; HostSocket host; } sockets[MAX_SOCKETS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static size_t opened, datagrams_sent, datagrams_received;

static int trace(void) {
    static int on=-1;
    if (on<0) { const char *t=getenv("SF_SOCKET_TRACE"); on=t && *t && *t!='0'; }
    return on;
}
static int32_t fail(int guest_errno) { *runtime_errno()=guest_errno; return -1; }
static int guest_errno_of(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return G_EAGAIN;       case WSAEINPROGRESS: return G_EINPROGRESS;
    case WSAEBADF: case WSAENOTSOCK: return G_EBADF;
    case WSAEFAULT: return G_EFAULT;            case WSAEINVAL: return G_EINVAL;
    case WSAEMFILE: return G_EMFILE;            case WSAEMSGSIZE: return G_EMSGSIZE;
    case WSAENOPROTOOPT: return G_ENOPROTOOPT;  case WSAEPROTONOSUPPORT: return G_EPROTONOSUPPORT;
    case WSAEAFNOSUPPORT: return G_EAFNOSUPPORT; case WSAEADDRINUSE: return G_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return G_EADDRNOTAVAIL; case WSAENETDOWN: return G_ENETDOWN;
    case WSAENETUNREACH: return G_ENETUNREACH;  case WSAECONNABORTED: return G_ECONNABORTED;
    case WSAECONNRESET: return G_ECONNRESET;    case WSAENOBUFS: return G_ENOBUFS;
    case WSAEISCONN: return G_EISCONN;          case WSAENOTCONN: return G_ENOTCONN;
    case WSAETIMEDOUT: return G_ETIMEDOUT;      case WSAECONNREFUSED: return G_ECONNREFUSED;
    case WSAEHOSTUNREACH: return G_EHOSTUNREACH;
    default: return G_ENETDOWN;
    }
#else
    return runtime_guest_errno(e);
#endif
}
static int32_t fail_host(void) { return fail(guest_errno_of(host_error())); }

static int ready(void) {
#ifdef _WIN32
    static int state; /* 0 not tried, 1 ready, -1 failed */
    pthread_mutex_lock(&lock);
    if (!state) { WSADATA data; state=WSAStartup(MAKEWORD(2,2),&data) ? -1 : 1; }
    int ok=state>0;
    pthread_mutex_unlock(&lock);
    return ok;
#else
    return 1;
#endif
}
static int host_of(int32_t fd, HostSocket *host, int *nonblocking) {
    int ok=0;
    pthread_mutex_lock(&lock);
    if (fd>=FIRST_FD && fd<FIRST_FD+MAX_SOCKETS && sockets[fd-FIRST_FD].used) {
        *host=sockets[fd-FIRST_FD].host;
        if (nonblocking) *nonblocking=sockets[fd-FIRST_FD].nonblocking;
        ok=1;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
static void set_nonblocking(HostSocket host, int on) {
#ifdef _WIN32
    u_long mode=on ? 1 : 0; ioctlsocket(host,FIONBIO,&mode);
#else
    int flags=fcntl(host,F_GETFL,0);
    if (flags>=0) fcntl(host,F_SETFL,on ? flags|O_NONBLOCK : flags&~O_NONBLOCK);
#endif
}
static int to_host(const GuestSockaddrIn *guest, uint32_t length, struct sockaddr_in *host) {
    if (!guest || length<sizeof(*guest) || guest->family!=G_AF_INET) return 0;
    memset(host,0,sizeof(*host));
    host->sin_family=AF_INET; host->sin_port=guest->port; host->sin_addr.s_addr=guest->address; /* network order in both */
    return 1;
}
static void to_guest(const struct sockaddr_in *host, GuestSockaddrIn *guest, uint32_t *length) {
    if (!guest || !length) return;
    GuestSockaddrIn value={.length=sizeof(value),.family=G_AF_INET,.port=host->sin_port,.address=host->sin_addr.s_addr};
    memcpy(guest,&value,*length<sizeof(value) ? *length : sizeof(value));
    *length=sizeof(value);
}

static ABI int32_t guest_socket(int32_t domain, int32_t type, int32_t protocol) {
    if (domain!=G_AF_INET) return fail(G_EAFNOSUPPORT);
    /* 1 stream, 2 datagram as everywhere; the PS4's 6 (DGRAM_P2P) and 10 (STREAM_P2P) are its
     * console-to-console variants of the same two. */
    int host_type=type==1 || type==10 ? SOCK_STREAM : type==2 || type==6 ? SOCK_DGRAM : 0;
    if (!host_type) return fail(G_EPROTONOSUPPORT);
    if (!ready()) return fail(G_ENETDOWN);
    HostSocket host=socket(AF_INET,host_type,host_type==SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    (void)protocol;
    if (host==HOST_INVALID) return fail_host();
    int32_t fd=-1;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_SOCKETS;++i) if (!sockets[i].used) {
        sockets[i].used=1; sockets[i].nonblocking=0; sockets[i].host=host; fd=FIRST_FD+i; ++opened; break;
    }
    pthread_mutex_unlock(&lock);
    if (fd<0) { host_close(host); return fail(G_EMFILE); }
    printf("Runtime: %s socket %d opened\n",host_type==SOCK_STREAM ? "TCP" : "UDP",fd);
    return fd;
}
static ABI int32_t guest_bind(int32_t fd, const GuestSockaddrIn *address, uint32_t length) {
    HostSocket host; struct sockaddr_in in;
    if (!host_of(fd,&host,NULL)) return fail(G_EBADF);
    if (!to_host(address,length,&in)) return fail(G_EINVAL);
    if (bind(host,(struct sockaddr *)&in,sizeof(in))) {
        int e=guest_errno_of(host_error());
        printf("Runtime: socket %d could not bind port %u (errno %d)\n",fd,ntohs(in.sin_port),e);
        return fail(e);
    }
    printf("Runtime: socket %d bound to port %u\n",fd,ntohs(in.sin_port));
    return 0;
}
static ABI int32_t guest_setsockopt(int32_t fd, int32_t level, int32_t name, const void *value, uint32_t length) {
    HostSocket host;
    if (!host_of(fd,&host,NULL)) return fail(G_EBADF);
    if (!value || length<sizeof(int32_t)) return fail(G_EINVAL);
    int32_t number; memcpy(&number,value,sizeof(number));
    if (trace()) printf("Socket trace: setsockopt(%d, level %#x, option %#x, %d)\n",fd,level,name,number);
    if (level!=G_SOL_SOCKET) return fail(G_EINVAL); /* protocol-level options: none the hosts share */
    int host_name;
    switch (name) {
    case G_SO_NBIO:
        set_nonblocking(host,number!=0);
        pthread_mutex_lock(&lock); sockets[fd-FIRST_FD].nonblocking=number!=0; pthread_mutex_unlock(&lock);
        return 0;
    case G_SO_REUSEADDR: host_name=SO_REUSEADDR; break;
    case G_SO_KEEPALIVE: host_name=SO_KEEPALIVE; break;
    case G_SO_BROADCAST: host_name=SO_BROADCAST; break;
    case G_SO_SNDBUF: host_name=SO_SNDBUF; break;
    case G_SO_RCVBUF: host_name=SO_RCVBUF; break;
    default: return fail(G_EINVAL); /* as the console answers options it does not have */
    }
    if (setsockopt(host,SOL_SOCKET,host_name,(const char *)&number,sizeof(number))) return fail_host();
    return 0;
}
static ABI int64_t guest_sendto(int32_t fd, const void *data, uint64_t size, int32_t flags,
                                const GuestSockaddrIn *to, uint32_t to_length) {
    HostSocket host; struct sockaddr_in in;
    (void)flags;
    if (!host_of(fd,&host,NULL)) return fail(G_EBADF);
    if (!data && size) return fail(G_EFAULT);
    if (size>INT32_MAX) return fail(G_EMSGSIZE);
    int has_address=to && to_host(to,to_length,&in);
    if (to && !has_address) return fail(G_EINVAL);
    int n=has_address ? sendto(host,data,(int)size,0,(struct sockaddr *)&in,sizeof(in)) : send(host,data,(int)size,0);
    if (n<0) return fail_host();
    __atomic_add_fetch(&datagrams_sent,1,__ATOMIC_RELAXED);
    if (trace() && has_address) {
        const uint8_t *a=(const uint8_t *)&in.sin_addr.s_addr;
        printf("Socket trace: sendto(%d, %d bytes) to %u.%u.%u.%u:%u\n",fd,n,a[0],a[1],a[2],a[3],ntohs(in.sin_port));
    }
    return n;
}
static int readable_now(HostSocket host) {
    fd_set set; FD_ZERO(&set); FD_SET(host,&set);
    struct timeval zero={0,0};
    return select((int)host+1,&set,NULL,NULL,&zero)>0;
}
static ABI int64_t guest_recvfrom(int32_t fd, void *data, uint64_t size, int32_t flags,
                                  GuestSockaddrIn *from, uint32_t *from_length) {
    HostSocket host; int nonblocking;
    if (!host_of(fd,&host,&nonblocking)) return fail(G_EBADF);
    if (!data && size) return fail(G_EFAULT);
    if (size>INT32_MAX) size=INT32_MAX;
    if ((flags&G_MSG_DONTWAIT) && !nonblocking && !readable_now(host)) return fail(G_EAGAIN);
    struct sockaddr_in in; HostLength length=sizeof(in);
    memset(&in,0,sizeof(in));
    int n=recvfrom(host,data,(int)size,flags&G_MSG_PEEK ? MSG_PEEK : 0,(struct sockaddr *)&in,&length);
    if (n<0) {
#ifdef _WIN32
        /* Windows reports a datagram longer than the buffer as an error after filling the buffer;
         * a UDP "port unreachable" for an earlier send arrives as a reset: not an error to a
         * connectionless reader. */
        if (host_error()==WSAEMSGSIZE) n=(int)size;
        else if (host_error()==WSAECONNRESET) return fail(G_EAGAIN);
        else
#endif
        return fail_host();
    }
    to_guest(&in,from,from_length);
    __atomic_add_fetch(&datagrams_received,1,__ATOMIC_RELAXED);
    if (trace()) {
        const uint8_t *a=(const uint8_t *)&in.sin_addr.s_addr;
        printf("Socket trace: recvfrom(%d) %d bytes from %u.%u.%u.%u:%u\n",fd,n,a[0],a[1],a[2],a[3],ntohs(in.sin_port));
    }
    return n;
}
/* The guest's sets name its descriptors; the host's sets hold the host sockets behind them. */
static int32_t collect(const GuestFdSet *guest, int32_t count, fd_set *host, HostSocket *highest) {
    FD_ZERO(host);
    if (!guest) return 0;
    for (int32_t fd=0;fd<count;++fd) {
        if (!(guest->bits[fd/64]>>(fd%64)&1)) continue;
        HostSocket socket_;
        if (!host_of(fd,&socket_,NULL)) return -1;
        FD_SET(socket_,host);
        if (socket_>*highest) *highest=socket_;
    }
    return 0;
}
static int give_back(GuestFdSet *guest, int32_t count, const fd_set *host) {
    int set=0;
    if (!guest) return 0;
    for (int32_t fd=0;fd<count;++fd) {
        if (!(guest->bits[fd/64]>>(fd%64)&1)) continue;
        HostSocket socket_;
        if (host_of(fd,&socket_,NULL) && FD_ISSET(socket_,host)) ++set;
        else guest->bits[fd/64]&=~(UINT64_C(1)<<(fd%64));
    }
    return set;
}
static ABI int32_t guest_select(int32_t count, GuestFdSet *reads, GuestFdSet *writes, GuestFdSet *errors, const GuestTimeval *timeout) {
    if (count<0 || count>GUEST_FD_SETSIZE) return fail(G_EINVAL);
    fd_set r,w,x; HostSocket highest=0;
    if (collect(reads,count,&r,&highest) || collect(writes,count,&w,&highest) || collect(errors,count,&x,&highest))
        return fail(G_EBADF);
    struct timeval wait, *waiting=NULL;
    if (timeout) {
        if (timeout->seconds<0 || timeout->microseconds<0 || timeout->microseconds>=1000000) return fail(G_EINVAL);
        wait.tv_sec=(long)(timeout->seconds>86400 ? 86400 : timeout->seconds); wait.tv_usec=(long)timeout->microseconds;
        waiting=&wait;
    }
    if (!reads && !writes && !errors) { /* a sleep: Windows' select refuses empty sets */
        if (waiting) { struct timespec t={wait.tv_sec,wait.tv_usec*1000L}; nanosleep(&t,NULL); }
        return 0;
    }
    int n=select((int)highest+1,reads ? &r : NULL,writes ? &w : NULL,errors ? &x : NULL,waiting);
    if (n<0) return fail_host();
    return give_back(reads,count,&r)+give_back(writes,count,&w)+give_back(errors,count,&x);
}

static const RuntimeExport exports[]={
    {"socket",guest_socket}, {"bind",guest_bind}, {"setsockopt",guest_setsockopt},
    {"sendto",guest_sendto}, {"recvfrom",guest_recvfrom}, {"select",guest_select},
};
uintptr_t runtime_socket_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_socket_report(void) {
    printf("Runtime: sockets opened=%zu, datagrams sent=%zu, received=%zu\n",opened,datagrams_sent,datagrams_received);
}
