/*
 * G1CFW/2: injected handler for the private 0xF0 command on Even G1 1.5.60.
 *
 * Compiled by tools/build_blob.py into a position-independent blob that is
 * appended to the stock application image. The stock BLE receive entry calls
 * spec_ble_command_hook(data, len) and dispatches the packet further only when
 * the hook returns non-zero; the profile redirects that call here. Packets that
 * are not ours are passed through unchanged. Stock entry points are called via
 * absolute addresses verified against the exact 1.5.60 binary (see fw_abi.h).
 */
#include <stdint.h>
#include "fw_abi.h"

#define CFW_SID 0xF0u
#define CFW_PROTOCOL 11u

enum { CFW_PING = 0x00, CFW_BLIT = 0x01, CFW_FILL = 0x02, CFW_FLUSH = 0x03, CFW_INFO = 0x04,
       CFW_PANEL = 0x05, CFW_TEXT = 0x06, CFW_COPY = 0x07, CFW_STREAM = 0x10, CFW_STATS = 0x11 };
enum { CFW_OK = 0x00, CFW_BAD_LENGTH = 0x01, CFW_BAD_COMMAND = 0x02, CFW_BAD_RECT = 0x03,
       CFW_PANEL_OFF = 0x04, CFW_BAD_SEQ = 0x05, CFW_BAD_CRC = 0x06, CFW_TOO_BIG = 0x07,
       CFW_BAD_RECORD = 0x08, CFW_BAD_RLE = 0x09, CFW_BAD_FONT = 0x0A };

/* Stock commands of the BMP upload flow whose staging RAM the CFW now owns. */
#define STOCK_BMP_DATA 0x15u
#define STOCK_BMP_CRC  0x16u
#define STOCK_BMP_END  0x20u

/* --- CFW context in the reclaimed staging buffer ---------------------- */
#define CFW_CTX_MAGIC 0x31574643u  /* "CFW1" */
#define CFW_MSG_MAX   4096u

typedef struct {
    uint32_t magic;
    uint16_t msg_len;      /* bytes assembled so far */
    uint16_t msg_total;    /* declared length of the message in flight */
    uint16_t msg_crc;      /* declared CRC-16 */
    uint8_t  msg_active;   /* a FIRST packet was seen and no LAST yet */
    uint8_t  msg_seq;      /* sequence number of the last accepted packet */
    uint16_t stream_msgs;  /* messages completed */
    uint16_t stream_errors;
    uint16_t stock_swallowed;
    uint8_t  last_status;
    uint8_t  reserved[9];
} cfw_ctx;                 /* 32 bytes, then the message buffer */

static cfw_ctx *cfw_context(void)
{
    cfw_ctx *c = (cfw_ctx *)FW_CFW_RAM;
    if (c->magic != CFW_CTX_MAGIC) {
        uint8_t *p = (uint8_t *)c;
        for (uint32_t i = 0; i < sizeof *c; i++) {
            p[i] = 0;
        }
        c->magic = CFW_CTX_MAGIC;
    }
    return c;
}

static uint8_t *cfw_msg_buf(void)
{
    return FW_CFW_RAM + sizeof(cfw_ctx);
}

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection. */
static uint16_t cfw_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

/* --- PackBits reader: n in 0..127 = n+1 literal bytes follow; n in
 * 129..255 = repeat the next byte 257-n times; 128 is a no-op. ---------- */
typedef struct {
    const uint8_t *p, *end;
    uint32_t run;      /* bytes left in the current run */
    uint8_t  repeat;   /* 1 = run repeats `value`, 0 = run is literal */
    uint8_t  value;
    uint8_t  error;
} cfw_rle;

static void cfw_rle_init(cfw_rle *r, const uint8_t *p, uint32_t n)
{
    r->p = p;
    r->end = p + n;
    r->run = 0;
    r->repeat = 0;
    r->value = 0;
    r->error = 0;
}

static uint8_t cfw_rle_next(cfw_rle *r)
{
    while (r->run == 0) {
        if (r->p >= r->end) {
            r->error = 1;
            return 0;
        }
        uint8_t n = *r->p++;
        if (n < 128) {
            r->run = n + 1u;
            r->repeat = 0;
        } else if (n > 128) {
            if (r->p >= r->end) {
                r->error = 1;
                return 0;
            }
            r->run = 257u - n;
            r->repeat = 1;
            r->value = *r->p++;
        }
    }
    r->run--;
    if (r->repeat) {
        return r->value;
    }
    if (r->p >= r->end) {
        r->error = 1;
        return 0;
    }
    return *r->p++;
}

