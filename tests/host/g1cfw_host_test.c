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
static uint32_t host_wear_word;
#define FW_WEAR_WORD() host_wear_word
#define FW_REFLASH_FB host_reflash
static int host_lcd_depth, host_unlocked_pushes;   /* every push must hold the LCD mutex */
static int host_lcd_lock(int64_t timeout) { (void)timeout; host_lcd_depth++; return 0; }
static int host_lcd_unlock(void) { host_lcd_depth--; return 0; }
#define FW_LCD_LOCK host_lcd_lock
#define FW_LCD_UNLOCK host_lcd_unlock
#define FW_SEND_OF(ctx) host_send
#define FW_SYSTEM_SUSPEND host_suspend
#define FW_SYSTEM_RESUME host_resume
static int host_panel_resume(uint8_t *panel);
static int host_panel_suspend(uint8_t *panel);
#define FW_PANEL_CB_OF(panel, slot) ((slot) == 0x10 ? host_panel_on : (slot) == 0x0c ? host_panel_off : \
                                     (slot) == 0x08 ? host_panel_resume : host_panel_suspend)
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
static int brightness_writes, last_brightness;
static int host_write_brightness(uint32_t level) { brightness_writes++; last_brightness = (int)level; return 0; }
#define FW_PANEL_WRITE_BRIGHTNESS host_write_brightness
static int host_jbd_resume(uint8_t *panel);
static int host_jbd_suspend(uint8_t *panel);
#define FW_JBD_PANEL_RESUME host_jbd_resume
#define FW_JBD_PANEL_SUSPEND host_jbd_suspend
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
static int margin_rows, margin_dirty, margin_max_row;   /* single-row pushes of frame row 0 */
static int host_reflash(int col, int row, int x0, int y0, int x1, int y1)
{
    reflash_calls++;
    if (host_lcd_depth <= 0) host_unlocked_pushes++;
    int r[6] = { col, row, x0, y0, x1, y1 };
    memcpy(last_rect, r, sizeof r);
    if (host_frame) memcpy(pushed_row, host_frame, sizeof pushed_row);
    if (y0 == 0 && y1 == 0 && x0 == 0 && x1 == 640 && host_frame) {
        margin_rows++;
        if (row > margin_max_row) margin_max_row = row;
        for (int i = 0; i < 320; i++) if (host_frame[i]) margin_dirty = 1;
    }
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
static int resume_calls, suspend_calls, resume_refuses;
static int host_panel_resume(uint8_t *panel) { panel_cb_calls++; resume_calls++; if (resume_refuses) return -1; *(int *)(panel + FW_PANEL_POWERED) = 1; return 0; }
static int host_panel_suspend(uint8_t *panel) { panel_cb_calls++; suspend_calls++; *(int *)(panel + FW_PANEL_POWERED) = 0; return 0; }
/* The driver pair neither reads nor writes the powered flag; the CFW keeps it. */
static int jbd_resume_calls, jbd_suspend_calls;
static int host_jbd_resume(uint8_t *panel) { (void)panel; panel_cb_calls++; jbd_resume_calls++; return 0; }
static int host_jbd_suspend(uint8_t *panel) { (void)panel; panel_cb_calls++; jbd_suspend_calls++; return 0; }

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

static int32_t rd32le(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Grey nibble of canvas pixel (x, y). */
static int px(int x, int y)
{
    uint8_t b = frame[y * 320 + x / 2];
    return (x & 1) ? (b & 0xF) : (b >> 4);
}

static int count_level(int x0, int y0, int x1, int y1, int level)
{
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (px(x, y) == level) n++;
    return n;
}

static void write_pgm(const char *dir, int index, const char *label)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%03d-%s.pgm", dir, index, label);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P5\n640 200\n255\n");
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 640; x++) {
            fputc(px(x, y) * 17, f);
        }
    }
    fclose(f);
}

/* Replay a tools/shapes_suite.py --dump stream: [1][len:u16][message] goes out
 * as STREAM packets and must be accepted; [2][ms:u32] runs the display thread
 * for that long while tweens are in flight (and writes a PGM of the frame when
 * `dir` is given); [3][len][label] names the following snapshots; [4][x:u16]
 * [y:u16][level] probes one pixel; [5] writes a snapshot. */
static int replay(const char *path, const char *dir)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 1;
    }
    fclose(f);
    char label[300] = "start";
    int shots = 0, msgs = 0, errors = 0;
    uint8_t seq = 0;
    uint16_t render_errors = cfw_animation()->render_errors;
    for (long pos = 0; pos < n;) {
        uint8_t tag = d[pos++];
        if (tag == 1 && pos + 2 <= n) {
            uint32_t len = (uint32_t)(d[pos] | d[pos + 1] << 8);
            pos += 2;
            if (pos + (long)len > n) {
                errors++;
                break;
            }
            const uint8_t *m = d + pos;
            pos += len;
            uint16_t crc = cfw_crc16(m, len);
            uint32_t sent = 0;
            int first = 1;
            while (first || sent < len) {
                uint8_t pkt[255];
                uint32_t k = 4;
                pkt[0] = 0xF0; pkt[1] = 0x10; pkt[2] = seq++;
                if (first) {
                    pkt[4] = (uint8_t)len; pkt[5] = (uint8_t)(len >> 8); pkt[6] = (uint8_t)crc; pkt[7] = (uint8_t)(crc >> 8);
                    k = 8;
                }
                uint32_t take = len - sent < sizeof pkt - k ? len - sent : (uint32_t)(sizeof pkt - k);
                memcpy(pkt + k, m + sent, take);
                sent += take;
                k += take;
                pkt[3] = (uint8_t)((first ? 1 : 0) | (sent >= len ? 2 : 0));
                first = 0;
                send_packet(pkt, k);
            }
            msgs++;
            if (last_reply_len < 5 || last_reply[1] != 0x10 || (last_reply[2] != CFW_OK && last_reply[2] != CFW_PANEL_OFF)) {
                printf("replay: message %d (%s, %u bytes) refused: status %d after %d records\n", msgs, label, len, last_reply[2], last_reply[4]);
                errors++;
            }
            if (cfw_animation()->render_errors != render_errors) {
                printf("replay: message %d (%s) left %u object renders failing while presenting\n", msgs, label,
                       cfw_animation()->render_errors - render_errors);
                render_errors = cfw_animation()->render_errors;
                errors++;
            }
        } else if (tag == 2 && pos + 4 <= n) {
            uint32_t ms = (uint32_t)rd32le(d + pos);
            pos += 4;
            cfw_anims *a = cfw_animation();
            uint32_t period = a->period_ms ? a->period_ms : 20;
            for (uint32_t t = 0; t <= ms / period && a->active; t++) {
                cfw_display_tick(state, 0x10, 0, 2);
            }
            if (a->render_errors != render_errors) {
                /* A tween frame that an object refused to draw: on the glasses it flickers,
                 * and a SHOW arriving mid-tween is refused with the same status. */
                printf("replay: %u object renders failed during the %u ms wait after %s\n",
                       a->render_errors - render_errors, ms, label);
                render_errors = a->render_errors;
                errors++;
            }
            if (dir) {
                write_pgm(dir, shots, label);
            }
            shots++;
        } else if (tag == 4 && pos + 5 <= n) {
            /* pixel probe: [x:u16][y:u16][level], checked against the frame as composed */
            int x = d[pos] | d[pos + 1] << 8, y = d[pos + 2] | d[pos + 3] << 8, want = d[pos + 4];
            pos += 5;
            if (x < 0 || x >= 640 || y < 0 || y >= 200 || px(x, y) != want) {
                printf("replay: probe (%d,%d) in %s is %d, expected %d\n", x, y, label, (x >= 0 && x < 640 && y >= 0 && y < 200) ? px(x, y) : -1, want);
                errors++;
            }
        } else if (tag == 5) {
            if (dir) {
                write_pgm(dir, shots, label);
            }
            shots++;
        } else if (tag == 3 && pos + 1 <= n) {
            uint32_t len = d[pos++];
            memcpy(label, d + pos, len);
            label[len] = 0;
            for (uint32_t i = 0; i < len; i++) if (label[i] == '/' || label[i] == ' ') label[i] = '_';
            pos += len;
        } else {
            printf("replay: bad tag %d at offset %ld\n", tag, pos - 1);
            errors++;
            break;
        }
    }
    free(d);
    guards_intact();
    printf("replay: %d messages, %d snapshots, %d errors%s\n", msgs, shots, errors, failures ? ", guard failures" : "");
    return (errors || failures) ? 1 : 0;
}

