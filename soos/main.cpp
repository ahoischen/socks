#include <3ds.h>

extern "C"
{
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
    
#include "consoleshit.h"
}

#include <exception>

//int __stacksize__ = 0x18000;

#include "fs/lppfs.hpp"
#include "appman.hpp"

using namespace MM::FS;
using MM::AppMan;
using MM::AppFile;

static jmp_buf __exc;
static int  __excno;
PrintConsole console;

#define max(x,y) ((x)<(y)?(y):(x))
#define min(x,y) ((x)<(y)?(x):(y))

const int port = 6459;

#define hangmacro()\
({\
    puts("Press a key to exit...");\
    while(aptMainLoop())\
    {\
        hidScanInput();\
        if(hidKeysDown())\
        {\
            goto killswitch;\
        }\
        gfxFlushBuffers();\
        gspWaitForVBlank();\
    }\
})

static int haznet = 0;

int wait4wifi(int ping_once)\
{\
    haznet = 0;\
    while(aptMainLoop())\
    {\
        u32 wifi = 0;\
        hidScanInput();\
        if(hidKeysHeld() & KEY_SELECT) return 0;\
        if(ACU_GetWifiStatus(&wifi) >= 0 && wifi) { haznet = 1; break; }\
        if(ping_once) return 0;\
        gspWaitForVBlank();\
    }\
    return haznet;\
}

