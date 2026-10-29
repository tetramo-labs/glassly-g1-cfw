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
static uint8_t ctx[0x20];
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
static int host_reflash(int col, int row, int x0, int y0, int x1, int y1)
{
    reflash_calls++;
    int r[6] = { col, row, x0, y0, x1, y1 };
    memcpy(last_rect, r, sizeof r);
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
    CHECK(memcmp(last_reply + 3, "G1CFW/11", 9) == 0 && last_reply[12] == 1 && last_reply[13] == 5 && last_reply[14] == 60);

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

    guards_intact();
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures != 0;
}
