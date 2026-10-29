/* Host test for the injected G1CFW handler: stock entry points are stubbed and
 * the frame buffer is a guarded array, so bounds bugs show up as failures here
 * instead of on the glasses. Built and run by tests/test_host.py. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t *host_get_ctx(void);
static int host_hook(const void *data, uint32_t len);
static uint8_t *host_get_state(void);
static int host_reflash(int col, int row, int x0, int y0, int x1, int y1);
#define FW_GET_CTX host_get_ctx
#define FW_SPEC_BLE_COMMAND_HOOK host_hook
#define FW_GET_STATE host_get_state
#define FW_REFLASH_FB host_reflash
#define FW_SEND_OF(ctx) host_send
#define FW_SYSTEM_SUSPEND host_suspend
#define FW_SYSTEM_RESUME host_resume
#define FW_PANEL_CB_OF(panel, slot) ((slot) == 0x10 ? host_panel_on : host_panel_off)
static uint8_t host_ram[0x1400 + 64];
static int host_utf_draw(int u, const char *t, unsigned f, int x0, int y0, int x1, int y1, unsigned l, unsigned d, int p10, void *cb, int p12);
static int host_set_rows(void *rows);
static int host_manual_flush(void);
#define FW_GUI_UTF_DRAW host_utf_draw
#define FW_GUI_SET_ROWS host_set_rows
#define FW_GUI_MANUAL_FLUSH host_manual_flush
static void *host_malloc(uint32_t size);
#define FW_MALLOC host_malloc
static uint8_t host_peek_mem[64];
#define FW_PEEK_PTR(addr) (host_peek_mem + ((addr) & 63))
static uint8_t host_ram2[0x140c + 64];
#define FW_CFW_RAM2 (host_ram2 + 32)
static uint8_t host_ram3[32 + 0x13a4 + 32];
#define FW_CFW_RAM3 (host_ram3 + 32)
static int host_display_post(uint8_t screen, uint32_t p2, uint32_t p3);
static int host_msleep(int ms);
#define FW_DISPLAY_POST host_display_post
#define FW_K_MSLEEP host_msleep
static uint32_t host_touch_code; static uint8_t host_key_sem_ready; static int host_sem_gives; static void *host_last_sem;
static uint8_t host_key_sem[16];
static void host_sem_give(void *sem);
#define FW_TOUCH_EVENT (&host_touch_code)
#define FW_KEY_EVENT_SEM ((void *)host_key_sem)
#define FW_KEY_EVENT_SEM_READY (&host_key_sem_ready)
#define FW_K_SEM_GIVE host_sem_give
static uint8_t host_event_pending = 0xFF; static int host_enqueues; static uint8_t host_enqueued[8]; static uint32_t host_enqueued_len;
static int host_enqueue(const void *data, uint32_t len);
#define FW_EVENT_PENDING (&host_event_pending)
#define FW_ENQUEUE_BT_DATA host_enqueue
static int32_t host_imu[3];
#define FW_IMU_ATTITUDE host_imu
static uint8_t host_mic_mode = 1; static uint32_t host_mic_to_flash = 1;
#define FW_MIC_MODE (&host_mic_mode)
#define FW_MIC_TO_FLASH (&host_mic_to_flash)
static uint8_t host_arena[48 * 1024 + 64];
#define FW_ADDR_OF(ptr) ((uint32_t)((uint8_t *)(ptr) - host_arena))
#define FW_PTR_OF(addr) (host_arena + (addr))
#define FW_CFW_RAM (host_ram + 32)
static int host_panel_on(uint8_t *panel);
static int host_panel_off(uint8_t *panel);
static int host_suspend(uint8_t *s);
static int host_resume(uint8_t *s);
#define FW_FRAME_OF(panel) ((void)(panel), host_frame)
static int host_send(const void *data, uint32_t len);
static uint8_t *host_frame;
#include "../../cfw/src/g1cfw.c"

#define GUARD 64
static uint8_t state[0x1100];
static uint8_t frame_store[GUARD + 5 + 200 * 320 + GUARD];
static uint8_t *frame;
static uint8_t ctx[0x260];
static uint8_t last_reply[64];
static uint32_t last_reply_len;
static int hook_calls, reflash_calls, last_rect[6];

static int host_send(const void *data, uint32_t len)
{
    if (len > sizeof last_reply) {
        abort();
    }
    memcpy(last_reply, data, len);
    last_reply_len = len;
    return 0;
}

static uint8_t *host_get_ctx(void) { return ctx; }
static int host_hook(const void *data, uint32_t len) { (void)data; (void)len; hook_calls++; return 1; }
static uint8_t *host_get_state(void) { return state; }
static uint8_t pushed_row[320];   /* frame row 0 as it looked during the push */
static int host_reflash(int col, int row, int x0, int y0, int x1, int y1)
{
    reflash_calls++;
    int r[6] = { col, row, x0, y0, x1, y1 };
    memcpy(last_rect, r, sizeof r);
    if (host_frame) memcpy(pushed_row, host_frame, sizeof pushed_row);
    return 0;
}

