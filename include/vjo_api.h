/* Kernel <-> SceShell interface of Vita JP Overlay. Shared by kernel/,
 * shell/ and core/ (for the plain data types). */
#ifndef VJO_API_H
#define VJO_API_H

#include <stdint.h>

/* 14: 32 KiB model reads that wait for the kernel worker, Open reports the
 * file's modification time, results report resident weights.
 * 15: results carry the memory check (free USER, needed bytes, reserve) and
 * each recognized line's crop, CTC frames and time.
 * 16: requests carry a fallback model, Open chooses the model or the fallback,
 * results report when the fallback ran.
 * 17: boot-safety guard: vjoBootInfo tells every plugin whether this boot is
 * a safe one (no hooks, the Shell and game plugins unload themselves),
 * vjoBootMark lets the Shell record the stages it reaches.
 * 18: vjoMemInfo, the self-test's kernel memory sample (exact free/total of
 * the kernel's and the caller's partitions, the kernel ledger).
 * 19: OCR captures in the game's process: the display hook copies grey rows of
 * the region straight into the game worker's buffer (vjoOcrCaptureRows); no
 * kernel capture buffer for OCR (vjoOcrReadRaw is gone). Requests carry the
 * region and flags, results the capture and a step log. vjoMemInfo reports each
 * memory query route's step and result, and the game worker may call it.
 * 20: the job's policy rides in VjoGameOcrRequest.flags (the stable-text wait,
 * its limit, waiting first, blank lines skipped); two new step kinds (settle,
 * blank). A job may publish its first pass, void it (a VJO_ROWS_SIG read from
 * row 0 it does not finish) and publish its retry's. Sizes are unchanged. */
#define VJO_API_VERSION 21

/* Normalized rectangle, 0..65535 on both axes. w == 0 means full screen. */
typedef struct {
    uint16_t x, y, w, h;
} VjoRect;

enum VjoTrigger {
    VJO_TRIGGER_SELECT = 0,
    VJO_TRIGGER_START = 1,
    VJO_TRIGGER_L_R = 2,
    VJO_TRIGGER_SELECT_L = 3,
    VJO_TRIGGER_SELECT_R = 4,
    VJO_TRIGGER_REAR_DOUBLE_TAP = 5,
    VJO_TRIGGER_COUNT
};

/* vjoWaitEvent bits */
#define VJO_EV_TRIGGER       0x01u /* toggle trigger pressed */
#define VJO_EV_REGION_STABLE 0x02u /* region changed, then unchanged (animation aside) for ~300 ms */
#define VJO_EV_GAME_START    0x04u
#define VJO_EV_GAME_EXIT     0x08u
#define VJO_EV_CAPTURE_DONE  0x10u /* raw rows ready (or failed, see VjoState.capture_result) */
#define VJO_EV_SUBTITLE      0x20u /* subtitle trigger pressed */
#define VJO_EV_REGION_QUIET  0x40u /* the region became quiet (VjoState.quiet) */
#define VJO_EV_ALL           0x7Fu

/* VjoState.alloc_status */
#define VJO_ALLOC_NONE 0  /* no game running */
#define VJO_ALLOC_OK   1
#define VJO_ALLOC_FAIL 2  /* not enough memory for capture buffers */

typedef struct {
    uint32_t size;          /* sizeof(VjoState), set by caller */
    int32_t game_pid;
    uint32_t fb_width, fb_height, fb_pitch, fb_pixelformat;
    uint32_t scene;         /* the screen in the region: a new number per change (animation aside), 0 = none yet */
    uint32_t stable;        /* 1 if the scene has settled */
    uint32_t alloc_status;
    int32_t capture_result; /* 0 ok (rows readable via vjoReadRaw), <0 error code */
    uint32_t width, height; /* captured region size in pixels */
    uint32_t capture_scene; /* scene of the captured pixels; 0 = unknown (or VJO_CAPTURE_FULL) */
    uint32_t raw_stride;    /* bytes per raw row (A8B8G8R8) */
    uint32_t done_seq;      /* sequence number of the last finished capture */
    uint32_t unsettled_ms;  /* the region has been changing this long without settling (0 = settled) */
    /* not settled, but only a small spot keeps changing (an icon not
     * masked yet): worth capturing */
    uint32_t quiet;
    uint32_t region_seq;    /* the last successful region capture (not VJO_CAPTURE_FULL); 0 = none */
    /* the scene if that capture shows it now (animated cells aside, and
     * the quiet spot for one taken since it started), else 0: a capture
     * taken before an icon was masked shows the screen once it is */
    uint32_t region_scene;
} VjoState;