int main(int argc, char **argv)
{
    host_frame = frame = frame_store + GUARD + 5;
    memset(frame_store, 0xA5, sizeof frame_store);
    memset(frame, 0, 200 * 320);
    uint8_t *panel = state + FW_PANEL_OFFSET;
    *(uint32_t *)(panel + FW_PANEL_FRAME) = 0x20012340u;
    *(int *)(panel + FW_PANEL_COL_OFFSET) = 7;
    *(int *)(panel + FW_PANEL_ROW_OFFSET) = 35;
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    if (argc >= 3 && strcmp(argv[1], "--replay") == 0) {
        return replay(argv[2], argc > 3 ? argv[3] : NULL);
    }

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
    CHECK(memcmp(last_reply + 3, "G1CFW/45", 9) == 0 && last_reply[12] == 1 && last_reply[13] == 5 && last_reply[14] == 60);

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
    /* The modes that bring the panel up blank the 280 rows outside the
     * canvas (the panel memory powers up lit) and push the frame. */
    uint8_t panel_off_cmd[] = { 0xF0, 0x05, 0x00 };
    margin_rows = 0;
    CHECK(send_packet(panel_off_cmd, sizeof panel_off_cmd) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 1 && last_reply[4] == 0);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_PANEL_OFF && margin_rows == 0);
    uint8_t panel_on_cmd[] = { 0xF0, 0x05, 0x01 };
    reflash_calls = 0;
    CHECK(send_packet(panel_on_cmd, sizeof panel_on_cmd) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 1);
    CHECK(margin_rows == 280 && reflash_calls == 281 && last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_OK);
    uint8_t panel_force_off[] = { 0xF0, 0x05, 0x03 };
    CHECK(send_packet(panel_force_off, sizeof panel_force_off) == 0 && last_reply[2] == CFW_OK && last_reply[4] == 0 && panel_cb_calls == 1);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_PANEL_OFF);
    CHECK(margin_rows == 280);
    uint8_t panel_force_on[] = { 0xF0, 0x05, 0x02 };
    CHECK(send_packet(panel_force_on, sizeof panel_force_on) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 1 && panel_cb_calls == 2);
    CHECK(margin_rows == 560);
    CHECK(send_packet(flush, sizeof flush) == 0 && last_reply[2] == CFW_OK);
    uint8_t panel_bad[] = { 0xF0, 0x05, 0x09 };
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
    /* With the panel off the records still run into the frame buffer and
     * the message reports panel-off with every record done. */
    *(int *)(panel + FW_PANEL_POWERED) = 0;
    memset(frame, 0, 200 * 320);
    CHECK(send_packet(s1, sizeof s1) == 0 && send_packet(s2, sizeof s2) == 0 && send_packet(s3, sizeof s3) == 0);
    CHECK(last_reply[2] == CFW_PANEL_OFF && last_reply[4] == 2 && frame[6 * 320 + 2] == 0x77 && reflash_calls == 1);
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    /* Out-of-order sequence is refused and aborts the message. */
    CHECK(send_packet(s1, sizeof s1) == 0 && last_reply_len == 0);
    uint8_t bad_seq[sizeof s2]; memcpy(bad_seq, s2, sizeof s2); bad_seq[2] = 9;
    CHECK(send_packet(bad_seq, sizeof bad_seq) == 0 && last_reply[2] == CFW_BAD_SEQ && last_reply[3] == 9);
    CHECK(send_packet(s3, sizeof s3) == 0 && last_reply[2] == CFW_BAD_SEQ);  /* message no longer active */
    /* Bad CRC is reported on the last packet. */
    uint8_t s1_bad[sizeof s1]; memcpy(s1_bad, s1, sizeof s1); s1_bad[6] ^= 0xFF;
    CHECK(send_packet(s1_bad, sizeof s1_bad) == 0 && last_reply_len == 0);
    CHECK(send_packet(s2, sizeof s2) == 0 && send_packet(s3, sizeof s3) == 0 && last_reply[2] == CFW_BAD_CRC);
    /* ONCE: the same message sent again with flag bit 2 is acknowledged, not re-run. */
    memset(frame, 0, 200 * 320);
    reflash_calls = 0;
    CHECK(send_packet(s1, sizeof s1) == 0 && send_packet(s2, sizeof s2) == 0 && send_packet(s3, sizeof s3) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1);
    uint8_t s3_once[sizeof s3]; memcpy(s3_once, s3, sizeof s3); s3_once[3] = 0x02 | 0x04;
    uint8_t s1_next[sizeof s1]; memcpy(s1_next, s1, sizeof s1); s1_next[2] = 10;
    uint8_t s2_next[sizeof s2]; memcpy(s2_next, s2, sizeof s2); s2_next[2] = 11;
    s3_once[2] = 12;
    CHECK(send_packet(s1_next, sizeof s1_next) == 0 && send_packet(s2_next, sizeof s2_next) == 0 && send_packet(s3_once, sizeof s3_once) == 0);
    CHECK(last_reply[2] == CFW_OK && last_reply[3] == 12 && last_reply[4] == 2 && reflash_calls == 1);   /* answered, nothing pushed again */
    s1_next[2] = 13; s2_next[2] = 14; s3_once[2] = 15; s3_once[3] = 0x02;
    CHECK(send_packet(s1_next, sizeof s1_next) == 0 && send_packet(s2_next, sizeof s2_next) == 0 && send_packet(s3_once, sizeof s3_once) == 0);
    CHECK(last_reply[2] == CFW_OK && reflash_calls == 2);                                                  /* without the flag it runs */
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
    CHECK(send_packet(stats, sizeof stats) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 5 && last_reply[5] == 5 && last_reply[7] == 2);   /* the ONCE repeat did not count */

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

    /* COPY (07) left the firmware in G1CFW/42 to make room for the vector contract. */
    uint8_t copy_gone[] = { 0xF0, 0x07, 0x00, 10, 0, 10, 0, 4, 0, 3, 0, 10, 0, 9, 0 };
    CHECK(send_packet(copy_gone, sizeof copy_gone) == 0 && last_reply[2] == CFW_BAD_COMMAND);

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

    /* Shapes: the 20-byte records of the g2flash rasterizer, every type at once. */
    memset(frame, 0, 200 * 320);
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    static const int16_t shape_recs[][12] = {   /* type, flags, colour, width, p0..p7 */
        { 1, 0, 15, 1, -50, 199, 700, 170 },                       /* clipped at both ends, drawn first */
        { 3, 0, 10, 0, 320, 10, 100, 60, 0 },                      /* rect fill (320,10) 100x60 */
        { 3, 0, 15, 0, 430, 10, 100, 60, 16 },                     /* rounded rect fill, r 16 */
        { 2, 0, 15, 4, 540, 10, 90, 60, 20 },                      /* rounded outline, 4 px */
        { 5, 0, 15, 0, 100, 100, 60 },                             /* disc r 60 at (100,100) */
        { 4, 0, 12, 8, 240, 100, 60 },                             /* ring r 60, 8 px thick */
        { 5, 0, 8, 0, 480, 130, 1 },                               /* r 1: a 3x3 dot */
        { 1, 0, 15, 1, 20, 190, 200, 190 },                        /* hairline */
        { 1, 0, 9, 6, 20, 175, 200, 175 },                         /* 6 px stroke */
        { 7, 0, 15, 0, 320, 190, 400, 190, 360, 150 },             /* filled triangle */
        { 9, 0, 11, 0, 410, 150, 490, 160, 480, 195, 420, 185 },   /* filled quad */
        { 8, 0, 15, 1, 500, 150, 550, 158, 545, 195, 505, 188 },   /* quad outline */
        { 10, 0, 15, 1, 20, 60, 140, -40, 220, 60 },               /* quadratic, control point above the frame */
        { 11, 0, 15, 4, 320, 100, 380, 75, 440, 125, 500, 100 },   /* cubic, 4 px */
        { 12, 0, 15, 3, 560, 120, 40, 180, 360 },                  /* upper half arc */
        { 13, 0, 6, 0, 600, 160, 30, 0, 360 },                     /* full pie: a disc */
        { 13, 0, 15, 0, 300, 20, 15, -90, 135 },                   /* sector */
    };
    enum { SHAPE_N = sizeof shape_recs / sizeof shape_recs[0] };
    uint8_t shapes[4 + 20 * SHAPE_N];
    shapes[0] = 0xF0; shapes[1] = 0x0D; shapes[2] = 0x02; shapes[3] = SHAPE_N;
    for (int i = 0; i < SHAPE_N; i++) {
        uint8_t *rec = shapes + 4 + 20 * i;
        rec[0] = (uint8_t)shape_recs[i][0]; rec[1] = (uint8_t)shape_recs[i][1]; rec[2] = (uint8_t)shape_recs[i][2]; rec[3] = (uint8_t)shape_recs[i][3];
        for (int k = 0; k < 8; k++) { rec[4 + 2 * k] = (uint8_t)shape_recs[i][4 + k]; rec[5 + 2 * k] = (uint8_t)(shape_recs[i][4 + k] >> 8); }
    }
    reflash_calls = 0;
    CHECK(send_packet(shapes, sizeof shapes) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1 && last_rect[4] == 640 && last_rect[5] == 199);
    /* disc r 60: about pi * 60.5^2 = 11500 pixels, edges exactly at cx +- r */
    int disc = count_level(40, 40, 161, 161, 15);
    CHECK(disc > 11000 && disc < 11700);
    CHECK(px(100, 100) == 15 && px(100, 39) == 0 && px(100, 40) == 15 && px(100, 160) == 15 && px(100, 161) == 0);
    CHECK(px(39, 100) == 0 && px(40, 100) == 15 && px(160, 100) == 15 && px(161, 100) == 0);
    /* ring r 60 w 8: hollow centre, band exactly 8 px thick */
    CHECK(px(240, 100) == 0 && px(240, 40) == 12 && px(240, 47) == 12 && px(240, 48) == 0 && px(180, 100) == 12 && px(188, 100) == 0);
    /* rect fill exact size; rounded corner removed, straight edges kept */
    CHECK(px(320, 10) == 10 && px(419, 69) == 10 && px(420, 10) == 0 && px(320, 70) == 0);
    CHECK(px(430, 10) == 0 && px(432, 12) == 0 && px(440, 12) == 15 && px(480, 10) == 15 && px(430, 40) == 15);
    /* rounded outline: corner pixel empty, edge mid-points 4 px thick, centre empty */
    CHECK(px(540, 10) == 0 && px(585, 10) == 15 && px(585, 13) == 15 && px(585, 14) == 0 && px(585, 40) == 0 && px(540, 40) == 15 && px(543, 40) == 15 && px(544, 40) == 0);
    /* r 1 is a 3x3 block */
    CHECK(px(479, 129) == 8 && px(481, 131) == 8 && px(478, 130) == 0 && px(482, 130) == 0 && px(480, 128) == 0);
    /* lines: a hairline on its row; a 6 px stroke covers rows y-3 .. y+2 (pixel centres inside the quad) */
    CHECK(px(20, 190) == 15 && px(200, 190) == 15 && px(19, 190) == 0 && px(100, 189) == 0 && px(100, 191) == 0);
    CHECK(px(100, 175) == 9 && px(100, 172) == 9 && px(100, 177) == 9 && px(100, 171) == 0 && px(100, 178) == 0);
    /* polygons */
    CHECK(px(360, 185) == 15 && px(330, 185) == 15 && px(300, 160) == 0 && px(405, 150) == 0 && px(450, 172) == 11 && px(500, 150) == 15 && px(525, 172) == 0);
    /* half arc through its top and both ends, nothing at the centre or below; a 360 degree pie is a disc */
    CHECK(px(560, 80) == 15 && px(520, 120) == 15 && px(600, 120) == 15 && px(560, 120) == 0 && px(560, 160) == 0);
    int pie = count_level(569, 129, 632, 192, 6);
    CHECK(pie > 2750 && pie < 3100);
    /* sector from -90 to 135 degrees covers the right half and the lower left, not the upper left */
    CHECK(px(300, 20) == 15 && px(305, 10) == 15 && px(310, 25) == 15 && px(295, 30) == 15 && px(290, 25) == 0 && px(290, 15) == 0);
    /* curves drew along their hulls; the long line was clipped at both ends */
    CHECK(count_level(20, 0, 221, 61, 15) > 150 && count_level(318, 70, 503, 130, 15) > 400);
    CHECK(px(0, 197) == 15 && px(639, 172) == 15);
    guards_intact();
    uint8_t shapes_bad[24] = { 0xF0, 0x0D, 0x00, 1, 16, 0, 15, 1 };
    CHECK(send_packet(shapes_bad, sizeof shapes_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    shapes_bad[4] = 0;
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
    CHECK(memcmp(host_ram2 + 32, "OBJ4", 4) == 0 && host_ram2[0] == 0 && host_ram2[sizeof host_ram2 - 1] == 0);
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
    uint8_t anim[] = { 0xF0, 0x25, 40, 1, 1, 0, 0x03, 0x00, 4, 0, 110, 0, 50, 0 };   /* x and y, 4 frames, linear */
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
    uint8_t anim_bad[] = { 0xF0, 0x25, 40, 1, 9, 9, 0x03, 0x00, 4, 0, 0, 0, 0, 0 };
    CHECK(send_packet(anim_bad, sizeof anim_bad) == 0 && last_reply[2] == CFW_NO_OBJECT);
    CHECK(send_packet(anim, 11) == 0 && last_reply[2] == CFW_BAD_LENGTH);   /* values cut short */
    CHECK(send_packet(anim, 7) == 0 && last_reply[2] == CFW_BAD_LENGTH);    /* entry header cut short */
    uint8_t anim_mask[] = { 0xF0, 0x25, 40, 1, 1, 0, 0x00, 0x02, 4, 0, 1, 0 };   /* a rect has no stroke width */
    CHECK(send_packet(anim_mask, sizeof anim_mask) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t anim_none[] = { 0xF0, 0x25, 40, 1, 1, 0, 0x00, 0x00, 4, 0 };
    CHECK(send_packet(anim_none, sizeof anim_none) == 0 && last_reply[2] == CFW_BAD_RECORD);
    /* Easing end points land exactly on the target. */
    uint8_t anim2[] = { 0xF0, 0x25, 0, 1, 1, 0, 0x03, 0x00, 3, 1, 10, 0, 10, 0 };
    CHECK(send_packet(anim2, sizeof anim2) == 0 && last_reply[2] == CFW_OK && last_sleep == 40);
    for (int i = 0; i < 3; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(frame[10 * 320 + 5] == 0xCC && frame[13 * 320 + 8] == 0xCC && frame[14 * 320 + 5] == 0 && frame[50 * 320 + 55] == 0);
    CHECK(host_ram2[0] == 0 && host_ram2[sizeof host_ram2 - 1] == 0);
    const uint8_t astate[] = { 0xF0, 0x26 };
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply[4] == 40 && last_reply[5] == 7 && last_reply[9] == 0);
    /* A shapes-asset object (kind 3) draws its records but has nothing to tween. */
    uint8_t shp_asset[] = { 0xF0, 0x08, 11, 0, 0x02, 0x00, 0, 0, 0, 0, 1, 1, 0, 15, 1, 0, 0, 0, 0, 5, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(send_packet(shp_asset, sizeof shp_asset) == 0 && last_reply[2] == CFW_OK);
    uint8_t o5[] = { 0xF0, 0x20, 5, 0, 1, 0, 3, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0x0F, 0, 0 };
    CHECK(send_packet(o5, sizeof o5) == 0 && last_reply[2] == CFW_OK);
    uint8_t show5[] = { 0xF0, 0x21, 0x01, 1, 5, 0 };
    CHECK(send_packet(show5, sizeof show5) == 0 && last_reply[2] == CFW_OK);
    CHECK(px(0, 0) == 15 && px(5, 5) == 15 && px(1, 0) == 0 && px(6, 6) == 0);
    uint8_t anim5[] = { 0xF0, 0x25, 0, 1, 5, 0, 0x03, 0x00, 1, 0, 0, 0, 0, 0 };
    CHECK(send_packet(anim5, sizeof anim5) == 0 && last_reply[2] == CFW_BAD_RECORD);

    /* Image (14) and text (15) records in a shape list draw assets by id. */
    CHECK(send_packet(put5, sizeof put5) == 0 && last_reply[2] == CFW_OK);          /* 16x2 1bpp image, asset 5 */
    CHECK(send_packet(puts2, sizeof puts2) == 0 && last_reply[2] == CFW_OK);        /* string asset 9 */
    memset(frame, 0, 200 * 320);
    utf_calls = 0;
    uint8_t shapes_assets[4 + 40] = { 0xF0, 0x0D, 0x00, 2,
        14, 0, 9, 0, 20, 0, 50, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,               /* image asset 5 at (20,50), level 9 */
        15, 0, 15, 1, 100, 0, 100, 0, 64, 0, 40, 0, 9, 0, 2, 0, 0, 0, 0, 0 };       /* text asset 9 in (100,100) 64x40, font 1, 2 lines */
    CHECK(send_packet(shapes_assets, sizeof shapes_assets) == 0 && last_reply[2] == CFW_OK);
    CHECK(px(20, 50) == 9 && px(27, 50) == 9 && px(28, 50) == 0 && px(20, 51) == 0 && px(28, 51) == 9);
    CHECK(utf_calls == 1 && utf_font == 1 && utf_lines == 2 && utf_box[0] == 100 && utf_box[2] == 164 && utf_box[3] == 139);
    uint8_t shapes_no_asset[24] = { 0xF0, 0x0D, 0x00, 1, 14, 0, 15, 0, 0, 0, 0, 0, 77, 0 };
    CHECK(send_packet(shapes_no_asset, sizeof shapes_no_asset) == 0 && last_reply[2] == CFW_OK);   /* unknown asset: draws nothing */

    /* Kind 4: one shape record per object, animated by parameter tweens. */
    uint8_t o6[] = { 0xF0, 0x20, 6, 0, 1, 0, 4, 0x01, 5, 1, 15, 0, 100, 0, 100, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  /* disc r 20 at (100,100) */
    CHECK(send_packet(o6, sizeof o6) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(o6, sizeof o6 - 1) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    uint8_t o6_bad[sizeof o6];
    memcpy(o6_bad, o6, sizeof o6); o6_bad[8] = 16;                       /* record type 16 */
    CHECK(send_packet(o6_bad, sizeof o6_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    memcpy(o6_bad, o6, sizeof o6); o6_bad[6] = 6;                        /* kind 6 */
    CHECK(send_packet(o6_bad, sizeof o6_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t show6[] = { 0xF0, 0x21, 0x03, 1, 6, 0 };
    reflash_calls = 0;
    CHECK(send_packet(show6, sizeof show6) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1);
    CHECK(px(100, 100) == 15 && px(100, 80) == 15 && px(100, 79) == 0 && px(120, 100) == 15 && px(121, 100) == 0 && px(0, 0) == 0);
    /* Tween centre x, radius and colour over 4 frames; each frame pushes the union of the boxes. */
    post_calls = 0; reflash_calls = 0;
    uint8_t tween[] = { 0xF0, 0x25, 40, 1, 6, 0, 0x05, 0x01, 4, 0, 0x2C, 0x01, 40, 0, 8, 0 };   /* cx 300, r 40, level 8 */
    CHECK(send_packet(tween, sizeof tween) == 0 && last_reply[2] == CFW_OK && post_calls == 1);
    cfw_display_tick(state, 0x10, 0, 2);   /* frame 1: cx 150, r 25, level 14 */
    CHECK(reflash_calls == 1 && last_rect[2] == 78 && last_rect[3] == 73 && last_rect[4] == 178 && last_rect[5] == 127);
    CHECK(px(150, 100) == 14 && px(126, 100) == 14 && px(124, 100) == 0 && px(100, 100) == 0 && px(90, 100) == 0);
    for (int i = 0; i < 3; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(reflash_calls == 4 && post_calls == 4);
    CHECK(px(300, 100) == 8 && px(260, 100) == 8 && px(259, 100) == 0 && px(340, 100) == 8 && px(341, 100) == 0 && px(150, 100) == 0);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(reflash_calls == 4 && post_calls == 4);
    /* Tweens merge by object: a second object's tween joins a running one, a
     * retarget restarts from the value reached. */
    uint8_t o8[] = { 0xF0, 0x20, 8, 0, 1, 0, 4, 0x01, 3, 1, 15, 0, 10, 0, 150, 0, 20, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  /* rect fill 20x10 at (10,150) */
    CHECK(send_packet(o8, sizeof o8) == 0 && last_reply[2] == CFW_OK);
    uint8_t show68[] = { 0xF0, 0x21, 0x03, 2, 6, 0, 8, 0 };
    CHECK(send_packet(show68, sizeof show68) == 0 && last_reply[2] == CFW_OK && px(29, 155) == 15 && px(30, 155) == 0);
    uint8_t tween_a[] = { 0xF0, 0x25, 0, 1, 6, 0, 0x01, 0x00, 4, 0, 100, 0 };        /* disc back to cx 100 in 4 */
    CHECK(send_packet(tween_a, sizeof tween_a) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 250 */
    CHECK(px(250, 100) == 8 && px(300, 100) == 0);
    uint8_t tween_b[] = { 0xF0, 0x25, 0, 1, 8, 0, 0x04, 0x00, 2, 0, 60, 0 };         /* rect width to 60 in 2 */
    CHECK(send_packet(tween_b, sizeof tween_b) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 200; width 40 */
    CHECK(px(200, 100) == 8 && px(49, 155) == 15 && px(50, 155) == 0);
    uint8_t tween_c[] = { 0xF0, 0x25, 0, 1, 6, 0, 0x01, 0x00, 2, 0, 0xF4, 0x01 };    /* retarget: cx 500 in 2 from 200 */
    CHECK(send_packet(tween_c, sizeof tween_c) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 350; width 60 (done) */
    CHECK(px(350, 100) == 8 && px(200, 100) == 0 && px(69, 155) == 15 && px(70, 155) == 0);
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 500 (done) */
    CHECK(px(500, 100) == 8 && px(350, 100) == 0);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 0);
    /* frames 0 snaps and presents at once, without a display tick. */
    post_calls = 0; reflash_calls = 0;
    uint8_t snap[] = { 0xF0, 0x25, 0, 1, 8, 0, 0x00, 0x01, 0, 0, 3, 0 };           /* level 3 now */
    CHECK(send_packet(snap, sizeof snap) == 0 && last_reply[2] == CFW_OK && post_calls == 0 && reflash_calls == 1);
    CHECK(px(20, 155) == 3 && last_rect[3] == 149 && last_rect[5] == 160);
    /* Cubic-bezier easing: a linear curve matches code 0, an ease-in curve starts slower. */
    uint8_t cur_lin[] = { 0xF0, 0x25, 0, 1, 6, 0, 0x01, 0x00, 4, 0x80, 100, 0, 0, 0, 255, 255 };     /* disc cx 500 -> 100, curve (0,0,255,255) */
    CHECK(send_packet(cur_lin, sizeof cur_lin) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(px(400, 100) == 8 && px(358, 100) == 0 && px(443, 100) == 0);                              /* 500 - 400/4 within a pixel, r 40 */
    for (int i = 0; i < 3; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(px(100, 100) == 8 && px(400, 100) == 0);
    uint8_t cur_in[] = { 0xF0, 0x25, 0, 1, 6, 0, 0x01, 0x00, 4, 0x80, 0xF4, 0x01, 107, 0, 255, 255 }; /* cx 100 -> 500, ease-in */
    CHECK(send_packet(cur_in, sizeof cur_in) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(px(130, 100) == 8 && px(200, 100) == 0);                                                   /* well short of the linear 200 */
    for (int i = 0; i < 3; i++) cfw_display_tick(state, 0x10, 0, 2);
    CHECK(px(500, 100) == 8 && px(130, 100) == 0);
    CHECK(send_packet(cur_in, sizeof cur_in - 2) == 0 && last_reply[2] == CFW_BAD_LENGTH);          /* curve cut short */
    uint8_t cur_bad[] = { 0xF0, 0x25, 0, 1, 6, 0, 0x01, 0x00, 4, 7, 100, 0 };                        /* easing 7: unknown */
    CHECK(send_packet(cur_bad, sizeof cur_bad) == 0 && last_reply[2] == CFW_BAD_RECORD);
    /* The journal: OBJ_STATE [FF][since] replays definition changes two at a time. */
    uint8_t jq[] = { 0xF0, 0x23, 0xFF, 0, 0 };
    CHECK(send_packet(jq, sizeof jq) == 0 && last_reply[2] == CFW_OK && last_reply[5] == 0xFF);       /* revision 0 is long gone */
    uint16_t jrev = (uint16_t)(last_reply[3] | last_reply[4] << 8);
    jq[3] = (uint8_t)jrev; jq[4] = (uint8_t)(jrev >> 8);
    CHECK(send_packet(jq, sizeof jq) == 0 && last_reply[2] == CFW_OK && last_reply[5] == 0 && last_reply_len == 6);   /* up to date */
    uint8_t o30[] = { 0xF0, 0x20, 30, 0, 3, 0, 2, 0x01, 0, 0, 0, 0, 2, 0, 2, 0, 0, 0, 0x0F, 0, 0 };
    CHECK(send_packet(o30, sizeof o30) == 0 && last_reply[2] == CFW_OK);
    uint8_t drop30[] = { 0xF0, 0x24, 1, 30, 0 };
    CHECK(send_packet(drop30, sizeof drop30) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(jq, sizeof jq) == 0 && last_reply[2] == CFW_OK && last_reply[5] == 2 && last_reply_len == 16);
    CHECK(last_reply[6] == 1 && last_reply[7] == 30 && last_reply[9] == 3 && last_reply[11] == 2 && last_reply[12] == 30 && last_reply[14] == 0);
    uint16_t jrev2 = (uint16_t)(last_reply[3] | last_reply[4] << 8);
    CHECK(jrev2 == (uint16_t)(jrev + 2));
    jq[3] = (uint8_t)(jrev + 1); jq[4] = (uint8_t)((jrev + 1) >> 8);
    CHECK(send_packet(jq, sizeof jq) == 0 && last_reply[5] == 1 && last_reply[6] == 2);                /* only the drop is newer */
    for (int i = 0; i < 12; i++) { o30[2] = (uint8_t)(31 + i); CHECK(send_packet(o30, sizeof o30) == 0 && last_reply[2] == CFW_OK); }
    jq[3] = (uint8_t)jrev; jq[4] = (uint8_t)(jrev >> 8);
    CHECK(send_packet(jq, sizeof jq) == 0 && last_reply[5] == 0xFF);                                    /* fell off the ring */
    for (int i = 0; i < 12; i++) { drop30[3] = (uint8_t)(31 + i); CHECK(send_packet(drop30, sizeof drop30) == 0 && last_reply[2] == CFW_OK); }
    /* OBJ_STATE with a page lists the object table four at a time. */
    uint8_t ostate_page[] = { 0xF0, 0x23, 0 };
    CHECK(send_packet(ostate_page, sizeof ostate_page) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply_len == 4 + 4 * 4);
    CHECK(last_reply[4] == 1 && last_reply[8] == 5 && last_reply[12] == 6 && last_reply[16] == 8);      /* the table in definition order */
    ostate_page[2] = 1;
    CHECK(send_packet(ostate_page, sizeof ostate_page) == 0 && last_reply[2] == CFW_OK && last_reply_len == 4);
    /* Freeze leaves an object where its tween reached it; finish jumps to the targets. */
    CHECK(send_packet(tween_a, sizeof tween_a) == 0 && last_reply[2] == CFW_OK);     /* disc cx -> 100 in 4 */
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 400 */
    uint8_t freeze6[] = { 0xF0, 0x24, 2, 6, 0 };
    CHECK(send_packet(freeze6, sizeof freeze6) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 0 && px(400, 100) == 8);
    CHECK(send_packet(tween_a, sizeof tween_a) == 0 && last_reply[2] == CFW_OK);     /* cx 400 -> 100 in 4 */
    cfw_display_tick(state, 0x10, 0, 2);                                             /* cx 325 */
    reflash_calls = 0;
    uint8_t finish_all[] = { 0xF0, 0x24, 3 };
    CHECK(send_packet(finish_all, sizeof finish_all) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1 && px(100, 100) == 8 && px(325, 100) == 0);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 0);
    /* A PUT or a HIDE stops the object's tween; the set holds 16 objects. */
    CHECK(send_packet(tween_a, sizeof tween_a) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(o6, sizeof o6) == 0 && last_reply[2] == CFW_OK);             /* redefined: tween gone */
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 0 && px(100, 100) == 8);
    CHECK(send_packet(show68, sizeof show68) == 0 && last_reply[2] == CFW_OK && px(100, 100) == 15 && px(500, 100) == 0);
    uint8_t many[2 + 2 + 17 * 8];
    many[0] = 0xF0; many[1] = 0x25; many[2] = 0; many[3] = 17;
    for (int i = 0; i < 17; i++) {
        uint8_t dot[sizeof o6];
        memcpy(dot, o6, sizeof o6); dot[2] = (uint8_t)(20 + i); dot[12] = (uint8_t)(10 + i); dot[16] = 2;
        CHECK(send_packet(dot, sizeof dot) == 0 && last_reply[2] == CFW_OK);
        uint8_t *e = many + 4 + 8 * i;
        e[0] = (uint8_t)(20 + i); e[1] = 0; e[2] = 0x01; e[3] = 0; e[4] = 2; e[5] = 0; e[6] = 200; e[7] = 0;
    }
    CHECK(send_packet(many, sizeof many) == 0 && last_reply[2] == CFW_NO_MEMORY);
    many[3] = 16;
    CHECK(send_packet(many, sizeof many - 8) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 1);
    const uint8_t hide_all[] = { 0xF0, 0x22, 0x00 };
    CHECK(send_packet(hide_all, sizeof hide_all) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(astate, sizeof astate) == 0 && last_reply[3] == 0);
    cfw_display_tick(state, 0x10, 0, 2);   /* the queued refresh finds nothing to do and stops */
    CHECK(send_packet(clear_all, sizeof clear_all) == 0 && last_reply[2] == CFW_OK);
    CHECK(host_ram2[0] == 0 && host_ram2[sizeof host_ram2 - 1] == 0);
    guards_intact();

    /* Text level: the stock drawer writes full-bright glyphs (the host stub
     * writes nothing, so the box is prefilled), which the object scales. */
    CHECK(send_packet(puts2, sizeof puts2) == 0 && last_reply[2] == CFW_OK);        /* string asset 9 again */
    uint8_t o3dim[] = { 0xF0, 0x20, 3, 0, 8, 0, 1, 0x01, 32, 0, 32, 0, 0x40, 0x02, 28, 0, 9, 0, 0x05, 0, 1 };  /* text at level 5 */
    CHECK(send_packet(o3dim, sizeof o3dim) == 0 && last_reply[2] == CFW_OK);
    memset(frame, 0, 200 * 320);
    for (int row = 32; row < 60; row++) memset(frame + row * 320 + 16, 0xF9, 288);  /* 15 and 9 inside the box */
    frame[32 * 320 + 15] = 0xFF;                                                     /* just left of the box */
    uint8_t show3[] = { 0xF0, 0x21, 0x02, 1, 3, 0 };                                   /* no clear: the prefill stands in for glyphs */
    CHECK(send_packet(show3, sizeof show3) == 0 && last_reply[2] == CFW_OK);
    CHECK(frame[32 * 320 + 16] == 0x53 && frame[59 * 320 + 303] == 0x53 && frame[32 * 320 + 15] == 0xFF && frame[60 * 320 + 16] == 0 && frame[32 * 320 + 304] == 0);
    uint8_t dim_tween[] = { 0xF0, 0x25, 0, 1, 3, 0, 0x00, 0x01, 1, 0, 15, 0 };         /* level back to 15 in one frame */
    CHECK(send_packet(dim_tween, sizeof dim_tween) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(send_packet(ostate, sizeof ostate) == 0 && last_reply[2] == CFW_OK);
    /* A glide lands the text box on an odd column and past the right edge
     * between frames (G1CFW/43): the box is widened to byte columns and
     * clipped to the frame instead of refusing the frame, so the tick
     * renders it and a SHOW arriving mid-tween is not refused. */
    CHECK(cfw_animation()->render_errors == 0);
    uint8_t odd_tween[] = { 0xF0, 0x25, 0, 1, 3, 0, 0x01, 0x00, 1, 0, 0x45, 0x00 };   /* x 32 -> 69 in one frame: box 69..645 */
    CHECK(send_packet(odd_tween, sizeof odd_tween) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(cfw_animation()->render_errors == 0);
    uint8_t show3push[] = { 0xF0, 0x21, 0x03, 1, 3, 0 };
    CHECK(send_packet(show3push, sizeof show3push) == 0 && last_reply[2] == CFW_OK);
    uint8_t back_tween[] = { 0xF0, 0x25, 0, 1, 3, 0, 0x01, 0x00, 0, 0, 32, 0 };        /* snap back to x 32 */
    CHECK(send_packet(back_tween, sizeof back_tween) == 0 && last_reply[2] == CFW_OK);

    /* Paths (asset kind 3): commands in Q4 compiled on PUT; a square with a
     * same-winding square hole fills even-odd with the hole, nonzero without. */
    uint8_t path_cmds[2 * (4 * 5 + 1)];
    {
        int n = 0;
        int16_t sq[2][8] = { { 100, 20, 260, 20, 260, 180, 100, 180 }, { 140, 60, 220, 60, 220, 140, 140, 140 } };
        for (int c = 0; c < 2; c++) {
            for (int v = 0; v < 4; v++) {
                path_cmds[n++] = v == 0 ? 0 : 1;
                int16_t x = (int16_t)(sq[c][2 * v] * 16), y = (int16_t)(sq[c][2 * v + 1] * 16);
                path_cmds[n++] = (uint8_t)x; path_cmds[n++] = (uint8_t)(x >> 8);
                path_cmds[n++] = (uint8_t)y; path_cmds[n++] = (uint8_t)(y >> 8);
            }
            path_cmds[n++] = 4;
        }
        CHECK(n == (int)sizeof path_cmds);
    }
    uint8_t path_put[10 + sizeof path_cmds] = { 0xF0, 0x08, 30, 0, 0x03, 0x00, 1, 0, 0, 0 };   /* asset 30, even-odd */
    memcpy(path_put + 10, path_cmds, sizeof path_cmds);
    CHECK(send_packet(path_put, sizeof path_put) == 0 && last_reply[2] == CFW_OK);
    path_put[2] = 31; path_put[6] = 0;                                                         /* asset 31, nonzero */
    CHECK(send_packet(path_put, sizeof path_put) == 0 && last_reply[2] == CFW_OK);
    path_put[2] = 32; path_put[5] = 1;                                                         /* append: not for paths */
    CHECK(send_packet(path_put, sizeof path_put) == 0 && last_reply[2] == CFW_BAD_ASSET);
    path_put[5] = 0;
    CHECK(send_packet(path_put, sizeof path_put - 1) == 0 && last_reply[2] == CFW_BAD_ASSET);   /* unclosed */
    path_put[10] = 7;
    CHECK(send_packet(path_put, sizeof path_put) == 0 && last_reply[2] == CFW_BAD_ASSET);       /* unknown command */
    path_put[10] = 0;

    CHECK(send_packet(ainfo, sizeof ainfo) == 0 && last_reply[2] == CFW_OK);
    int assets_before = last_reply[3];
    uint8_t o9[] = { 0xF0, 0x20, 9, 0, 1, 0, 5, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 30, 0, 0x0F, 0, 0 };    /* path 30 at 1x */
    CHECK(send_packet(o9, sizeof o9) == 0 && last_reply[2] == CFW_OK);
    uint8_t show9[] = { 0xF0, 0x21, 0x03, 1, 9, 0 };
    CHECK(send_packet(show9, sizeof show9) == 0 && last_reply[2] == CFW_OK);
    CHECK(px(120, 40) == 15 && px(180, 100) == 0 && px(90, 40) == 0 && px(100, 20) == 15 && px(259, 179) == 15 && px(260, 180) == 0);
    o9[16] = 31;                                                                                 /* nonzero: no hole */
    CHECK(send_packet(o9, sizeof o9) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(show9, sizeof show9) == 0 && last_reply[2] == CFW_OK && px(180, 100) == 15);
    o9[16] = 30; o9[8] = 100; o9[12] = 128; o9[13] = 0;                                          /* even-odd at half size, 100 px right */
    CHECK(send_packet(o9, sizeof o9) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(show9, sizeof show9) == 0 && last_reply[2] == CFW_OK);
    CHECK(px(160, 20) == 15 && px(190, 50) == 0 && px(229, 89) == 15 && px(230, 90) == 0 && px(120, 40) == 0);
    o9[16] = 9;                                                                                  /* a string asset is not a path */
    CHECK(send_packet(o9, sizeof o9) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(show9, sizeof show9) == 0 && last_reply[2] == CFW_BAD_ASSET);
    o9[16] = 30;
    CHECK(send_packet(o9, sizeof o9) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(ainfo, sizeof ainfo) == 0 && last_reply[3] == assets_before);

    /* Rotation rides the tween engine: angle (Q4 degrees) and pivot are
     * values 10-12. A horizontal bar snapped a quarter turn stands up. */
    uint8_t o10[] = { 0xF0, 0x20, 10, 0, 1, 0, 4, 0x01, 3, 1, 15, 0, 0x2C, 0x01, 80, 0, 100, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  /* rect fill (300,80) 100x20 */
    CHECK(send_packet(o10, sizeof o10) == 0 && last_reply[2] == CFW_OK);
    uint8_t show10[] = { 0xF0, 0x21, 0x03, 1, 10, 0 };
    CHECK(send_packet(show10, sizeof show10) == 0 && last_reply[2] == CFW_OK && px(310, 90) == 15 && px(350, 50) == 0);
    uint8_t rot90[] = { 0xF0, 0x25, 0, 1, 10, 0, 0x00, 0x1C, 0, 0, 0xA0, 0x05, 0x5E, 0x01, 90, 0 };   /* angle 1440 (90 deg), pivot (350,90), snap */
    reflash_calls = 0;
    CHECK(send_packet(rot90, sizeof rot90) == 0 && last_reply[2] == CFW_OK && reflash_calls == 1 && last_rect[4] == 640);
    CHECK(px(350, 50) == 15 && px(350, 130) == 15 && px(350, 30) == 0 && px(310, 90) == 0 && px(380, 90) == 0 && px(345, 90) == 15);
    uint8_t rot_back[] = { 0xF0, 0x25, 0, 1, 10, 0, 0x00, 0x04, 2, 0, 0, 0 };                      /* angle back to 0 over 2 frames */
    CHECK(send_packet(rot_back, sizeof rot_back) == 0 && last_reply[2] == CFW_OK);
    cfw_display_tick(state, 0x10, 0, 2);                                                           /* 45 degrees */
    CHECK(px(350, 90) == 15 && px(320, 60) == 15 && px(310, 90) == 0 && px(350, 50) == 0);
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(px(310, 90) == 15 && px(350, 50) == 0 && px(320, 60) == 0);
    /* A rotated ring is still a ring: its lit area barely depends on the angle. */
    uint8_t o12[] = { 0xF0, 0x20, 12, 0, 1, 0, 4, 0x01, 4, 1, 15, 6, 0x40, 0x01, 100, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  /* ring r 40 w 6 at (320,100) */
    CHECK(send_packet(o12, sizeof o12) == 0 && last_reply[2] == CFW_OK);
    uint8_t show12[] = { 0xF0, 0x21, 0x03, 1, 12, 0 };
    CHECK(send_packet(show12, sizeof show12) == 0 && last_reply[2] == CFW_OK);
    int ring0 = count_level(270, 50, 371, 151, 15);
    CHECK(ring0 > 1300 && ring0 < 1550);
    for (int deg = 30; deg < 360; deg += 30) {
        uint8_t spin[] = { 0xF0, 0x25, 0, 1, 12, 0, 0x00, 0x1C, 0, 0, (uint8_t)(deg * 16), (uint8_t)((deg * 16) >> 8), 0x40, 0x01, 100, 0 };
        CHECK(send_packet(spin, sizeof spin) == 0 && last_reply[2] == CFW_OK);
        int lit = count_level(270, 50, 371, 151, 15);
        if (lit < ring0 - ring0 / 8 || lit > ring0 + ring0 / 8) {
            printf("rotated ring at %d degrees lights %d pixels, unrotated %d\n", deg, lit, ring0);
            failures++;
        }
    }
    CHECK(send_packet(o1, sizeof o1) == 0 && last_reply[2] == CFW_OK);
    uint8_t rot_rect[] = { 0xF0, 0x25, 0, 1, 1, 0, 0x00, 0x04, 0, 0, 0, 0 };                       /* a kind-2 rect cannot rotate */
    CHECK(send_packet(rot_rect, sizeof rot_rect) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t rot_path[] = { 0xF0, 0x25, 0, 1, 9, 0, 0x00, 0x1E, 0, 0, 0, 0, 0xA0, 0x05, 190, 0, 50, 0 }; /* path: scale 0?? no: mask bits 1,2 invalid with 0x200 */
    CHECK(send_packet(rot_path, sizeof rot_path) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t rot_path_ok[] = { 0xF0, 0x25, 0, 1, 9, 0, 0x00, 0x1C, 0, 0, 0xA0, 0x05, 190, 0, 50, 0 };  /* the half-size path a quarter turn about its centre */
    CHECK(send_packet(show9, sizeof show9) == 0 && last_reply[2] == CFW_OK);
    CHECK(send_packet(rot_path_ok, sizeof rot_path_ok) == 0 && last_reply[2] == CFW_OK);
    CHECK(px(160, 20) == 15 && px(190, 50) == 0 && px(229, 89) == 15);
    CHECK(send_packet(clear_all, sizeof clear_all) == 0 && last_reply[2] == CFW_OK);
    guards_intact();
    CHECK(host_ram3[0] == 0 && host_ram3[31] == 0 && host_ram3[sizeof host_ram3 - 1] == 0);

    /* Touch gestures: the redirected stock helper hands codes to the stock
     * semaphore by default, reports them when asked, and can swallow them. */
    uint8_t in_state[] = { 0xF0, 0x31 };
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && last_reply_len == 20);
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
    uint8_t in_bad[] = { 0xF0, 0x30, 0x20 };
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

    /* Wear events switch the panel unless the host keeps it. */
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0;
    panel_cb_calls = 0;
    cfw_stock_event(6, 0, 0);               /* worn: panel comes on through its own callback */
    CHECK(panel_cb_calls == 1 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1);
    cfw_stock_event(6, 0, 0);               /* already on: no second callback */
    CHECK(panel_cb_calls == 1);
    cfw_stock_event(8, 0, 0);               /* placed in the case: panel off */
    CHECK(panel_cb_calls == 2 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 0);
    cfw_stock_event(7, 0, 0);               /* taken off while already off: nothing */
    CHECK(panel_cb_calls == 2);
    uint8_t in_manual[] = { 0xF0, 0x30, 0x10 };
    CHECK(send_packet(in_manual, sizeof in_manual) == 0 && last_reply[2] == CFW_OK);
    cfw_stock_event(6, 0, 0);               /* host owns the panel: wear ignored */
    CHECK(panel_cb_calls == 2);
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[14] == 3);   /* the earlier taken-off event switched it off once */
    CHECK(send_packet(in_release, sizeof in_release) == 0 && last_reply[2] == CFW_OK);
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;

    /* Work-mode changes do the same without a central connected, and the
     * frame buffer is pushed again after the panel comes on. */
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0;
    panel_cb_calls = 0;
    host_frame[7] = 0xA5;
    pushed_row[7] = 0;
    panel[FW_PANEL_BRIGHTNESS_LEVEL] = 0x15;
    brightness_writes = resume_calls = suspend_calls = jbd_resume_calls = jbd_suspend_calls = 0;
    resume_refuses = 1;                     /* as on hardware: resume refuses, the power-on path follows */
    margin_rows = margin_dirty = margin_max_row = 0;
    host_frame[0] = 0x3C;
    cfw_work_mode(3);                       /* normal standby: worn, panel on, brightness written, frame pushed */
    CHECK(resume_calls == 1 && panel_cb_calls == 2 && brightness_writes == 1 && last_brightness == 0x15);
    CHECK(*(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1 && pushed_row[7] == 0xA5 && jbd_resume_calls == 0);
    /* The 280 panel rows outside the canvas (row origin 35) were blanked
     * with a zeroed copy of frame row 0, which is intact afterwards. */
    CHECK(margin_rows == 280 && !margin_dirty && margin_max_row == 479 && host_frame[0] == 0x3C && host_frame[7] == 0xA5);
    host_frame[0] = 0;
    cfw_work_mode(2);                       /* normal run: already on */
    CHECK(panel_cb_calls == 2);
    /* The frame is pushed again 600 ms and 1.2 s after power-up (the first
     * push shows nothing on hardware), on the 200 ms ticks. */
    state[FW_STATE_WORK_MODE] = 3;
    pushed_row[7] = 0;
    cfw_work_mode_tick(); cfw_work_mode_tick();
    CHECK(pushed_row[7] == 0);
    cfw_work_mode_tick();                   /* third tick: push */
    CHECK(pushed_row[7] == 0xA5 && panel_cb_calls == 2);
    pushed_row[7] = 0;
    cfw_work_mode_tick(); cfw_work_mode_tick();
    CHECK(pushed_row[7] == 0);
    cfw_work_mode_tick();                   /* sixth tick: push again */
    CHECK(pushed_row[7] == 0xA5);
    pushed_row[7] = 0;
    cfw_work_mode_tick();                   /* and then no more */
    CHECK(pushed_row[7] == 0);
    cfw_work_mode(4);                       /* taken off: suspend callback (clears the flag) */
    CHECK(suspend_calls == 1 && panel_cb_calls == 3 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 0);
    cfw_work_mode(9);                       /* waking from deep sleep: left to the next check */
    CHECK(panel_cb_calls == 3);
    resume_refuses = 0;
    panel_cb_calls = 2;                     /* the checks below count on/off style switches from here */
    state[FW_STATE_WORK_MODE] = 4;
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[14] == 5 && last_reply[15] == 4 && last_reply[16] == 9 && last_reply[17] == 4 && last_reply[18] == 7);

    /* The 200 ms checker pass reconciles the panel with the current mode
     * (the stock display-off path switches it without a mode change). */
    state[FW_STATE_WORK_MODE] = 3;
    state[FW_STATE_WEAR_DETECT] = 0;        /* wear detection switched off in the user settings */
    pushed_row[7] = 0;
    cfw_work_mode_tick();                   /* worn, panel found off: back on, frame pushed */
    CHECK(panel_cb_calls == 3 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1 && pushed_row[7] == 0xA5);
    CHECK(state[FW_STATE_WEAR_DETECT] == 1); /* and the setting is back on */
    cfw_work_mode_tick();                   /* steady: nothing */
    CHECK(panel_cb_calls == 3);
    state[FW_STATE_WORK_MODE] = 5;
    cfw_work_mode_tick();                   /* charging in the case: off */
    CHECK(panel_cb_calls == 4 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 0);
    CHECK(cfw_suspend_gate(state) == 1);    /* stock suspend refused while the CFW manages the panel */
    CHECK(send_packet(in_state, sizeof in_state) == 0 && last_reply[14] == 7 && last_reply[17] == 4 && last_reply[18] == 10 && last_reply[19] == 1);
    CHECK(send_packet(in_manual, sizeof in_manual) == 0 && last_reply[2] == CFW_OK);
    state[FW_STATE_WORK_MODE] = 3;
    state[FW_STATE_WEAR_DETECT] = 0;
    cfw_work_mode_tick();                   /* host owns the panel: no switch, setting left alone */
    CHECK(panel_cb_calls == 4 && cfw_suspend_gate(state) == 0 && state[FW_STATE_WEAR_DETECT] == 0);
    CHECK(send_packet(in_release, sizeof in_release) == 0 && last_reply[2] == CFW_OK);
    CHECK(last_reply[3] == 0 && last_reply[4] == 56 && last_reply[5] == 53);   /* default thresholds echoed */

    /* While stock says worn, the radio-link wear word decides with the CFW
     * thresholds: on a table it sits in the stock dead band. */
    state[FW_STATE_WORK_MODE] = 3;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;
    host_wear_word = 48;                    /* unfolded on the table */
    cfw_work_mode_tick();
    CHECK(panel_cb_calls == 5 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 0);
    host_wear_word = 55;                    /* between the thresholds: unchanged */
    cfw_work_mode_tick();
    CHECK(panel_cb_calls == 5);
    host_wear_word = 66;                    /* on a head */
    cfw_work_mode_tick();
    CHECK(panel_cb_calls == 6 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1);
    uint8_t in_thresholds[] = { 0xF0, 0x30, 0x00, 70, 66 };
    CHECK(send_packet(in_thresholds, sizeof in_thresholds) == 0 && last_reply[2] == CFW_OK && last_reply[4] == 70 && last_reply[5] == 66);
    cfw_work_mode_tick();                   /* 66 is now at the off threshold */
    CHECK(panel_cb_calls == 7 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 0);
    uint8_t in_bad_thresholds[] = { 0xF0, 0x30, 0x00, 50, 60 };
    CHECK(send_packet(in_bad_thresholds, sizeof in_bad_thresholds) == 0 && last_reply[2] == CFW_BAD_RECORD);
    uint8_t in_defaults[] = { 0xF0, 0x30, 0x00, 56, 53 };
    CHECK(send_packet(in_defaults, sizeof in_defaults) == 0 && last_reply[2] == CFW_OK);
    host_wear_word = 0;                     /* no statistics yet: the mode decides */
    cfw_work_mode_tick();
    CHECK(panel_cb_calls == 8 && *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1);
    host_frame[7] = 0;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;

    /* View offsets: added to the stock origins at flush time. */
    uint8_t mic_q_early[] = { 0xF0, 0x40, 0x02 };
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_COL_OFFSET) = 0;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_ROW_OFFSET) = 210;
    state[FW_PANEL_OFFSET + FW_PANEL_HEIGHT_GEAR] = 2;
    state[FW_PANEL_OFFSET + FW_PANEL_DEPTH_GEAR] = 3;
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;
    uint8_t view_get[] = { 0xF0, 0x14, 0x01 };
    CHECK(send_packet(view_get, sizeof view_get) == 0 && last_reply[2] == CFW_OK && last_reply_len == 13);
    CHECK(last_reply[3] == 0 && last_reply[5] == 0 && last_reply[7] == 0 && last_reply[9] == 210 && last_reply[11] == 2 && last_reply[12] == 3);
    reflash_calls = 0;
    margin_rows = margin_dirty = margin_max_row = 0;
    uint8_t view_set[] = { 0xF0, 0x14, 0x00, 0xF8, 0xFF, 0x23, 0x00 };   /* dx -8, dy +35 */
    CHECK(send_packet(view_set, sizeof view_set) == 0 && last_reply[2] == CFW_OK);
    /* The 280 panel rows outside the moved canvas (origin 210 + 35) are blanked, then the frame is pushed. */
    CHECK(margin_rows == 280 && !margin_dirty && margin_max_row == 479);
    CHECK(reflash_calls == 281 && last_rect[0] == 0 && last_rect[1] == 245 && last_rect[2] == 0 && last_rect[4] == 640 && last_rect[5] == 199);
    CHECK(last_reply[3] == 0xF8 && last_reply[4] == 0xFF && last_reply[5] == 0x23 && last_reply[9] == 210);
    /* A panel power-up parks frame row 0 beside the view state, not over it (G1CFW/37 regression). */
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0;
    cfw_work_mode(3);
    CHECK(*(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) == 1);
    CHECK(send_packet(view_get, sizeof view_get) == 0 && last_reply[3] == 0xF8 && last_reply[5] == 0x23);
    CHECK(send_packet(mic_q_early, sizeof mic_q_early) == 0 && last_reply[2] == CFW_OK);   /* and neither is the mic state */
    uint8_t view_far[] = { 0xF0, 0x14, 0x00, 0x00, 0x00, 0xE8, 0x03 };   /* dy 1000: out of range, no change */
    CHECK(send_packet(view_far, sizeof view_far) == 0 && last_reply[2] == CFW_BAD_RECT);
    CHECK(send_packet(view_get, sizeof view_get) == 0 && last_reply[3] == 0xF8 && last_reply[5] == 0x23);
    uint8_t view_short[] = { 0xF0, 0x14, 0x00, 0x01 };
    CHECK(send_packet(view_short, sizeof view_short) == 0 && last_reply[2] == CFW_BAD_LENGTH);
    uint8_t view_zero[] = { 0xF0, 0x14, 0x00, 0, 0, 0, 0 };
    reflash_calls = 0;
    CHECK(send_packet(view_zero, sizeof view_zero) == 0 && last_reply[2] == CFW_OK && reflash_calls == 281 && last_rect[1] == 210);
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0;
    reflash_calls = 0;
    CHECK(send_packet(view_zero, sizeof view_zero) == 0 && last_reply[2] == CFW_OK && reflash_calls == 0);   /* panel off: nothing to blank */
    *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;
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
    CHECK(rd32le(last_reply + 5) == 16623 && rd32le(last_reply + 9) == -32906 && rd32le(last_reply + 13) == 1325);
    CHECK(sleep_calls == 1 && last_sleep == 50 && post_calls == 2);
    host_imu[2] = -7190;
    cfw_display_tick(state, 0x10, 0, 2);
    CHECK(last_reply[3] == 1 && rd32le(last_reply + 13) == -7190 && post_calls == 3);
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
    CHECK(send_packet(imu_stop, sizeof imu_stop) == 0 && last_reply[2] == CFW_OK && last_reply[3] == 0 && rd32le(last_reply + 7) == 8);
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

    /* A corrupt retry must not overwrite the cached execution result. Test
     * success, deferred drawing and a message with a partially applied error. */
    for (int result = 0; result < 3; result++) {
        const uint8_t *m = result == 2 ? msg2 : msg;
        unsigned n = result == 2 ? sizeof msg2 : sizeof msg;
        uint16_t sum = cfw_crc16(m, n);
        uint8_t packet[64] = {0xF0, 0x10, 30, 3, (uint8_t)n, 0, (uint8_t)sum, (uint8_t)(sum >> 8)};
        memcpy(packet + 8, m, n);
        *(int *)(panel + FW_PANEL_POWERED) = result != 1;
        CHECK(send_packet(packet, n + 8) == 0);
        uint8_t expected = result == 0 ? CFW_OK : result == 1 ? CFW_PANEL_OFF : CFW_BAD_RECT;
        CHECK(last_reply[2] == expected);
        uint8_t done = last_reply[4];
        int pushes = reflash_calls, messages = cfw_context()->stream_msgs;
        packet[6] ^= 1;
        CHECK(send_packet(packet, n + 8) == 0 && last_reply[2] == CFW_BAD_CRC);
        CHECK(cfw_context()->last_status == CFW_BAD_CRC);
        packet[6] ^= 1;
        packet[2] = 31;
        packet[3] |= CFW_STREAM_ONCE;
        CHECK(send_packet(packet, n + 8) == 0 && last_reply[2] == expected && last_reply[4] == done);
        CHECK(reflash_calls == pushes && cfw_context()->stream_msgs == messages);
    }

    /* VIEW may place only part (or none) of a canvas on the physical panel.
     * No negative/480+ row reaches the stock driver's nine-bit address mask;
     * horizontal shifting still restores every framebuffer byte. */
    *(int *)(panel + FW_PANEL_POWERED) = 1;
    *(int *)(panel + FW_PANEL_ROW_OFFSET) = 0;
    memcpy(snapshot, frame, sizeof snapshot);
    const int clips[][5] = {
        {0, 0, 200, 0, 199}, {-10, 0, 200, 10, 199},
        {400, 0, 200, 0, 79}, {-199, 0, 200, 199, 199},
        {479, 0, 200, 0, 0}, {-200, 0, 200, 0, -1},
        {480, 0, 200, 0, -1}, {-55, 50, 10, 55, 59},
        {425, 50, 10, 50, 54}, {-60, 50, 10, 0, -1}
    };
    for (unsigned i = 0; i < sizeof clips / sizeof clips[0]; i++) {
        cfw_view_state()->dy = (int16_t)clips[i][0];
        cfw_view_state()->dx = i & 1 ? -8 : 8;
        cfw_rect r = {0, (uint16_t)clips[i][1], 640, (uint16_t)clips[i][2]};
        reflash_calls = 0;
        CHECK(cfw_flush(panel, &r) == CFW_OK);
        if (clips[i][4] < 0) {
            CHECK(reflash_calls == 0);
        } else {
            CHECK(reflash_calls == 1 && last_rect[3] == clips[i][3] && last_rect[5] == clips[i][4]);
            CHECK(last_rect[1] + last_rect[3] >= 0 && last_rect[1] + last_rect[5] < 480);
        }
        CHECK(memcmp(frame, snapshot, sizeof snapshot) == 0);
    }
    guards_intact();
    CHECK(host_unlocked_pushes == 0 && host_lcd_depth == 0);
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures != 0;
}
