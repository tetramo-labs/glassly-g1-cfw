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
#define CFW_PROTOCOL 39u

enum { CFW_PING = 0x00, CFW_BLIT = 0x01, CFW_FILL = 0x02, CFW_FLUSH = 0x03, CFW_INFO = 0x04,
       CFW_PANEL = 0x05, CFW_TEXT = 0x06, CFW_COPY = 0x07, CFW_ASSET_PUT = 0x08,
       CFW_ASSET_DRAW = 0x09, CFW_ASSET_TEXT = 0x0A, CFW_ASSET_CLEAR = 0x0B, CFW_ASSET_INFO = 0x0C,
       CFW_SHAPES = 0x0D, CFW_STREAM = 0x10, CFW_STATS = 0x11, CFW_PEEK = 0x12,
       CFW_OBJ_PUT = 0x20, CFW_OBJ_SHOW = 0x21, CFW_OBJ_HIDE = 0x22, CFW_OBJ_STATE = 0x23,
       CFW_OBJ_CONTROL = 0x24, CFW_OBJ_ANIM = 0x25, CFW_ANIM_STATE = 0x26,
       CFW_VIEW = 0x14,
       CFW_INPUT_CONTROL = 0x30, CFW_INPUT_STATE = 0x31, CFW_INPUT_EVENT = 0x32,
       CFW_IMU_CONTROL = 0x33, CFW_IMU_SAMPLE = 0x34, CFW_MIC_CONTROL = 0x40 };
enum { CFW_OK = 0x00, CFW_BAD_LENGTH = 0x01, CFW_BAD_COMMAND = 0x02, CFW_BAD_RECT = 0x03,
       CFW_PANEL_OFF = 0x04, CFW_BAD_SEQ = 0x05, CFW_BAD_CRC = 0x06, CFW_TOO_BIG = 0x07,
       CFW_BAD_RECORD = 0x08, CFW_BAD_RLE = 0x09, CFW_BAD_FONT = 0x0A, CFW_NO_MEMORY = 0x0B,
       CFW_NO_ASSET = 0x0C, CFW_BAD_ASSET = 0x0D, CFW_NO_OBJECT = 0x0E };

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
    uint8_t  asset_count;  /* directory entries in use */
    uint32_t arena;        /* heap arena base (0 until first PUT) */
    uint32_t arena_used;   /* bump pointer */
    uint32_t arena_size;   /* bytes obtained from the stock heap */
} cfw_ctx;                 /* 32 bytes, then the message buffer, then the directory */

/* --- asset store: directory in the reclaimed RAM, bytes in a heap arena --- */
#define CFW_ARENA_MAX    (48u * 1024u)   /* first attempt; halves down to 8 KB */
#define CFW_ARENA_MIN    (8u * 1024u)
#define CFW_ASSET_MAX    56u   /* 16-byte entries after the 4 KB message buffer */
enum { CFW_ASSET_1BPP = 0, CFW_ASSET_4BPP = 1, CFW_ASSET_STRING = 2, CFW_ASSET_PATH = 3 };

typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint8_t  open;      /* 1 while PUT appends may follow */
    uint16_t w, h;      /* pixels for images; unused for strings */
    uint32_t bytes;     /* bytes stored so far */
    uint32_t offset;    /* into the arena */
} cfw_asset;

static cfw_asset *cfw_assets(void)
{
    return (cfw_asset *)(FW_CFW_RAM + sizeof(cfw_ctx) + CFW_MSG_MAX);
}

static cfw_asset *cfw_asset_find(cfw_ctx *c, uint16_t id)
{
    cfw_asset *a = cfw_assets();
    for (uint32_t i = 0; i < c->asset_count; i++) {
        if (a[i].id == id) {
            return &a[i];
        }
    }
    return 0;
}

static uint8_t *cfw_asset_bytes(cfw_ctx *c, const cfw_asset *a)
{
    return FW_PTR_OF(c->arena) + a->offset;
}

static uint32_t cfw_asset_expected(const cfw_asset *a)
{
    if (a->kind == CFW_ASSET_1BPP) {
        return ((a->w + 7u) / 8u) * a->h;
    }
    if (a->kind == CFW_ASSET_4BPP) {
        return (a->w / 2u) * a->h;
    }
    return a->bytes;  /* strings: whatever was uploaded */
}

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

/* --- view: where the 640x200 frame lands on the panel -----------------------
 * The stock UI realises the phone's height gear as the panel row origin
 * ((8 - gear) * 35 rows; both temples read 210 = gear 2) and the depth gear
 * as an opposite horizontal shift of the two lenses' content (pos_x table in
 * cal_panel_canvas_coord: gears 1..9 -> 22, 8, 0, -4, -8, -10, -12, -14, -16,
 * negated on the master temple). The panel's column space is exactly 640
 * wide (a column origin clips one edge and wraps the other), so `dy` moves
 * the row origin while `dx` shifts the content: every push temporarily
 * shifts the affected frame rows by dx, blanks the strip the canvas no
 * longer covers, pushes, and restores the rows. Drawing stays in canvas
 * coordinates. State lives in the second RAM region after the input state;
 * the per-row edge bytes saved during a shifted push live in the third. */
typedef struct {
    uint32_t magic;
    int16_t  dx, dy;           /* dx: even, |dx| <= CFW_VIEW_DX_MAX content shift; dy: row origin offset */
    uint16_t flushes;          /* pushes since boot */
    uint16_t pad;
} cfw_view;
#define CFW_VIEW_MAGIC 0x32574556u  /* "VEW2" */
#define CFW_VIEW_DX_MAX 24           /* 2 * 12 saved bytes per row fit the third region */
#define CFW_VIEW_DY_MAX 480
static cfw_view *cfw_view_state(void);

/* Shift the rows of `r` by dx inside the frame and return the panel span to
 * push; `undo` reverses it. Saved edge bytes: 2 * |dx| / 2 per row. */
static void cfw_view_shift(uint8_t *frame, const cfw_rect *r, int dx, int *px0, int *px1, int undo)
{
    int shift = dx / 2;
    int a = shift < 0 ? -shift : shift;
    int x0 = (int)r->x0, x1 = (int)(r->x0 + r->w);
    int p0 = x0 == 0 ? 0 : x0 + dx, p1 = x1 == FW_FRAME_WIDTH ? FW_FRAME_WIDTH : x1 + dx;
    if (p0 < 0) p0 = 0;
    if (p1 > FW_FRAME_WIDTH) p1 = FW_FRAME_WIDTH;
    *px0 = p0;
    *px1 = p1;
    if (p1 <= p0) {
        return;
    }
    int b0 = p0 / 2, b1 = p1 / 2;
    int src_lo = b0 - shift, src_hi = b1 - shift;
    if (src_lo < 0) src_lo = 0;
    if (src_hi > FW_FRAME_STRIDE) src_hi = FW_FRAME_STRIDE;
    int n = src_hi - src_lo, d_lo = src_lo + shift;
    uint8_t *save = FW_CFW_RAM3;
    for (uint32_t y = 0; y < r->h; y++) {
        uint8_t *row = frame + (r->y0 + y) * FW_FRAME_STRIDE;
        uint8_t *keep = save + y * 2 * a;
        uint32_t k = 0;
        if (!undo) {
            for (int j = b0; j < b1; j++) {          /* bytes the inverse move cannot restore */
                if (j < src_lo || j >= src_hi) keep[k++] = row[j];
            }
            if (n > 0) {
                if (shift > 0) { for (int i = n; i-- > 0;) row[d_lo + i] = row[src_lo + i]; }
                else           { for (int i = 0; i < n; i++) row[d_lo + i] = row[src_lo + i]; }
            }
            for (int j = b0; j < b1; j++) {
                if (j < d_lo || j >= d_lo + n) row[j] = 0;   /* strip the canvas no longer covers */
            }
        } else {
            if (n > 0) {
                if (shift > 0) { for (int i = 0; i < n; i++) row[src_lo + i] = row[d_lo + i]; }
                else           { for (int i = n; i-- > 0;) row[src_lo + i] = row[d_lo + i]; }
            }
            for (int j = b0; j < b1; j++) {
                if (j < src_lo || j >= src_hi) row[j] = keep[k++];
            }
        }
    }
}

static uint8_t cfw_flush(uint8_t *panel, const cfw_rect *r)
{
    if (*(int *)(panel + FW_PANEL_POWERED) == 0) {
        return CFW_PANEL_OFF;
    }
    cfw_view *v = cfw_view_state();
    v->flushes++;
    int col = *(int *)(panel + FW_PANEL_COL_OFFSET);
    int row = *(int *)(panel + FW_PANEL_ROW_OFFSET) + v->dy;
    /* The stock routine sends (x1 - x0) / 2 bytes per row, so x1 is exclusive
     * in pixels even though y1 is inclusive. Passing x0 + w - 1 would drop the
     * last pixel pair of every row (the stock full-frame caller passes 0x27f
     * and loses columns 638-639); 640 is accepted by its clamp. */
    if (v->dx == 0) {
        return FW_REFLASH_FB(col, row, (int)r->x0, (int)r->y0, (int)(r->x0 + r->w),
                             (int)(r->y0 + r->h - 1)) < 0 ? CFW_PANEL_OFF : CFW_OK;
    }
    uint8_t *frame = FW_FRAME_OF(panel);
    int px0, px1;
    cfw_view_shift(frame, r, v->dx, &px0, &px1, 0);
    int rc = 0;
    if (px1 > px0) {
        rc = FW_REFLASH_FB(col, row, px0, (int)r->y0, px1, (int)(r->y0 + r->h - 1));
    }
    cfw_view_shift(frame, r, v->dx, &px0, &px1, 1);
    return rc < 0 ? CFW_PANEL_OFF : CFW_OK;
}