typedef struct {
    uint32_t buttons;       /* SCE_CTRL_* of the real pad, unfiltered */
    uint8_t lx, ly, rx, ry; /* analog sticks, 128 = centre */
} VjoInput;

/* vjoRequestCapture flags */
#define VJO_CAPTURE_FULL 0x1u /* the whole frame instead of the region (Anki screenshot) */
#define VJO_CAPTURE_ONCE 0x2u /* JPEG reader: release after copying the final row */
#define VJO_CAPTURE_DISCARD 0x4u /* release an idle single-pass capture after an encoder error */

/* Shared syscall and copied-payload statuses. Physical retail hardware returned
 * queued -3 as 0xBFFFFFFD. Keep every custom status negative with bit 30 clear,
 * so that observed return transformation leaves the API 13+ contract unchanged.
 * Native SDK/file errors are returned untouched in their own namespaces. */
#define VJO_ERR_NO_GAME      ((int32_t)0xBFFFFFFFu)
#define VJO_ERR_NO_MEMORY    ((int32_t)0xBFFFFFFEu)
#define VJO_ERR_BUSY         ((int32_t)0xBFFFFFFDu)
#define VJO_ERR_TIMEOUT      ((int32_t)0xBFFFFFFCu)
#define VJO_ERR_FORMAT       ((int32_t)0xBFFFFFFBu)
#define VJO_ERR_ARG          ((int32_t)0xBFFFFFFAu) /* bad argument */
#define VJO_ERR_COPY         ((int32_t)0xBFFFFFF9u)
#define VJO_ERR_CANCELLED    ((int32_t)0xBFFFFFF7u)
#define VJO_ERR_PERM         ((int32_t)0xBFFFFFF8u) /* caller is not the registered role */

/* vjoSetGameActive's mode for the foreground process. */
enum VjoGameMode {
    VJO_GAME_NONE = 0,      /* a system app */
    VJO_GAME = 1,
    VJO_GAME_STATIC_FB = 2, /* draws into a framebuffer it set once and submits no
                             * frames (the PSP emulator): sampled from its pad calls */
};

/* The boot-safety guard's view of this boot (core/boot_guard.h has the
 * values: VJO_BOOT_NORMAL/SAFE, VJO_BOOT_STAGE_*, VJO_BOOT_FAULT_*). */
typedef struct {
    uint32_t size;        /* sizeof(VjoBootInfo), set by the caller */
    uint32_t seq;         /* this boot's number */
    int32_t mode;         /* VJO_BOOT_NORMAL or VJO_BOOT_SAFE */
    int32_t stage;        /* furthest VJO_BOOT_STAGE_* reached so far */
    int32_t inert;        /* the Shell plugin unloaded itself this boot */
    int32_t fault;        /* VJO_BOOT_FAULT_* seen this boot */
    int32_t failures;     /* normal boots in a row that did not complete */
    int32_t prev_mode, prev_stage, prev_inert, prev_fault; /* the previous boot */
    int32_t write_status; /* the last boot.txt write: 0 ok, else the error */
} VjoBootInfo;

/* vjoMemInfo: what the kernel can read about memory for the self-test
 * (docs/self-test.md). total/free are exact partition figures from the
 * kernel; a negative rc means that figure is unavailable and why. Three routes
 * are tried, each reported on its own, because only a console run tells which
 * works on its firmware (3.65's answer to the first route in API 18 was
 * 0x80020001, #74).
 *
 * Every partition object the firmware hands back is checked before
 * PhyMemPartInfoCore reads it: kernel objects live below 0x40000000 (TTBR0;
 * VitaSDK psp2kern/kernel/sysmem/data_transfers.h). A pointer in
 * PSVshellPlus's range 0x1000-0x1000000 (info.c:57, :91) is read; one higher in
 * the kernel half only inside a kernel memory block. The *_step fields name
 * where a route stopped (0: it read its figures):
 *   VJO_MEMSTEP_FIND 1       FindMemBlockByAddr / the function lookup
 *   VJO_MEMSTEP_INFO 2       MemBlockGetInfoEx / the address-space query
 *   VJO_MEMSTEP_RANGE 3      the object pointer is outside the kernel half (or 0, or misaligned)
 *   VJO_MEMSTEP_NO_BLOCK 4   in the kernel half above 0x1000000, but in no kernel memory block
 *   VJO_MEMSTEP_CORE 5       PhyMemPartInfoCore itself (its lookup, or the call: rc tells) */