extern "C" void __system_allocateHeaps(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    
    extern u32 __ctru_heap;
    extern u32 __ctru_heap_size;
    extern u32 __ctru_linear_heap;
    extern u32 __ctru_linear_heap_size;
    
    u32 tmp = 0;
    
    // Distribute available memory into halves, aligning to page size.
    //u32 size = (osGetMemRegionFree(MEMREGION_SYSTEM) / 2) & 0xFFFFF000;
    __ctru_heap_size = 0x130000;
    __ctru_linear_heap_size = 0x120000; 
    
    //*(u32*)0x00100998 = size;
    
    
    // Allocate the application heap
    __ctru_heap = 0x08000000;
    svcControlMemory(&tmp, __ctru_heap, 0x0, __ctru_heap_size, (MemOp)MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    
    // Allocate the linear heap
    //__ctru_linear_heap = 0x14000000;
    //svcControlMemory(&tmp, 0x1C000000 - __ctru_linear_heap_size, 0x0, __ctru_linear_heap_size, (MemOp)MEMOP_FREE, (MemPerm)(0));
    Result res = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, (MemOp)MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    if(res < 0) *(u32*)0x00100070 = res;
    if(__ctru_linear_heap < 0x10000000) *(u32*)0x00100071 = __ctru_linear_heap;
    
    // Set up newlib heap
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

int pollsock(int sock, int wat, int timeout = 0)
{
    struct pollfd pd;
    pd.fd = sock;
    pd.events = wat;
    
    if(poll(&pd, 1, timeout) == 1)
        return pd.revents & wat;
    return 0;
}

void _ded()
{
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSwapBuffers();
    gfxSwapBuffers();
    gfxFlushBuffers();
    
    puts("\e[0m\n\n- The application has crashed\n\n");
    
    try
    {
        throw;
    }
    catch(std::exception &e)
    {
        printf("std::exception: %s\n", e.what());
    }
    catch(Result res)
    {
        printf("Result: %08X\n", res);
        //NNERR(res);
    }
    catch(int e)
    {
        printf("(int) %i\n", e);
    }
    catch(...)
    {
        puts("<unknown exception>");
    }
    
    puts("\n");
    
    hangmacro();
    
    killswitch:
    longjmp(__exc, 1);
}

class bufsoc
{
public:
    
    typedef struct
    {
        u32 packetid : 8;
        u32 size : 24;
        u8 data[0];
    } packet;
    
    int sock;
    u8* buf;
    int bufsize;
    int recvsize;
    
    bufsoc(int sock, int bufsize = 1024 * 1024)
    {
        this->bufsize = bufsize;
        buf = new u8[bufsize];
        
        recvsize = 0;
        this->sock = sock;
    }
    
    ~bufsoc()
    {
        delete[] buf;
    }
    
    int avail()
    {
        return pollsock(sock, POLLIN) == POLLIN;
    }
    
    int readbuf(int flags = 0)
    {
        u32 hdr = 0;
        int ret = recv(sock, &hdr, 4, flags);
        if(ret < 0) return -errno;
        if(ret < 4) return -1;
        *(u32*)buf = hdr;
        
        packet* p = pack();
        
        int mustwri = p->size;
        int offs = 4;
        while(mustwri)
        {
            ret = recv(sock, buf + offs , mustwri, flags);
            if(ret <= 0) return -errno;
            mustwri -= ret;
            offs += ret;
        }
        
        recvsize = offs;
        return offs;
    }
    
    int wribuf(int flags = 0)
    {
        int mustwri = pack()->size + 4;
        int offs = 0;
        int ret = 0;
        while(mustwri)
        {
            ret = send(sock, buf + offs , mustwri, flags);
            if(ret < 0) return -errno;
            mustwri -= ret;
            offs += ret;
        }
        
        return offs;
    }
    
    packet* pack()
    {
        return (packet*)buf;
    }
    
    int errformat(char* c, ...)
    {
        char* wat = nullptr;
        int len = 0;
        
        va_list args;
        va_start(args, c);
        len = vasprintf(&wat, c, args);
        va_end(args);
        
        if(len < 0)
        {
            puts("out of memory");
            return -1;
        }
        
        packet* p = pack();
        
        printf("Packet error %i: %s\n", p->packetid, wat);
        
        p->data[0] = p->packetid;
        p->packetid = 1;
        p->size = len + 2;
        strcpy((char*)(p->data + 1), wat);
        delete wat;
        
        return wribuf();
    }
};

int main()
{
  // =====[PROGINIT]=====
  
  gfxInit(GSP_RGB565_OES, GSP_RGBA4_OES, false);
  
  // =====[VARS]=====
  
  u32 kDown;
  u32 kHeld;
  u32 kUp;
  circlePosition cpos;
  touchPosition touch;
  
  int cx, cy;
  
  Result res = 0;
  
  bool speedup = 0;
  
  int sock = 0;
  
  struct sockaddr_in sai;
  socklen_t sizeof_sai = sizeof(sai);
  
  FSSession* sdmc = nullptr;
  
  bufsoc* soc = nullptr;
  
  AppFile* install = nullptr;
  
  // =====[PREINIT]=====
  
  consoleInit(GFX_TOP, &console);
  puts("Hello!");
  
  APT_CheckNew3DS(&speedup);
  
  osSetSpeedupEnable(1);
  
  if((__excno = setjmp(__exc))) goto killswitch;
  
#ifdef _3DS
  std::set_unexpected(_ded);
  std::set_terminate(_ded);
#endif

  res = acInit();
  if(res < 0)
  {
      printf("Failed to init ac (%08X)\n");
      hangmacro();
  }
  
  res = amInit();
  if(res < 0)
  {
      printf("Failed to init the application manager (%08X)\n");
      puts("\nMake sure to patch service access somehow!\n");
      hangmacro();
  }
  
  nsInit();
  
  puts("[MAIN] Initializing SDMC");
  
  res = FSSession::OpenSession(&sdmc, ARCHIVE_SDMC, {PATH_ASCII, 1, ""});
  if(res < 0) throw res;
  
  res = socInit((u32*)memalign(0x1000, 0x100000), 0x100000);
  if(res < 0) throw res;
  
  
  netreset:
  
  consoleClear();
  
  puts("socks v1.0\n");
  
  if(haznet && errno == EINVAL)
  {
      errno = 0;
      //puts("Waiting for wifi to reset");
      while(wait4wifi(1)) gspWaitForVBlank();
  }
  
  if(wait4wifi(1))
  {
      cy = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
      if(cy <= 0)
      {
          printf("socket error: (%i) %s\n", errno, strerror(errno));
          hangmacro();
      }
      
      sock = cy;
      
      struct sockaddr_in sao;
      sao.sin_family = AF_INET;
      sao.sin_addr.s_addr = gethostid();
      sao.sin_port = htons(port);
      
      if(bind(sock, (struct sockaddr*)&sao, sizeof(sao)) < 0)
      {
          printf("bind error: (%i) %s\n", errno, strerror(errno));
          hangmacro();
      }
      
      //fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
      
      if(listen(sock, 1) < 0)
      {
          printf("listen error: (%i) %s\n", errno, strerror(errno));
          hangmacro();
      }
  }
  
  reloop:
  
  if(haznet)
  do
  {
      char buf[256];
      gethostname(buf, sizeof(buf));
      printf("\nListening on %s:%i\n", buf, port);
  }
  while(0);
  else puts("\nWaiting for wifi...");
  
  // =====[RUN]=====
  
  while (aptMainLoop())
  {
    hidScanInput();
    kDown = hidKeysDown();
    kHeld = hidKeysHeld();
    kUp = hidKeysUp();
    hidCircleRead(&cpos);
    if(kHeld & KEY_TOUCH) hidTouchRead(&touch);
    
    if(kHeld & KEY_SELECT)
    {
        break;
    }
    
    if(!soc)
    {
        if(!haznet)
        {
            if(wait4wifi(1)) goto netreset;
        }
        else if(pollsock(sock, POLLIN) == POLLIN)
        {
            int cli = accept(sock, (struct sockaddr*)&sai, &sizeof_sai);
            if(cli < 0)
            {
                printf("Failed to accept client: (%i) %s\n", errno, strerror(errno));
                if(errno == EINVAL) goto netreset;
            }
            else
            {
                soc = new bufsoc(cli, 0x10000);
            }
        }
        else if(pollsock(sock, POLLERR) == POLLERR)
        {
            printf("POLLERR (%i) %s", errno, strerror(errno));
            goto netreset;
        }
    }
        
    if(soc)
    {
        if(soc->avail())
        while(1)
        {
            hidScanInput();
            if(hidKeysHeld() & KEY_SELECT)
            {
                delete soc;
                soc = nullptr;
                break;
            }
            
            puts("reading");
            cy = soc->readbuf();
            if(cy <= 0)
            {
                printf("Failed to recvbuf: (%i) %s\n", errno, strerror(errno));
                delete soc;
                soc = nullptr;
                break;
            }
            else
            {
                bufsoc::packet* k = soc->pack();
                
                /*if(k->packetid != 4)*/ printf("#%i 0x%X | %i\n", k->packetid, k->size, cy);
                
                reread:
                switch(k->packetid)
                {
                    case 0: //CONNECT
                    case 1: //ERROR
                        puts("forced dc");
                        delete soc;
                        soc = nullptr;
                        break;
                        
                    case 2: //STARTINSTALL
                        if(install)
                        {
                            soc->errformat("Installation is already in progress!");
                            break;
                        }
                        else
                        {
                            FS_MediaType med = (FS_MediaType)k->data[0];
                            u64 tid = *(u64*)(k->data + 4);
                            
                            printf("DeletTitle: %016llX %i\n", tid, med);
                            
                            printf("amDeletTitle %08X\n", AM_DeleteTitle(med, tid));
                            printf("amDeletTickt %08X\n", AM_DeleteTicket(tid));
                            
                            AM_QueryAvailableExternalTitleDatabase(nullptr);
                            
                            res = AppMan::BeginInstall(&install, med);
                            printf("AppMan::BeginInstall %08X\n", res);
                            if(res < 0)
                            {
                                soc->errformat("Failed to start installation: %08X", res);
                                break;
                            }
                            
                            k->size = 0;
                            soc->wribuf();
                        }
                        break;
                        
                    case 3: //ENDINSTALL
                        if(!install)
                        {
                            soc->errformat("No installation was started!");
                            break;
                        }
                        else
                        {
                            if(k->data[0]) res = AppMan::FugInstall(&install);
                            else res = AppMan::FinInstall(&install);
                            
                            printf("AppMan::%sInstall %08X\n", k->data[0] ? "Fug" : "Fin", res);
                            
                            if(res < 0)
                            {
                                soc->errformat("Failed to finish installation: %08X", res);
                                break;
                            }
                            
                            k->size = 0;
                            soc->wribuf();
                        }
                        break;
                        
                    case 4: //INSTALLDATA
                        if(!install)
                        {
                            soc->errformat("No installation was started!");
                            break;
                        }
                        else
                        {
                            do
                            {
                                if(k->size & 0xFFF)
                                {
                                    soc->errformat("The received buffer size must be 0xFFF-aligned!");
                                    res = -1;
                                    break;
                                }
                                
                                u32 remain = k->size;
                                u8* ptr = k->data;
                                
                                while(remain)
                                {
                                    res = install->write(ptr, 0x1000);
                                    if(res < 0) break;
                                    
                                    remain -= 0x1000;
                                    ptr += 0x1000;
                                }
                            }
                            while(0);
                            //printf("AppFile::write %08X\n", res);
                            if(res < 0)
                            {
                                soc->errformat("Failed to write install: %08X", res);
                                AppMan::FugInstall(&install);
                                install = nullptr;
                                break;
                            }
                        }
                        continue;
                        
                    case 5: //LAUNCHAPP
                    {
                        u64 tid = *(u64*)(k->data + 4);
                        u8 med = k->data[0];
                        u8 flag = k->data[1];
                        res = APT_PrepareToDoApplicationJump(flag, tid, med);
                        if(res < 0)
                        {
                            soc->errformat("Can't prepare to launch application: %08X", res);
                            break;
                        }
                        else
                        {
                            res = APT_DoApplicationJump(nullptr, 0, nullptr);
                            if(res < 0)
                            {
                                soc->errformat("Can't launch application: %08X", res);
                            }
                            else
                            {
                                k->packetid = 0;
                                soc->errformat("Launching application, exiting");
                                delete soc;
                                soc = nullptr;
                            }
                            break;
                        }
                        break;
                    }
                    break;
                    
                    case 6: //NSS_LAUNCH
                    {
                        u64 tid = *(u64*)(k->data + 4);
                        u32 flag = *(u32*)(k->data);
                        u32 pid = 0;
                        res = NS_LaunchTitle(tid, flag, &pid);
                        if(res < 0)
                        {
                            soc->errformat("Can't launch title: %08X", res);
                            break;
                        }
                        else
                        {
                            k->size = 4;
                            *(u32*)(k->data) = pid;
                            soc->wribuf();
                        }
                        break;
                    }
                    break;
                        
                    default:
                        printf("Invalid packet ID: %i\n", k->packetid);
                        delete soc;
                        soc = nullptr;
                        break;
                }
                
                break;
            }
        }
        
        if(!soc) goto reloop;
    }
    
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  // =====[END]=====
  
  killswitch:
  
  if(sdmc) delete sdmc;
  
  if(install) AppMan::FugInstall(&install);
  
  if(sock) close(sock);
  
  puts("Shutting down sockets...");
  SOCU_ShutdownSockets();
  
  socExit();
  nsExit();
  amExit();
  acExit();
  gfxExit();
  
  return 0;
}