/* [flags][rect] header plus a separate pixel buffer (packet or asset). */
static uint8_t cfw_blit_from(const uint8_t *hdr, const uint8_t *px, uint32_t px_len)
{
    cfw_rect r;
    cfw_rle rle;
    uint8_t flags = hdr[0];
    if (!cfw_parse_rect(hdr + 1, &r)) {
        return CFW_BAD_RECT;
    }
    uint32_t stride = (flags & CFW_BLIT_1BPP) ? (r.w + 7) / 8 : r.w / 2;
    if (!(flags & CFW_BLIT_RLE) && px_len < stride * r.h) {
        return CFW_BAD_LENGTH;
    }
    cfw_rle_init(&rle, px, px_len);
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

/* [flags][rect][pixels]: write a rectangle into the frame buffer. */
static uint8_t cfw_blit(const uint8_t *p, uint32_t n)
{
    if (n < 9) {
        return CFW_BAD_LENGTH;
    }
    return cfw_blit_from(p, p + 9, n - 9);
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
    } else if (p[0] == 4) {
        FW_PANEL_CB_OF(state + FW_PANEL_OFFSET, FW_PANEL_CB_SUSPEND)(state + FW_PANEL_OFFSET);
    } else if (p[0] == 5) {
        FW_PANEL_CB_OF(state + FW_PANEL_OFFSET, FW_PANEL_CB_RESUME)(state + FW_PANEL_OFFSET);
    } else if (p[0] == 6) {
        FW_JBD_PANEL_SUSPEND(state + FW_PANEL_OFFSET);
        *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 0;
    } else if (p[0] == 7) {
        FW_JBD_PANEL_RESUME(state + FW_PANEL_OFFSET);
        *(int *)(state + FW_PANEL_OFFSET + FW_PANEL_POWERED) = 1;
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

/* Path assets are flattened into the third CFW RAM region (see the vector section). */
#define CFW_PATH_MAX_EDGES 128u
#define CFW_PATH_HDR       4u
#define CFW_ANGLE_TURN     (360 * 16)      /* angles are degrees in Q4 */
typedef struct { int32_t x0, y0, x1, y1; } cfw_edge;
typedef struct {
    cfw_edge edges[CFW_PATH_MAX_EDGES];
    int32_t  xs[CFW_PATH_MAX_EDGES];
    int8_t   winds[CFW_PATH_MAX_EDGES];
} cfw_vwork;
_Static_assert(sizeof(cfw_vwork) <= FW_CFW_RAM3_SIZE, "third CFW RAM region: the path edge workspace");

static cfw_vwork *cfw_vector_work(void)
{
    return (cfw_vwork *)FW_CFW_RAM3;
}

static int cv_compile(cfw_vwork *w, const uint8_t *src, uint32_t len);

/* [id:u16][kind][flags][w:u16][h:u16][data...]: store an asset. flags bit 0
 * appends to the open asset with this id (same kind/size), otherwise a new
 * asset replaces any entry with the same id. Only the newest asset can grow.
 * The arena comes from the stock heap on first use. */
static uint8_t cfw_asset_put(const uint8_t *p, uint32_t n)
{
    cfw_ctx *c = cfw_context();
    if (n < 8) {
        return CFW_BAD_LENGTH;
    }
    uint16_t id = rd16(p);
    uint8_t kind = p[2], flags = p[3];
    uint32_t w = rd16(p + 4), h = rd16(p + 6);
    const uint8_t *data = p + 8;
    uint32_t len = n - 8;
    if (kind > CFW_ASSET_PATH || (kind == CFW_ASSET_PATH && ((flags & 1u) || w > 1))) {
        return CFW_BAD_ASSET;
    }
    if (kind <= CFW_ASSET_4BPP && (w == 0 || h == 0 || (w & 1u) || w > FW_FRAME_WIDTH || h > FW_FRAME_HEIGHT)) {
        return CFW_BAD_RECT;
    }
    int edges = 0;
    if (kind == CFW_ASSET_PATH) {
        /* [w] is the fill rule (0 nonzero, 1 even-odd); the data are the path
         * commands, flattened here into the workspace before anything is
         * allocated, so a bad path costs nothing. */
        edges = cv_compile(cfw_vector_work(), data, len);
        if (edges < 0) {
            return CFW_BAD_ASSET;
        }
        len = CFW_PATH_HDR + 16u * (uint32_t)edges;
    }
    if (!c->arena) {
        /* The stock heap also holds the 64 KB frame buffer and the upload
         * queue, so take the largest block it will still give us. */
        void *arena = 0;
        uint32_t size = CFW_ARENA_MAX;
        while (!arena && size >= CFW_ARENA_MIN) {
            arena = FW_MALLOC(size);
            if (!arena) {
                size -= 8u * 1024u;
            }
        }
        if (!arena) {
            return CFW_NO_MEMORY;
        }
        c->arena = FW_ADDR_OF(arena);
        c->arena_size = size;
        c->arena_used = 0;
        c->asset_count = 0;
    }
    cfw_asset *a = cfw_asset_find(c, id);
    if (flags & 1u) {
        if (!a || !a->open || a->kind != kind || a->w != w || a->h != h) {
            return CFW_BAD_ASSET;
        }
    } else {
        if (a) {
            /* Replace: drop it from the directory; its bytes are reclaimed only
             * if it was the newest asset. */
            cfw_asset *last = &cfw_assets()[c->asset_count - 1];
            if (a == last) {
                c->arena_used = a->offset;
            }
            for (cfw_asset *q = a; q < last; q++) {
                *q = q[1];
            }
            c->asset_count--;
        }
        if (c->asset_count >= CFW_ASSET_MAX) {
            return CFW_NO_MEMORY;
        }
        if (c->asset_count) {
            cfw_assets()[c->asset_count - 1].open = 0;
        }
        a = &cfw_assets()[c->asset_count++];
        a->id = id;
        a->kind = kind;
        a->open = 1;
        a->w = (uint16_t)w;
        a->h = (uint16_t)h;
        a->bytes = 0;
        a->offset = c->arena_used;
    }
    uint32_t limit = (kind == CFW_ASSET_STRING) ? CFW_TEXT_MAX : cfw_asset_expected(a);
    if (kind != CFW_ASSET_PATH && a->bytes + len > limit) {
        return CFW_BAD_ASSET;
    }
    if (c->arena_used + len > c->arena_size) {
        return CFW_NO_MEMORY;
    }
    uint8_t *dst = cfw_asset_bytes(c, a) + a->bytes;
    if (kind == CFW_ASSET_PATH) {
        const cfw_vwork *vw = cfw_vector_work();
        dst[0] = (uint8_t)edges; dst[1] = (uint8_t)(edges >> 8); dst[2] = (uint8_t)w; dst[3] = 0;
        for (int i = 0; i < edges; i++) {
            const int32_t *c4 = &vw->edges[i].x0;
            for (int k = 0; k < 4; k++) {
                uint32_t v = (uint32_t)c4[k];
                dst[CFW_PATH_HDR + 16 * i + 4 * k] = (uint8_t)v;
                dst[CFW_PATH_HDR + 16 * i + 4 * k + 1] = (uint8_t)(v >> 8);
                dst[CFW_PATH_HDR + 16 * i + 4 * k + 2] = (uint8_t)(v >> 16);
                dst[CFW_PATH_HDR + 16 * i + 4 * k + 3] = (uint8_t)(v >> 24);
            }
        }
        a->open = 0;
    } else {
        for (uint32_t i = 0; i < len; i++) {
            dst[i] = data[i];
        }
    }
    a->bytes += len;
    c->arena_used += len;
    if (kind <= CFW_ASSET_4BPP && a->bytes == limit) {
        a->open = 0;
    }
    return CFW_OK;
}

/* [id:u16][flags][x:u16][y:u16]: blit an image asset. flags as BLIT (bit 1
 * flush, high nibble grey level for 1bpp). */
static uint8_t cfw_asset_draw(const uint8_t *p, uint32_t n)
{
    cfw_ctx *c = cfw_context();
    uint8_t hdr[9];
    if (n < 7) {
        return CFW_BAD_LENGTH;
    }
    cfw_asset *a = c->arena ? cfw_asset_find(c, rd16(p)) : 0;
    if (!a) {
        return CFW_NO_ASSET;
    }
    if (a->kind == CFW_ASSET_STRING || a->bytes != cfw_asset_expected(a)) {
        return CFW_BAD_ASSET;
    }
    /* Reuse the BLIT parser on a synthetic header followed by the stored bytes:
     * the bytes sit in the arena, so build the header beside a pointer walk. */
    hdr[0] = (uint8_t)((p[2] & 0xF2u) | (a->kind == CFW_ASSET_1BPP ? CFW_BLIT_1BPP : 0));
    hdr[1] = p[3]; hdr[2] = p[4]; hdr[3] = p[5]; hdr[4] = p[6];
    hdr[5] = (uint8_t)a->w; hdr[6] = (uint8_t)(a->w >> 8);
    hdr[7] = (uint8_t)a->h; hdr[8] = (uint8_t)(a->h >> 8);
    return cfw_blit_from(hdr, cfw_asset_bytes(c, a), a->bytes);
}

/* [id:u16][font][flags][box][max_lines]: TEXT with a string asset. */
static uint8_t cfw_asset_text(const uint8_t *p, uint32_t n)
{
    cfw_ctx *c = cfw_context();
    uint8_t packet[11 + CFW_TEXT_MAX];
    if (n < 13) {
        return CFW_BAD_LENGTH;
    }
    cfw_asset *a = c->arena ? cfw_asset_find(c, rd16(p)) : 0;
    if (!a) {
        return CFW_NO_ASSET;
    }
    if (a->kind != CFW_ASSET_STRING || a->bytes == 0) {
        return CFW_BAD_ASSET;
    }
    for (uint32_t i = 0; i < 11; i++) {
        packet[i] = p[2 + i];
    }
    const uint8_t *str = cfw_asset_bytes(c, a);
    for (uint32_t i = 0; i < a->bytes; i++) {
        packet[11 + i] = str[i];
    }
    return cfw_text(packet, 11 + a->bytes);
}

static uint8_t cfw_asset_clear(void)
{
    cfw_ctx *c = cfw_context();
    c->asset_count = 0;
    c->arena_used = 0;
    return CFW_OK;
}

static void cfw_asset_info(void)
{
    cfw_ctx *c = cfw_context();
    uint8_t body[10];
    body[0] = c->asset_count;
    body[1] = CFW_ASSET_MAX;
    body[2] = (uint8_t)c->arena_used; body[3] = (uint8_t)(c->arena_used >> 8); body[4] = (uint8_t)(c->arena_used >> 16);
    body[5] = (uint8_t)(c->arena_size >> 10);
    body[6] = c->arena != 0;
    body[7] = (uint8_t)(c->arena >> 8); body[8] = (uint8_t)(c->arena >> 16); body[9] = (uint8_t)(c->arena >> 24);
    cfw_reply(CFW_ASSET_INFO, CFW_OK, body, sizeof body);
}

/* --- vector shapes (ported from g2flash patches/shapes.c) --------------------
 * Record: [type][flags][color][width][p0..p7 as int16 LE] = 20 bytes, types
 * 1-13 (docs/G1_CFW_PROTOCOL.md); `flags` is the object visibility bit and
 * is ignored here. Integer math throughout: a clipped pixel and a clipped
 * horizontal span are the two primitives; circles, rings and rounded corners
 * are scan-converted per row from an integer square root; polygons, wide
 * lines, curves and sectors go through one even-odd scanline filler in 1/16
 * pixel units (Q4) sampled at pixel centres, so shared edges never gap or
 * double-cover. Coordinates are clamped to +-1024 before any Q4 product so
 * everything stays within 32 bits. */
#define CFW_SHAPE_RECORD 20u
#define CFW_SHAPE_PARAMS 8u
enum { CFW_SHAPE_LINE = 1, CFW_SHAPE_RECT = 2, CFW_SHAPE_RECT_FILL = 3, CFW_SHAPE_CIRCLE = 4,
       CFW_SHAPE_CIRCLE_FILL = 5, CFW_SHAPE_TRI = 6, CFW_SHAPE_TRI_FILL = 7, CFW_SHAPE_QUAD = 8,
       CFW_SHAPE_QUAD_FILL = 9, CFW_SHAPE_BEZIER2 = 10, CFW_SHAPE_BEZIER3 = 11, CFW_SHAPE_ARC = 12,
       CFW_SHAPE_PIE = 13 };
#define CFW_Q4           16
#define CFW_COORD_MAX    1023
#define CFW_COORD_MIN    (-1024)
#define CFW_DIM_MAX      2047
#define CFW_POLY_MAX     8u
#define CFW_BEZIER_SEGS  16u
#define CFW_ARC_MAX_SEGS 72u

typedef struct {
    uint8_t *frame;
    int32_t  y0, y1;           /* rows [y0, y1) may be written */
} cfw_raster;

typedef struct {
    uint8_t type, color, width;
    int16_t p[CFW_SHAPE_PARAMS];
} cfw_shape;

static int16_t rd16s(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

/* Geometry parameters of a record type as a bit mask (p0 = bit 0); 0 for an
 * unknown type. Also the parameters a tween may change besides colour and
 * width. */
static uint32_t cfw_shape_mask(uint8_t type)
{
    switch (type) {
    case CFW_SHAPE_LINE: return 0x0Fu;
    case CFW_SHAPE_RECT: case CFW_SHAPE_RECT_FILL: case CFW_SHAPE_ARC: case CFW_SHAPE_PIE: return 0x1Fu;
    case CFW_SHAPE_CIRCLE: case CFW_SHAPE_CIRCLE_FILL: return 0x07u;
    case CFW_SHAPE_TRI: case CFW_SHAPE_TRI_FILL: case CFW_SHAPE_BEZIER2: return 0x3Fu;
    case CFW_SHAPE_QUAD: case CFW_SHAPE_QUAD_FILL: case CFW_SHAPE_BEZIER3: return 0xFFu;
    default: return 0;
    }
}

static void cfw_shape_decode(const uint8_t *rec, cfw_shape *s)
{
    s->type = rec[0];
    s->color = (uint8_t)(rec[2] & 0xF);
    s->width = rec[3];
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        s->p[k] = rd16s(rec + 4 + 2 * k);
    }
}

static uint32_t cfw_isqrt(uint32_t v)
{
    uint32_t res = 0;
    uint32_t bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* Floor division for den > 0 (C's / truncates toward zero). */
static int32_t cfw_div_floor(int32_t num, int32_t den)
{
    int32_t q = num / den;
    if ((num % den) != 0 && num < 0) q--;
    return q;
}

static int32_t cfw_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void rs_pixel(const cfw_raster *r, int32_t x, int32_t y, uint8_t c)
{
    if (x < 0 || x >= FW_FRAME_WIDTH || y < r->y0 || y >= r->y1) return;
    uint8_t *b = r->frame + y * FW_FRAME_STRIDE + (x >> 1);
    *b = (x & 1) ? (uint8_t)((*b & 0xF0) | c) : (uint8_t)((*b & 0x0F) | (c << 4));
}

/* Inclusive [x0, x1] on row y; empty when x0 > x1. */
static void rs_hspan(const cfw_raster *r, int32_t x0, int32_t x1, int32_t y, uint8_t c)
{
    if (y < r->y0 || y >= r->y1 || x0 > x1 || x1 < 0 || x0 >= FW_FRAME_WIDTH) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= FW_FRAME_WIDTH) x1 = FW_FRAME_WIDTH - 1;
    uint8_t *row = r->frame + y * FW_FRAME_STRIDE;
    if (x0 & 1) {
        row[x0 >> 1] = (uint8_t)((row[x0 >> 1] & 0xF0) | c);
        x0++;
    }
    if (x0 > x1) return;
    if (!(x1 & 1)) {
        row[x1 >> 1] = (uint8_t)((row[x1 >> 1] & 0x0F) | (c << 4));
        x1--;
    }
    if (x0 > x1) return;
    uint8_t v = (uint8_t)(c * 0x11);
    for (int32_t i = x0 >> 1; i <= (x1 >> 1); i++) row[i] = v;
}

static void rs_fill_rect(const cfw_raster *r, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c)
{
    if (w <= 0 || h <= 0) return;
    int32_t row0 = y < r->y0 ? r->y0 : y;
    int32_t row1 = y + h;
    if (row1 > r->y1) row1 = r->y1;
    for (int32_t row = row0; row < row1; row++) rs_hspan(r, x, x + w - 1, row, c);
}

/* Half-width (Q4) of a circle of radius rad16 at vertical offset dy16 from its
 * centre, or -1 when the row misses the circle. */
static int32_t rs_chord_half(int32_t rad16, int32_t dy16)
{
    if (dy16 < 0) dy16 = -dy16;
    if (dy16 >= rad16) return -1;
    uint32_t rr = (uint32_t)rad16 * (uint32_t)rad16;
    uint32_t dd = (uint32_t)dy16 * (uint32_t)dy16;
    return (int32_t)cfw_isqrt(rr - dd);
}

/* First/last pixel covered by the continuous span [a16, b16). */
static int rs_span_pixels(int32_t a16, int32_t b16, int32_t *first, int32_t *last)
{
    *first = cfw_div_floor(a16 + 7, CFW_Q4);
    *last = cfw_div_floor(b16 + 7, CFW_Q4) - 1;
    return *first <= *last;
}

/* Pixel extents of the rounded rectangle (x,y,w,h,rad) on `row`. Corners are
 * quarter circles of radius rad (clamped to half the shorter side). */
static int rs_rrect_row(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad,
                        int32_t row, int32_t *l, int32_t *rt)
{
    if (w <= 0 || h <= 0 || row < y || row >= y + h) return 0;
    int32_t maxr = (w < h ? w : h) / 2;
    if (rad > maxr) rad = maxr;
    if (rad < 0) rad = 0;
    *l = x;
    *rt = x + w - 1;
    if (rad == 0) return 1;
    int32_t dy16 = -1;
    if (row < y + rad)              dy16 = (y + rad - row) * CFW_Q4 - 8;
    else if (row >= y + h - rad)    dy16 = (row - (y + h - rad)) * CFW_Q4 + 8;
    if (dy16 < 0) return 1;
    int32_t half = rs_chord_half(rad * CFW_Q4, dy16);
    if (half < 0) return 0;
    int32_t first, last;
    int32_t a16 = x * CFW_Q4 + rad * CFW_Q4 - half;
    int32_t b16 = (x + w) * CFW_Q4 - rad * CFW_Q4 + half;
    if (!rs_span_pixels(a16, b16, &first, &last)) return 0;
    *l = first;
    *rt = last;
    return 1;
}

/* Rounded rectangle: filled when wd <= 0, else a border of width wd. */
static void rs_rrect(const cfw_raster *r, int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t rad, int32_t wd, uint8_t c)
{
    if (w <= 0 || h <= 0) return;
    int inner = wd > 0 && w > 2 * wd && h > 2 * wd;
    int32_t row0 = y < r->y0 ? r->y0 : y;
    int32_t row1 = y + h;
    if (row1 > r->y1) row1 = r->y1;
    for (int32_t row = row0; row < row1; row++) {
        int32_t ol, orr, il, ir;
        if (!rs_rrect_row(x, y, w, h, rad, row, &ol, &orr)) continue;
        if (inner && rs_rrect_row(x + wd, y + wd, w - 2 * wd, h - 2 * wd, rad - wd, row, &il, &ir)) {
            rs_hspan(r, ol, il - 1, row, c);
            rs_hspan(r, ir + 1, orr, row, c);
        } else {
            rs_hspan(r, ol, orr, row, c);
        }
    }
}

/* Circle centred on pixel (cx,cy): filled when wd <= 0, else a ring of width wd. */
static void rs_circle(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t wd, uint8_t c)
{
    if (rad < 0) return;
    /* radius r + 1/2 around the pixel centre: an odd 2r+1 px diameter, so a
     * radius-1 dot is a 3x3 block and a ring of width wd is exactly wd px thick */
    int32_t rad16 = rad * CFW_Q4 + 8;
    int32_t in16 = wd > 0 && rad > wd ? (rad - wd) * CFW_Q4 + 8 : -1;
    int32_t cx16 = cx * CFW_Q4 + 8;
    for (int32_t row = cy - rad; row <= cy + rad; row++) {
        if (row < r->y0 || row >= r->y1) continue;
        int32_t dy16 = (cy - row) * CFW_Q4;
        int32_t ho = rs_chord_half(rad16, dy16);
        if (ho < 0) continue;
        int32_t of, ol;
        if (!rs_span_pixels(cx16 - ho, cx16 + ho, &of, &ol)) continue;
        int32_t hi = in16 > 0 ? rs_chord_half(in16, dy16) : -1;
        int32_t inf, inl;
        if (hi > 0 && rs_span_pixels(cx16 - hi, cx16 + hi, &inf, &inl)) {
            rs_hspan(r, of, inf - 1, row, c);
            rs_hspan(r, inl + 1, ol, row, c);
        } else {
            rs_hspan(r, of, ol, row, c);
        }
    }
}

/* Polygon with Q4 vertices, even-odd, sampled at pixel centres. */
static void rs_fill_poly_q4(const cfw_raster *r, const int32_t *px, const int32_t *py, uint32_t n, uint8_t c)
{
    if (n < 3 || n > CFW_POLY_MAX) return;
    int32_t ymin = py[0], ymax = py[0];
    for (uint32_t i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    int32_t row0 = cfw_div_floor(ymin, CFW_Q4);
    int32_t row1 = cfw_div_floor(ymax, CFW_Q4);
    if (row0 < r->y0) row0 = r->y0;
    if (row1 >= r->y1) row1 = r->y1 - 1;
    for (int32_t row = row0; row <= row1; row++) {
        int32_t yc = row * CFW_Q4 + 8;
        int32_t xs[CFW_POLY_MAX];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = (i + 1 == n) ? 0 : i + 1;
            int32_t ax = px[i], ay = py[i], bx = px[j], by = py[j];
            if (ay == by) continue;
            if (ay > by) {
                int32_t t = ax; ax = bx; bx = t;
                t = ay; ay = by; by = t;
            }
            if (yc < ay || yc >= by) continue;
            xs[k++] = ax + cfw_div_floor((yc - ay) * (bx - ax), by - ay);
        }
        for (uint32_t i = 1; i < k; i++) {          /* insertion sort */
            int32_t v = xs[i];
            uint32_t j = i;
            while (j > 0 && xs[j - 1] > v) { xs[j] = xs[j - 1]; j--; }
            xs[j] = v;
        }
        for (uint32_t i = 0; i + 1 < k; i += 2) {
            int32_t first, last;
            if (rs_span_pixels(xs[i], xs[i + 1], &first, &last)) rs_hspan(r, first, last, row, c);
        }
    }
}

static void rs_hairline(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;
    for (;;) {
        rs_pixel(r, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Line between Q4 points. Widths >= 2 become a filled quad with square caps
 * (half a width past each end), so consecutive segments of a flattened curve
 * overlap instead of leaving wedge gaps at the joints. */
static void rs_line_q4(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wd, uint8_t c)
{
    if (wd <= 1) {
        rs_hairline(r, cfw_div_floor(x0, CFW_Q4), cfw_div_floor(y0, CFW_Q4),
                    cfw_div_floor(x1, CFW_Q4), cfw_div_floor(y1, CFW_Q4), c);
        return;
    }
    int32_t dx = x1 - x0, dy = y1 - y0;
    uint32_t len = cfw_isqrt((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
    int32_t hw = wd * 8;                                 /* half width in Q4 */
    if (len == 0) {
        int32_t px[4] = { x0 - hw, x0 + hw, x0 + hw, x0 - hw };
        int32_t py[4] = { y0 - hw, y0 - hw, y0 + hw, y0 + hw };
        rs_fill_poly_q4(r, px, py, 4, c);
        return;
    }
    int32_t ex = dx * hw / (int32_t)len, ey = dy * hw / (int32_t)len;
    int32_t nx = -ey, ny = ex;
    int32_t px[4] = { x0 - ex + nx, x1 + ex + nx, x1 + ex - nx, x0 - ex - nx };
    int32_t py[4] = { y0 - ey + ny, y1 + ey + ny, y1 + ey - ny, y0 - ey - ny };
    rs_fill_poly_q4(r, px, py, 4, c);
}

static int32_t cfw_pix_q4(int32_t v)
{
    return v * CFW_Q4 + 8;
}

static void rs_line(const cfw_raster *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wd, uint8_t c)
{
    rs_line_q4(r, cfw_pix_q4(x0), cfw_pix_q4(y0), cfw_pix_q4(x1), cfw_pix_q4(y1), wd, c);
}

/* Closed polygon outline through integer vertices. */
static void rs_stroke_poly(const cfw_raster *r, const int32_t *px, const int32_t *py, uint32_t n, int32_t wd, uint8_t c)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1 == n) ? 0 : i + 1;
        rs_line(r, px[i], py[i], px[j], py[j], wd, c);
    }
}

static void rs_bezier2(const cfw_raster *r, int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                       int32_t x1, int32_t y1, int32_t wd, uint8_t c)
{
    int32_t px = cfw_pix_q4(x0), py = cfw_pix_q4(y0);
    for (uint32_t i = 1; i <= CFW_BEZIER_SEGS; i++) {
        int32_t u = (int32_t)(CFW_BEZIER_SEGS - i), t = (int32_t)i;
        int32_t nx = (u * u * x0 + 2 * u * t * cx + t * t * x1) * CFW_Q4 / 256 + 8;
        int32_t ny = (u * u * y0 + 2 * u * t * cy + t * t * y1) * CFW_Q4 / 256 + 8;
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

static void rs_bezier3(const cfw_raster *r, int32_t x0, int32_t y0, int32_t ax, int32_t ay,
                       int32_t bx, int32_t by, int32_t x1, int32_t y1, int32_t wd, uint8_t c)
{
    int32_t px = cfw_pix_q4(x0), py = cfw_pix_q4(y0);
    for (uint32_t i = 1; i <= CFW_BEZIER_SEGS; i++) {
        int32_t u = (int32_t)(CFW_BEZIER_SEGS - i), t = (int32_t)i;
        int32_t uu = u * u, tt = t * t;
        int32_t nx = (uu * u * x0 + 3 * uu * t * ax + 3 * u * tt * bx + tt * t * x1) * CFW_Q4 / 4096 + 8;
        int32_t ny = (uu * u * y0 + 3 * uu * t * ay + 3 * u * tt * by + tt * t * y1) * CFW_Q4 / 4096 + 8;
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

/* sin(deg) * 32768 for 0..90 degrees. */
static const uint16_t cfw_sin_q15[91] = {
    0, 572, 1144, 1715, 2286, 2856, 3425, 3993, 4560, 5126, 5690, 6252, 6813,
    7371, 7927, 8481, 9032, 9580, 10126, 10668, 11207, 11743, 12275, 12803, 13328, 13848,
    14365, 14876, 15384, 15886, 16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174,
    20622, 21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730, 25102, 25466,
    25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088, 28378, 28660, 28932, 29197, 29452,
    29698, 29935, 30163, 30382, 30592, 30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928,
    32052, 32166, 32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763, 32767,
};

static int32_t cfw_sin_deg(int32_t a)
{
    a %= 360;
    if (a < 0) a += 360;
    int32_t sign = 1;
    if (a >= 180) { a -= 180; sign = -1; }
    if (a > 90) a = 180 - a;
    return sign * (int32_t)cfw_sin_q15[a];
}

static int32_t cfw_cos_deg(int32_t a)
{
    return cfw_sin_deg(a + 90);
}

/* Q4 point on the circle (cx,cy,rad) at integer degrees. 0 = +x, angles grow
 * toward +y (clockwise on screen). */
static void rs_arc_point(int32_t cx, int32_t cy, int32_t rad, int32_t deg, int32_t *x, int32_t *y)
{
    int32_t r16 = rad * CFW_Q4;
    *x = cfw_pix_q4(cx) + ((r16 * cfw_cos_deg(deg) + 16384) >> 15);
    *y = cfw_pix_q4(cy) + ((r16 * cfw_sin_deg(deg) + 16384) >> 15);
}

static uint32_t rs_arc_segments(int32_t span)
{
    uint32_t n = (uint32_t)(span < 0 ? -span : span) / 6u + 1u;
    return n > CFW_ARC_MAX_SEGS ? CFW_ARC_MAX_SEGS : n;
}

static void rs_arc(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1, int32_t wd, uint8_t c)
{
    if (rad <= 0) return;
    int32_t span = cfw_clamp(a1 - a0, -360, 360);
    uint32_t n = rs_arc_segments(span);
    int32_t px, py;
    rs_arc_point(cx, cy, rad, a0, &px, &py);
    for (uint32_t i = 1; i <= n; i++) {
        int32_t nx, ny;
        rs_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &nx, &ny);
        rs_line_q4(r, px, py, nx, ny, wd, c);
        px = nx; py = ny;
    }
}

static void rs_pie(const cfw_raster *r, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1, uint8_t c)
{
    if (rad <= 0) return;
    int32_t span = cfw_clamp(a1 - a0, -360, 360);
    if (span == 0) return;
    uint32_t n = rs_arc_segments(span);
    int32_t px[3], py[3];
    px[0] = cfw_pix_q4(cx);
    py[0] = cfw_pix_q4(cy);
    rs_arc_point(cx, cy, rad, a0, &px[1], &py[1]);
    for (uint32_t i = 1; i <= n; i++) {
        rs_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &px[2], &py[2]);
        rs_fill_poly_q4(r, px, py, 3, c);
        px[1] = px[2];
        py[1] = py[2];
    }
}

/* Draw one decoded record; unknown types draw nothing. */
static void cfw_shape_draw(const cfw_raster *r, const cfw_shape *s)
{
    int32_t p[CFW_SHAPE_PARAMS];
    for (uint32_t i = 0; i < CFW_SHAPE_PARAMS; i++) {
        p[i] = cfw_clamp(s->p[i], CFW_COORD_MIN, CFW_COORD_MAX);
    }
    uint8_t c = s->color;
    int32_t wd = s->width < 1 ? 1 : s->width;
    switch (s->type) {
    case CFW_SHAPE_LINE:
        rs_line(r, p[0], p[1], p[2], p[3], wd, c);
        break;
    case CFW_SHAPE_RECT:
    case CFW_SHAPE_RECT_FILL:
        rs_rrect(r, p[0], p[1], cfw_clamp(s->p[2], 0, CFW_DIM_MAX), cfw_clamp(s->p[3], 0, CFW_DIM_MAX),
                 cfw_clamp(s->p[4], 0, CFW_DIM_MAX), s->type == CFW_SHAPE_RECT ? wd : 0, c);
        break;
    case CFW_SHAPE_CIRCLE:
    case CFW_SHAPE_CIRCLE_FILL:
        rs_circle(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX), s->type == CFW_SHAPE_CIRCLE ? wd : 0, c);
        break;
    case CFW_SHAPE_TRI:
    case CFW_SHAPE_QUAD: {
        int32_t px[4] = { p[0], p[2], p[4], p[6] };
        int32_t py[4] = { p[1], p[3], p[5], p[7] };
        rs_stroke_poly(r, px, py, s->type == CFW_SHAPE_TRI ? 3u : 4u, wd, c);
        break;
    }
    case CFW_SHAPE_TRI_FILL:
    case CFW_SHAPE_QUAD_FILL: {
        uint32_t n = s->type == CFW_SHAPE_TRI_FILL ? 3u : 4u;
        int32_t px[4], py[4];
        for (uint32_t i = 0; i < n; i++) {
            px[i] = cfw_pix_q4(p[2 * i]);
            py[i] = cfw_pix_q4(p[2 * i + 1]);
        }
        rs_fill_poly_q4(r, px, py, n, c);
        break;
    }
    case CFW_SHAPE_BEZIER2:
        rs_bezier2(r, p[0], p[1], p[2], p[3], p[4], p[5], wd, c);
        break;
    case CFW_SHAPE_BEZIER3:
        rs_bezier3(r, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], wd, c);
        break;
    case CFW_SHAPE_ARC:
        rs_arc(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX), p[3], p[4], wd, c);
        break;
    case CFW_SHAPE_PIE:
        rs_pie(r, p[0], p[1], cfw_clamp(p[2], 0, CFW_COORD_MAX), p[3], p[4], c);
        break;
    default:
        break;
    }
}

/* Frame box a record can touch (stroke, caps and rounding included), for
 * dirty-rect pushes. Returns 0 for an unknown type; w = 0 when off screen. */
static int cfw_shape_box(const cfw_shape *s, cfw_rect *r)
{
    uint32_t mask = cfw_shape_mask(s->type);
    int32_t x0, y0, x1, y1;
    int32_t m = s->width / 2 + 2;
    if (!mask) {
        return 0;
    }
    if (s->type == CFW_SHAPE_RECT || s->type == CFW_SHAPE_RECT_FILL) {
        x0 = s->p[0]; y0 = s->p[1]; x1 = x0 + s->p[2]; y1 = y0 + s->p[3];
        m = 1;
    } else if (s->type == CFW_SHAPE_CIRCLE || s->type == CFW_SHAPE_CIRCLE_FILL ||
               s->type == CFW_SHAPE_ARC || s->type == CFW_SHAPE_PIE) {
        int32_t rad = s->p[2] < 0 ? 0 : s->p[2];
        x0 = s->p[0] - rad; y0 = s->p[1] - rad; x1 = s->p[0] + rad + 1; y1 = s->p[1] + rad + 1;
    } else {
        x0 = y0 = 0x7FFF; x1 = y1 = -0x7FFF;
        for (uint32_t i = 0; i < CFW_SHAPE_PARAMS; i += 2) {
            if (!(mask & (1u << i))) continue;
            if (s->p[i] < x0) x0 = s->p[i];
            if (s->p[i] + 1 > x1) x1 = s->p[i] + 1;
            if (s->p[i + 1] < y0) y0 = s->p[i + 1];
            if (s->p[i + 1] + 1 > y1) y1 = s->p[i + 1] + 1;
        }
    }
    x0 -= m; y0 -= m; x1 += m; y1 += m;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FW_FRAME_WIDTH) x1 = FW_FRAME_WIDTH;
    if (y1 > FW_FRAME_HEIGHT) y1 = FW_FRAME_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        r->w = 0;
        return 1;
    }
    x0 &= ~1;
    x1 = (x1 + 1) & ~1;
    r->x0 = (uint32_t)x0; r->y0 = (uint32_t)y0; r->w = (uint32_t)(x1 - x0); r->h = (uint32_t)(y1 - y0);
    return 1;
}

/* SHAPES payload: [flags][count][records]. Every type is checked before a
 * pixel is written; flags bit 1 pushes the whole frame afterwards. */
static uint8_t cfw_shapes(const uint8_t *p, uint32_t n)
{
    if (n < 2) {
        return CFW_BAD_LENGTH;
    }
    uint8_t flags = p[0];
    uint32_t count = p[1];
    if (n - 2 < count * CFW_SHAPE_RECORD) {
        return CFW_BAD_LENGTH;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!cfw_shape_mask(p[2 + i * CFW_SHAPE_RECORD])) {
            return CFW_BAD_RECORD;
        }
    }
    uint8_t *panel = cfw_panel();
    cfw_raster r = { cfw_row(panel, 0, 0), 0, FW_FRAME_HEIGHT };
    for (uint32_t i = 0; i < count; i++) {
        cfw_shape s;
        cfw_shape_decode(p + 2 + i * CFW_SHAPE_RECORD, &s);
        cfw_shape_draw(&r, &s);
    }
    if (flags & CFW_FLAG_FLUSH) {
        cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
        return cfw_flush(panel, &all);
    }
    return CFW_OK;
}

/* --- filled paths and rotation (ported from g2flash patches/vector.c) -------
 * A path asset (kind 3) is uploaded as compiled commands in Q4 (1/16 px):
 * 0 M(x,y), 1 L(x,y), 2 Q(cx,cy,x,y), 3 C(c0,c1,x,y), 4 Z; every contour
 * closes with Z. ASSET_PUT flattens it into at most 128 edges stored in the
 * arena behind a [count:u16][rule][pad] header. A kind-5 object draws the
 * asset scaled (p2, Q8), translated (p0, p1) and rotated about its pivot,
 * filled nonzero or even-odd. Rotated kind-4 primitives become contours too.
 * The edge workspace is the third CFW RAM region, which the view shift only
 * uses during a push, never while a frame is being composed. */
static int cv_edge(cfw_vwork *w, uint32_t *n, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 == x1 && y0 == y1) return 1;
    if (*n >= CFW_PATH_MAX_EDGES) return 0;
    w->edges[*n].x0 = x0; w->edges[*n].y0 = y0; w->edges[*n].x1 = x1; w->edges[*n].y1 = y1;
    ++*n;
    return 1;
}

