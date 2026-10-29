/*
 * Stock entry points of the Even G1 application 1.5.60 (app build
 * 2025-03-21 11:38:43). Every address was read from the exact binary with
 * tools/g1dis.py; the 1.6.3 decompilation names are given for reference only.
 * Thumb addresses carry bit 0.
 */
#pragma once
#include <stdint.h>

#define FW_BASE_MAJOR 1
#define FW_BASE_MINOR 5
#define FW_BASE_PATCH 60

/* 0x1880c: returns the BLE context (1.6.3: FUN_00018320). */
typedef uint8_t *(*fw_get_ctx_fn)(void);
#ifndef FW_GET_CTX
#define FW_GET_CTX ((fw_get_ctx_fn)0x0001880d)
#endif

/* ctx+0xc: synchronous Nordic UART notify, used by the stock #t reply. */
typedef int (*fw_send_fn)(const void *data, uint32_t len);
#define FW_CTX_SEND_OFFSET 0x0c
#ifndef FW_SEND_OF
#define FW_SEND_OF(ctx) (*(fw_send_fn *)((ctx) + FW_CTX_SEND_OFFSET))
#endif

/* 0xef28: stock text-command hook; non-zero return means "not consumed". */
typedef int (*fw_hook_fn)(const void *data, uint32_t len);
#ifndef FW_SPEC_BLE_COMMAND_HOOK
#define FW_SPEC_BLE_COMMAND_HOOK ((fw_hook_fn)0x0000ef29)
#endif

/* 0x16d3c: returns the GlassesState singleton (1.6.3: __get_dashboard_state). */
typedef uint8_t *(*fw_get_state_fn)(void);
#ifndef FW_GET_STATE
#define FW_GET_STATE ((fw_get_state_fn)0x00016d3d)
#endif

/* jdb_panel_context is embedded in GlassesState at +0xb6c. Offsets below are
 * relative to that context and were read from _reflash_fb_data_to_lcd (0x4a22c)
 * and its full-frame caller at 0x3eae8. */
#define FW_PANEL_OFFSET      0xb6c
#define FW_PANEL_FRAME       0x344   /* uint8_t *frame_buffers: 200 rows x 320 bytes, 4bpp */
#define FW_PANEL_COL_OFFSET  0x348   /* int: panel column origin */
#define FW_PANEL_ROW_OFFSET  0x34c   /* int: panel row origin */
#define FW_PANEL_POWERED     0x35c   /* int: non-zero while the panel accepts writes */
#define FW_PANEL_HEIGHT_GEAR 0x354   /* uint8_t raster_height_gear 0-8: row origin = (8 - gear) * 35 */
#define FW_PANEL_DEPTH_GEAR  0x355   /* uint8_t canvas_distance_gear 1-9 (cal_panel_canvas_coord) */
#define FW_FRAME_WIDTH       640
#define FW_FRAME_HEIGHT      200
#define FW_FRAME_STRIDE      320
#ifndef FW_FRAME_OF
#define FW_FRAME_OF(panel) ((uint8_t *)(uintptr_t)(*(uint32_t *)((panel) + FW_PANEL_FRAME)))
#endif

/* 0x4a22c: push frame rows [y0,y1] (inclusive) columns [x0,x1) (x1 exclusive:
 * it sends x1/2 - x0/2 bytes per row) to the panel over SPI, taking the LCD
 * mutex per row and busy-waiting 1 ms at the end. Returns -1 when the panel
 * is not powered. x1 is clamped to 0x280, y1 to 200. */
typedef int (*fw_reflash_fn)(int col_off, int row_off, int x0, int y0, int x1, int y1);
#ifndef FW_REFLASH_FB
#define FW_REFLASH_FB ((fw_reflash_fn)0x0004a22d)
#endif

/* Stock BLE receive path copies at most this many bytes per packet. */
#define FW_MAX_PACKET 256

/* Panel power, as used by the stock silent-mode handlers. Both take the
 * GlassesState pointer and manage the state byte at +0xfea (0 running,
 * 1 suspended, 2 suspending, 3 resuming). Resume is a no-op unless suspended
 * and suspend is a no-op unless running. */
typedef int (*fw_system_power_fn)(uint8_t *state);
#ifndef FW_SYSTEM_SUSPEND
#define FW_SYSTEM_SUSPEND ((fw_system_power_fn)0x0002d78d)   /* 0x2d78c: global_system_suspend */
#endif
#ifndef FW_SYSTEM_RESUME
#define FW_SYSTEM_RESUME ((fw_system_power_fn)0x0002d809)    /* 0x2d808: global_system_resume */
#endif
#define FW_STATE_POWER_OFFSET 0xfea

/* jdb_panel_context callback table (register_jdb_panel_context at 0x4a114):
 * +0x00 init, +0x04 suspend (0x49dfc), +0x08 resume (0x49f28), +0x0c off
 * (0x49d9c), +0x10 on (0x49e48). panel_on powers the rail, re-initialises the
 * panel and sets +0x35c; panel_off clears it. Both take the context pointer. */