/* BLIT flags */
#define CFW_BLIT_1BPP   0x01u   /* input is 1 bit per pixel, MSB first, rows padded to bytes */
#define CFW_FLAG_FLUSH  0x02u   /* push the rectangle to the panel after writing */
#define CFW_BLIT_RLE    0x04u   /* pixel bytes are PackBits-encoded */
#define CFW_BLIT_LEVEL(f) (((f) >> 4) ? ((f) >> 4) : 0xFu)  /* nibble for set 1bpp pixels */

typedef struct {
    uint32_t x0, y0, w, h;
} cfw_rect;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Parse [x0:u16][y0:u16][w:u16][h:u16]; pixel columns are pairs, so x0 and w
 * must be even. Everything must stay inside the 640x200 frame. */
static int cfw_parse_rect(const uint8_t *p, cfw_rect *r)
{
    r->x0 = rd16(p);
    r->y0 = rd16(p + 2);
    r->w = rd16(p + 4);
    r->h = rd16(p + 6);
    if ((r->x0 | r->w) & 1u || r->w == 0 || r->h == 0) {
        return 0;
    }
    if (r->x0 + r->w > FW_FRAME_WIDTH || r->y0 + r->h > FW_FRAME_HEIGHT) {
        return 0;
    }
    return 1;
}

static uint8_t *cfw_panel(void)
{
    return FW_GET_STATE() + FW_PANEL_OFFSET;
}

static uint8_t *cfw_row(uint8_t *panel, uint32_t y, uint32_t x)
{
    uint8_t *frame = FW_FRAME_OF(panel);
    return frame + y * FW_FRAME_STRIDE + x / 2;
}

static uint8_t cfw_flush(uint8_t *panel, const cfw_rect *r)
{
    if (*(int *)(panel + FW_PANEL_POWERED) == 0) {
        return CFW_PANEL_OFF;
    }
    int col = *(int *)(panel + FW_PANEL_COL_OFFSET);
    int row = *(int *)(panel + FW_PANEL_ROW_OFFSET);
    /* The stock routine sends (x1 - x0) / 2 bytes per row, so x1 is exclusive
     * in pixels even though y1 is inclusive. Passing x0 + w - 1 would drop the
     * last pixel pair of every row (the stock full-frame caller passes 0x27f
     * and loses columns 638-639); 640 is accepted by its clamp. */
    return FW_REFLASH_FB(col, row, (int)r->x0, (int)r->y0, (int)(r->x0 + r->w),
                         (int)(r->y0 + r->h - 1)) < 0 ? CFW_PANEL_OFF : CFW_OK;
}

/* [flags][rect][pixels]: write a rectangle into the frame buffer. */
static uint8_t cfw_blit(const uint8_t *p, uint32_t n)
{
    cfw_rect r;
    cfw_rle rle;
    if (n < 9) {
        return CFW_BAD_LENGTH;
    }
    uint8_t flags = p[0];
    if (!cfw_parse_rect(p + 1, &r)) {
        return CFW_BAD_RECT;
    }
    const uint8_t *px = p + 9;
    uint32_t stride = (flags & CFW_BLIT_1BPP) ? (r.w + 7) / 8 : r.w / 2;
    if (!(flags & CFW_BLIT_RLE) && n - 9 < stride * r.h) {
        return CFW_BAD_LENGTH;
    }
    cfw_rle_init(&rle, px, n - 9);
    uint8_t *panel = cfw_panel();
    uint8_t level = (uint8_t)CFW_BLIT_LEVEL(flags);
    uint8_t row[FW_FRAME_STRIDE];
    for (uint32_t y = 0; y < r.h; y++) {
        const uint8_t *src = px + y * stride;
        if (flags & CFW_BLIT_RLE) {
            for (uint32_t i = 0; i < stride; i++) {
                row[i] = cfw_rle_next(&rle);
            }
            if (rle.error) {
                return CFW_BAD_RLE;
            }
            src = row;
        }
        uint8_t *dst = cfw_row(panel, r.y0 + y, r.x0);
        if (flags & CFW_BLIT_1BPP) {
            for (uint32_t x = 0; x < r.w; x += 2) {
                uint8_t bits = src[x / 8] >> (6 - (x & 7));
                dst[x / 2] = (uint8_t)(((bits & 2) ? level << 4 : 0) | ((bits & 1) ? level : 0));
            }
        } else {
            for (uint32_t i = 0; i < r.w / 2; i++) {
                dst[i] = src[i];
            }
        }
    }
    return (flags & CFW_FLAG_FLUSH) ? cfw_flush(panel, &r) : CFW_OK;
}