/* De Casteljau subdivision of a cubic (p0, c0, c1, p1 in Q4), at most 8 levels
 * and half a pixel of flatness. */
static int cv_curve(cfw_vwork *w, uint32_t *n, const int32_t *p, uint32_t depth)
{
    int32_t ux = 3 * p[2] - 2 * p[0] - p[6], uy = 3 * p[3] - 2 * p[1] - p[7];
    int32_t vx = 3 * p[4] - 2 * p[6] - p[0], vy = 3 * p[5] - 2 * p[7] - p[1];
    if (ux >= -16 && ux <= 16 && uy >= -16 && uy <= 16 && vx >= -16 && vx <= 16 && vy >= -16 && vy <= 16) {
        return cv_edge(w, n, p[0], p[1], p[6], p[7]);
    }
    if (depth == 8) return 0;
    int32_t a[8], b[8];
    for (uint32_t k = 0; k < 2; k++) {
        int32_t ab = (p[k] + p[k + 2]) / 2, bc = (p[k + 2] + p[k + 4]) / 2, cd = (p[k + 4] + p[k + 6]) / 2;
        int32_t abc = (ab + bc) / 2, bcd = (bc + cd) / 2, mid = (abc + bcd) / 2;
        a[k] = p[k]; a[k + 2] = ab; a[k + 4] = abc; a[k + 6] = mid;
        b[k] = mid; b[k + 2] = bcd; b[k + 4] = cd; b[k + 6] = p[k + 6];
    }
    return cv_curve(w, n, a, depth + 1) && cv_curve(w, n, b, depth + 1);
}

