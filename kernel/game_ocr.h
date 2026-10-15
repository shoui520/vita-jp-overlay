#ifndef VJO_KERNEL_GAME_OCR_H
#define VJO_KERNEL_GAME_OCR_H

#include "../include/vjo_game_ocr.h"
#include <psp2kern/types.h>

/* All *_locked helpers require g.lock. No helper takes g.game_lock. */
/* g.mevf bits: the kernel worker finished a model operation; the display hook
 * finished an armed vjoOcrCaptureRows; vjoOcrSubmit queued a job (the game
 * worker's vjoOcrTake waits for it); vjoOcrComplete published a result (the
 * Shell's vjoOcrRead waits for it). */
#define MEV_DONE 0x1u
#define MEV_ROWS 0x2u
#define MEV_JOB 0x4u
#define MEV_RESULT 0x8u
void game_ocr_init(void);
int game_ocr_capture_busy_locked(void);
int game_ocr_shutdown_locked(void);
/* Threads still inside vjoOcrTake's or vjoOcrRead's wait. */
int game_ocr_waiters_locked(void);
/* module_stop's part, without g.lock: game_ocr_shutdown_locked, then waits for the
 * woken takes and reads to leave (an undone stop if one does not). 0 or VJO_ERR_BUSY. */
int game_ocr_stop(void);
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

#endif