typedef int (*fw_panel_cb_fn)(uint8_t *panel);
#define FW_PANEL_CB_OFF 0x0c
#define FW_PANEL_CB_ON  0x10
#ifndef FW_PANEL_CB_OF
#define FW_PANEL_CB_OF(panel, slot) ((fw_panel_cb_fn)(uintptr_t)(*(uint32_t *)((panel) + (slot))))
#endif

/* RAM owned by the CFW. The stock BMP flow (commands 0x15/0x16/0x20) stages
 * sectors in a 4 KB buffer at 0x200174e4 (1.6.3: DAT_20017d30) and spills
 * into the 1 KB after it; the next live variable is at 0x200188e4. The CFW
 * consumes those stock commands, so nothing else touches this range. */
#define FW_CFW_RAM_BASE 0x200174e4u
#define FW_CFW_RAM_SIZE 0x1400u
#ifndef FW_CFW_RAM
#define FW_CFW_RAM ((uint8_t *)FW_CFW_RAM_BASE)
#endif

/* Stock UTF-8 text drawer (gui_utf_draw at 0x466d4). Draws left-aligned text
 * into the frame buffer rows named by the global the setter below points at;
 * x/y are absolute frame pixels, (x1, y1) the box's bottom-right, max_lines
 * the line cap, dim_glyphs how many leading glyphs are dimmed, and the last
 * three must be 0/NULL/0 or it draws nothing. Font 0 is the proportional
 * 20 px face (26-row cells, 27 px pitch) from internal flash for ASCII; fonts
 * 1 and 2 are digit faces 32 and 39 rows tall. Non-ASCII glyphs are fetched
 * from external flash under a mutex, so call from thread context only. */
typedef int (*fw_utf_draw_fn)(int unused, const char *utf8, unsigned font, int x0, int y0, int x1, int y1,
                              unsigned max_lines, unsigned dim_glyphs, int p10, void *effect_cb, int p12);
#ifndef FW_GUI_UTF_DRAW
#define FW_GUI_UTF_DRAW ((fw_utf_draw_fn)0x000466d5)
#endif
/* 0x45880: store the row-pointer array (panel + 0x24) in the drawer's global. */
typedef int (*fw_set_rows_fn)(void *rows);
#ifndef FW_GUI_SET_ROWS
#define FW_GUI_SET_ROWS ((fw_set_rows_fn)0x00045881)
#endif
/* 0x459b8: clear flag bit 1 so the drawer neither clears the box nor flushes. */
typedef int (*fw_void_fn)(void);
#ifndef FW_GUI_MANUAL_FLUSH
#define FW_GUI_MANUAL_FLUSH ((fw_void_fn)0x000459b9)
#endif
#define FW_PANEL_ROWS 0x24   /* int *rows[200] inside the panel context */

/* 0x7a15c: stock malloc wrapper (the call that allocates the 64 KB frame
 * buffer at boot). Returns NULL when the heap is exhausted. The CFW takes one
 * arena at first use and never frees it. */
typedef void *(*fw_malloc_fn)(uint32_t size);
#ifndef FW_MALLOC
#define FW_MALLOC ((fw_malloc_fn)0x0007a15d)
#endif
/* Heap pointers are kept as 32-bit addresses in the CFW context. */
#ifndef FW_ADDR_OF
#define FW_ADDR_OF(ptr) ((uint32_t)(uintptr_t)(ptr))
#define FW_PTR_OF(addr) ((uint8_t *)(uintptr_t)(addr))
#endif

/* Diagnostics: map a 32-bit address to a readable pointer (identity on the
 * target; the host test substitutes its own memory). */
#ifndef FW_PEEK_PTR
#define FW_PEEK_PTR(addr) ((const uint8_t *)(uintptr_t)(addr))
#endif

/* Second CFW RAM region: the stock navigation map buffer (0x1210 bytes at
 * 0x2001bf23, referenced only by the stubbed UI block) and the translate
 * screen's 512-byte buffer after it. PEEK showed it untouched after boot.
 * A third region, 0x20013b84..0x20014f28 (DashBoard_Reflash/navigation
 * scratch), is also free but unused so far. */
#define FW_CFW_RAM2_BASE 0x2001bf24u
#define FW_CFW_RAM2_SIZE 0x140cu
#ifndef FW_CFW_RAM2
#define FW_CFW_RAM2 ((uint8_t *)FW_CFW_RAM2_BASE)
#endif

/* Display-thread tick for animation. 0x80ed8 posts a stock refresh message
 * for `screen` to the display queue (display_reflash + k_yield); the display
 * thread then calls display_reflash_handler(state, screen, p3, mode), which the
 * profile redirects to cfw_display_tick. 0x7ff98 is k_msleep(ms). */