/* [flags][rect][nibble]: fill a rectangle with one grey level. */
static uint8_t cfw_fill(const uint8_t *p, uint32_t n)
{
    cfw_rect r;
    if (n < 10) {
        return CFW_BAD_LENGTH;
    }
    if (!cfw_parse_rect(p + 1, &r)) {
        return CFW_BAD_RECT;
    }
    uint8_t *panel = cfw_panel();
    uint8_t value = (uint8_t)((p[9] & 0xF) * 0x11);
    for (uint32_t y = 0; y < r.h; y++) {
        uint8_t *dst = cfw_row(panel, r.y0 + y, r.x0);
        for (uint32_t i = 0; i < r.w / 2; i++) {
            dst[i] = value;
        }
    }
    return (p[0] & CFW_FLAG_FLUSH) ? cfw_flush(panel, &r) : CFW_OK;
}

/* [rect]: push a rectangle of the frame buffer to the panel. */
static uint8_t cfw_flush_cmd(const uint8_t *p, uint32_t n)
{
    cfw_rect r;
    if (n < 8) {
        return CFW_BAD_LENGTH;
    }
    if (!cfw_parse_rect(p, &r)) {
        return CFW_BAD_RECT;
    }
    return cfw_flush(cfw_panel(), &r);
}

static void cfw_reply(uint8_t sub, uint8_t status, const uint8_t *body, uint32_t n)
{
    uint8_t buf[20];
    uint32_t i;
    if (n > sizeof buf - 3) {
        n = sizeof buf - 3;
    }
    buf[0] = CFW_SID;
    buf[1] = sub;
    buf[2] = status;
    for (i = 0; i < n; i++) {
        buf[3 + i] = body[i];
    }
    fw_send_fn send = FW_SEND_OF(FW_GET_CTX());
    send(buf, 3 + n);
}

static void cfw_info(void)
{
    uint8_t *panel = cfw_panel();
    uint8_t body[12];
    uint32_t frame = *(uint32_t *)(panel + FW_PANEL_FRAME);
    int col = *(int *)(panel + FW_PANEL_COL_OFFSET);
    int row = *(int *)(panel + FW_PANEL_ROW_OFFSET);
    body[0] = *(int *)(panel + FW_PANEL_POWERED) != 0;
    body[1] = FW_MAX_PACKET - 1;
    body[2] = (uint8_t)col;
    body[3] = (uint8_t)(col >> 8);
    body[4] = (uint8_t)row;
    body[5] = (uint8_t)(row >> 8);
    for (int i = 0; i < 4; i++) {
        body[6 + i] = (uint8_t)(frame >> (8 * i));
    }
    body[10] = (uint8_t)(FW_FRAME_WIDTH >> 2);
    body[11] = FW_FRAME_HEIGHT;
    cfw_reply(CFW_INFO, CFW_OK, body, sizeof body);
}

/* [mode]: 1 resumes the panel through the stock system-resume path, 0 suspends
 * it; 2 calls the panel context's own on callback (bypassing the stock
 * wear/dock policy) and 3 its off callback. Reply body:
 * [power state byte][panel powered]. */
