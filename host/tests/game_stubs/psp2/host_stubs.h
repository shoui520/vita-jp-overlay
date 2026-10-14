#ifndef VJO_GAME_HOST_STUBS
#define VJO_GAME_HOST_STUBS
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize, SceUInt;
typedef int64_t SceOff;
typedef struct { unsigned size; int size_user, size_cdram, size_phycont; } SceKernelFreeMemorySizeInfo;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0D808060
#define SCE_O_RDONLY 0
#define SCE_SEEK_SET SEEK_SET
#define SCE_SEEK_END SEEK_END
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_NO_RESIDENT 1
#define SCE_KERNEL_STOP_SUCCESS 0
#define SCE_KERNEL_STOP_FAIL 1
#define sceClibMemset memset
#define sceClibStrcmp strcmp
#define sceClibMemcpy memcpy
#define sceClibMemcmp memcmp
#define sceClibSnprintf snprintf
#define sceClibVsnprintf vsnprintf
#define SCE_KERNEL_ERROR_INVALID_UID ((int)0x80024501u)
#define SCE_KERNEL_ERROR_UNKNOWN_UID ((int)0x80028001u)
#define SCE_KERNEL_MEMORY_TYPE_NORMAL 0xD0
#define SCE_KERNEL_MEMORY_ACCESS_W 2
#define SCE_KERNEL_MEMORY_ACCESS_R 4
typedef struct { unsigned size; void *mappedBase; unsigned mappedSize; int memoryType; unsigned access, type; } SceKernelMemBlockInfo;
int sceClibPrintf(const char *, ...);
int sceKernelGetMemBlockInfoByRange(void *, unsigned, SceKernelMemBlockInfo *);
int sceKernelGetMemBlockInfoByAddr(void *, SceKernelMemBlockInfo *);
/* psp2/display.h */
typedef struct { SceSize size; void *base; unsigned pitch, pixelformat, width, height; } SceDisplayFrameBuf;
typedef struct { SceSize size; void *base; unsigned pitch, pixelformat, width, height, flags; } SceDisplayFrameBufExt;
#define SCE_DISPLAY_SETBUF_NEXTFRAME 1
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *, int);
uint64_t sceKernelGetProcessTimeWide(void);
SceUID sceKernelGetProcessId(void);
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *);
SceUID sceKernelAllocMemBlock(const char *, int, unsigned, void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, unsigned, unsigned, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, int *, void *);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(unsigned);
/* psp2/kernel/threadmgr.h: a thread's run time (ocr-speed: each line's share of a core) */
typedef uint64_t SceKernelSysClock;
typedef struct { SceSize size; SceUID processId; char name[32]; unsigned attr, status; void *entry, *stack;
                 int stackSize, initPriority, currentPriority, initCpuAffinityMask, currentCpuAffinityMask,
                 currentCpuId, lastExecutedCpuId; unsigned waitType; SceUID waitId; int exitStatus;
                 SceKernelSysClock runClocks; unsigned intrPreemptCount, threadPreemptCount, threadReleaseCount;
                 int changeCpuCount; int fNotifyCallback; int reserved; } SceKernelThreadInfo;
int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo *);
SceUID sceKernelGetThreadId(void);
/* psp2/power.h: the ARM clock (ocr-speed: ocr_boost_clock) */
int scePowerGetArmClockFrequency(void);
int scePowerSetArmClockFrequency(int);
int sceAppMgrAppParamGetString(int, int, char *, SceSize);
SceUID sceIoOpen(const char *, int, unsigned);
SceOff sceIoLseek(SceUID, SceOff, int);
int sceIoRead(SceUID, void *, SceSize);
int sceIoClose(SceUID);
int vjoGetVersion(void);
#endif