/* Raise a quadratic (p0, c, p1 in p[0..5]) to the cubic in p[0..7]. */
static void cv_quad_to_cubic(int32_t *p)
{
    p[6] = p[4]; p[7] = p[5];
    for (uint32_t k = 0; k < 2; k++) {
        p[k + 4] = (p[k + 6] + 2 * p[k + 2]) / 3;
        p[k + 2] = (p[k] + 2 * p[k + 2]) / 3;
    }
}

/* Flatten a command list into the workspace; returns the edge count or -1. */
static int cv_compile(cfw_vwork *w, const uint8_t *src, uint32_t len)
{
    uint32_t pos = 0, n = 0;
    int open = 0;
    int32_t x = 0, y = 0, sx = 0, sy = 0;
    if (!len) return -1;
    while (pos < len) {
        uint8_t op = src[pos++];
        if (op > 4) return -1;
        uint32_t pairs = op == 4 ? 0 : (op == 2 ? 2 : (op == 3 ? 3 : 1));
        if (len - pos < pairs * 4) return -1;
        int32_t p[8];
        p[0] = x; p[1] = y;
        for (uint32_t k = 2; k < 8; k++) p[k] = k - 2 < pairs * 2 ? rd16s(src + pos + 2 * (k - 2)) : 0;
        pos += pairs * 4;
        if (op == 0) {
            if (open) return -1;
            x = sx = p[2]; y = sy = p[3]; open = 1;
        } else {
            if (!open) return -1;
            if (op == 4) {
                if (!cv_edge(w, &n, x, y, sx, sy)) return -1;
                x = sx; y = sy; open = 0;
            } else if (op == 1) {
                if (!cv_edge(w, &n, x, y, p[2], p[3])) return -1;
                x = p[2]; y = p[3];
            } else {
                if (op == 2) cv_quad_to_cubic(p);
                if (!cv_curve(w, &n, p, 0)) return -1;
                x = p[6]; y = p[7];
            }
        }
    }
    return open ? -1 : (int)n;
}

/* sin of a Q4-degree angle, Q15, interpolated between whole degrees. */
static int32_t cv_sin(int32_t a)
{
    a %= CFW_ANGLE_TURN;
    if (a < 0) a += CFW_ANGLE_TURN;
    int32_t deg = a / 16, f = a % 16;
    int32_t v = deg == 90 ? 32768 : (deg == 270 ? -32768 : cfw_sin_deg(deg));
    int32_t next = deg + 1 == 90 ? 32768 : (deg + 1 == 270 ? -32768 : cfw_sin_deg(deg + 1));
    return v + ((next - v) * f) / 16;
}

/* Rotate every edge about the Q4 pivot; a whole number of turns is a no-op.
 * Coordinates are clamped to +-16384 so the Q15 products stay in 32 bits. */
static void cv_transform(cfw_vwork *w, uint32_t n, int32_t angle, int32_t px, int32_t py)
{
    if (angle % CFW_ANGLE_TURN == 0) return;
    int32_t sn = cv_sin(angle), cs = cv_sin(angle + 90 * 16);
    for (uint32_t i = 0; i < n; i++) {
        int32_t *c = &w->edges[i].x0;
        for (int e = 0; e < 4; e += 2) {
            int32_t dx = cfw_clamp(c[e] - px, -16384, 16384), dy = cfw_clamp(c[e + 1] - py, -16384, 16384);
            c[e] = px + ((dx * cs - dy * sn + 16384) >> 15);
            c[e + 1] = py + ((dx * sn + dy * cs + 16384) >> 15);
        }
    }
}

static void cv_swap(cfw_vwork *w, uint32_t a, uint32_t b)
{
    int32_t x = w->xs[a]; w->xs[a] = w->xs[b]; w->xs[b] = x;
    int8_t k = w->winds[a]; w->winds[a] = w->winds[b]; w->winds[b] = k;
}

static void cv_sift(cfw_vwork *w, uint32_t root, uint32_t count)
{
    while (root * 2 + 1 < count) {
        uint32_t child = root * 2 + 1;
        if (child + 1 < count && w->xs[child + 1] > w->xs[child]) child++;
        if (w->xs[root] >= w->xs[child]) break;
        cv_swap(w, root, child);
        root = child;
    }
}

/* Scanline fill of the workspace edges, nonzero (rule 0) or even-odd. */
static void cv_fill(const cfw_raster *r, cfw_vwork *w, uint32_t n, uint8_t rule, uint8_t color)
{
    int32_t ymin = FW_FRAME_HEIGHT * 16, ymax = 0;
    for (uint32_t i = 0; i < n; i++) {
        cfw_edge *e = &w->edges[i];
        for (int k = 0; k < 4; k++) {
            int32_t *c = &e->x0 + k;
            *c = cfw_clamp(*c, -16384, 16384);
        }
        if (e->y0 < ymin) ymin = e->y0;
        if (e->y1 < ymin) ymin = e->y1;
        if (e->y0 > ymax) ymax = e->y0;
        if (e->y1 > ymax) ymax = e->y1;
    }
    int32_t first = cfw_clamp(cfw_div_floor(ymin, 16), r->y0, r->y1);
    int32_t last = cfw_clamp(cfw_div_floor(ymax, 16), r->y0 - 1, r->y1 - 1);
    for (int32_t row = first; row <= last; row++) {
        int32_t yc = row * 16 + 8;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            cfw_edge e = w->edges[i];
            int8_t winding = 1;
            if (e.y0 == e.y1) continue;
            if (e.y0 > e.y1) {
                int32_t v = e.x0; e.x0 = e.x1; e.x1 = v;
                v = e.y0; e.y0 = e.y1; e.y1 = v;
                winding = -1;
            }
            if (yc < e.y0 || yc >= e.y1) continue;
            w->xs[k] = e.x0 + cfw_div_floor((yc - e.y0) * (e.x1 - e.x0), e.y1 - e.y0);
            w->winds[k++] = winding;
        }
        for (uint32_t i = k / 2; i > 0; i--) cv_sift(w, i - 1, k);
        for (uint32_t i = k; i > 1; i--) { cv_swap(w, 0, i - 1); cv_sift(w, 0, i - 1); }
        int32_t winding = 0, start = 0;
        for (uint32_t i = 0; i < k;) {
            int32_t x = w->xs[i], delta = 0;
            uint32_t hits = 0;
            do { delta += w->winds[i++]; hits++; } while (i < k && w->xs[i] == x);
            int before = rule ? (winding & 1) : (winding != 0);
            winding = rule ? (winding ^ (int32_t)(hits & 1)) : (winding + delta);
            int after = rule ? (winding & 1) : (winding != 0);
            if (!before && after) start = x;
            if (before && !after) {
                int32_t a, b;
                if (rs_span_pixels(start, x, &a, &b)) rs_hspan(r, a, b, row, color);
            }
        }
    }
}

/* Draw a compiled path asset: [count:u16][rule][pad][edges], scaled by p[2]
 * (Q8) about its origin, moved by (p[0], p[1]) pixels, rotated. */
static void cv_path_draw(const cfw_raster *r, cfw_vwork *w, const uint8_t *asset, const int16_t *p,
                         int32_t angle, int32_t px, int32_t py, uint8_t color)
{
    uint32_t count = rd16(asset);
    int32_t scale = cfw_clamp(p[2], 0, 2048);
    if (count > CFW_PATH_MAX_EDGES) count = CFW_PATH_MAX_EDGES;
    const uint8_t *src = asset + CFW_PATH_HDR;
    for (uint32_t i = 0; i < count; i++) {
        int32_t *c = &w->edges[i].x0;
        for (int k = 0; k < 4; k++) {
            int32_t v = (int32_t)((uint32_t)src[4 * k] | (uint32_t)src[4 * k + 1] << 8 | (uint32_t)src[4 * k + 2] << 16 | (uint32_t)src[4 * k + 3] << 24);
            c[k] = v * scale / 256 + (int32_t)p[k & 1] * 16;
        }
        src += 16;
    }
    cv_transform(w, count, angle, px, py);
    cv_fill(r, w, count, asset[2], color);
}

/* Arc contour in Q4 about (cx, cy): optional pie spokes, closed for a full
 * turn. 8 degrees per segment keeps a ring (two contours) and its stroke
 * within the 128-edge workspace; returns 0 when it would not fit. */
static int cv_ellipse(cfw_vwork *w, uint32_t *n, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t span, int pie)
{
    int32_t px = cx + ((rad * cv_sin((a0 + 90) * 16)) >> 15);
    int32_t py = cy + ((rad * cv_sin(a0 * 16)) >> 15);
    int32_t sx = px, sy = py;
    int ok = 1;
    if (pie) ok &= cv_edge(w, n, cx, cy, px, py);
    uint32_t seg = (uint32_t)(span < 0 ? -span : span) / 8 + 1;
    for (uint32_t i = 1; i <= seg; i++) {
        int32_t a = a0 * 16 + span * 16 * (int32_t)i / (int32_t)seg;
        int32_t nx = cx + ((rad * cv_sin(a + 90 * 16)) >> 15);
        int32_t ny = cy + ((rad * cv_sin(a)) >> 15);
        ok &= cv_edge(w, n, px, py, nx, ny);
        px = nx; py = ny;
    }
    if (pie) ok &= cv_edge(w, n, px, py, cx, cy);
    else if (span == 360 || span == -360) ok &= cv_edge(w, n, px, py, sx, sy);
    return ok;
}

/* Rounded rectangle contour, eight segments per corner. */
static int cv_roundrect(cfw_vwork *w, uint32_t *n, int32_t x, int32_t y, int32_t ww, int32_t hh, int32_t rad)
{
    if (ww <= 0 || hh <= 0) return 1;
    rad = cfw_clamp(rad, 0, (ww < hh ? ww : hh) / 2);
    int32_t cx[4] = { x + ww - rad, x + ww - rad, x + rad, x + rad };
    int32_t cy[4] = { y + rad, y + hh - rad, y + hh - rad, y + rad };
    int32_t px = x + ww - rad, py = y, sx = px, sy = py;
    int ok = 1;
    for (int k = 0; k < 4; k++) {
        for (int j = 1; j <= 8; j++) {
            int32_t a = (-90 + k * 90 + j * 90 / 8) * 16;
            int32_t nx = cx[k] + ((rad * cv_sin(a + 90 * 16)) >> 15);
            int32_t ny = cy[k] + ((rad * cv_sin(a)) >> 15);
            ok &= cv_edge(w, n, px, py, nx, ny);
            px = nx; py = ny;
        }
    }
    return ok & cv_edge(w, n, px, py, sx, sy);
}

/* A rotated primitive: contours filled even-odd, or stroked edge by edge.
 * A shape whose contours would not fit the workspace draws nothing. */
static void cv_shape_draw(const cfw_raster *r, cfw_vwork *w, const cfw_shape *s, int32_t angle, int32_t px, int32_t py)
{
    uint32_t n = 0;
    int fill = 0, ok = 1;
    int32_t p[8];
    for (uint32_t i = 0; i < 8; i++) p[i] = (int32_t)cfw_clamp(s->p[i], CFW_COORD_MIN, CFW_COORD_MAX) * 16 + 8;
    int32_t wd = s->width ? s->width : 1;
    switch (s->type) {
    case CFW_SHAPE_RECT:
    case CFW_SHAPE_RECT_FILL: {
        int32_t x = p[0] - 8, y = p[1] - 8;
        int32_t ww = cfw_clamp(s->p[2], 0, CFW_DIM_MAX) * 16, hh = cfw_clamp(s->p[3], 0, CFW_DIM_MAX) * 16;
        int32_t rad = cfw_clamp(s->p[4], 0, CFW_DIM_MAX) * 16;
        ok &= cv_roundrect(w, &n, x, y, ww, hh, rad);
        if (s->type == CFW_SHAPE_RECT) ok &= cv_roundrect(w, &n, x + wd * 16, y + wd * 16, ww - wd * 32, hh - wd * 32, rad - wd * 16);
        fill = 1;
        break;
    }
    case CFW_SHAPE_CIRCLE:
    case CFW_SHAPE_CIRCLE_FILL: {
        int32_t rad = cfw_clamp(s->p[2], 0, CFW_COORD_MAX) * 16 + 8;
        ok &= cv_ellipse(w, &n, p[0], p[1], rad, 0, 360, 0);
        if (s->type == CFW_SHAPE_CIRCLE && rad > wd * 16) ok &= cv_ellipse(w, &n, p[0], p[1], rad - wd * 16, 0, 360, 0);
        fill = 1;
        break;
    }
    case CFW_SHAPE_ARC:
    case CFW_SHAPE_PIE: {
        int32_t span = cfw_clamp((int32_t)s->p[4] - s->p[3], -360, 360);
        ok &= cv_ellipse(w, &n, p[0], p[1], cfw_clamp(s->p[2], 0, CFW_COORD_MAX) * 16, s->p[3] % 360, span, s->type == CFW_SHAPE_PIE);
        fill = s->type == CFW_SHAPE_PIE;
        break;
    }
    case CFW_SHAPE_BEZIER2:
        cv_quad_to_cubic(p);
        /* fall through */
    case CFW_SHAPE_BEZIER3:
        ok = cv_curve(w, &n, p, 0);
        break;
    default: {
        uint32_t vertices = s->type == CFW_SHAPE_LINE ? 2 : ((s->type == CFW_SHAPE_TRI || s->type == CFW_SHAPE_TRI_FILL) ? 3 : 4);
        for (uint32_t i = 1; i < vertices; i++) cv_edge(w, &n, p[2 * i - 2], p[2 * i - 1], p[2 * i], p[2 * i + 1]);
        if (vertices > 2) cv_edge(w, &n, p[2 * vertices - 2], p[2 * vertices - 1], p[0], p[1]);
        fill = s->type == CFW_SHAPE_TRI_FILL || s->type == CFW_SHAPE_QUAD_FILL;
        break;
    }
    }
    if (!ok) return;
    cv_transform(w, n, angle, px, py);
    if (fill) {
        cv_fill(r, w, n, 1, s->color);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const cfw_edge *e = &w->edges[i];
        rs_line_q4(r, cfw_clamp(e->x0, -16384, 16384), cfw_clamp(e->y0, -16384, 16384),
                   cfw_clamp(e->x1, -16384, 16384), cfw_clamp(e->y1, -16384, 16384), wd, s->color);
    }
}

typedef struct cfw_anims cfw_anims;
static cfw_anims *cfw_animation(void);

/* --- animation: tweens stepped on the display thread ---------------------- */
#define CFW_ANIM_MAX    16u
#define CFW_ANIM_VALUES 13u          /* p0..p7, level (colour), width, angle, pivot x, pivot y */
#define CFW_ANIM_LEVEL_BIT 0x100u
#define CFW_ANIM_WIDTH_BIT 0x200u
#define CFW_ANIM_ROTATE_BITS 0x1C00u   /* angle (Q4 degrees), pivot x, pivot y */
typedef struct {
    uint16_t id, mask;         /* mask: one bit per animated value */
    uint8_t  frames, frame, easing, pad;
    int16_t  from[CFW_ANIM_VALUES], to[CFW_ANIM_VALUES];
} cfw_anim;                    /* 60 bytes; finished when frame >= frames */

