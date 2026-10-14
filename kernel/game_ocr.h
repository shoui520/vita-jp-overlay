#ifndef VJO_KERNEL_GAME_OCR_H
#define VJO_KERNEL_GAME_OCR_H

#include "../include/vjo_game_ocr.h"
#include <psp2kern/types.h>

/* All *_locked helpers require g.lock. No helper takes g.game_lock. */
/* g.mevf bits: the kernel worker finished a model operation; the display hook
 * finished an armed vjoOcrCaptureRows. */
#define MEV_DONE 0x1u
#define MEV_ROWS 0x2u
void game_ocr_init(void);
int game_ocr_capture_busy_locked(void);
int game_ocr_shutdown_locked(void);
void game_ocr_shell_changed_locked(void);
void game_ocr_foreground_changed_locked(void);
/* The registered game worker's process (0: none), for vjoMemInfo's caller check. */
SceUID game_ocr_worker_pid_locked(void);
/* Called only by the existing kernel worker; disk I/O runs without g.lock. */
void game_ocr_model_tick(void);
/* Called with g.game_lock held; takes g.lock in this fixed order. */
void game_ocr_process_gone(SceUID pid);
/* Display hook (display.c on_frame, game context, no lock): copies an armed
 * vjoOcrCaptureRows of process pid from this frame. */
void game_ocr_rows_hook(SceUID pid, uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h);
/* ocr-speed: one of the game's frames (display hook, game context): the frame pacing per OCR job. */
void game_ocr_frame(int64_t now_us);

#endif
