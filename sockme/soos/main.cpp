#define _WIN32_WINNT 0x0501
#include <platform.hpp>

extern "C"
{
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>
#if __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#ifndef WIN32
#include <arpa/inet.h>
#include <netdb.h>
typedef int SOCKET;
typedef struct pollfd WSAPOLLFD;
#endif
#include <poll.h>

#include "inet_pton.h"
}

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
//#include <mstcpip.h>
typedef int socklen_t;
#define errno WSAGetLastError()
#endif

#include <exception>

using ::abs;
using namespace std;



#define bufsize 0x8000

const int port = 6459;

//#define errfail(func) {printf( #func " fail: (%i) %s\n", errno, strerror(errno)); return errno;}
#ifdef WIN32
#define errfail(func)\
{\
    wchar_t *s = NULL;\
    FormatMessageW\
    (\
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,\
        NULL, errno, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&s, 0, NULL\
    );\
    printf(#func " fail (line #%03i): (%i) %S\n", __LINE__, errno, s);\
    LocalFree(s);\
    goto killswitch;\
}
#else
#define errfail(func) { printf(#func " fail (line #%03i): (%i) %s\n", __LINE__, errno, strerror(errno)); goto killswitch; }
#endif

int pollsock(SOCKET sock, int wat, int timeout = 0)
{
#ifdef WIN32
    fd_set fd;
    fd.fd_count = 1;
    fd.fd_array[0] = sock;
    TIMEVAL t;
    t.tv_sec = timeout / 1000;
    t.tv_usec = (timeout % 1000) * 1e6;
    int ret = select(1, (wat & POLLIN) ? &fd : nullptr , nullptr, (wat & POLLERR) ? &fd : nullptr, &t);
    if(ret == SOCKET_ERROR) return (wat & POLLERR) == POLLERR;
    return ret ? wat : 0;
#else
    WSAPOLLFD pd;
    pd.fd = sock;
    pd.events = wat;
    
    if(poll(&pd, 1, timeout) == 1)
        return pd.revents & wat;
#endif
    return 0;
}

int wribuf(int sock, u8* buf, int flags = 0)
{
    int mustwri = (*(u32*)(buf + 1) & 0xFFFFFF) + 4;
    int offs = 0;
    int ret = 0;
    while(mustwri)
    {
        ret = send(sock, (char*)(buf + offs), mustwri, flags);
        if(ret < 0) return -errno;
        mustwri -= ret;
        offs += ret;
    }
    
    return offs;
}


int sock = 0;
FILE* f = 0;
struct sockaddr_in sao;
socklen_t sizeof_sao = sizeof(sao);
int ret = 0;
int fsize = 0;
int offs = 0;
int mustread = 0;

u8 buf[bufsize + 4];

uint64_t dummy[2] = {0, 0};

int main(int argc, char** argv)
{
    if(argc <3)
    {
        printf("Usage: %s <file> <IP address>\n", argv[0]);
        return 1;
    }
    
    sao.sin_family = AF_INET;
    sao.sin_port = htons(port);
    
    if(!inet_pton4(argv[2], (unsigned char*)&sao.sin_addr))
    {
        printf("Malformatted IP address: '%s'\n", argv[1]);
        return 1;
    }
    
#ifdef WIN32
    
    WSADATA socHandle;
    
    ret = WSAStartup(MAKEWORD(2,2), &socHandle);
    if(ret)
    {
        printf("WSAStartup failed: %i\n", ret);
        return 1;
    }
    
#endif
    
    f = fopen(argv[1], "rb");
    if(!f) errfail(fopen);
    
    
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(sock < 0) errfail(socket);
    
    ret = connect(sock, (sockaddr*)&sao, sizeof_sao);
    if(ret < 0) errfail(connect);
    
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fsize += 0xFFF;
    fsize &= ~0xFFF;
    fseek(f, 0, SEEK_SET);
    offs = 0;
    
    
    reinstall:
    
    puts("sending install header");
    
    do
    {
        dummy[0] = 0x0000000100000C02ULL;
        
        u32 ech[6] = {0x240, 0x140, 0x80, 0x240, 0x140, 0x80};
        u32 tmp = 0;
        u32 wat = 0;
        
        fseek(f, 0, SEEK_SET);
        fread(&tmp, 1, 4, f);
        wat += ((tmp + 0x3F) & ~0x3F);
        
        fseek(f, 8, SEEK_SET);
        fread(&tmp, 1, 4, f);
        wat += ((tmp + 0x3F) & ~0x3F);
        
        fseek(f, 12, SEEK_SET);
        fread(&tmp, 1, 4, f);
        wat += ((tmp + 0x3F) & ~0x3F);
        
        fseek(f, wat, SEEK_SET);
        
        fread(&tmp, 1, 4, f);
        wat += ech[tmp >> 24] + 0x4C;
        
        fseek(f, wat, SEEK_SET);
        fread(dummy + 1, 1, 8, f);
        dummy[1] = __builtin_bswap64(dummy[1]);
        
        fseek(f, 0, SEEK_SET);
        
        printf("TitleID: %016llX\n", dummy[1]);
        printf("TitleSZ: 0x%X\n", fsize);
        
        wribuf(sock, (u8*)dummy);
    }
    while(0);
    
    waitsock:
    
    puts("waiting for response packet");
    while(pollsock(sock, POLLIN) != POLLIN);
    
    while(fsize)
    {
        int ret = 0;
        
        if(pollsock(sock, POLLIN) == POLLIN)
        while(1)
        {
            u32 hdr;
            
            ret = recv(sock, (char*)&hdr, 4, 0);
            if(ret <= 0) errfail(recv);
            
            printf("Packet ID #%i: ", (hdr & 0xFF));
            
            *(u32*)(buf) = hdr;
            
            if(hdr >> 8)
            {
                ret = recv(sock, (char*)(buf + 4), hdr >> 8, 0);
                if(ret <= 0) errfail(recv);
                
                hdr >>= 8;
                u8* ptr = (u8*)(buf + 4);
                while(hdr--) putchar(*ptr++);
            }
            putchar('\n');
            
            hdr = *(u32*)(buf);
            
            if((hdr & 0xFF) == 1)
            {
                switch(buf[4])
                {
                    case 2:
                        do
                        {
                            puts("cancelling installation");
                            uint64_t dummy = 0x0000000100000103ULL;
                            wribuf(sock, (u8*)&dummy);
                            goto waitsock;
                        }
                        while(0);
                        goto reinstall;
                    
                    default:
                        errfail(_slave_);
                }
            }
            if((hdr & 0xFF) == 3)
            {
                puts("installation cancelled, retrying");
                goto reinstall;
            }
            
            break;
        }
        
        mustread = fsize < bufsize ? fsize : bufsize;
        if(mustread < bufsize)
        {
            memset(buf + 4, 0, bufsize);
            mustread += 0xFFF;
            mustread &= ~0xFFF;
        }
        
        buf[0] = 0x04;
        buf[1] = mustread & 0xFF;
        buf[2] = (mustread >> 8) & 0xFF;
        buf[4] = (mustread >> 16) & 0xFF;
        
        fread(buf + 4, 1, mustread, f);
        
        ret = wribuf(sock, buf);
        if(ret < 0)
        {
            printf("send error %i %s\n", errno, strerror(errno));
            errfail(wribuf);
        }
        
        fsize -= mustread;
        
        printf("Remain: 0x%08X\n", fsize);
    }
    putchar('\n');
    
    do
    {
        uint64_t dummy = 0x0000000000000103ULL;
        wribuf(sock, (u8*)&dummy);
    }
    while(0);
    
    do
    {
        u32 hdr;
        
        ret = recv(sock, (char*)&hdr, 4, 0);
        if(ret <= 0) errfail(recv);
        
        printf("Packet ID #%i: ", (hdr & 0xFF));
        
        *(u32*)(buf) = hdr;
        
        if(hdr >> 8)
        {
            ret = recv(sock, (char*)(buf + 4), hdr >> 8, 0);
            if(ret <= 0) errfail(recv);
            
            hdr >>= 8;
            u8* ptr = buf + 4;
            while(hdr--) putchar(*ptr++);
        }
        putchar('\n');
        
        hdr = *(u32*)(buf);
        
        if((hdr & 0xFF) == 1)
        {
            close(sock);
            errfail(_slave_);
        }
    }
    while(0);
    
    do
    {
        dummy[0] = 0x0000000100000C05ULL;
        wribuf(sock, (u8*)dummy);
    }
    while(0);
    
    do
    {
        u32 hdr;
        
        ret = recv(sock, (char*)&hdr, 4, 0);
        if(ret <= 0) errfail(recv);
        
        printf("Packet ID #%i: ", (hdr & 0xFF));
        
        *(u32*)(buf) = hdr;
        
        if(hdr >> 8)
        {
            ret = recv(sock, (char*)(buf + 4), hdr >> 8, 0);
            if(ret <= 0) errfail(recv);
            
            hdr >>= 8;
            u8* ptr = buf + 4;
            while(hdr--) putchar(*ptr++);
        }
        putchar('\n');
        
        hdr = *(u32*)(buf);
        
        if((hdr & 0xFF) == 1)
        {
            close(sock);
            errfail(_slave_);
        }
    }
    while(0);
    
    killswitch:
    
    if(f) fclose(f);
    
    close(sock);
    
#ifdef WIN32
    WSACleanup();
#endif
    
    return 0;
}