struct cfw_anims {
    uint32_t magic;            /* CFW_ANIM_MAGIC when valid */
    uint8_t  active;           /* tweens in flight */
    uint8_t  period_ms;
    uint8_t  count;
    uint8_t  ticking;          /* a refresh is queued or running */
    uint32_t ticks;            /* frames rendered since boot */
    uint32_t full_pushes;      /* frames that needed a whole-frame push */
    uint16_t clip_y0, clip_y1; /* row clip for composition (exclusive end); 0,0 = none */
    cfw_anim items[CFW_ANIM_MAX];
};
#define CFW_ANIM_MAGIC 0x324d4e41u  /* "ANM2" */


/* --- retained objects (g2flash modes 37-41, G1-sized) --------------------
 * Objects are small descriptors in the second RAM region; pixels come from
 * assets, the font engine or shape records. SHOW composes an ordered list of
 * object ids into a cleared frame and pushes it. */
#define CFW_OBJ_MAX    96u
#define CFW_OBJ_MAGIC  0x344a424fu  /* "OBJ4" */
enum { CFW_OBJ_IMAGE = 0, CFW_OBJ_TEXT = 1, CFW_OBJ_RECT = 2, CFW_OBJ_SHAPES = 3, CFW_OBJ_SHAPE = 4, CFW_OBJ_PATH = 5 };

typedef struct {
    uint16_t id, version;
    uint8_t  kind, flags;      /* flags: bit 0 visible */
    int16_t  p[CFW_SHAPE_PARAMS]; /* kind 4: shape parameters; kinds 0-3: p[0] x, p[1] y, p[2] w, p[3] h */
    uint16_t asset;            /* image or string asset id, or shapes asset id */
    uint8_t  level, font;      /* grey level (kind 4: colour) / text font */
    uint8_t  lines, type;      /* text lines / kind 4 record type */
    uint8_t  width, pad;       /* kind 4 stroke width */
    int16_t  angle, px, py;    /* kinds 4 and 5: rotation in Q4 degrees about the pivot (canvas pixels) */
} cfw_obj;                     /* 36 bytes */

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t revision;         /* bumps on every mutation */
    uint16_t shown[CFW_OBJ_MAX];  /* current SHOW list */
    uint16_t shown_count;
    uint16_t pad;
} cfw_objs;                    /* 8 + 192 + 4 = 204 bytes, then the objects */

static cfw_objs *cfw_objects(void)
{
    cfw_objs *o = (cfw_objs *)FW_CFW_RAM2;
    if (o->magic != CFW_OBJ_MAGIC) {
        uint8_t *p = (uint8_t *)o;
        for (uint32_t i = 0; i < sizeof *o; i++) {
            p[i] = 0;
        }
        o->magic = CFW_OBJ_MAGIC;
    }
    return o;
}

static cfw_obj *cfw_obj_table(void)
{
    return (cfw_obj *)(FW_CFW_RAM2 + sizeof(cfw_objs));
}

static cfw_obj *cfw_obj_find(cfw_objs *o, uint16_t id)
{
    cfw_obj *t = cfw_obj_table();
    for (uint32_t i = 0; i < o->count; i++) {
        if (t[i].id == id) {
            return &t[i];
        }
    }
    return 0;
}

static cfw_anims *cfw_animation(void)
{
    cfw_anims *a = (cfw_anims *)(FW_CFW_RAM2 + sizeof(cfw_objs) + CFW_OBJ_MAX * sizeof(cfw_obj));
    if (a->magic != CFW_ANIM_MAGIC) {
        uint8_t *p = (uint8_t *)a;
        for (uint32_t i = 0; i < sizeof *a; i++) {
            p[i] = 0;
        }
        a->magic = CFW_ANIM_MAGIC;
        a->period_ms = 20;   /* about 30 fps with the 12 ms per-frame floor */
    }
    return a;
}

/* The running tween of an object, if any. */
static cfw_anim *cfw_anim_find(cfw_anims *a, uint16_t id)
{
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->items[i].frame < a->items[i].frames && a->items[i].id == id) {
            return &a->items[i];
        }
    }
    return 0;
}

static void cfw_anim_cancel(uint16_t id)
{
    cfw_anim *g = cfw_anim_find(cfw_animation(), id);
    if (g) {
        g->frames = 0;
    }
}

static void cfw_anim_clear(void)
{
    cfw_anims *a = cfw_animation();
    a->count = 0;
    a->active = 0;
}

/* The tween values of an object: p0..p7, then level, then width. */
static int16_t cfw_obj_value(const cfw_obj *obj, uint32_t k)
{
    switch (k) {
    case 8:  return obj->level;
    case 9:  return obj->width;
    case 10: return obj->angle;
    case 11: return obj->px;
    case 12: return obj->py;
    default: return obj->p[k];
    }
}

static void cfw_obj_set_value(cfw_obj *obj, uint32_t k, int32_t v)
{
    switch (k) {
    case 8:  obj->level = (uint8_t)(v & 0xF); break;
    case 9:  obj->width = (uint8_t)cfw_clamp(v, 0, 255); break;
    case 10: obj->angle = (int16_t)v; break;
    case 11: obj->px = (int16_t)v; break;
    case 12: obj->py = (int16_t)v; break;
    default: obj->p[k] = (int16_t)v; break;
    }
}

/* Values a tween may change for this object (OBJ_ANIM mask bits). */
static uint32_t cfw_obj_tween_mask(const cfw_obj *obj)
{
    switch (obj->kind) {
    case CFW_OBJ_IMAGE: return 0x003u | CFW_ANIM_LEVEL_BIT;
    case CFW_OBJ_TEXT:  return 0x00Fu | CFW_ANIM_LEVEL_BIT;
    case CFW_OBJ_RECT:  return 0x00Fu | CFW_ANIM_LEVEL_BIT;
    case CFW_OBJ_SHAPE: return cfw_shape_mask(obj->type) | CFW_ANIM_LEVEL_BIT | CFW_ANIM_WIDTH_BIT | CFW_ANIM_ROTATE_BITS;
    case CFW_OBJ_PATH:  return 0x007u | CFW_ANIM_LEVEL_BIT | CFW_ANIM_ROTATE_BITS;
    default:            return 0;
    }
}

static void cfw_obj_shape(const cfw_obj *obj, cfw_shape *s)
{
    s->type = obj->type;
    s->color = (uint8_t)(obj->level & 0xF);
    s->width = obj->width;
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        s->p[k] = obj->p[k];
    }
}

/* [id:u16][version:u16][kind][flags] then, for kinds 0-3 and 5,
 * [x:i16][y:i16][w:u16][h:u16][asset:u16][level][font][lines] (19 bytes in
 * all; kind 5: w is the Q8 scale) or, for kind 4, one 20-byte SHAPES record
 * (26 bytes in all). A PUT resets the rotation. */
static uint8_t cfw_obj_put(const uint8_t *p, uint32_t n)
{
    cfw_objs *o = cfw_objects();
    if (n < 6) {
        return CFW_BAD_LENGTH;
    }
    uint16_t id = rd16(p);
    uint8_t kind = p[4];
    if (kind > CFW_OBJ_PATH) {
        return CFW_BAD_RECORD;
    }
    if (n < (kind == CFW_OBJ_SHAPE ? 6u + CFW_SHAPE_RECORD : 19u)) {
        return CFW_BAD_LENGTH;
    }
    if (kind == CFW_OBJ_SHAPE && !cfw_shape_mask(p[6])) {
        return CFW_BAD_RECORD;
    }
    cfw_obj *obj = cfw_obj_find(o, id);
    if (!obj) {
        if (o->count >= CFW_OBJ_MAX) {
            return CFW_NO_MEMORY;
        }
        obj = &cfw_obj_table()[o->count++];
    }
    obj->id = id;
    obj->version = rd16(p + 2);
    obj->kind = kind;
    obj->flags = p[5];
    for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
        obj->p[k] = 0;
    }
    obj->asset = 0;
    obj->font = obj->lines = obj->type = obj->width = obj->pad = 0;
    obj->angle = obj->px = obj->py = 0;
    if (kind == CFW_OBJ_SHAPE) {
        cfw_shape s;
        cfw_shape_decode(p + 6, &s);
        obj->type = s.type;
        obj->level = s.color;
        obj->width = s.width;
        for (uint32_t k = 0; k < CFW_SHAPE_PARAMS; k++) {
            obj->p[k] = s.p[k];
        }
    } else {
        obj->p[0] = rd16s(p + 6);
        obj->p[1] = rd16s(p + 8);
        obj->p[2] = rd16s(p + 10);
        obj->p[3] = rd16s(p + 12);
        obj->asset = rd16(p + 14);
        obj->level = p[16];
        obj->font = p[17];
        obj->lines = p[18];
    }
    cfw_anim_cancel(id);
    o->revision++;
    return CFW_OK;
}

/* Bounding box of an object for dirty-rect flushes; returns 0 when unknown. */
static int cfw_obj_bbox(const cfw_obj *obj, cfw_rect *r)
{
    int x0 = obj->p[0], y0 = obj->p[1], w = obj->p[2], h = obj->p[3];
    if (obj->kind == CFW_OBJ_SHAPE) {
        cfw_shape s;
        if (obj->angle % CFW_ANGLE_TURN) {
            return 0;
        }
        cfw_obj_shape(obj, &s);
        return cfw_shape_box(&s, r);
    }
    if (obj->kind == CFW_OBJ_PATH) {
        return 0;
    }
    if (obj->kind == CFW_OBJ_IMAGE) {
        cfw_ctx *c = cfw_context();
        cfw_asset *a = c->arena ? cfw_asset_find(c, obj->asset) : 0;
        if (!a) {
            return 0;
        }
        w = a->w;
        h = a->h;
    } else if (obj->kind == CFW_OBJ_SHAPES) {
        return 0;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    int x1 = x0 + w, y1 = y0 + h;             /* exclusive */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FW_FRAME_WIDTH) x1 = FW_FRAME_WIDTH;
    if (y1 > FW_FRAME_HEIGHT) y1 = FW_FRAME_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        r->w = 0;
        return 1;                             /* off screen: nothing to push */
    }
    x0 &= ~1;
    x1 = (x1 + 1) & ~1;
    r->x0 = (uint32_t)x0; r->y0 = (uint32_t)y0; r->w = (uint32_t)(x1 - x0); r->h = (uint32_t)(y1 - y0);
    return 1;
}

static void cfw_rect_union(cfw_rect *acc, const cfw_rect *r)
{
    if (r->w == 0) {
        return;
    }
    if (acc->w == 0) {
        *acc = *r;
        return;
    }
    uint32_t x1 = acc->x0 + acc->w > r->x0 + r->w ? acc->x0 + acc->w : r->x0 + r->w;
    uint32_t y1 = acc->y0 + acc->h > r->y0 + r->h ? acc->y0 + acc->h : r->y0 + r->h;
    if (r->x0 < acc->x0) acc->x0 = r->x0;
    if (r->y0 < acc->y0) acc->y0 = r->y0;
    acc->w = x1 - acc->x0;
    acc->h = y1 - acc->y0;
}

/* Draw one object into the frame (no flush). When the animation row clip is
 * set, images, rects and shapes are limited to those rows; text is drawn
 * whole when its box touches them (glyph cells are idempotent); a shapes
 * asset always whole. */
static uint8_t cfw_obj_render(const cfw_obj *obj)
{
    uint8_t buf[16];
    cfw_anims *an = cfw_animation();
    int clip = an->clip_y1 > an->clip_y0;
    int cy0 = clip ? an->clip_y0 : 0, cy1 = clip ? an->clip_y1 : FW_FRAME_HEIGHT;
    if (!(obj->flags & 1u)) {
        return CFW_OK;
    }
    switch (obj->kind) {
    case CFW_OBJ_IMAGE: {
        if (obj->p[0] < 0 || obj->p[1] < 0) {
            return CFW_BAD_RECT;
        }
        cfw_ctx *c = cfw_context();
        cfw_asset *a = c->arena ? cfw_asset_find(c, obj->asset) : 0;
        if (!a) {
            return CFW_NO_ASSET;
        }
        if (a->kind == CFW_ASSET_STRING || a->bytes != cfw_asset_expected(a)) {
            return CFW_BAD_ASSET;
        }
        int r0 = 0, r1 = a->h;                /* asset rows to draw */
        if (obj->p[1] + (int)a->h <= cy0 || obj->p[1] >= cy1) {
            return CFW_OK;
        }
        if (obj->p[1] < cy0) r0 = cy0 - obj->p[1];
        if (obj->p[1] + (int)a->h > cy1) r1 = cy1 - obj->p[1];
        uint32_t stride = a->kind == CFW_ASSET_1BPP ? (a->w + 7u) / 8u : a->w / 2u;
        int y = obj->p[1] + r0;
        buf[0] = (uint8_t)(((obj->level & 0xF) << 4) | (a->kind == CFW_ASSET_1BPP ? CFW_BLIT_1BPP : 0));
        buf[1] = (uint8_t)obj->p[0]; buf[2] = (uint8_t)(obj->p[0] >> 8);
        buf[3] = (uint8_t)y; buf[4] = (uint8_t)(y >> 8);
        buf[5] = (uint8_t)a->w; buf[6] = (uint8_t)(a->w >> 8);
        buf[7] = (uint8_t)(r1 - r0); buf[8] = (uint8_t)((r1 - r0) >> 8);
        return cfw_blit_from(buf, cfw_asset_bytes(c, a) + (uint32_t)r0 * stride, (uint32_t)(r1 - r0) * stride);
    }
    case CFW_OBJ_TEXT: {
        int x1 = obj->p[0] + obj->p[2], y1 = obj->p[1] + obj->p[3] - 1;
        if (obj->p[0] < 0 || obj->p[1] < 0 || x1 > FW_FRAME_WIDTH || y1 >= FW_FRAME_HEIGHT) {
            return CFW_BAD_RECT;
        }
        if (y1 < cy0 || obj->p[1] >= cy1) {
            return CFW_OK;
        }
        buf[0] = (uint8_t)obj->asset; buf[1] = (uint8_t)(obj->asset >> 8);
        buf[2] = obj->font; buf[3] = 0;
        buf[4] = (uint8_t)obj->p[0]; buf[5] = (uint8_t)(obj->p[0] >> 8);
        buf[6] = (uint8_t)obj->p[1]; buf[7] = (uint8_t)(obj->p[1] >> 8);
        buf[8] = (uint8_t)x1; buf[9] = (uint8_t)(x1 >> 8);
        buf[10] = (uint8_t)y1; buf[11] = (uint8_t)(y1 >> 8);
        buf[12] = obj->lines ? obj->lines : 1;
        uint8_t status = cfw_asset_text(buf, 13);
        if (status == CFW_OK && (obj->level & 0xF) < 15) {
            /* The stock drawer only knows full brightness: scale the box's
             * pixels by the object's level so text can fade like a shape. */
            uint32_t level = obj->level & 0xF;
            int r0 = obj->p[1] > cy0 ? obj->p[1] : cy0, r1 = y1 + 1 < cy1 ? y1 + 1 : cy1;
            for (int row = r0; row < r1; row++) {
                uint8_t *b = cfw_row(cfw_panel(), (uint32_t)row, (uint32_t)obj->p[0]);
                for (int i = 0; i < obj->p[2] / 2; i++) {
                    b[i] = (uint8_t)((((b[i] >> 4) * level / 15) << 4) | ((b[i] & 0xF) * level / 15));
                }
            }
        }
        return status;
    }
    case CFW_OBJ_RECT: {
        cfw_raster r = { cfw_row(cfw_panel(), 0, 0), cy0, cy1 };
        rs_fill_rect(&r, obj->p[0], obj->p[1], obj->p[2], obj->p[3], (uint8_t)(obj->level & 0xF));
        return CFW_OK;
    }
    case CFW_OBJ_SHAPE: {
        cfw_raster r = { cfw_row(cfw_panel(), 0, 0), cy0, cy1 };
        cfw_shape s;
        cfw_obj_shape(obj, &s);
        if (obj->angle % CFW_ANGLE_TURN) {
            cv_shape_draw(&r, cfw_vector_work(), &s, obj->angle, obj->px * 16 + 8, obj->py * 16 + 8);
        } else {
            cfw_shape_draw(&r, &s);
        }
        return CFW_OK;
    }
    case CFW_OBJ_PATH: {
        cfw_ctx *c = cfw_context();
        cfw_asset *a = c->arena ? cfw_asset_find(c, obj->asset) : 0;
        if (!a) {
            return CFW_NO_ASSET;
        }
        if (a->kind != CFW_ASSET_PATH || a->bytes < CFW_PATH_HDR) {
            return CFW_BAD_ASSET;
        }
        cfw_raster r = { cfw_row(cfw_panel(), 0, 0), cy0, cy1 };
        cv_path_draw(&r, cfw_vector_work(), cfw_asset_bytes(c, a), obj->p, obj->angle, obj->px * 16 + 8, obj->py * 16 + 8,
                     (uint8_t)(obj->level & 0xF));
        return CFW_OK;
    }
    case CFW_OBJ_SHAPES: {
        /* A shapes asset: kind 2 (string) bytes holding [count][records]. */
        cfw_ctx *c = cfw_context();
        cfw_asset *a = c->arena ? cfw_asset_find(c, obj->asset) : 0;
        if (!a || a->kind != CFW_ASSET_STRING || a->bytes < 1) {
            return CFW_NO_ASSET;
        }
        uint8_t hdr_and_body[2 + CFW_TEXT_MAX];
        hdr_and_body[0] = 0;  /* no flush */
        const uint8_t *src = cfw_asset_bytes(c, a);
        for (uint32_t i = 0; i < a->bytes; i++) {
            hdr_and_body[1 + i] = src[i];
        }
        return cfw_shapes(hdr_and_body, 1 + a->bytes);
    }
    default:
        return CFW_BAD_RECORD;
    }
}