static void cfw_panel_cmd(const uint8_t *p, uint32_t n)
{
    uint8_t *state = FW_GET_STATE();
    uint8_t body[2];
    if (n < 1) {
        cfw_reply(CFW_PANEL, CFW_BAD_LENGTH, 0, 0);
        return;
    }
    if (p[0] == 1) {
        FW_SYSTEM_RESUME(state);
    } else if (p[0] == 0) {
        FW_SYSTEM_SUSPEND(state);
    } else if (p[0] == 2) {
        FW_PANEL_CB_OF(state + FW_PANEL_OFFSET, FW_PANEL_CB_ON)(state + FW_PANEL_OFFSET);
    } else if (p[0] == 3) {
        FW_PANEL_CB_OF(state + FW_PANEL_OFFSET, FW_PANEL_CB_OFF)(state + FW_PANEL_OFFSET);
    } else {
        cfw_reply(CFW_PANEL, CFW_BAD_COMMAND, 0, 0);
        return;
    }
    body[0] = state[FW_STATE_POWER_OFFSET];
    body[1] = *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) != 0;
    cfw_reply(CFW_PANEL, CFW_OK, body, sizeof body);
}

/* [font][flags][x0:u16][y0:u16][x1:u16][y1:u16][max_lines][utf8...]: draw
 * text with the stock font engine. flags bit 0 clears the box first, bit 1
 * pushes it afterwards. x0/x1 must be even (the drawer truncates odd x), and
 * the box bottom must leave room for a full glyph cell. */
#define CFW_TEXT_CLEAR 0x01u
#define CFW_TEXT_MAX   240u

static uint8_t cfw_text(const uint8_t *p, uint32_t n)
{
    static const uint8_t cell_rows[3] = { 27, 32, 39 };
    char text[CFW_TEXT_MAX + 1];
    if (n < 12) {
        return CFW_BAD_LENGTH;
    }
    uint8_t font = p[0], flags = p[1];
    uint32_t x0 = rd16(p + 2), y0 = rd16(p + 4), x1 = rd16(p + 6), y1 = rd16(p + 8);
    uint32_t lines = p[10];
    uint32_t len = n - 11;
    if (font > 2) {
        return CFW_BAD_FONT;
    }
    if (len == 0 || len > CFW_TEXT_MAX) {
        return CFW_BAD_LENGTH;
    }
    if ((x0 | x1) & 1u || x0 >= x1 || x1 > FW_FRAME_WIDTH || y0 >= y1 || lines == 0 ||
        y1 + cell_rows[font] > FW_FRAME_HEIGHT) {
        return CFW_BAD_RECT;
    }
    for (uint32_t i = 0; i < len; i++) {
        text[i] = (char)p[11 + i];
    }
    text[len] = 0;
    uint8_t *panel = cfw_panel();
    cfw_rect box = { x0, y0, x1 - x0, y1 - y0 + 1 };
    if (flags & CFW_TEXT_CLEAR) {
        for (uint32_t y = 0; y < box.h; y++) {
            uint8_t *dst = cfw_row(panel, box.y0 + y, box.x0);
            for (uint32_t i = 0; i < box.w / 2; i++) {
                dst[i] = 0;
            }
        }
    }
    FW_GUI_SET_ROWS(panel + FW_PANEL_ROWS);
    FW_GUI_MANUAL_FLUSH();
    FW_GUI_UTF_DRAW(0, text, font, (int)x0, (int)y0, (int)x1, (int)y1, lines, 0, 0, 0, 0);
    return (flags & CFW_FLAG_FLUSH) ? cfw_flush(panel, &box) : CFW_OK;
}

/* [flags][src rect][dst x:u16][dst y:u16]: copy a rectangle within the frame
 * (overlap-safe, so it can scroll). flags bit 1 pushes the destination. */
