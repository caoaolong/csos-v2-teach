#ifndef CSOS_COMP_IPC_H
#define CSOS_COMP_IPC_H

#include <stdint.h>

/*
 * compositor ↔ client 协议（v1，单客户端）。
 */

#define COMP_CMD_FIFO "/pipe/comp-cmd"
#define COMP_EVT_FIFO "/pipe/comp-evt"

#define COMP_IPC_MAGIC 0x43504943u /* 'CPIC' */

#define COMP_FONT_W 8
#define COMP_FONT_H 16

/* C→S：渲染命令（窗口相对坐标，color 为 XRGB 0x00RRGGBB） */
#define COMP_MSG_FILL 1 /* payload: u32 x,y,w,h,color */
#define COMP_MSG_TEXT 2 /* payload: u32 x,y,color; u16 len; u8 bytes[len] */

/* S→C：窗口与输入 */
#define COMP_MSG_WIN 1  /* payload: u32 x,y,w,h（屏坐标，客户端只读） */
#define COMP_MSG_EVTS 2 /* payload: input_event_t[n]（见 input.h，原样转发） */

#define COMP_MAX_PAYLOAD 4000

typedef struct comp_hdr
{
    uint32_t magic;
    uint16_t type;
    uint16_t len; /* payload 字节数 */
} comp_hdr_t;

typedef struct comp_rect
{
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} comp_rect_t;

/* 阻塞写完 len 字节（对端关闭返回 -1；满管靠 syscall 层阻塞等待） */
static inline long comp_write_all(int fd, const void *buf, unsigned long len)
{
    const char *p = (const char *)buf;
    unsigned long off = 0;

    while (off < len)
    {
        long r = write(fd, p + off, len - off);

        if (r == 0)
            return -1; /* 对端全关 */
        if (r < 0)
            return -1;
        off += (unsigned long)r;
    }
    return 0;
}

/* 阻塞读满 len 字节；读到 EOF（对端全关）返回 -1，无数据则 -1 */
static inline long comp_read_all(int fd, void *buf, unsigned long len)
{
    char *p = (char *)buf;
    unsigned long off = 0;

    while (off < len)
    {
        long r = read(fd, p + off, len - off);

        if (r == 0)
            return -1; /* EOF：对端全关 */
        if (r < 0)
            return -1;
        off += (unsigned long)r;
    }
    return 0;
}

/* 发一帧（阻塞写完） */
static inline long comp_send(int fd, uint16_t type, const void *payload,
                             unsigned long len)
{
    comp_hdr_t h;

    if (len > COMP_MAX_PAYLOAD)
        return -1;
    h.magic = COMP_IPC_MAGIC;
    h.type = type;
    h.len = (uint16_t)len;
    if (comp_write_all(fd, &h, sizeof(h)) != 0)
        return -1;
    if (len > 0 && comp_write_all(fd, payload, len) != 0)
        return -1;
    return 0;
}

#define COMP_RBUFSZ 4096

typedef struct comp_reader
{
    uint8_t buf[COMP_RBUFSZ];
    unsigned long n; /* 已缓存字节数 */
} comp_reader_t;

static inline void comp_reader_init(comp_reader_t *r)
{
    r->n = 0;
}

/* 非阻塞拉一帧：1=拿到（hdr_out/payload_out），0=数据不够稍后重试，
 * -1=对端关闭或致命错误（调用方应重连）。payload 超 cap 时整帧丢弃。 */
static inline int comp_reader_feed(comp_reader_t *r, int fd, comp_hdr_t *hdr_out,
                                   uint8_t *payload_out, unsigned long payload_cap)
{
    unsigned long i;
    unsigned long off;
    long rr;
    comp_hdr_t h;
    unsigned long total;

    /* 先把本次可读的全吞进来（非阻塞 fd 无数据时返回 -1/EAGAIN） */
    errno = 0;
    rr = read(fd, r->buf + r->n, (unsigned long)(COMP_RBUFSZ - r->n));
    if (rr == 0)
        return -1; /* EOF：写端全关 */
    if (rr > 0)
        r->n += (unsigned long)rr;
    else if (r->n == 0)
        return 0;

    /* 按 magic 重同步 */
    off = 0;
    while (off + sizeof(comp_hdr_t) <= r->n)
    {
        uint32_t m = (uint32_t)r->buf[off] |
                     ((uint32_t)r->buf[off + 1] << 8) |
                     ((uint32_t)r->buf[off + 2] << 16) |
                     ((uint32_t)r->buf[off + 3] << 24);

        if (m == COMP_IPC_MAGIC)
            break;
        off++;
    }
    if (off > 0)
    {
        /* 丢弃魔数前的垃圾 */
        for (i = 0; i + off < r->n; i++)
            r->buf[i] = r->buf[i + off];
        r->n -= off;
    }
    if (r->n < sizeof(comp_hdr_t))
    {
        /* 魔数都没凑齐：若缓冲已满还无魔数，直接清空 */
        if (r->n == COMP_RBUFSZ)
            r->n = 0;
        return 0;
    }
    h.magic = (uint32_t)r->buf[0] |
              ((uint32_t)r->buf[1] << 8) |
              ((uint32_t)r->buf[2] << 16) |
              ((uint32_t)r->buf[3] << 24);
    h.type = (uint16_t)(r->buf[4] | ((uint16_t)r->buf[5] << 8));
    h.len = (uint16_t)(r->buf[6] | ((uint16_t)r->buf[7] << 8));
    total = sizeof(comp_hdr_t) + h.len;
    if (h.len > COMP_MAX_PAYLOAD || total > COMP_RBUFSZ)
    {
        r->n = 0; /* 畸形帧长：清空重同步 */
        return 0;
    }
    if (r->n < total)
        return 0; /* 等 payload */
    if ((unsigned long)h.len > payload_cap)
    {
        /* 装不下：整帧丢弃 */
        for (i = 0; i + total < r->n; i++)
            r->buf[i] = r->buf[i + total];
        r->n -= total;
        return 0;
    }
    for (i = 0; i < (unsigned long)h.len; i++)
        payload_out[i] = r->buf[sizeof(comp_hdr_t) + i];
    for (i = 0; i + total < r->n; i++)
        r->buf[i] = r->buf[i + total];
    r->n -= total;
    *hdr_out = h;
    return 1;
}

#endif /* CSOS_COMP_IPC_H */