/* [flags][count][ids:u16...]: compose the listed objects in order. flags bit 0
 * clears the frame first; bit 1 pushes the whole frame afterwards. */
static uint8_t cfw_obj_show(const uint8_t *p, uint32_t n)
{
    cfw_objs *o = cfw_objects();
    if (n < 2) {
        return CFW_BAD_LENGTH;
    }
    uint8_t flags = p[0];
    uint32_t count = p[1];
    if (count > CFW_OBJ_MAX || n - 2 < count * 2) {
        return CFW_BAD_LENGTH;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!cfw_obj_find(o, rd16(p + 2 + 2 * i))) {
            return CFW_NO_OBJECT;
        }
    }
    uint8_t *panel = cfw_panel();
    if (flags & 1u) {
        uint8_t *frame = cfw_row(panel, 0, 0);
        for (uint32_t i = 0; i < FW_FRAME_STRIDE * FW_FRAME_HEIGHT; i++) {
            frame[i] = 0;
        }
    }
    o->shown_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t id = rd16(p + 2 + 2 * i);
        o->shown[o->shown_count++] = id;
        uint8_t status = cfw_obj_render(cfw_obj_find(o, id));
        if (status != CFW_OK) {
            return status;
        }
    }
    o->revision++;
    if (flags & CFW_FLAG_FLUSH) {
        cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
        return cfw_flush(panel, &all);
    }
    return CFW_OK;
}

/* Blank the frame, forget the SHOW list and stop every tween (objects stay). */
static uint8_t cfw_obj_hide(const uint8_t *p, uint32_t n)
{
    cfw_objs *o = cfw_objects();
    uint8_t flags = n ? p[0] : CFW_FLAG_FLUSH;
    uint8_t *panel = cfw_panel();
    uint8_t *frame = cfw_row(panel, 0, 0);
    for (uint32_t i = 0; i < FW_FRAME_STRIDE * FW_FRAME_HEIGHT; i++) {
        frame[i] = 0;
    }
    o->shown_count = 0;
    o->revision++;
    cfw_anim_clear();
    if (flags & CFW_FLAG_FLUSH) {
        cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
        return cfw_flush(panel, &all);
    }
    return CFW_OK;
}

/* Reply body: [revision:u16][count][shown][id,version pairs of the first 4 shown]. */
static void cfw_obj_state(void)
{
    cfw_objs *o = cfw_objects();
    uint8_t body[4 + 4 * 4];
    uint32_t n = 4;
    body[0] = (uint8_t)o->revision; body[1] = (uint8_t)(o->revision >> 8);
    body[2] = (uint8_t)o->count;
    body[3] = (uint8_t)o->shown_count;
    for (uint32_t i = 0; i < o->shown_count && i < 4; i++) {
        cfw_obj *obj = cfw_obj_find(o, o->shown[i]);
        body[n++] = (uint8_t)obj->id; body[n++] = (uint8_t)(obj->id >> 8);
        body[n++] = (uint8_t)obj->version; body[n++] = (uint8_t)(obj->version >> 8);
    }
    cfw_reply(CFW_OBJ_STATE, CFW_OK, body, n);
}

static uint8_t cfw_obj_present(const cfw_rect *dirty);

/* [op][id:u16]: op 0 drops every object, op 1 drops one object; op 2 freezes
 * and op 3 finishes the tweens of one object, or of every object when the id
 * is 0xFFFF or absent. */
static uint8_t cfw_obj_control(const uint8_t *p, uint32_t n)
{
    cfw_objs *o = cfw_objects();
    if (n < 1) {
        return CFW_BAD_LENGTH;
    }
    if (p[0] == 2 || p[0] == 3) {
        cfw_anims *a = cfw_animation();
        uint16_t id = n >= 3 ? rd16(p + 1) : 0xFFFF;
        int finished = 0;
        for (uint32_t i = 0; i < a->count; i++) {
            cfw_anim *g = &a->items[i];
            if (g->frame >= g->frames || (id != 0xFFFF && g->id != id)) {
                continue;
            }
            cfw_obj *obj = cfw_obj_find(o, g->id);
            if (p[0] == 3 && obj) {
                for (uint32_t k = 0; k < CFW_ANIM_VALUES; k++) {
                    if (g->mask & (1u << k)) {
                        cfw_obj_set_value(obj, k, g->to[k]);
                    }
                }
                finished = 1;
            }
            g->frames = 0;
        }
        o->revision++;
        if (finished) {
            cfw_obj_present(0);
        }
        return CFW_OK;
    }
    if (p[0] == 0) {
        o->count = 0;
        o->shown_count = 0;
        o->revision++;
        cfw_anim_clear();
        return CFW_OK;
    }
    if (p[0] == 1 && n >= 3) {
        cfw_obj *obj = cfw_obj_find(o, rd16(p + 1));
        if (!obj) {
            return CFW_NO_OBJECT;
        }
        cfw_anim_cancel(obj->id);
        cfw_obj *last = &cfw_obj_table()[o->count - 1];
        for (cfw_obj *q = obj; q < last; q++) {
            uint8_t *d = (uint8_t *)q;
            const uint8_t *src = (const uint8_t *)(q + 1);
            for (uint32_t i = 0; i < sizeof *q; i++) {
                d[i] = src[i];  /* byte copy: no memcpy in the blob */
            }
        }
        o->count--;
        o->revision++;
        return CFW_OK;
    }
    return CFW_BAD_COMMAND;
}

/* Eased interpolation of `t` in [0, frames]; returns 0..1024. */
static int32_t cfw_ease(uint8_t easing, int32_t t, int32_t frames)
{
    if (frames <= 0 || t >= frames) {
        return 1024;
    }
    int32_t u = t * 1024 / frames;            /* linear 0..1024 */
    if (easing == 1) {                        /* smoothstep: 3u^2 - 2u^3 */
        return (3 * u * u - 2 * u * u / 1024 * u) / 1024;
    }
    if (easing == 2) {                        /* ease-out: 1 - (1-u)^2 */
        int32_t v = 1024 - u;
        return 1024 - v * v / 1024;
    }
    if (easing == 3) {                        /* ease-in: u^2 */
        return u * u / 1024;
    }
    return u;
}

/* Re-render the current SHOW list into a cleared frame; push `dirty` when it
 * covers every change, otherwise the whole frame. */
static uint8_t cfw_obj_present(const cfw_rect *dirty)
{
    cfw_objs *o = cfw_objects();
    cfw_anims *an = cfw_animation();
    uint8_t *panel = cfw_panel();
    uint8_t *frame = cfw_row(panel, 0, 0);
    if (dirty && dirty->w == 0) {
        return CFW_OK;                        /* nothing visible changed */
    }
    uint32_t row0 = dirty ? dirty->y0 : 0, row1 = dirty ? dirty->y0 + dirty->h : FW_FRAME_HEIGHT;
    for (uint32_t i = row0 * FW_FRAME_STRIDE; i < row1 * FW_FRAME_STRIDE; i++) {
        frame[i] = 0;
    }
    an->clip_y0 = (uint16_t)row0;
    an->clip_y1 = dirty ? (uint16_t)row1 : 0;
    for (uint32_t i = 0; i < o->shown_count; i++) {
        cfw_obj *obj = cfw_obj_find(o, o->shown[i]);
        if (obj) {
            cfw_obj_render(obj);
        }
    }
    an->clip_y0 = an->clip_y1 = 0;
    cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
    return cfw_flush(panel, dirty ? dirty : &all);
}

/* Union an object's box into `dirty`, or flag a whole-frame push. */
static void cfw_anim_touch(const cfw_obj *obj, cfw_rect *dirty, int *full)
{
    cfw_rect box;
    if (cfw_obj_bbox(obj, &box)) {
        cfw_rect_union(dirty, &box);
    } else {
        *full = 1;
    }
}

/* Advance every tween one frame; returns the number still in flight and the
 * union of the old and new boxes of what changed (w = 0 when nothing did;
 * *full set when a box is unknown). */
static uint32_t cfw_anim_step(cfw_anims *a, cfw_rect *dirty, int *full)
{
    cfw_objs *o = cfw_objects();
    uint32_t left = 0;
    dirty->x0 = dirty->y0 = dirty->w = dirty->h = 0;
    *full = 0;
    for (uint32_t i = 0; i < a->count; i++) {
        cfw_anim *g = &a->items[i];
        if (g->frame >= g->frames) {
            continue;
        }
        g->frame++;
        int32_t k = cfw_ease(g->easing, g->frame, g->frames);
        cfw_obj *obj = cfw_obj_find(o, g->id);
        if (obj) {
            cfw_anim_touch(obj, dirty, full);
            for (uint32_t v = 0; v < CFW_ANIM_VALUES; v++) {
                if (g->mask & (1u << v)) {
                    cfw_obj_set_value(obj, v, g->from[v] + ((int32_t)(g->to[v] - g->from[v]) * k) / 1024);
                }
            }
            cfw_anim_touch(obj, dirty, full);
        }
        if (g->frame < g->frames) {
            left++;
        }
    }
    o->revision++;
    return left;
}

/* --- touch input: gesture codes from touch_key_thread ---------------------
 * The stock helper that hands a gesture to key_event_thread is redirected
 * here, so every classified touch passes through the CFW. By default the
 * gesture still reaches the stock handler (dashboard, mic, sync to the other
 * temple). INPUT_CONTROL bit 0 reports each gesture to the host as an
 * unsolicited INPUT_EVENT; bit 1 keeps it from the stock handler. The state
 * lives in the second RAM region after the animation table. */
typedef struct {
    uint32_t magic;
    uint8_t  flags;         /* bits: 0 notify gestures, 1 swallow gestures, 2 notify stock events, 3 swallow stock events */
    uint8_t  last_code;
    uint16_t count;         /* gestures seen since boot */
    uint16_t stock_passed;  /* gestures handed to key_event_thread */
    uint8_t  last_event;    /* last stock event code posted */
    uint8_t  panel_autos;   /* panel switches made on wear events */
    uint16_t events;        /* stock events seen since boot */
    uint16_t events_sent;   /* stock events forwarded to the stock path */
    uint8_t  last_mode;     /* last work mode asked of change_work_mode */
    uint8_t  wear_on;       /* wear word at or above this: worn, panel on */
    uint16_t mode_changes;  /* change_work_mode entries seen since boot */
    uint16_t checks;        /* check_work_mode passes seen (one per 200 ms) */
    uint16_t suspends_skipped; /* stock display suspends the CFW refused */
    uint8_t  wear_off;      /* wear word at or below this: not worn, panel off */
    uint8_t  push_ticks;    /* 200 ms passes left until the frame is pushed again after power-up */
} cfw_input;
#define CFW_PUSH_TICKS 6u       /* pushes again 600 ms and 1.2 s after power-up */
#define CFW_INPUT_MAGIC 0x36504e49u  /* "INP6" */
/* The stock classification (>= 0x33 worn, < 0x28 not worn) leaves a pair
 * lying unfolded on a table (47-50 here) in its dead band for ever; on a
 * head the word read 62-74, folded in the case 25. Defaults sit between
 * head and table, with hysteresis. */
#define CFW_WEAR_ON_DEFAULT  56u
#define CFW_WEAR_OFF_DEFAULT 53u
#define CFW_INPUT_NOTIFY        0x01u
#define CFW_INPUT_SWALLOW       0x02u
#define CFW_INPUT_NOTIFY_EVENT  0x04u
#define CFW_INPUT_SWALLOW_EVENT 0x08u
#define CFW_INPUT_NO_AUTO_PANEL 0x10u   /* set: leave panel power to the host */
#define CFW_INPUT_FLAGS (CFW_INPUT_NOTIFY | CFW_INPUT_SWALLOW | CFW_INPUT_NOTIFY_EVENT | CFW_INPUT_SWALLOW_EVENT | CFW_INPUT_NO_AUTO_PANEL)
enum { CFW_EVENT_TOUCH = 0, CFW_EVENT_STOCK = 1 };
enum { CFW_STOCK_WORN = 6, CFW_STOCK_TAKEN_OFF = 7, CFW_STOCK_IN_CASE_OPEN = 8, CFW_STOCK_IN_CASE_CLOSED = 0x0b };

/* With the stock refresh pipeline stubbed nothing asks for the panel when
 * the glasses are put on, so the CFW switches it: on through the panel's
 * own callback (the stock resume path refuses while docked) followed by a
 * push of the frame buffer, which survives the power cycle while the panel
 * comes up blank; off again when taken off or cased. Fed by the stock
 * events while a central is connected and by every work-mode change. */
static cfw_input *cfw_input_state(void);

/* The stock panel_init no longer clears the panel memory outside the canvas
 * (fw_abi.h: FW_PANEL_MEM_ROWS), so after a power-up those rows show random
 * content. Push one blanked frame row to every row outside the canvas; the
 * row is parked in the second CFW RAM region (cfw_margin_row) and restored. */
static uint8_t *cfw_margin_row(void);
static void cfw_blank_margins(uint8_t *panel)
{
    uint8_t *row0 = FW_FRAME_OF(panel);
    uint8_t *keep = cfw_margin_row();
    int col = *(int *)(panel + FW_PANEL_COL_OFFSET);
    int top = *(int *)(panel + FW_PANEL_ROW_OFFSET) + cfw_view_state()->dy;
    for (uint32_t i = 0; i < FW_FRAME_STRIDE; i++) {
        keep[i] = row0[i];
        row0[i] = 0;
    }
    for (int y = 0; y < FW_PANEL_MEM_ROWS; y++) {
        if (y >= top && y < top + FW_FRAME_HEIGHT) {
            continue;
        }
        FW_REFLASH_FB(col, y, 0, 0, FW_FRAME_WIDTH, 0);   /* frame row 0 lands on panel row y */
    }
    for (uint32_t i = 0; i < FW_FRAME_STRIDE; i++) {
        row0[i] = keep[i];
    }
}