static uint8_t cfw_copy(const uint8_t *p, uint32_t n)
{
    cfw_rect src, dst;
    if (n < 13) {
        return CFW_BAD_LENGTH;
    }
    if (!cfw_parse_rect(p + 1, &src)) {
        return CFW_BAD_RECT;
    }
    dst = src;
    dst.x0 = rd16(p + 9);
    dst.y0 = rd16(p + 11);
    if (dst.x0 & 1u || dst.x0 + dst.w > FW_FRAME_WIDTH || dst.y0 + dst.h > FW_FRAME_HEIGHT) {
        return CFW_BAD_RECT;
    }
    uint8_t *panel = cfw_panel();
    uint32_t bytes = src.w / 2;
    /* Walk rows in the direction that keeps the source ahead of the write. */
    if (dst.y0 > src.y0) {
        for (uint32_t y = src.h; y-- > 0;) {
            const uint8_t *s = cfw_row(panel, src.y0 + y, src.x0);
            uint8_t *d = cfw_row(panel, dst.y0 + y, dst.x0);
            for (uint32_t i = 0; i < bytes; i++) {
                d[i] = s[i];
            }
        }
    } else {
        for (uint32_t y = 0; y < src.h; y++) {
            const uint8_t *s = cfw_row(panel, src.y0 + y, src.x0);
            uint8_t *d = cfw_row(panel, dst.y0 + y, dst.x0);
            if (dst.y0 == src.y0 && dst.x0 > src.x0) {
                for (uint32_t i = bytes; i-- > 0;) {
                    d[i] = s[i];
                }
            } else {
                for (uint32_t i = 0; i < bytes; i++) {
                    d[i] = s[i];
                }
            }
        }
    }
    return (p[0] & CFW_FLAG_FLUSH) ? cfw_flush(panel, &dst) : CFW_OK;
}

/* Run one command record (same bodies as the packet commands). */
static uint8_t cfw_run_record(uint8_t cmd, const uint8_t *p, uint32_t n)
{
    switch (cmd) {
    case CFW_BLIT:  return cfw_blit(p, n);
    case CFW_FILL:  return cfw_fill(p, n);
    case CFW_FLUSH: return cfw_flush_cmd(p, n);
    case CFW_TEXT:  return cfw_text(p, n);
    case CFW_COPY:  return cfw_copy(p, n);
    default:        return CFW_BAD_COMMAND;
    }
}

/* A message is a list of records: [cmd:u8][len:u16][payload]. Returns the
 * status of the first failing record and its index in *done. */
static uint8_t cfw_run_message(const uint8_t *m, uint32_t n, uint8_t *done)
{
    uint32_t pos = 0;
    *done = 0;
    while (pos < n) {
        if (n - pos < 3) {
            return CFW_BAD_RECORD;
        }
        uint8_t cmd = m[pos];
        uint32_t len = rd16(m + pos + 1);
        pos += 3;
        if (len > n - pos) {
            return CFW_BAD_RECORD;
        }
        uint8_t status = cfw_run_record(cmd, m + pos, len);
        if (status != CFW_OK) {
            return status;
        }
        pos += len;
        (*done)++;
    }
    return CFW_OK;
}

#define CFW_STREAM_FIRST 0x01u
#define CFW_STREAM_LAST  0x02u

/* STREAM packet: [seq][flags][data]. FIRST data starts with [total:u16][crc:u16].
 * Only the LAST packet (or an error) is answered: [status][seq][records done]. */
static void cfw_stream(const uint8_t *p, uint32_t n)
{
    cfw_ctx *c = cfw_context();
    uint8_t *buf = cfw_msg_buf();
    uint8_t body[2] = { 0, 0 };
    uint8_t status = CFW_OK;
    if (n < 2) {
        cfw_reply(CFW_STREAM, CFW_BAD_LENGTH, body, sizeof body);
        return;
    }
    uint8_t seq = p[0], flags = p[1];
    const uint8_t *data = p + 2;
    uint32_t len = n - 2;
    body[0] = seq;
    if (flags & CFW_STREAM_FIRST) {
        if (len < 4) {
            status = CFW_BAD_LENGTH;
        } else {
            c->msg_total = rd16(data);
            c->msg_crc = rd16(data + 2);
            c->msg_len = 0;
            c->msg_active = 1;
            data += 4;
            len -= 4;
            if (c->msg_total > CFW_MSG_MAX) {
                status = CFW_TOO_BIG;
            }
        }
    } else if (!c->msg_active) {
        status = CFW_BAD_SEQ;
    } else if ((uint8_t)(c->msg_seq + 1) != seq) {
        status = CFW_BAD_SEQ;
    }
    if (status == CFW_OK) {
        if (len > (uint32_t)(c->msg_total - c->msg_len)) {
            status = CFW_TOO_BIG;
        } else {
            for (uint32_t i = 0; i < len; i++) {
                buf[c->msg_len + i] = data[i];
            }
            c->msg_len = (uint16_t)(c->msg_len + len);
            c->msg_seq = seq;
        }
    }
    if (status == CFW_OK && !(flags & CFW_STREAM_LAST)) {
        return;  /* more packets follow; no reply keeps the link pipelined */
    }
    if (status == CFW_OK) {
        c->msg_active = 0;
        if (c->msg_len != c->msg_total) {
            status = CFW_BAD_LENGTH;
        } else if (cfw_crc16(buf, c->msg_len) != c->msg_crc) {
            status = CFW_BAD_CRC;
        } else {
            status = cfw_run_message(buf, c->msg_len, &body[1]);
            c->stream_msgs++;
        }
    }
    if (status != CFW_OK) {
        c->msg_active = 0;
        c->stream_errors++;
    }
    c->last_status = status;
    cfw_reply(CFW_STREAM, status, body, sizeof body);
}