static int host_suspend(uint8_t *s)
{
    if (s[0xfea] == 0) { s[0xfea] = 1; *(int *)(s + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0; }
    return 0;
}
static int host_resume(uint8_t *s)
{
    if (s[0xfea] == 1) { s[0xfea] = 0; *(int *)(s + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1; }
    return 0;
}

static int panel_cb_calls;
static int host_panel_on(uint8_t *panel) { panel_cb_calls++; *(int *)(panel + FW_PANEL_POWERED) = 1; return 0; }
static int host_panel_off(uint8_t *panel) { panel_cb_calls++; *(int *)(panel + FW_PANEL_POWERED) = 0; return 0; }

static char utf_text[256]; static unsigned utf_font, utf_lines; static int utf_box[4], utf_calls, rows_set, manual_flush_calls; static void *rows_arg;
static int host_utf_draw(int u, const char *t, unsigned f, int x0, int y0, int x1, int y1, unsigned l, unsigned d, int p10, void *cb, int p12)
{
    if (u || d || p10 || cb || p12) abort();
    strncpy(utf_text, t, sizeof utf_text - 1); utf_font = f; utf_lines = l;
    utf_box[0] = x0; utf_box[1] = y0; utf_box[2] = x1; utf_box[3] = y1; utf_calls++;
    return 0;
}
static int host_set_rows(void *rows) { rows_arg = rows; rows_set++; return 0; }
static int host_manual_flush(void) { manual_flush_calls++; return 0; }

static int malloc_calls; static uint32_t malloc_size;
/* Refuse 48 KB and 40 KB like the real heap did; grant the 32 KB retry. */
static void *host_malloc(uint32_t size) { malloc_calls++; malloc_size = size; return size > 32 * 1024 ? NULL : host_arena + 32; }

static void host_sem_give(void *sem) { host_sem_gives++; host_last_sem = sem; }
static int host_enqueue(const void *data, uint32_t len) { if (len > sizeof host_enqueued) abort(); memcpy(host_enqueued, data, len); host_enqueued_len = len; host_enqueues++; return 0; }
static int post_calls, sleep_calls, last_sleep; static uint8_t last_screen;
static int host_display_post(uint8_t screen, uint32_t p2, uint32_t p3) { (void)p2; (void)p3; post_calls++; last_screen = screen; return 0; }
static int host_msleep(int ms) { sleep_calls++; last_sleep = ms; return 0; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void guards_intact(void)
{
    for (int i = 0; i < GUARD; i++) {
        CHECK(frame_store[i] == 0xA5);
        CHECK(frame_store[sizeof frame_store - 1 - i] == 0xA5);
    }
}

static int send_packet(const uint8_t *packet, uint32_t len)
{
    last_reply_len = 0;
    return cfw_rx(packet, len);
}

int main(void)
{
    host_frame = frame = frame_store + GUARD + 5;
    memset(frame_store, 0xA5, sizeof frame_store);
    memset(frame, 0, 200 * 320);
    uint8_t *panel = state + FW_PANEL_OFFSET;
    *(uint32_t *)(panel + FW_PANEL_FRAME) = 0x20012340u;
    *(int *)(panel + FW_PANEL_COL_OFFSET) = 7;
    *(int *)(panel + FW_PANEL_ROW_OFFSET) = 35;
    *(int *)(panel + FW_PANEL_POWERED) = 1;

    /* Pass-through: anything that is not 0xF0 goes to the stock hook untouched. */
    const uint8_t stock[] = { 0x25, 0x06, 0x00 };  /* stock heartbeat: passed through */
    CHECK(send_packet(stock, sizeof stock) == 1 && hook_calls == 1 && last_reply_len == 0);
    const uint8_t text[] = { '#', 't' };
    CHECK(send_packet(text, sizeof text) == 1 && hook_calls == 2);
    CHECK(send_packet(stock, 0) == 1 && hook_calls == 3);

    /* Ping. */
    const uint8_t ping[] = { 0xF0, 0x00 };
    CHECK(send_packet(ping, sizeof ping) == 0 && hook_calls == 3);
    CHECK(last_reply_len == 15 && last_reply[0] == 0xF0 && last_reply[1] == 0 && last_reply[2] == 0);
    CHECK(memcmp(last_reply + 3, "G1CFW/26", 9) == 0 && last_reply[12] == 1 && last_reply[13] == 5 && last_reply[14] == 60);

    /* Short and unknown commands are answered, never dispatched. */
    const uint8_t only_sid[] = { 0xF0 };
    CHECK(send_packet(only_sid, 1) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    const uint8_t unknown[] = { 0xF0, 0x7F };
    CHECK(send_packet(unknown, 2) == 0 && last_reply[1] == 0x7F && last_reply[2] == CFW_BAD_COMMAND);

    /* Info. */
    const uint8_t info[] = { 0xF0, 0x04 };
    CHECK(send_packet(info, 2) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 1);
    CHECK(last_reply[5] == 7 && last_reply[7] == 35 && last_reply[13] == 160 && last_reply[14] == 200);

    /* Fill with flush. */
    uint8_t fill[12] = { 0xF0, 0x02, 0x02, 10, 0, 20, 0, 8, 0, 3, 0, 0x0C };
    CHECK(send_packet(fill, sizeof fill) == 0 && last_reply[2] == CFW_OK);
    CHECK(frame[20 * 320 + 5] == 0xCC && frame[20 * 320 + 8] == 0xCC && frame[20 * 320 + 9] == 0);
    CHECK(frame[22 * 320 + 5] == 0xCC && frame[23 * 320 + 5] == 0 && frame[20 * 320 + 4] == 0);
    CHECK(reflash_calls == 1 && last_rect[0] == 7 && last_rect[1] == 35 && last_rect[2] == 10 && last_rect[3] == 20);
    CHECK(last_rect[4] == 18 && last_rect[5] == 22);  /* x1 exclusive, y1 inclusive */

    /* Rectangle validation: odd x, odd w, zero size, overflow, and a too-short payload. */
    uint8_t bad[12];
    memcpy(bad, fill, sizeof bad); bad[3] = 11;
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(bad, fill, sizeof bad); bad[7] = 7;
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(bad, fill, sizeof bad); bad[9] = 0;
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(bad, fill, sizeof bad); bad[3] = 0x7E; bad[4] = 0x02; /* x0 = 638, w = 8 */
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(bad, fill, sizeof bad); bad[5] = 0xC6; /* y0 = 198, h = 3 */
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(bad, fill, sizeof bad); bad[3] = 0xFE; bad[4] = 0xFF; bad[7] = 0x04; bad[8] = 0; /* x0 = 65534: no wraparound */
    CHECK(send_packet(bad, sizeof bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    CHECK(send_packet(fill, 11) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* 1bpp blit of a 16x2 rect at (4, 100), level 9, without flush. */
    uint8_t blit[] = { 0xF0, 0x01, 0x01 | 0x90, 4, 0, 100, 0, 16, 0, 2, 0, 0xA5, 0xFF, 0x00, 0x81 };
    CHECK(send_packet(blit, sizeof blit) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1);
    CHECK(frame[100 * 320 + 2] == 0x90 && frame[100 * 320 + 3] == 0x90 && frame[100 * 320 + 4] == 0x09);
    CHECK(frame[100 * 320 + 5] == 0x09 && frame[100 * 320 + 6] == 0x99 && frame[100 * 320 + 9] == 0x99);
    CHECK(frame[100 * 320 + 10] == 0 && frame[100 * 320 + 1] == 0);
    CHECK(frame[101 * 320 + 2] == 0 && frame[101 * 320 + 5] == 0 && frame[101 * 320 + 6] == 0x90 && frame[101 * 320 + 9] == 0x09);
    /* Default level is 0xF when the high nibble is zero. */
    uint8_t blit_default[] = { 0xF0, 0x01, 0x01, 0, 0, 0, 0, 8, 0, 1, 0, 0x80 };
    CHECK(send_packet(blit_default, sizeof blit_default) == 0 && frame[0] == 0xF0 && frame[1] == 0);
    /* 4bpp blit with flush; payload too short is refused before any write. */
    uint8_t raw[] = { 0xF0, 0x01, 0x02, 0x7C, 0x02, 0xC7, 0, 4, 0, 1, 0, 0x12, 0x34 };
    CHECK(send_packet(raw, sizeof raw) == 0 && last_reply[2] == CFW_OK && reflash_calls == 2);
    CHECK(frame[199 * 320 + 318] == 0x12 && frame[199 * 320 + 319] == 0x34 && last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(send_packet(raw, sizeof raw - 1) == 0 && last_reply[2] == CFW_BAD_LENGTH && reflash_calls == 2);

    /* Flush only, and the panel-off path. */
    uint8_t flush[] = { 0xF0, 0x03, 0, 0, 0, 0, 0x80, 0x02, 0xC8, 0 };
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_OK && reflash_calls == 3);
    *(int *)(panel + FW_PANEL_POWERED) = 0;
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_PANEL_OFF && reflash_calls == 3);
    CHECK(send_packet(fill, sizeof fill) == 0 && last_reply[2] == CFW_PANEL_OFF && frame[20 * 320 + 5] == 0xCC);

    /* Panel power: suspend then resume through the stubbed stock entry points. */
    uint8_t panel_off_cmd[] = { 0xF0, 0x05, 0x00 };
    CHECK(send_packet(panel_off_cmd, sizeof panel_off_cmd) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 1 && last_reply[4] == 0);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_PANEL_OFF);
    uint8_t panel_on_cmd[] = { 0xF0, 0x05, 0x01 };
    CHECK(send_packet(panel_on_cmd, sizeof panel_on_cmd) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 1);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_OK);
    uint8_t panel_force_off[] = { 0xF0, 0x05, 0x03 };
    CHECK(send_packet(panel_force_off, sizeof panel_force_off) == 0 && last_reply[2] == CFW_OK && last_reply[4] == 0 && panel_cb_calls == 1);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_PANEL_OFF);
    uint8_t panel_force_on[] = { 0xF0, 0x05, 0x02 };
    CHECK(send_packet(panel_force_on, sizeof panel_force_on) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 1 && panel_cb_calls == 2);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_OK);
    uint8_t panel_bad[] = { 0xF0, 0x05, 0x07 };
    CHECK(send_packet(panel_bad, sizeof panel_bad) == 0 && last_reply[2] == CFW_BAD_COMMAND);
    CHECK(send_packet(panel_bad, 2) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* Stock BMP flow commands are swallowed, never dispatched, never answered. */
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    hook_calls = 0;
    const uint8_t bmp[] = { 0x15, 0x01, 0xAA };
    CHECK(send_packet(bmp, sizeof bmp) == 0 && hook_calls == 0 && last_reply_len == 0);
    const uint8_t bmp_end[] = { 0x20, 0x0D, 0x0E };
    CHECK(send_packet(bmp_end, sizeof bmp_end) == 0 && hook_calls == 0);
    CHECK(memcmp(host_ram + 32, "CFW1", 4) == 0);  /* context initialised lazily with its magic */
    CHECK(host_ram[0] == 0 && host_ram[sizeof host_ram - 1] == 0);  /* guards around the arena */

    /* CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
    CHECK(cfw_crc16((const uint8_t *)"123456789", 9) == 0x29B1);

    /* Stream: a FILL record split across three packets, batched with a FLUSH. */
    memset(frame, 0, 200 * 320);
    reflash_calls = 0;
    uint8_t msg[] = { 0x02, 10, 0, 0x00, 4, 0, 6, 0, 8, 0, 2, 0, 0x07,   /* FILL x4 y6 w8 h2 level 7, no flush */
                      0x03, 8, 0, 4, 0, 6, 0, 8, 0, 2, 0 };                /* FLUSH same rect */
    uint16_t crc = cfw_crc16(msg, sizeof msg);
    uint8_t s1[] = { 0xF0, 0x10, 7, 0x01, (uint8_t)sizeof msg, 0, (uint8_t)crc, (uint8_t)(crc >> 8), msg[0], msg[1], msg[2], msg[3], msg[4], msg[5], msg[6], msg[7] };
    CHECK(send_packet(s1, sizeof s1) == 0 && last_reply_len == 0);  /* no reply mid-message */
    uint8_t s2[2 + 2 + 8];
    s2[0] = 0xF0; s2[1] = 0x10; s2[2] = 8; s2[3] = 0x00; memcpy(s2 + 4, msg + 8, 8);
    CHECK(send_packet(s2, sizeof s2) == 0 && last_reply_len == 0);
    uint8_t s3[2 + 2 + 8];
    s3[0] = 0xF0; s3[1] = 0x10; s3[2] = 9; s3[3] = 0x02; memcpy(s3 + 4, msg + 16, 8);
    CHECK(send_packet(s3, sizeof s3) == 0 && last_reply_len == 5 && last_reply[2] == CFW_OK && last_reply[3] == 9 && last_reply[4] == 2);
    CHECK(frame[6 * 320 + 2] == 0x77 && frame[6 * 320 + 5] == 0x77 && frame[6 * 320 + 6] == 0 && frame[7 * 320 + 2] == 0x77 && frame[8 * 320 + 2] == 0);
    CHECK(reflash_calls == 1 && last_rect[2] == 4 && last_rect[4] == 12 && last_rect[5] == 7);
    /* Out-of-order sequence is refused and aborts the message. */
    CHECK(send_packet(s1, sizeof s1) == 0 && last_reply_len == 0);
    uint8_t bad_seq[sizeof s2]; memcpy(bad_seq, s2, sizeof s2); bad_seq[2] = 9;
    CHECK(send_packet(bad_seq, sizeof bad_seq) == 0 && last_reply[2] == CFW_BAD_SEQ && last_reply[3] == 9);
    CHECK(send_packet(s3, sizeof s3) == 0 && last_reply[2] == CFW_BAD_SEQ);  /* message no longer active */
    /* Bad CRC is reported on the last packet. */
    uint8_t s1_bad[sizeof s1]; memcpy(s1_bad, s1, sizeof s1); s1_bad[6] ^= 0xFF;
    CHECK(send_packet(s1_bad, sizeof s1_bad) == 0 && last_reply_len == 0);
    CHECK(send_packet(s2, sizeof s2) == 0 && send_packet(s3, sizeof s3) == 0 && last_reply[2] == CFW_BAD_CRC);
    /* Oversized declared length is refused immediately. */
    uint8_t big[] = { 0xF0, 0x10, 1, 0x01, 0x01, 0x20, 0, 0 };
    CHECK(send_packet(big, sizeof big) == 0 && last_reply[2] == CFW_TOO_BIG);
    /* A failing record reports its index. */
    uint8_t msg2[] = { 0x03, 8, 0, 0, 0, 0, 0, 2, 0, 1, 0,   /* FLUSH ok */
                       0x03, 8, 0, 1, 0, 0, 0, 2, 0, 1, 0 }; /* FLUSH odd x: bad rect */
    crc = cfw_crc16(msg2, sizeof msg2);
    uint8_t one[2 + 2 + 4 + sizeof msg2];
    one[0] = 0xF0; one[1] = 0x10; one[2] = 20; one[3] = 0x03; one[4] = sizeof msg2; one[5] = 0; one[6] = (uint8_t)crc; one[7] = (uint8_t)(crc >> 8);
    memcpy(one + 8, msg2, sizeof msg2);
    CHECK(send_packet(one, sizeof one) == 0 && last_reply[2] == CFW_BAD_RECT && last_reply[4] == 1);

    /* RLE 1bpp blit: 16x2 at (0,0) level 0xF. Rows: FF 00 | 00 FF via PackBits:
     * literal 4 bytes = [0x03, FF, 00, 00, FF]. Then a repeat form: 6 rows of 0x00
     * for a 16x6 rect = 12 bytes = [0xF5 (257-245=12), 0x00]. */
    memset(frame, 0, 200 * 320);
    uint8_t rle1[] = { 0xF0, 0x01, 0x01 | 0x04, 0, 0, 0, 0, 16, 0, 2, 0, 0x03, 0xFF, 0x00, 0x00, 0xFF };
    CHECK(send_packet(rle1, sizeof rle1) == 0 && last_reply[2] == CFW_OK);
    CHECK(frame[0] == 0xFF && frame[3] == 0xFF && frame[4] == 0 && frame[7] == 0 && frame[320 + 0] == 0 && frame[320 + 4] == 0xFF && frame[320 + 7] == 0xFF);
    uint8_t rle2[] = { 0xF0, 0x01, 0x01 | 0x04, 0, 0, 0, 0, 16, 0, 6, 0, 0xF5, 0x00 };
    CHECK(send_packet(rle2, sizeof rle2) == 0 && last_reply[2] == CFW_OK && frame[0] == 0 && frame[320 + 4] == 0);
    uint8_t rle_short[] = { 0xF0, 0x01, 0x01 | 0x04, 0, 0, 0, 0, 16, 0, 6, 0, 0xF6, 0x00 };  /* 11 bytes for 12 needed */
    CHECK(send_packet(rle_short, sizeof rle_short) == 0 && last_reply[2] == CFW_BAD_RLE);
    /* 4bpp RLE: 8x1 at (2,0): 4 bytes, repeat 0x5A. */
    uint8_t rle3[] = { 0xF0, 0x01, 0x04, 2, 0, 0, 0, 8, 0, 1, 0, 0xFD, 0x5A };
    CHECK(send_packet(rle3, sizeof rle3) == 0 && last_reply[2] == CFW_OK && frame[1] == 0x5A && frame[4] == 0x5A && frame[5] == 0);

    /* Stats reflect the traffic above. */
    const uint8_t stats[] = { 0xF0, 0x11 };
    CHECK(send_packet(stats, sizeof stats) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 2 && last_reply[5] == 5 && last_reply[7] == 2);

    /* TEXT: clear box, draw "Hi" with font 0 at (32,32)-(608,59), flush. */
    memset(frame, 0xAA, 200 * 320);
    reflash_calls = 0;
    uint8_t textpk[] = { 0xF0, 0x06, 0, 0x03, 32, 0, 32, 0, 0x60, 0x02, 59, 0, 1, 'H', 'i' };
    CHECK(send_packet(textpk, sizeof textpk) == 0 && last_reply[2] == CFW_OK && utf_calls == 1);
    CHECK(strcmp(utf_text, "Hi") == 0 && utf_font == 0 && utf_lines == 1 && utf_box[0] == 32 && utf_box[1] == 32 && utf_box[2] == 608 && utf_box[3] == 59);
    CHECK(rows_set == 1 && rows_arg == panel + FW_PANEL_ROWS && manual_flush_calls == 1);
    CHECK(frame[32 * 320 + 16] == 0 && frame[59 * 320 + 303] == 0 && frame[32 * 320 + 15] == 0xAA && frame[60 * 320 + 16] == 0xAA);
    CHECK(reflash_calls == 1 && last_rect[2] == 32 && last_rect[3] == 32 && last_rect[4] == 608 && last_rect[5] == 59);
    /* Refusals: font 3, odd x, box too low for a 26-row cell, empty textpk, too long. */
    uint8_t text_bad[sizeof textpk]; memcpy(text_bad, textpk, sizeof textpk); text_bad[2] = 3;
    CHECK(send_packet(text_bad, sizeof text_bad) == 0 && last_reply[2] == CFW_BAD_FONT);
    memcpy(text_bad, textpk, sizeof textpk); text_bad[4] = 33;
    CHECK(send_packet(text_bad, sizeof text_bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(text_bad, textpk, sizeof textpk); text_bad[10] = 174;
    CHECK(send_packet(text_bad, sizeof text_bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    memcpy(text_bad, textpk, sizeof textpk); text_bad[10] = 173;
    CHECK(send_packet(text_bad, sizeof text_bad) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(textpk, 13) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    uint8_t text_long[13 + 241]; memcpy(text_long, textpk, 13); memset(text_long + 13, 'x', 241);
    CHECK(send_packet(text_long, sizeof text_long) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    CHECK(utf_calls == 2);
    /* TEXT inside a stream message. */
    uint8_t tmsg[3 + 13];
    tmsg[0] = 0x06; tmsg[1] = 13; tmsg[2] = 0; memcpy(tmsg + 3, textpk + 2, 13);
    crc = cfw_crc16(tmsg, sizeof tmsg);
    uint8_t tpk[8 + sizeof tmsg];
    tpk[0] = 0xF0; tpk[1] = 0x10; tpk[2] = 40; tpk[3] = 0x03; tpk[4] = sizeof tmsg; tpk[5] = 0; tpk[6] = (uint8_t)crc; tpk[7] = (uint8_t)(crc >> 8);
    memcpy(tpk + 8, tmsg, sizeof tmsg);
    CHECK(send_packet(tpk, sizeof tpk) == 0 && last_reply[2] == CFW_OK && last_reply[4] == 1 && utf_calls == 3);

    /* COPY: scroll a 4x3 block up by one row (overlapping), then sideways, then flush. */
    memset(frame, 0, 200 * 320);
    for (int y = 0; y < 3; y++) { frame[(10 + y) * 320 + 5] = (uint8_t)(0x11 * (y + 1)); frame[(10 + y) * 320 + 6] = (uint8_t)(0xA0 + y); }
    uint8_t up[] = { 0xF0, 0x07, 0x00, 10, 0, 10, 0, 4, 0, 3, 0, 10, 0, 9, 0 };   /* src (10,10) 4x3 -> dst (10,9) */
    CHECK(send_packet(up, sizeof up) == 0 && last_reply[2] == CFW_OK);
    CHECK(frame[9 * 320 + 5] == 0x11 && frame[10 * 320 + 5] == 0x22 && frame[11 * 320 + 5] == 0x33 && frame[12 * 320 + 5] == 0x33);
    uint8_t down[] = { 0xF0, 0x07, 0x00, 10, 0, 9, 0, 4, 0, 3, 0, 10, 0, 11, 0 };  /* src (10,9) 4x3 -> dst (10,11): overlap downward */
    CHECK(send_packet(down, sizeof down) == 0 && last_reply[2] == CFW_OK);
    CHECK(frame[11 * 320 + 5] == 0x11 && frame[12 * 320 + 5] == 0x22 && frame[13 * 320 + 5] == 0x33 && frame[13 * 320 + 6] == 0xA2);
    uint8_t right[] = { 0xF0, 0x07, 0x02, 10, 0, 11, 0, 4, 0, 1, 0, 12, 0, 11, 0 }; /* same row, shift right by 2 px, flush */
    reflash_calls = 0;
    CHECK(send_packet(right, sizeof right) == 0 && last_reply[2] == CFW_OK && frame[11 * 320 + 6] == 0x11 && frame[11 * 320 + 7] == 0xA0 && frame[11 * 320 + 5] == 0x11);
    CHECK(reflash_calls == 1 && last_rect[2] == 12 && last_rect[3] == 11 && last_rect[4] == 16 && last_rect[5] == 11);
    uint8_t copy_bad[] = { 0xF0, 0x07, 0x00, 10, 0, 10, 0, 4, 0, 3, 0, 0x7E, 0x02, 9, 0 }; /* dst x 638 + 4 > 640 */
    CHECK(send_packet(copy_bad, sizeof copy_bad) == 0 && last_reply[2] == CFW_BAD_RECT);
    CHECK(send_packet(up, 12) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* Assets: a 16x2 1bpp image uploaded in two PUTs, drawn, then a string asset. */
    memset(frame, 0, 200 * 320);
    const uint8_t no_asset[] = { 0xF0, 0x09, 5, 0, 0x00, 0, 0, 0, 0 };
    CHECK(send_packet(no_asset, sizeof no_asset) == 0 && last_reply[2] == CFW_NO_ASSET && malloc_calls == 0);
    uint8_t put1[] = { 0xF0, 0x08, 5, 0, 0x00, 0x00, 16, 0, 2, 0, 0xFF, 0x00 };          /* id 5, 1bpp 16x2, first 2 bytes */
    CHECK(send_packet(put1, sizeof put1) == 0 && last_reply[2] == CFW_OK && malloc_calls == 3 && malloc_size == 32 * 1024);
    CHECK(send_packet(no_asset, sizeof no_asset) == 0 && last_reply[2] == CFW_BAD_ASSET);  /* incomplete */
    uint8_t put2[] = { 0xF0, 0x08, 5, 0, 0x00, 0x01, 16, 0, 2, 0, 0x00, 0xFF };          /* append remaining 2 bytes */
    CHECK(send_packet(put2, sizeof put2) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(put2, sizeof put2) == 0 && last_reply[2] == CFW_BAD_ASSET);          /* closed: no more appends */
    uint8_t drawa[] = { 0xF0, 0x09, 5, 0, 0x92, 4, 0, 100, 0 };                          /* level 9, flush, at (4,100) */
    reflash_calls = 0;
    CHECK(send_packet(drawa, sizeof drawa) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1);
    CHECK(frame[100 * 320 + 2] == 0x99 && frame[100 * 320 + 5] == 0x99 && frame[100 * 320 + 6] == 0 && frame[101 * 320 + 2] == 0 && frame[101 * 320 + 6] == 0x99);
    CHECK(last_rect[2] == 4 && last_rect[3] == 100 && last_rect[4] == 20 && last_rect[5] == 101);
    uint8_t puts[] = { 0xF0, 0x08, 9, 0, 0x02, 0x00, 0, 0, 0, 0, 'H', 'e', 'y' };        /* string asset id 9 */
    CHECK(send_packet(puts, sizeof puts) == 0 && last_reply[2] == CFW_OK);
    uint8_t atext[] = { 0xF0, 0x0A, 9, 0, 0, 0x02, 32, 0, 32, 0, 0x60, 0x02, 59, 0, 1 };
    utf_calls = 0;
    CHECK(send_packet(atext, sizeof atext) == 0 && last_reply[2] == CFW_OK && utf_calls == 1 && strcmp(utf_text, "Hey") == 0 && utf_box[3] == 59);
    CHECK(send_packet(drawa, 8) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    uint8_t text_as_img[] = { 0xF0, 0x09, 9, 0, 0x00, 0, 0, 0, 0 };
    CHECK(send_packet(text_as_img, sizeof text_as_img) == 0 && last_reply[2] == CFW_BAD_ASSET);
    /* Replacing the newest asset reclaims its bytes; INFO and CLEAR. */
    const uint8_t ainfo[] = { 0xF0, 0x0C };
    CHECK(send_packet(ainfo, sizeof ainfo) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 2 && last_reply[5] == 7 && last_reply[8] == 32 && last_reply[9] == 1);
    uint8_t puts2[] = { 0xF0, 0x08, 9, 0, 0x02, 0x00, 0, 0, 0, 0, 'Y', 'o' };
    CHECK(send_packet(puts2, sizeof puts2) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(ainfo, sizeof ainfo) == 0 && last_reply[3] == 2 && last_reply[5] == 6);
    const uint8_t aclear[] = { 0xF0, 0x0B };
    CHECK(send_packet(aclear, sizeof aclear) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(ainfo, sizeof ainfo) == 0 && last_reply[3] == 0 && last_reply[5] == 0);
    CHECK(send_packet(drawa, sizeof drawa) == 0 && last_reply[2] == CFW_NO_ASSET);
    CHECK(host_arena[0] == 0 && host_arena[sizeof host_arena - 1] == 0);
    uint8_t bad_kind[] = { 0xF0, 0x08, 1, 0, 0x07, 0x00, 2, 0, 1, 0, 0 };
    CHECK(send_packet(bad_kind, sizeof bad_kind) == 0 && last_reply[2] == CFW_BAD_ASSET);

    /* Shapes: filled rect, outlined rect, line, filled circle, ring, triangles; clipping. */
    memset(frame, 0, 200 * 320);
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    uint8_t shapes[2 + 2 + 16 * 7];
    shapes[0] = 0xF0; shapes[1] = 0x0D; shapes[2] = 0x02; shapes[3] = 7;
    int16_t recs[7][10] = {
        { 1, 1, 0x0F, 1, 10, 10, 13, 11, 0, 0 },      /* filled rect (10,10)-(13,11) */
        { 1, 0, 0x0A, 1, 20, 20, 25, 24, 0, 0 },      /* outlined rect */
        { 0, 0, 0x0F, 1, 100, 50, 104, 50, 0, 0 },    /* horizontal line */
        { 2, 1, 0x0F, 1, 300, 100, 2, 0, 0, 0 },      /* filled circle r=2 */
        { 2, 0, 0x0F, 1, 400, 100, 2, 0, 0, 0 },      /* ring r=2 */
        { 3, 1, 0x0F, 1, 500, 150, 510, 150, 505, 160 }, /* filled triangle */
        { 1, 1, 0x0F, 1, -10, 195, 5, 300, 0, 0 },    /* clipped rect to the bottom-left corner */
    };
    for (int i = 0; i < 7; i++) {
        uint8_t *rec = shapes + 4 + 16 * i;
        rec[0] = (uint8_t)recs[i][0]; rec[1] = (uint8_t)recs[i][1]; rec[2] = (uint8_t)recs[i][2]; rec[3] = (uint8_t)recs[i][3];
        for (int k = 0; k < 6; k++) { rec[4 + 2 * k] = (uint8_t)recs[i][4 + k]; rec[5 + 2 * k] = (uint8_t)(recs[i][4 + k] >> 8); }
    }
    reflash_calls = 0;
    CHECK(send_packet(shapes, sizeof shapes) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1 && last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(frame[10 * 320 + 5] == 0xFF && frame[10 * 320 + 6] == 0xFF && frame[11 * 320 + 6] == 0xFF && frame[10 * 320 + 7] == 0 && frame[12 * 320 + 5] == 0);
    CHECK(frame[20 * 320 + 10] == 0xAA && frame[22 * 320 + 10] == 0xA0 && frame[22 * 320 + 11] == 0 && frame[22 * 320 + 12] == 0x0A && frame[24 * 320 + 11] == 0xAA);
    CHECK(frame[50 * 320 + 50] == 0xFF && frame[50 * 320 + 52] == 0xF0 && frame[50 * 320 + 49] == 0 && frame[49 * 320 + 50] == 0);
    CHECK(frame[100 * 320 + 150] == 0xFF && frame[98 * 320 + 150] == 0xF0 && frame[98 * 320 + 149] == 0 && frame[102 * 320 + 150] == 0xF0);
    CHECK(frame[100 * 320 + 200] == 0 && frame[100 * 320 + 199] == 0xF0 && frame[100 * 320 + 201] == 0xF0 && frame[98 * 320 + 200] == 0xF0 && frame[99 * 320 + 199] == 0x0F);
    CHECK(frame[150 * 320 + 250] == 0xFF && frame[150 * 320 + 255] == 0xF0 && frame[160 * 320 + 252] == 0x0F && frame[160 * 320 + 250] == 0 && frame[161 * 320 + 252] == 0);
    CHECK(frame[195 * 320 + 0] == 0xFF && frame[199 * 320 + 2] == 0xFF && frame[199 * 320 + 3] == 0 && frame[194 * 320 + 0] == 0);
    guards_intact();
    uint8_t shapes_bad[] = { 0xF0, 0x0D, 0x00, 1, 9, 0, 0, 0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0 };
    CHECK(send_packet(shapes_bad, sizeof shapes_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    CHECK(send_packet(shapes, 10) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* PEEK returns up to 16 bytes. */
    for (int i = 0; i < 64; i++) host_peek_mem[i] = (uint8_t)i;
    uint8_t peek[] = { 0xF0, 0x12, 0x08, 0, 0, 0x20, 32 };
    CHECK(send_packet(peek, sizeof peek) == 0 && last_reply[2] == CFW_OK && last_reply_len == 19 && last_reply[3] == 8 && last_reply[18] == 23);
    CHECK(send_packet(peek, 6) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* Retained objects: a rect, an image (asset 5 from above, re-put), a text, shown in order. */
    uint8_t put5[] = { 0xF0, 0x08, 5, 0, 0x00, 0x00, 16, 0, 2, 0, 0xFF, 0x00, 0x00, 0xFF };
    CHECK(send_packet(put5, sizeof put5) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(puts2, sizeof puts2) == 0 && last_reply[2] == CFW_OK);  /* string asset 9 = "Yo" */
    uint8_t o1[] = { 0xF0, 0x20, 1, 0, 1, 0, 2, 0x01, 10, 0, 10, 0, 8, 0, 4, 0, 0, 0, 0x0C, 0, 0 };  /* rect 8x4 at (10,10) level C */
    uint8_t o2[] = { 0xF0, 0x20, 2, 0, 1, 0, 0, 0x01, 20, 0, 50, 0, 0, 0, 0, 0, 5, 0, 0x0F, 0, 0 };  /* image asset 5 at (20,50) */
    uint8_t o3[] = { 0xF0, 0x20, 3, 0, 7, 0, 1, 0x01, 32, 0, 32, 0, 0x40, 0x02, 28, 0, 9, 0, 0, 0, 1 }; /* text asset 9 box 576x28 */
    CHECK(send_packet(o1, sizeof o1) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(o2, sizeof o2) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(o3, sizeof o3) == 0 && last_reply[2] == CFW_OK);
    CHECK(memcmp(host_ram2 + 32, "OBJ2", 4) == 0 && host_ram2[0] == 0 && host_ram2[sizeof host_ram2 - 1] == 0);
    memset(frame, 0x33, 200 * 320);
    utf_calls = 0; reflash_calls = 0;
    uint8_t show[] = { 0xF0, 0x21, 0x03, 3, 1, 0, 2, 0, 3, 0 };
    CHECK(send_packet(show, sizeof show) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1 && last_rect[4] == 640);
    CHECK(frame[0] == 0 && frame[10 * 320 + 5] == 0xCC && frame[13 * 320 + 8] == 0xCC && frame[14 * 320 + 5] == 0);
    CHECK(frame[50 * 320 + 10] == 0xFF && frame[50 * 320 + 14] == 0 && frame[51 * 320 + 14] == 0xFF);
    CHECK(utf_calls == 1 && strcmp(utf_text, "Yo") == 0 && utf_box[0] == 32 && utf_box[2] == 608 && utf_box[3] == 59);
    const uint8_t ostate[] = { 0xF0, 0x23 };
    CHECK(send_packet(ostate, sizeof ostate) == 0 && last_reply[2] == CFW_OK && last_reply[5] == 3 && last_reply[6] == 3 && last_reply[7] == 1 && last_reply[9] == 1 && last_reply[11] == 2 && last_reply[15] == 3 && last_reply[17] == 7);
    /* Unknown id refused before anything is drawn; hide blanks; control drops one object. */
    uint8_t show_bad[] = { 0xF0, 0x21, 0x01, 1, 9, 9 };
    memset(frame, 0x33, 200 * 320);
    CHECK(send_packet(show_bad, sizeof show_bad) == 0 && last_reply[2] == CFW_NO_OBJECT && frame[0] == 0x33);
    const uint8_t hide[] = { 0xF0, 0x22 };
    CHECK(send_packet(hide, sizeof hide) == 0 && last_reply[2] == CFW_OK && frame[0] == 0 && frame[64000 - 1] == 0 && reflash_calls == 2);
    uint8_t drop[] = { 0xF0, 0x24, 1, 2, 0 };
    CHECK(send_packet(drop, sizeof drop) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(ostate, sizeof ostate) == 0 && last_reply[5] == 2 && last_reply[6] == 0);
    uint8_t show2[] = { 0xF0, 0x21, 0x00, 2, 1, 0, 3, 0 };
    CHECK(send_packet(show2, sizeof show2) == 0 && last_reply[2] == CFW_OK && reflash_calls == 2);
    CHECK(send_packet(show, sizeof show) == 0 && last_reply[2] == CFW_NO_OBJECT);
    uint8_t clear_all[] = { 0xF0, 0x24, 0 };
    CHECK(send_packet(clear_all, sizeof clear_all) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(ostate, sizeof ostate) == 0 && last_reply[5] == 0);
    CHECK(send_packet(o1, 20) == 0 && last_reply[2] == CFW_BAD_LENGTH);

    /* Animation: glide a rect object from (10,10) to (110,50) in 4 frames, driven by display ticks. */
    CHECK(send_packet(o1, sizeof o1) == 0 && last_reply[2] == CFW_OK);   /* rect id 1 at (10,10) */
    uint8_t show1[] = { 0xF0, 0x21, 0x03, 1, 1, 0 };
    CHECK(send_packet(show1, sizeof show1) == 0 && last_reply[2] == CFW_OK);
    post_calls = 0; sleep_calls = 0; reflash_calls = 0;
    uint8_t anim[] = { 0xF0, 0x25, 40, 1, 1, 0, 110, 0, 50, 0, 4, 0 };
    CHECK(send_packet(anim, sizeof anim) == 0 && last_reply[2] == CFW_OK && post_calls == 1 && last_screen == 0x10);
    cfw_display_tick(state, 0x10, 0, 2);   /* frame 1: linear 1/4 of the way */
    CHECK(reflash_calls == 1 && sleep_calls == 1 && last_sleep == 40 && post_calls == 2);
    /* Only the union of the old (10,10 8x4) and new (35,20 8x4) boxes was pushed. */
    CHECK(last_rect[2] == 10 && last_rect[3] == 10 && last_rect[4] == 44 && last_rect[5] == 23);
    CHECK(frame[20 * 320 + 17] == 0x0C && frame[20 * 320 + 18] == 0xCC && frame[20 * 320 + 16] == 0 && frame[23 * 320 + 21] == 0xC0 && frame[24 * 320 + 17] == 0);
    cfw_display_tick(state, 0x10, 0, 2);
    cfw_display_tick(state, 0x10, 0, 2);
    cfw_display_tick(state, 0x10, 0, 2);   /* frame 4: arrived, no further post */
    CHECK(reflash_calls == 4 && sleep_calls == 3 && post_calls == 4);
    CHECK(frame[50 * 320 + 55] == 0xCC && frame[53 * 320 + 58] == 0xCC && frame[10 * 320 + 5] == 0);
    cfw_display_tick(state, 0x10, 0, 2);   /* idle tick: nothing happens */
    CHECK(reflash_calls == 4 && post_calls == 4);
    cfw_display_tick(state, 0x06, 0, 2);   /* stock screen id: ignored */
    CHECK(reflash_calls == 4);
    uint8_t anim_bad[] = { 0xF0, 0x25, 40, 1, 9, 9, 0, 0, 0, 0, 4, 0 };
    CHECK(send_packet(anim_bad, sizeof anim_bad) == 0 && last_reply[2] == CFW_NO_OBJECT);
    CHECK(send_packet(anim, 9) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    /* Easing end points land exactly on the target. */
    uint8_t anim2[] = { 0xF0, 0x25, 0, 1, 1, 0, 10, 0, 10, 0, 3, 1 };
    CHECK(send_packet(anim2, sizeof anim2) == 0 && last_reply[2] == CFW_OK && last_sleep == 40);
    for (int i = 0; i < 3; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(frame[10 * 320 + 5] == 0xCC && frame[13 * 320 + 8] == 0xCC && frame[14 * 320 + 5] == 0 && frame[50 * 320 + 55] == 0);
    CHECK(host_ram2[0] == 0 && host_ram2[sizeof host_ram2 - 1] == 0);
    const uint8_t astate[] = { 0xF0, 0x26 };
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 40 && last_reply[5] == 7 && last_reply[9] == 0);
    /* A shapes object has no box, so its glide forces a full-frame push. */
    uint8_t shp_asset[] = { 0xF0, 0x08, 11, 0, 0x02, 0x00, 0, 0, 0, 0, 1, 0, 1, 0x0F, 1, 0, 0, 0, 0, 5, 0, 5, 0, 0, 0, 0, 0 };
    CHECK(send_packet(shp_asset, sizeof shp_asset) == 0 && last_reply[2] == CFW_OK);
    uint8_t o5[] = { 0xF0, 0x20, 5, 0, 1, 0, 3, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0x0F, 0, 0 };
    CHECK(send_packet(o5, sizeof o5) == 0 && last_reply[2] == CFW_OK);
    uint8_t show5[] = { 0xF0, 0x21, 0x01, 1, 5, 0 };
    CHECK(send_packet(show5, sizeof show5) == 0 && last_reply[2] == CFW_OK);
    uint8_t anim5[] = { 0xF0, 0x25, 0, 1, 5, 0, 0, 0, 0, 0, 1, 0 };
    CHECK(send_packet(anim5, sizeof anim5) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[5] == 8 && last_reply[9] == 1);

    /* Touch gestures: the redirected stock helper hands codes to the stock
     * semaphore by default, reports them when asked, and can swallow them. */
    uint8_t in_state[] = { 0xF0, 0x31 };
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply_len == 14);
    host_touch_code = 2;
    last_reply_len = 0;
    cfw_touch_event();                      /* semaphore not initialised yet: nothing given, nothing sent */
    CHECK(host_sem_gives == 0 && last_reply_len == 0);
    host_key_sem_ready = 1;
    cfw_touch_event();
    CHECK(host_sem_gives == 1 && host_last_sem == (void *)host_key_sem && last_reply_len == 0);
    uint8_t in_notify[] = { 0xF0, 0x30, 0x01 };
    CHECK(send_packet(in_notify, sizeof in_notify) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0x01);
    host_touch_code = 1;
    cfw_touch_event();
    CHECK(last_reply_len == 7 && last_reply[1] == 0x32 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 1 && last_reply[5] == 3 && last_reply[6] == 0);
    CHECK(host_sem_gives == 2);
    uint8_t in_grab[] = { 0xF0, 0x30, 0x03 };
    CHECK(send_packet(in_grab, sizeof in_grab) == 0 && last_reply[2] == CFW_OK);
    host_touch_code = 4;
    cfw_touch_event();
    CHECK(host_sem_gives == 2 && last_reply[3] == 0 && last_reply[4] == 4 && last_reply[5] == 4);
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[3] == 0x03 && last_reply[4] == 4 && last_reply[5] == 4 && last_reply[7] == 2);
    uint8_t in_bad[] = { 0xF0, 0x30, 0x10 };
    CHECK(send_packet(in_bad, sizeof in_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t in_short[] = { 0xF0, 0x30 };
    CHECK(send_packet(in_short, sizeof in_short) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[3] == 0x03);   /* rejected controls change nothing */
    uint8_t in_release[] = { 0xF0, 0x30, 0x00 };
    CHECK(send_packet(in_release, sizeof in_release) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0);
    CHECK((uint8_t *)cfw_input_state() + sizeof(cfw_input) <= FW_CFW_RAM2 + FW_CFW_RAM2_SIZE);

    /* Stock events: the redirected poster keeps the stock behaviour (direct
     * F5 packet or semaphore), reports when asked, and can swallow. */
    memset(ctx, 0, sizeof ctx);
    host_enqueues = 0; host_sem_gives = 0; sleep_calls = 0;
    cfw_stock_event(6, 0, 0);               /* worn; link not ready: semaphore path */
    CHECK(host_sem_gives == 1 && host_last_sem == ctx + 0x218 && host_enqueues == 0 && sleep_calls == 1 && last_sleep == 1);
    CHECK(host_event_pending == 6);
    ctx[0x248] = 1;
    host_event_pending = 0xFF;
    cfw_stock_event(2, 0, 0);               /* look up; link ready: direct packet */
    CHECK(host_enqueues == 1 && host_enqueued_len == 3 && host_enqueued[0] == 0xF5 && host_enqueued[1] == 2 && host_enqueued[2] == 0xCB);
    CHECK(host_event_pending == 0xFF && host_sem_gives == 1);
    host_event_pending = 0x05;
    cfw_stock_event(0xF1, 0, 0);            /* special range with another event pending: dropped */
    CHECK(host_enqueues == 1 && host_event_pending == 0x05);
    host_event_pending = 0xFF;
    uint8_t in_events[] = { 0xF0, 0x30, 0x04 };
    CHECK(send_packet(in_events, sizeof in_events) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0x04);
    cfw_stock_event(3, 0, 0);               /* bow head: reported and still sent */
    CHECK(last_reply_len == 7 && last_reply[1] == 0x32 && last_reply[3] == 1 && last_reply[4] == 3 && last_reply[5] == 4 && last_reply[6] == 0);
    CHECK(host_enqueues == 2 && host_enqueued[1] == 3);
    uint8_t in_grab_events[] = { 0xF0, 0x30, 0x0C };
    CHECK(send_packet(in_grab_events, sizeof in_grab_events) == 0 && last_reply[2] == CFW_OK);
    cfw_stock_event(7, 0, 0);               /* taken off: reported, swallowed */
    CHECK(last_reply[3] == 1 && last_reply[4] == 7 && host_enqueues == 2 && host_sem_gives == 1);
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[3] == 0x0C && last_reply[9] == 7 && last_reply[10] == 5 && last_reply[12] == 3);
    CHECK(send_packet(in_release, sizeof in_release) == 0 && last_reply[2] == CFW_OK);

    /* View offsets: added to the stock origins at flush time. */
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_COL_OFFSET) = 0;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_ROW_OFFSET) = 210;
    state[FW_PANEL_OFFSET + FW_PANEL_HEIGHT_GEAR] = 2;
    state[FW_PANEL_OFFSET + FW_PANEL_DEPTH_GEAR] = 3;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;
    uint8_t view_get[] = { 0xF0, 0x14, 0x01 };
    CHECK(send_packet(view_get, sizeof view_get) == 0 && last_reply[2] == CFW_OK && last_reply_len == 13);
    CHECK(last_reply[3] == 0 && last_reply[5] == 0 && last_reply[7] == 0 && last_reply[9] == 210 && last_reply[11] == 2 && last_reply[12] == 3);
    reflash_calls = 0;
    uint8_t view_set[] = { 0xF0, 0x14, 0x02, 0xF8, 0xFF, 0x23, 0x00 };   /* dx -8, dy +35, push */
    CHECK(send_packet(view_set, sizeof view_set) == 0 && last_reply[2] == CFW_OK);
    CHECK(reflash_calls == 1 && last_rect[0] == 0 && last_rect[1] == 245 && last_rect[2] == 0 && last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(last_reply[3] == 0xF8 && last_reply[4] == 0xFF && last_reply[5] == 0x23 && last_reply[9] == 210);
    uint8_t view_far[] = { 0xF0, 0x14, 0x00, 0x00, 0x00, 0xE8, 0x03 };   /* dy 1000: out of range, no change */
    CHECK(send_packet(view_far, sizeof view_far) == 0 && last_reply[2] == CFW_BAD_RECT);
    CHECK(send_packet(view_get, sizeof view_get) == 0 && last_reply[3] == 0xF8 && last_reply[5] == 0x23);
    uint8_t view_short[] = { 0xF0, 0x14, 0x00, 0x01 };
    CHECK(send_packet(view_short, sizeof view_short) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    uint8_t view_zero[] = { 0xF0, 0x14, 0x00, 0, 0, 0, 0 };
    CHECK(send_packet(view_zero, sizeof view_zero) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1);
    uint8_t flush_all[] = { 0xF0, 0x03, 0, 0, 0, 0, 0x80, 0x02, 0xC8, 0x00 };
    CHECK(send_packet(flush_all, sizeof flush_all) == 0 && last_reply[2] == CFW_OK && last_rect[0] == 0 && last_rect[1] == 210);

    /* Depth shift: content moves inside the frame during the push and comes back. */
    memset(frame, 0, 200 * 320);
    frame[0 * 320 + 50] = 0xF0;       /* canvas pixel (100, 0) */
    frame[3 * 320 + 319] = 0x0F;      /* canvas pixel (639, 3) at the right edge */
    frame[5 * 320 + 0] = 0xF0;        /* canvas pixel (0, 5) at the left edge */
    uint8_t snapshot[200 * 320];
    memcpy(snapshot, frame, sizeof snapshot);
    uint8_t view_dx[] = { 0xF0, 0x14, 0x00, 8, 0, 0, 0 };     /* dx +8 (right lens, near gear) */
    CHECK(send_packet(view_dx, sizeof view_dx) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(flush_all, sizeof flush_all) == 0 && last_reply[2] == CFW_OK);
    CHECK(last_rect[0] == 0 && last_rect[2] == 0 && last_rect[4] == 640);
    CHECK(pushed_row[54] == 0xF0 && pushed_row[50] == 0 && pushed_row[0] == 0 && pushed_row[3] == 0);
    CHECK(memcmp(frame, snapshot, sizeof snapshot) == 0);
    uint8_t flush_band[] = { 0xF0, 0x03, 0x60, 0x00, 0x03, 0x00, 0x40, 0x00, 0x01, 0x00 };   /* rect 96,3 64x1: interior */
    CHECK(send_packet(flush_band, sizeof flush_band) == 0 && last_reply[2] == CFW_OK);
    CHECK(last_rect[2] == 104 && last_rect[4] == 168 && last_rect[3] == 3 && last_rect[5] == 3);
    CHECK(memcmp(frame, snapshot, sizeof snapshot) == 0);
    uint8_t view_dxn[] = { 0xF0, 0x14, 0x00, 0xF4, 0xFF, 0, 0 };   /* dx -12 (left lens, far gear) */
    CHECK(send_packet(view_dxn, sizeof view_dxn) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(flush_all, sizeof flush_all) == 0 && last_reply[2] == CFW_OK && last_rect[2] == 0 && last_rect[4] == 640);
    CHECK(pushed_row[44] == 0xF0 && pushed_row[50] == 0);
    CHECK(memcmp(frame, snapshot, sizeof snapshot) == 0);
    uint8_t view_odd[] = { 0xF0, 0x14, 0x00, 7, 0, 0, 0 };
    CHECK(send_packet(view_odd, sizeof view_odd) == 0 && last_reply[2] == CFW_BAD_RECT);
    uint8_t view_big[] = { 0xF0, 0x14, 0x00, 26, 0, 0, 0 };
    CHECK(send_packet(view_big, sizeof view_big) == 0 && last_reply[2] == CFW_BAD_RECT);
    CHECK(send_packet(view_zero, sizeof view_zero) == 0 && last_reply[2] == CFW_OK);
    for (int i = 0; i < 32; i++) { CHECK(host_ram3[i] == 0); CHECK(host_ram3[sizeof host_ram3 - 1 - i] == 0); }

    /* IMU streaming rides the display-thread tick. */
    host_imu[0] = 16623; host_imu[1] = -32906; host_imu[2] = 1325;
    post_calls = 0; sleep_calls = 0;
    uint8_t imu_go[] = { 0xF0, 0x33, 50, 3, 0 };
    CHECK(send_packet(imu_go, sizeof imu_go) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 1 && last_reply[4] == 50 && last_reply[5] == 3);
    CHECK(post_calls == 1 && last_screen == 0x10);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_reply_len == 17 && last_reply[1] == 0x34 && last_reply[3] == 0 && last_reply[4] == 0);
    CHECK(*(int32_t *)(last_reply + 5) == 16623 && *(int32_t *)(last_reply + 9) == -32906 && *(int32_t *)(last_reply + 13) == 1325);
    CHECK(sleep_calls == 1 && last_sleep == 50 && post_calls == 2);
    host_imu[2] = -7190;
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_reply[3] == 1 && *(int32_t *)(last_reply + 13) == -7190 && post_calls == 3);
    cfw_display_tick(state, 0x10, 0, 2);          /* third and last sample: no further post */
    CHECK(last_reply[3] == 2 && post_calls == 3 && sleep_calls == 2);
    last_reply_len = 0;
    cfw_display_tick(state, 0x10, 0, 2);          /* idle */
    CHECK(last_reply_len == 0 && post_calls == 3);
    uint8_t imu_forever[] = { 0xF0, 0x33, 10, 0xFF, 0xFF };
    CHECK(send_packet(imu_forever, sizeof imu_forever) == 0 && last_reply[2] == CFW_OK && post_calls == 4);
    for (int i = 0; i < 5; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_reply[1] == 0x34 && last_reply[3] == 7 && post_calls == 9 && last_sleep == 10);
    uint8_t imu_stop[] = { 0xF0, 0x33, 0, 0, 0 };
    CHECK(send_packet(imu_stop, sizeof imu_stop) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && *(uint32_t *)(last_reply + 7) == 8);
    last_reply_len = 0;
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_reply_len == 0 && post_calls == 9);
    uint8_t imu_fast[] = { 0xF0, 0x33, 5, 1, 0 };
    CHECK(send_packet(imu_fast, sizeof imu_fast) == 0 && last_reply[2] == CFW_BAD_RECORD);
    CHECK((uint8_t *)cfw_imu_state() + sizeof(cfw_imu) <= FW_CFW_RAM2 + FW_CFW_RAM2_SIZE);

    /* Microphone control arms the stock capture session and drives its mode byte. */
    host_sem_gives = 0;
    uint8_t mic_q[] = { 0xF0, 0x40, 0x02 };
    CHECK(send_packet(mic_q, sizeof mic_q) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply_len == 10 && last_reply[8] == 1 && last_reply[9] == 0);
    uint8_t mic_on[] = { 0xF0, 0x40, 0x01 };
    CHECK(send_packet(mic_on, sizeof mic_on) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 1 && last_reply[4] == 1);
    CHECK(host_mic_mode == 0 && host_mic_to_flash == 0 && (state[0x1065] & 4) && host_sem_gives == 1 && host_last_sem == state + 0x68);
    CHECK(last_reply[8] == 0 && last_reply[9] == 4);
    CHECK(send_packet(mic_on, sizeof mic_on) == 0 && host_sem_gives == 1 && last_reply[4] == 2);   /* already armed: no second give */
    uint8_t mic_off[] = { 0xF0, 0x40, 0x00 };
    CHECK(send_packet(mic_off, sizeof mic_off) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && host_mic_mode == 1 && last_reply[6] == 1);
    state[0x1065] &= (uint8_t)~4;   /* the main thread clears the bit when the session ends */
    CHECK(send_packet(mic_on, sizeof mic_on) == 0 && host_sem_gives == 2 && host_mic_mode == 0);
    uint8_t mic_bad[] = { 0xF0, 0x40, 0x03 };
    CHECK(send_packet(mic_bad, sizeof mic_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    CHECK((uint8_t *)cfw_mic_state() + sizeof(cfw_mic) <= FW_CFW_RAM2 + FW_CFW_RAM2_SIZE);

    guards_intact();
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures != 0;
}