static void cfw_auto_panel(cfw_input *in, int on)
{
    uint8_t *panel = cfw_panel();
    int powered = *(int *)(panel + FW_PANEL_POWERED) != 0;
    if (on && !powered) {
        /* Resume callback first (what the stock silent-mode exit uses); it
         * refuses on these units, so the power-on path follows with the
         * brightness write. Both end in panel_init, whose whole-frame
         * clear is stubbed (tools/gen_profile.py) so the buffer survives
         * the power-up; the push is repeated on later ticks in case the
         * panel is not ready for the first one. */
        FW_PANEL_CB_OF(panel, FW_PANEL_CB_RESUME)(panel);
        if (*(int *)(panel + FW_PANEL_POWERED) == 0) {
            FW_PANEL_CB_OF(panel, FW_PANEL_CB_ON)(panel);
            FW_PANEL_WRITE_BRIGHTNESS(panel[FW_PANEL_BRIGHTNESS_LEVEL]);
        }
        in->panel_autos++;
        if (*(int *)(panel + FW_PANEL_POWERED) != 0) {
            cfw_blank_margins(panel);
        }
        cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
        cfw_flush(panel, &all);
        in->push_ticks = CFW_PUSH_TICKS;
    } else if (!on && powered) {
        FW_PANEL_CB_OF(panel, FW_PANEL_CB_SUSPEND)(panel);
        if (*(int *)(panel + FW_PANEL_POWERED) != 0) {
            FW_PANEL_CB_OF(panel, FW_PANEL_CB_OFF)(panel);
        }
        in->panel_autos++;
        in->push_ticks = 0;
    }
}


static cfw_input *cfw_input_state(void)
{
    cfw_input *in = (cfw_input *)(FW_CFW_RAM2 + sizeof(cfw_objs) + CFW_OBJ_MAX * sizeof(cfw_obj) + sizeof(cfw_anims));
    if (in->magic != CFW_INPUT_MAGIC) {
        uint8_t *p = (uint8_t *)in;
        for (uint32_t i = 0; i < sizeof *in; i++) {
            p[i] = 0;
        }
        in->magic = CFW_INPUT_MAGIC;
        in->wear_on = CFW_WEAR_ON_DEFAULT;
        in->wear_off = CFW_WEAR_OFF_DEFAULT;
    }
    return in;
}

/* Runs on touch_key_thread in place of the stock signal helper. */
int cfw_touch_event(void)
{
    cfw_input *in = cfw_input_state();
    uint8_t code = (uint8_t)*FW_TOUCH_EVENT;
    in->count++;
    in->last_code = code;
    if (in->flags & CFW_INPUT_NOTIFY) {
        uint8_t body[4] = { CFW_EVENT_TOUCH, code, (uint8_t)in->count, (uint8_t)(in->count >> 8) };
        cfw_reply(CFW_INPUT_EVENT, CFW_OK, body, sizeof body);
    }
    if (!(in->flags & CFW_INPUT_SWALLOW) && *FW_KEY_EVENT_SEM_READY) {
        in->stock_passed++;
        FW_K_SEM_GIVE(FW_KEY_EVENT_SEM);
    }
    return 0;
}

/* Runs in place of the stock event poster (see fw_abi.h). `code` is the
 * stock event; the two other register arguments are unused by the stock
 * body except as scratch. Re-implements the stock behaviour after noting
 * the event, unless the host asked for stock events to be swallowed. */
int cfw_stock_event(uint32_t code, uint32_t p2, uint32_t p3)
{
    (void)p2; (void)p3;
    cfw_input *in = cfw_input_state();
    in->events++;
    in->last_event = (uint8_t)code;
    if (!(in->flags & CFW_INPUT_NO_AUTO_PANEL)) {
        if (code == CFW_STOCK_WORN) {
            cfw_auto_panel(in, 1);
        } else if (code == CFW_STOCK_TAKEN_OFF || code == CFW_STOCK_IN_CASE_OPEN || code == CFW_STOCK_IN_CASE_CLOSED) {
            cfw_auto_panel(in, 0);
        }
    }
    if (in->flags & CFW_INPUT_NOTIFY_EVENT) {
        uint8_t body[4] = { CFW_EVENT_STOCK, (uint8_t)code, (uint8_t)in->events, (uint8_t)(in->events >> 8) };
        cfw_reply(CFW_INPUT_EVENT, CFW_OK, body, sizeof body);
    }
    if (in->flags & CFW_INPUT_SWALLOW_EVENT) {
        return 0;
    }
    uint8_t *ctx = FW_GET_CTX();
    if (code - 0xF0u <= 2u && *FW_EVENT_PENDING != 0xFF) {
        return 0;   /* a special-range event is still queued: stock drops this one */
    }
    *FW_EVENT_PENDING = (uint8_t)code;
    in->events_sent++;
    if (ctx[FW_CTX_EVENT_DIRECT] != 0 || *(uint32_t *)(ctx + FW_CTX_EVENT_READY) != 0) {
        uint8_t packet[3] = { 0xF5, (uint8_t)code, 0xCB };
        FW_ENQUEUE_BT_DATA(packet, sizeof packet);
        *FW_EVENT_PENDING = 0xFF;
        return 0;
    }
    FW_K_SEM_GIVE(ctx + FW_CTX_EVENT_SEM);
    FW_K_MSLEEP(1);
    return 0;
}

/* [flags] -> reply body [flags]. */
static void cfw_input_control(const uint8_t *p, uint32_t n)
{
    cfw_input *in = cfw_input_state();
    if ((n != 1 && n != 3) || (p[0] & ~CFW_INPUT_FLAGS) || (n == 3 && p[1] <= p[2])) {
        cfw_reply(CFW_INPUT_CONTROL, (n != 1 && n != 3) ? CFW_BAD_LENGTH : CFW_BAD_RECORD, 0, 0);
        return;
    }
    in->flags = p[0];
    if (n == 3) {
        /* [flags][wear_on][wear_off]: the CFW's own wear thresholds, on > off. */
        in->wear_on = p[1];
        in->wear_off = p[2];
    }
    uint8_t body[3] = { in->flags, in->wear_on, in->wear_off };
    cfw_reply(CFW_INPUT_CONTROL, CFW_OK, body, sizeof body);
}

/* Reply body: [flags][last_code][count:u16][stock_passed:u16][last_event][events:u16][events_sent:u16]
 * [panel_autos][work_mode][last_mode][mode_changes][checks][suspends_skipped] (low bytes of
 * the three counters: replies are capped at 20 bytes). */
static void cfw_input_report(void)
{
    cfw_input *in = cfw_input_state();
    uint8_t *state = FW_GET_STATE();
    uint8_t body[17] = { in->flags, in->last_code, (uint8_t)in->count, (uint8_t)(in->count >> 8),
                         (uint8_t)in->stock_passed, (uint8_t)(in->stock_passed >> 8), in->last_event,
                         (uint8_t)in->events, (uint8_t)(in->events >> 8),
                         (uint8_t)in->events_sent, (uint8_t)(in->events_sent >> 8), in->panel_autos,
                         state[FW_STATE_WORK_MODE], in->last_mode,
                         (uint8_t)in->mode_changes, (uint8_t)in->checks, (uint8_t)in->suspends_skipped };
    cfw_reply(CFW_INPUT_STATE, CFW_OK, body, sizeof body);
}

/* Runs at the entry of the stock change_work_mode (fw_abi.h) with the
 * requested mode, on whichever thread asked for the change. The work mode
 * follows the wear sensor whether or not a central is connected, while the
 * event poster above is only fed while one is, so this is what keeps the
 * panel in step with wearing: on for 2 (normal run) and 3 (normal standby,
 * worn without a stock page), off for 4 (taken off), 5 and 6 (in the case)
 * and 8 (deep sleep). 0, 1, 7 and 9 are left to the stock body. */
static void cfw_panel_for_mode(cfw_input *in, uint32_t mode)
{
    if (in->flags & CFW_INPUT_NO_AUTO_PANEL) {
        return;
    }
    if (mode == 2 || mode == 3) {
        /* Stock says worn, but its dead band is wide (fw_abi.h): classify
         * the radio-link word with the CFW thresholds. 0 means no
         * statistics yet (first seconds after boot): trust the mode. */
        uint32_t word = FW_WEAR_WORD();
        if (word == 0 || word >= in->wear_on) {
            cfw_auto_panel(in, 1);
        } else if (word <= in->wear_off) {
            cfw_auto_panel(in, 0);
        }
    } else if ((mode >= 4 && mode <= 6) || mode == 8) {
        cfw_auto_panel(in, 0);
    }
}

int cfw_work_mode(uint32_t mode)
{
    cfw_input *in = cfw_input_state();
    in->mode_changes++;
    in->last_mode = (uint8_t)mode;
    cfw_panel_for_mode(in, mode);
    return 0;
}

/* Runs at the entry of check_work_mode, once per 200 ms pass of the
 * low-speed thread, keeps the wear-detection setting on and reconciles
 * panel power with the current mode: the stock firmware also switches the
 * panel without a mode change (its display-off path suspends it in normal
 * standby), and this puts it back. */
int cfw_work_mode_tick(void)
{
    cfw_input *in = cfw_input_state();
    uint8_t *state = FW_GET_STATE();
    in->checks++;
    if (!(in->flags & CFW_INPUT_NO_AUTO_PANEL)) {
        /* With the wear-detection setting off the IPC handler reports
         * "worn" regardless of the radio link (fw_abi.h): the panel can
         * only follow the wearer while it is on. */
        state[FW_STATE_WEAR_DETECT] = 1;
    }
    cfw_panel_for_mode(in, state[FW_STATE_WORK_MODE]);
    if (in->push_ticks) {
        in->push_ticks--;
        if (in->push_ticks == CFW_PUSH_TICKS / 2 || in->push_ticks == 0) {
            uint8_t *panel = cfw_panel();
            if (*(int *)(panel + FW_PANEL_POWERED) != 0) {
                cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
                cfw_flush(panel, &all);
            }
        }
    }
    return 0;
}

/* Runs at the entry of global_system_suspend (fw_abi.h). While the CFW
 * manages the panel the stock suspend is refused outright (returns 1, the
 * detour then returns 0 to the caller): the stock display-off path would
 * otherwise cut the panel in normal standby, and the resume that pairs
 * with it is a no-op when nothing was suspended. */
int cfw_suspend_gate(uint8_t *state)
{
    (void)state;
    cfw_input *in = cfw_input_state();
    if (in->flags & CFW_INPUT_NO_AUTO_PANEL) {
        return 0;
    }
    in->suspends_skipped++;
    return 1;
}

#ifdef __thumb__
/* Detour target for the first four bytes of change_work_mode: repeats the
 * two displaced instructions, calls cfw_work_mode with the mode still in
 * r0 (r4 keeps it for the stock body), and resumes at the third. */
#define CFW_STR_(x) #x
#define CFW_STR(x) CFW_STR_(x)
__attribute__((naked)) void cfw_work_mode_entry(void)
{
    __asm__ volatile(
        "push {r4, r5, r6, lr}\n\t"
        "mov r4, r0\n\t"
        "bl cfw_work_mode\n\t"
        "mov r0, r4\n\t"
        "movw r12, #" CFW_STR(FW_CHANGE_WORK_MODE_RESUME_LO) "\n\t"
        "movt r12, #" CFW_STR(FW_CHANGE_WORK_MODE_RESUME_HI) "\n\t"
        "bx r12\n\t");
}

/* Detour target for check_work_mode: repeats the displaced push.w, keeps
 * the three register arguments across the call, resumes at the mov r4, r0. */
__attribute__((naked)) void cfw_work_mode_tick_entry(void)
{
    __asm__ volatile(
        "push.w {r4, r5, r6, r7, r8, r9, r10, lr}\n\t"
        "push {r0, r1, r2, r3}\n\t"
        "bl cfw_work_mode_tick\n\t"
        "pop {r0, r1, r2, r3}\n\t"
        "movw r12, #" CFW_STR(FW_CHECK_WORK_MODE_RESUME_LO) "\n\t"
        "movt r12, #" CFW_STR(FW_CHECK_WORK_MODE_RESUME_HI) "\n\t"
        "bx r12\n\t");
}

/* Detour target for global_system_suspend: repeats the displaced push and
 * byte load, returns 0 when the gate refuses, else resumes at mov r4, r0. */
__attribute__((naked)) void cfw_suspend_entry(void)
{
    __asm__ volatile(
        "push {r3, r4, r5, lr}\n\t"
        "mov r4, r0\n\t"
        "bl cfw_suspend_gate\n\t"
        "cbz r0, 1f\n\t"
        "movs r0, #0\n\t"
        "pop {r3, r4, r5, pc}\n\t"
        "1:\n\t"
        "mov r0, r4\n\t"
        "ldrb.w r3, [r0, #" CFW_STR(FW_STATE_POWER_OFFSET) "]\n\t"
        "movw r12, #" CFW_STR(FW_SYSTEM_SUSPEND_RESUME_LO) "\n\t"
        "movt r12, #" CFW_STR(FW_SYSTEM_SUSPEND_RESUME_HI) "\n\t"
        "bx r12\n\t");
}
#endif

static cfw_view *cfw_view_state(void)
{
    cfw_view *v = (cfw_view *)((uint8_t *)cfw_input_state() + sizeof(cfw_input));
    if (v->magic != CFW_VIEW_MAGIC) {
        uint8_t *p = (uint8_t *)v;
        for (uint32_t i = 0; i < sizeof *v; i++) {
            p[i] = 0;
        }
        v->magic = CFW_VIEW_MAGIC;
    }
    return v;
}

/* [op][dx:i16][dy:i16]: op 0 sets the offsets, blanks the panel outside the
 * new canvas and pushes the whole frame (bit 1, the old explicit push, is
 * accepted); op 1 queries. Reply body:
 * [dx:i16][dy:i16][stock_col:i16][stock_row:i16][height_gear][depth_gear][flushes:u16]. */
static void cfw_view_cmd(const uint8_t *p, uint32_t n)
{
    cfw_view *v = cfw_view_state();
    uint8_t *panel = cfw_panel();
    uint8_t status = CFW_OK;
    uint32_t want = (n >= 1 && (p[0] & 1)) ? 1u : 5u;
    if (n != want) {
        cfw_reply(CFW_VIEW, CFW_BAD_LENGTH, 0, 0);
        return;
    }
    if ((p[0] & 1) == 0) {
        int dx = rd16s(p + 1), dy = rd16s(p + 3);
        if ((dx & 1) || dx < -CFW_VIEW_DX_MAX || dx > CFW_VIEW_DX_MAX || dy < -CFW_VIEW_DY_MAX || dy > CFW_VIEW_DY_MAX) {
            cfw_reply(CFW_VIEW, CFW_BAD_RECT, 0, 0);
            return;
        }
        v->dx = (int16_t)dx;
        v->dy = (int16_t)dy;
        if (*(int *)(panel + FW_PANEL_POWERED) != 0) {
            /* The rows the canvas just left keep whatever was pushed there
             * (and a connecting host inherits the stock boot screens), so
             * blank everything outside the new canvas and push the frame. */
            cfw_blank_margins(panel);
            cfw_rect all = { 0, 0, FW_FRAME_WIDTH, FW_FRAME_HEIGHT };
            status = cfw_flush(panel, &all);
        }
    }
    int col = *(int *)(panel + FW_PANEL_COL_OFFSET), row = *(int *)(panel + FW_PANEL_ROW_OFFSET);
    uint8_t body[10] = { (uint8_t)v->dx, (uint8_t)(v->dx >> 8), (uint8_t)v->dy, (uint8_t)(v->dy >> 8),
                         (uint8_t)col, (uint8_t)(col >> 8), (uint8_t)row, (uint8_t)(row >> 8),
                         panel[FW_PANEL_HEIGHT_GEAR], panel[FW_PANEL_DEPTH_GEAR] };
    cfw_reply(CFW_VIEW, status, body, sizeof body);
}