typedef int (*fw_display_post_fn)(uint8_t screen, uint32_t p2, uint32_t p3);
#ifndef FW_DISPLAY_POST
#define FW_DISPLAY_POST ((fw_display_post_fn)0x00080ed9)
#endif
typedef int (*fw_msleep_fn)(int ms);
#ifndef FW_K_MSLEEP
#define FW_K_MSLEEP ((fw_msleep_fn)0x0007ff99)
#endif
#define FW_SCREEN_CFW 0x10

/* Touch input (1.5.60). The aw9320x capacitive IRQ wakes touch_key_thread
 * (0x2ba60), which classifies the press history into a gesture code, stores
 * it in the key-event word and calls the 14-byte helper at 0x2ba48, which
 * gives the key-event semaphore when it has been initialised. The profile
 * redirects that helper to cfw_touch_event. Codes: 1 single, 2 double,
 * 3 triple, 4 long-press start (stock turns the mic on), 5 long-press end,
 * 6 long-press timeout. 5/6/10/11/15 taps act inside the thread (reboot,
 * force bind, user-data reset, ESB band) and never reach the helper. */
#ifndef FW_TOUCH_EVENT
#define FW_TOUCH_EVENT ((volatile uint32_t *)0x20007cb4u)   /* gesture code (1.6.3: DAT_200084f8) */
#define FW_KEY_EVENT_SEM ((void *)0x20007cb8u)              /* struct k_sem __key_event_sem */
#define FW_KEY_EVENT_SEM_READY ((volatile uint8_t *)0x2001920cu)  /* set once k_sem_init succeeded */
#endif
typedef void (*fw_sem_give_fn)(void *sem);
#ifndef FW_K_SEM_GIVE
#define FW_K_SEM_GIVE ((fw_sem_give_fn)0x0007ff69)   /* 0x7ff68: k_sem_give trampoline */
#endif

/* Stock event poster (1.5.60 0x28f38; 1.6.3 FUN_00029774). Every stock
 * event the glasses report to the phone passes through it as a code in r0:
 * 2 look up, 3 bow head (imu_fusion_thread), 6 worn, 7 taken off out of the
 * case, 8 put in the open case, 0xb case closed (check_work_mode), plus key
 * and dashboard events. It parks the code in a pending byte and either
 * enqueues [F5][code][CB] for the connected central (enqueue_bt_data,
 * 0x184d8) or gives the BLE context's event semaphore at ctx+0x218 when the
 * link is not ready (ctx+0x248 and ctx+0x220 both zero). The profile
 * redirects its entry to cfw_stock_event, which re-implements this. */
#ifndef FW_EVENT_PENDING
#define FW_EVENT_PENDING ((volatile uint8_t *)0x20003035u)
#endif
#define FW_CTX_EVENT_SEM    0x218   /* struct k_sem inside the BLE context */
#define FW_CTX_EVENT_READY  0x220   /* uint32_t, non-zero: send directly */
#define FW_CTX_EVENT_DIRECT 0x248   /* uint8_t, non-zero: send directly */
typedef int (*fw_enqueue_fn)(const void *data, uint32_t len);
#ifndef FW_ENQUEUE_BT_DATA
#define FW_ENQUEUE_BT_DATA ((fw_enqueue_fn)0x000184d9)
#endif

/* IMU attitude (1.5.60): three int32 words in hundredths of a degree kept by
 * imu_fusion_thread on the master temple (1.6.3 DAT_20008484/88/8c). The
 * third is the pitch the head-up detector compares; the first behaves as
 * roll and the second as a drifting yaw. Zero on the non-master temple. */
#ifndef FW_IMU_ATTITUDE
#define FW_IMU_ATTITUDE ((volatile int32_t *)0x20007c40u)
#endif

/* Microphone (1.5.60). Capture is a session of do_pdm_transfer run by the
 * main thread: the phone's 0x0E 01 handler sets bit 2 of the byte at
 * state+0x1065 and gives the main semaphore at state+0x68 (dmic_record_start,
 * 0x3e1f0, does the same for quick notes); the session configures the PDM
 * device, reads 16 kHz blocks, LC3-encodes them (app_codec_lc3_test) and,
 * while the byte at 0x20003036 is 0, queues them for the BLE dispatcher,
 * which sends [F1][seq][200 bytes] (send_dmic_msg, 0x1a56a). That byte is
 * 1 to end the session (0x30ea8, from exit_mic_mode) and 2 to capture
 * without sending (0x30ecc). The word at 0x20007d14 non-zero diverts the
 * chunks to the quick-note flash recorder instead of BLE. */
#define FW_STATE_MIC_ARM    0x1065   /* uint8_t: bit 2 asks the main thread for a capture session */
#define FW_STATE_MAIN_SEM   0x68     /* struct k_sem the main thread waits on */
#ifndef FW_MIC_MODE
#define FW_MIC_MODE ((volatile uint8_t *)0x20003036u)        /* 0 stream, 1 stop, 2 capture only */
#define FW_MIC_TO_FLASH ((volatile uint32_t *)0x20007d14u)   /* non-zero: quick-note recorder owns the chunks */
#endif