#define VJO_MEMINFO_PARTS 8u
#define VJO_MEMSTEP_FIND 1
#define VJO_MEMSTEP_INFO 2
#define VJO_MEMSTEP_RANGE 3
#define VJO_MEMSTEP_NO_BLOCK 4
#define VJO_MEMSTEP_CORE 5
typedef struct {
    uint32_t size;           /* sizeof(VjoMemInfo), set by the caller */
    /* Route 1, a memory block's partition: FindMemBlockByAddr (size 0) ->
     * MemBlockGetInfoEx -> PhyMemPartInfoCore, for our module's block (the
     * kernel's partition, ScePhyMemPartKD when total is 36 MiB) and for the
     * caller's out pointer (SceShell's or the game's partition). */
    int32_t kernel_rc;
    uint32_t kernel_total, kernel_free;
    int32_t caller_rc;
    uint32_t caller_total, caller_free;
    uint32_t ledger_current, ledger_peak, ledger_limit; /* the kernel ledger's pool */
    uint32_t capture_bytes;  /* the kernel capture block held right now (Lens/Anki), 0 = none */
    int32_t kernel_step, caller_step;
    /* Route 2 (PSVshell, PSVshellPlus): the caller's address space control
     * block at +328 (main), +332 (CDRAM), +316 (phycont), each partition's
     * PhyMemPartInfoCore. cas_step: where the main partition stopped. */
    int32_t cas_rc;
    uint32_t cas_main_total, cas_main_free, cas_cdram_total, cas_cdram_free;
    uint32_t cas_phycont_total, cas_phycont_free;
    /* Route 3 (faps-coredump): a process's address space info
     * (SceSysmemForKernel_C3EF4055) and each of its partitions' InfoCore; the
     * one of 36 MiB is ScePhyMemPartKD. The kernel process's is asked first;
     * as_caller = 1 when that was refused and the caller's was read instead.
     * as_step: where it stopped, or for a partition that could not be read
     * the last such reason (as_skipped of them). */
    int32_t as_rc;
    uint32_t as_parts;
    uint32_t as_total[VJO_MEMINFO_PARTS], as_free[VJO_MEMINFO_PARTS];
    int32_t cas_step, as_step;
    uint32_t as_caller, as_skipped;
} VjoMemInfo;

/* Syscalls exported by VitaJPOverlay_Kernel (library VitaJPOverlayForUser).
 * All return < 0 on error. */
#if !defined(VJO_HOST)
#include <psp2common/types.h>
int vjoGetVersion(void);                 /* VJO_API_VERSION, no side effects */
int vjoRegisterShell(void);              /* caller becomes the shell if it has VitaJPOverlay_Shell loaded */
int vjoWaitEvent(uint32_t mask, uint32_t *out, SceUInt32 timeout_us); /* 0 timeout = forever */
int vjoGetState(VjoState *out);          /* set out->size first */
int vjoSetRegion(const VjoRect *r);      /* NULL = full screen */
int vjoSetTriggers(int toggle, int subtitle); /* enum VjoTrigger each, not equal */
int vjoSetGameActive(int pid, int mode);  /* shell: enum VjoGameMode of the foreground process */
int vjoSetInputBlock(int on);            /* block all game input (not the shell) */
int vjoPollInput(VjoInput *out);         /* raw pad, unfiltered */
int vjoRequestCapture(uint32_t flags);   /* VJO_CAPTURE_*; returns seq > 0; raw rows, the shell encodes the JPEG */
int vjoReadRaw(uint32_t row, uint32_t n, void *dst);      /* returns rows copied */
#endif
/* Boot-safety guard (core/boot_guard.h); declared for the host tests too. */
#ifdef __cplusplus
extern "C" {
#endif
int vjoBootInfo(VjoBootInfo *out);       /* any process; set out->size first */
int vjoBootMark(int stage);              /* shell: VJO_BOOT_STAGE_SHELL..OK reached; 0 = the shell is inert */
int vjoMemInfo(VjoMemInfo *out);         /* shell or the registered game worker: set out->size first */
#ifdef __cplusplus
}
#endif

#endif