/* --- IMU streaming: attitude samples sent from the display-thread tick ------ */
typedef struct {
    uint32_t magic;
    uint8_t  active;
    uint8_t  period_ms;
    uint16_t remaining;        /* samples left, 0xFFFF = until stopped */
    uint16_t seq;
    uint16_t pad;
    uint32_t sent;
} cfw_imu;
#define CFW_IMU_MAGIC 0x31554d49u  /* "IMU1" */

static cfw_imu *cfw_imu_state(void)
{
    cfw_imu *m = (cfw_imu *)((uint8_t *)cfw_view_state() + sizeof(cfw_view));
    if (m->magic != CFW_IMU_MAGIC) {
        uint8_t *p = (uint8_t *)m;
        for (uint32_t i = 0; i < sizeof *m; i++) {
            p[i] = 0;
        }
        m->magic = CFW_IMU_MAGIC;
        m->period_ms = 50;
    }
    return m;
}

/* One IMU_SAMPLE notification: [seq:u16][word0:i32][word1:i32][pitch:i32]. */
static void cfw_imu_sample(cfw_imu *m)
{
    uint8_t body[14];
    body[0] = (uint8_t)m->seq;
    body[1] = (uint8_t)(m->seq >> 8);
    for (uint32_t w = 0; w < 3; w++) {
        uint32_t x = (uint32_t)FW_IMU_ATTITUDE[w];
        body[2 + 4 * w] = (uint8_t)x;
        body[3 + 4 * w] = (uint8_t)(x >> 8);
        body[4 + 4 * w] = (uint8_t)(x >> 16);
        body[5 + 4 * w] = (uint8_t)(x >> 24);
    }
    cfw_reply(CFW_IMU_SAMPLE, CFW_OK, body, sizeof body);
    m->seq++;
    m->sent++;
    if (m->remaining != 0xFFFF && --m->remaining == 0) {
        m->active = 0;
    }
}

/* [period_ms][count:u16]: start streaming `count` samples (0 stops, 0xFFFF
 * runs until stopped) every `period_ms` (10-250) on the display-thread tick.
 * Reply body: [active][period_ms][remaining:u16][sent:u32]. */
static void cfw_imu_control(const uint8_t *p, uint32_t n)
{
    cfw_imu *m = cfw_imu_state();
    if (n != 3) {
        cfw_reply(CFW_IMU_CONTROL, CFW_BAD_LENGTH, 0, 0);
        return;
    }
    uint16_t count = rd16(p + 1);
    if (count == 0) {
        m->active = 0;
        m->remaining = 0;
    } else {
        if (p[0] < 10 || p[0] > 250) {
            cfw_reply(CFW_IMU_CONTROL, CFW_BAD_RECORD, 0, 0);
            return;
        }
        m->period_ms = p[0];
        m->remaining = count;
        m->active = 1;
        cfw_anims *a = cfw_animation();
        if (!a->ticking) {
            a->ticking = 1;
            FW_DISPLAY_POST(FW_SCREEN_CFW, 0, 0);
        }
    }
    uint8_t body[8] = { m->active, m->period_ms, (uint8_t)m->remaining, (uint8_t)(m->remaining >> 8),
                        (uint8_t)m->sent, (uint8_t)(m->sent >> 8), (uint8_t)(m->sent >> 16), (uint8_t)(m->sent >> 24) };
    cfw_reply(CFW_IMU_CONTROL, CFW_OK, body, sizeof body);
}

/* --- microphone: the stock PDM -> LC3 -> F1 capture session ----------------
 * [op]: 1 start, 0 stop, 2 query. Start arms a capture session on the main
 * thread the way the phone's open-mic command does; the session streams
 * [F1][seq][200 bytes LC3] notifications to the connected central until
 * stopped. Nothing stops it on disconnect, so the host must send 0 (or the
 * glasses reboot). Reply body [recording][starts:u16][stops:u16][mode][armed]. */
typedef struct {
    uint32_t magic;
    uint8_t  recording;
    uint8_t  pad0;
    uint16_t starts;
    uint16_t stops;
    uint16_t pad;
} cfw_mic;
#define CFW_MIC_MAGIC 0x3243494du  /* "MIC2" */

_Static_assert(sizeof(cfw_objs) + CFW_OBJ_MAX * sizeof(cfw_obj) + sizeof(cfw_anims) + sizeof(cfw_input) + sizeof(cfw_view)
               + sizeof(cfw_imu) + sizeof(cfw_mic) + FW_FRAME_STRIDE <= FW_CFW_RAM2_SIZE,
               "second CFW RAM region: objects, animations, input, view, IMU and mic state, then the parked margin row");

static cfw_mic *cfw_mic_state(void)
{
    cfw_mic *m = (cfw_mic *)((uint8_t *)cfw_imu_state() + sizeof(cfw_imu));
    if (m->magic != CFW_MIC_MAGIC) {
        uint8_t *p = (uint8_t *)m;
        for (uint32_t i = 0; i < sizeof *m; i++) {
            p[i] = 0;
        }
        m->magic = CFW_MIC_MAGIC;
    }
    return m;
}

/* Where frame row 0 is parked while the panel margins are blanked. */
static uint8_t *cfw_margin_row(void)
{
    return (uint8_t *)cfw_mic_state() + sizeof(cfw_mic);
}

static void cfw_mic_control(const uint8_t *p, uint32_t n)
{
    cfw_mic *m = cfw_mic_state();
    uint8_t *state = FW_GET_STATE();
    if (n != 1 || p[0] > 2) {
        cfw_reply(CFW_MIC_CONTROL, n != 1 ? CFW_BAD_LENGTH : CFW_BAD_RECORD, 0, 0);
        return;
    }
    if (p[0] == 1) {
        *FW_MIC_TO_FLASH = 0;      /* chunks go to BLE, not the quick-note recorder */
        *FW_MIC_MODE = 0;          /* stream while capturing */
        if (!(state[FW_STATE_MIC_ARM] & 4)) {
            state[FW_STATE_MIC_ARM] |= 4;
            FW_K_SEM_GIVE(state + FW_STATE_MAIN_SEM);
        }
        m->starts++;
        m->recording = 1;
    } else if (p[0] == 0) {
        *FW_MIC_MODE = 1;          /* the session loop exits on its next block */
        m->stops++;
        m->recording = 0;
    }
    uint8_t body[7] = { m->recording, (uint8_t)m->starts, (uint8_t)(m->starts >> 8),
                        (uint8_t)m->stops, (uint8_t)(m->stops >> 8), *FW_MIC_MODE,
                        (uint8_t)(state[FW_STATE_MIC_ARM] & 4) };
    cfw_reply(CFW_MIC_CONTROL, CFW_OK, body, sizeof body);
}

/* Called on the display thread in place of the stock refresh handler. */
int cfw_display_tick(uint8_t *state, uint32_t screen, uint32_t p3, uint32_t mode)
{
    (void)state; (void)p3; (void)mode;
    cfw_anims *a = cfw_animation();
    cfw_imu *m = cfw_imu_state();
    if (screen != FW_SCREEN_CFW || (!a->active && !m->active)) {
        a->ticking = 0;
        return 0;
    }
    if (m->active) {
        cfw_imu_sample(m);
    }
    uint32_t period = m->period_ms;
    if (a->active) {
        cfw_rect dirty;
        int full;
        uint32_t left = cfw_anim_step(a, &dirty, &full);
        if (full) {
            a->full_pushes++;
        }
        cfw_obj_present(full ? 0 : &dirty);
        a->ticks++;
        if (left == 0) {
            a->active = 0;
        }
        period = a->period_ms;   /* animation sets the pace while it runs */
    }
    if (!a->active && !m->active) {
        a->ticking = 0;
        return 0;
    }
    FW_K_MSLEEP((int)period);
    FW_DISPLAY_POST(FW_SCREEN_CFW, 0, 0);
    return 0;
}

/* Reply body: [active][period_ms][ticks:u32][full_pushes:u32]. */
static void cfw_anim_state(void)
{
    cfw_anims *a = cfw_animation();
    uint8_t body[10];
    body[0] = a->active;
    body[1] = a->period_ms;
    for (int i = 0; i < 4; i++) {
        body[2 + i] = (uint8_t)(a->ticks >> (8 * i));
        body[6 + i] = (uint8_t)(a->full_pushes >> (8 * i));
    }
    cfw_reply(CFW_ANIM_STATE, CFW_OK, body, sizeof body);
}

static uint32_t cfw_popcount(uint32_t v)
{
    uint32_t n = 0;
    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

/* [period_ms][count][entries]: entry [id:u16][mask:u16][frames][easing] then
 * one i16 target per set mask bit (bits 0-7 p0..p7, 8 level, 9 width).
 * Tweens start from the object's current values and merge into the running
 * set by id; frames 0 applies the targets at once. The display thread
 * renders the frames. */
static uint8_t cfw_obj_anim(const uint8_t *p, uint32_t n)
{
    cfw_anims *a = cfw_animation();
    cfw_objs *o = cfw_objects();
    if (n < 2) {
        return CFW_BAD_LENGTH;
    }
    uint32_t count = p[1], pos = 2, fresh = 0, slots = CFW_ANIM_MAX - a->count;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->items[i].frame >= a->items[i].frames) {
            slots++;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (n - pos < 6) {
            return CFW_BAD_LENGTH;
        }
        cfw_obj *obj = cfw_obj_find(o, rd16(p + pos));
        if (!obj) {
            return CFW_NO_OBJECT;
        }
        uint32_t mask = rd16(p + pos + 2);
        if (mask == 0 || (mask & ~cfw_obj_tween_mask(obj))) {
            return CFW_BAD_RECORD;
        }
        if (p[pos + 4] && !cfw_anim_find(a, obj->id)) {
            fresh++;
        }
        pos += 6 + 2 * cfw_popcount(mask);
        if (pos > n) {
            return CFW_BAD_LENGTH;
        }
    }
    if (fresh > slots) {
        return CFW_NO_MEMORY;
    }
    if (p[0] >= 1) {
        a->period_ms = p[0];
    }
    cfw_rect snapped = { 0, 0, 0, 0 };
    int full = 0, started = 0;
    pos = 2;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + pos;
        cfw_obj *obj = cfw_obj_find(o, rd16(e));
        uint32_t mask = rd16(e + 2);
        uint8_t frames = e[4];
        const uint8_t *v = e + 6;
        cfw_anim *g = cfw_anim_find(a, obj->id);
        if (frames == 0) {
            cfw_anim_touch(obj, &snapped, &full);
            for (uint32_t k = 0; k < CFW_ANIM_VALUES; k++) {
                if (mask & (1u << k)) {
                    cfw_obj_set_value(obj, k, rd16s(v));
                    v += 2;
                }
            }
            cfw_anim_touch(obj, &snapped, &full);
            if (g) {
                g->frames = 0;
            }
        } else {
            if (!g) {
                for (uint32_t j = 0; j < a->count && !g; j++) {
                    if (a->items[j].frame >= a->items[j].frames) {
                        g = &a->items[j];
                    }
                }
                if (!g) {
                    g = &a->items[a->count++];
                }
            }
            for (uint32_t k = 0; k < CFW_ANIM_VALUES; k++) {
                if (mask & (1u << k)) {
                    g->to[k] = rd16s(v);
                    v += 2;
                    if (k >= 11) {
                        /* The pivot is where the object turns, not a value
                         * to glide: it moves at once. */
                        cfw_obj_set_value(obj, k, g->to[k]);
                    }
                    g->from[k] = cfw_obj_value(obj, k);
                }
            }
            g->id = obj->id;
            g->mask = (uint16_t)mask;
            g->frames = frames;
            g->frame = 0;
            g->easing = e[5];
            started = 1;
        }
        pos += 6 + 2 * cfw_popcount(mask);
    }
    o->revision++;
    if (snapped.w || full) {
        cfw_obj_present(full ? 0 : &snapped);
    }
    if (started) {
        a->active = 1;
        if (!a->ticking) {
            a->ticking = 1;
            FW_DISPLAY_POST(FW_SCREEN_CFW, 0, 0);
        }
    }
    return CFW_OK;
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
    case CFW_ASSET_PUT:   return cfw_asset_put(p, n);
    case CFW_ASSET_DRAW:  return cfw_asset_draw(p, n);
    case CFW_ASSET_TEXT:  return cfw_asset_text(p, n);
    case CFW_ASSET_CLEAR: return cfw_asset_clear();
    case CFW_SHAPES: return cfw_shapes(p, n);
    case CFW_OBJ_PUT:     return cfw_obj_put(p, n);
    case CFW_OBJ_SHOW:    return cfw_obj_show(p, n);
    case CFW_OBJ_HIDE:    return cfw_obj_hide(p, n);
    case CFW_OBJ_CONTROL: return cfw_obj_control(p, n);
    case CFW_OBJ_ANIM:    return cfw_obj_anim(p, n);
    default:        return CFW_BAD_COMMAND;
    }
}

/* A message is a list of records: [cmd:u8][len:u16][payload]. Returns the
 * status of the first failing record and its index in *done. */
static uint8_t cfw_run_message(const uint8_t *m, uint32_t n, uint8_t *done)
{
    uint32_t pos = 0;
    uint8_t deferred = 0;
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
        if (status == CFW_PANEL_OFF) {
            /* The pixels are in the frame buffer and go out with the whole
             * frame when the panel comes on: keep running the message. */
            deferred = 1;
        } else if (status != CFW_OK) {
            return status;
        }
        pos += len;
        (*done)++;
    }
    return deferred ? CFW_PANEL_OFF : CFW_OK;
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
        if (status != CFW_PANEL_OFF) {
            c->stream_errors++;   /* a deferred message ran in full */
        }
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

/* [addr:u32][len]: read up to 16 bytes of memory (diagnostics for RAM
 * reclaim decisions). Reply body: the bytes. */
static void cfw_peek(const uint8_t *p, uint32_t n)
{
    if (n < 5) {
        cfw_reply(CFW_PEEK, CFW_BAD_LENGTH, 0, 0);
        return;
    }
    uint32_t addr = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    uint32_t len = p[4] > 16 ? 16 : p[4];
    cfw_reply(CFW_PEEK, CFW_OK, FW_PEEK_PTR(addr), len);
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
    case CFW_ASSET_PUT:
    case CFW_ASSET_DRAW:
    case CFW_ASSET_TEXT:
    case CFW_ASSET_CLEAR:
    case CFW_SHAPES:
    case CFW_OBJ_PUT:
    case CFW_OBJ_SHOW:
    case CFW_OBJ_HIDE:
    case CFW_OBJ_CONTROL:
    case CFW_OBJ_ANIM:
        cfw_reply(data[1], cfw_run_record(data[1], data + 2, len - 2), 0, 0);
        break;
    case CFW_OBJ_STATE:
        cfw_obj_state();
        break;
    case CFW_ANIM_STATE:
        cfw_anim_state();
        break;
    case CFW_ASSET_INFO:
        cfw_asset_info();
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
    case CFW_PEEK:
        cfw_peek(data + 2, len - 2);
        break;
    case CFW_INPUT_CONTROL:
        cfw_input_control(data + 2, len - 2);
        break;
    case CFW_INPUT_STATE:
        cfw_input_report();
        break;
    case CFW_VIEW:
        cfw_view_cmd(data + 2, len - 2);
        break;
    case CFW_IMU_CONTROL:
        cfw_imu_control(data + 2, len - 2);
        break;
    case CFW_MIC_CONTROL:
        cfw_mic_control(data + 2, len - 2);
        break;
    default:
        cfw_reply(data[1], CFW_BAD_COMMAND, 0, 0);
        break;
    }
    return 0;  /* consumed; never reaches the stock dispatcher */
}