static void cfw_stats(void)
{
    cfw_ctx *c = cfw_context();
    uint8_t body[12];
    body[0] = (uint8_t)c->stream_msgs;
    body[1] = (uint8_t)(c->stream_msgs >> 8);
    body[2] = (uint8_t)c->stream_errors;
    body[3] = (uint8_t)(c->stream_errors >> 8);
    body[4] = (uint8_t)c->stock_swallowed;
    body[5] = (uint8_t)(c->stock_swallowed >> 8);
    body[6] = c->last_status;
    body[7] = c->msg_active;
    body[8] = (uint8_t)c->msg_len;
    body[9] = (uint8_t)(c->msg_len >> 8);
    body[10] = (uint8_t)(CFW_MSG_MAX >> 8);
    body[11] = (uint8_t)(FW_CFW_RAM_SIZE >> 8);
    cfw_reply(CFW_STATS, CFW_OK, body, sizeof body);
}

static void cfw_ping(void)
{
    uint8_t id[16];
    uint32_t n = 0, rev = CFW_PROTOCOL;
    const char *prefix = "G1CFW/";
    while (*prefix) {
        id[n++] = (uint8_t)*prefix++;
    }
    if (rev >= 10) {
        id[n++] = (uint8_t)('0' + rev / 10);
    }
    id[n++] = (uint8_t)('0' + rev % 10);
    id[n++] = 0;
    id[n++] = FW_BASE_MAJOR;
    id[n++] = FW_BASE_MINOR;
    id[n++] = FW_BASE_PATCH;
    cfw_reply(CFW_PING, CFW_OK, id, n);
}

int cfw_rx(const uint8_t *data, uint32_t len)
{
    if (len >= 1 && (data[0] == STOCK_BMP_DATA || data[0] == STOCK_BMP_CRC || data[0] == STOCK_BMP_END)) {
        cfw_context()->stock_swallowed++;  /* the stock flow would write our RAM */
        return 0;
    }
    if (len < 1 || data[0] != CFW_SID) {
        return FW_SPEC_BLE_COMMAND_HOOK(data, len);
    }
    if (len < 2) {
        cfw_reply(0xFF, CFW_BAD_LENGTH, 0, 0);
        return 0;
    }
    switch (data[1]) {
    case CFW_PING:
        cfw_ping();
        break;
    case CFW_INFO:
        cfw_info();
        break;
    case CFW_BLIT:
        cfw_reply(CFW_BLIT, cfw_blit(data + 2, len - 2), 0, 0);
        break;
    case CFW_FILL:
        cfw_reply(CFW_FILL, cfw_fill(data + 2, len - 2), 0, 0);
        break;
    case CFW_FLUSH:
        cfw_reply(CFW_FLUSH, cfw_flush_cmd(data + 2, len - 2), 0, 0);
        break;
    case CFW_TEXT:
        cfw_reply(CFW_TEXT, cfw_text(data + 2, len - 2), 0, 0);
        break;
    case CFW_COPY:
        cfw_reply(CFW_COPY, cfw_copy(data + 2, len - 2), 0, 0);
        break;
    case CFW_PANEL:
        cfw_panel_cmd(data + 2, len - 2);
        break;
    case CFW_STREAM:
        cfw_stream(data + 2, len - 2);
        break;
    case CFW_STATS:
        cfw_stats();
        break;
    default:
        cfw_reply(data[1], CFW_BAD_COMMAND, 0, 0);
        break;
    }
    return 0;  /* consumed; never reaches the stock dispatcher */
}
