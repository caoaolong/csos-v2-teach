#ifndef CSOS_COMP_IPC_H
#define CSOS_COMP_IPC_H

#include <stdint.h>

#define COMP_CMD_FIFO "/pipe/comp-cmd"
#define COMP_EVT_FIFO "/pipe/comp-evt"

#define COMP_IPC_MAGIC 0x43504943u /* 'CPIC' */

#define COMP_FONT_W 8
#define COMP_FONT_H 16

/* C→S 渲染命令（窗口相对坐标，color 为 XRGB 0x00RRGGBB） */
#define COMP_MSG_FILL 1 /* payload: u32 x,y,w,h,color */
#define COMP_MSG_TEXT 2 /* payload: u32 x,y,color; u16 len; u8 bytes[len] */
/* C→S 窗口管理 */
#define COMP_MSG_CREATE 3   /* payload: comp_create_req */
#define COMP_MSG_DESTROY 4  /* 空 payload */
#define COMP_MSG_MOVE_REQ 5 /* payload: u32 x,y（屏坐标，建议值） */
/* S→C */
#define COMP_MSG_WIN_GEOM 3 /* payload: comp_win_t */
#define COMP_MSG_FOCUS 4    /* payload: u32 focused（0/1） */
#define COMP_MSG_EXPOSE 5   /* payload: comp_rect（窗口相对 damage 区，全 0=整窗） */
#define COMP_MSG_EVTS 2     /* payload: input_event_t[n]（见 input.h，原样转发） */
#define COMP_MSG_CURSOR 6   /* payload: comp_cursor_t（光标绝对屏坐标，已钳制） */
#define COMP_MSG_CLOSE 7    /* S→C 关窗通知：空 payload；客户端回 DESTROY 后退出 */

#define COMP_MAX_PAYLOAD 2048

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

/* 服务端分配的窗口：客户区 rect（屏坐标）+ 字号 scale */
typedef struct comp_win
{
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t scale;
} comp_win_t;

/* 客户端建窗请求：期望客户区尺寸（0=最大化）；name 为标题栏文本 */
typedef struct comp_create_req
{
    uint32_t w;
    uint32_t h;
    char name[32];
} comp_create_req_t;

/* 服务端光标位置：绝对屏坐标（钳制后），客户端直接同步 */
typedef struct comp_cursor
{
    uint32_t x;
    uint32_t y;
} comp_cursor_t;

/* 阻塞写完 len 字节 */
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

/* 阻塞读满 len 字节（O_NONBLOCK fd 无数据时 yield 重试，而非直接失败；
 * 对端关闭返回 -1）。注意普通 read() 读空回 EAGAIN，这里必须重试，
 * 否则“发完 CREATE 立刻读 WIN_GEOM”这种时序必 competing 失败。 */
static inline long comp_read_all(int fd, void *buf, unsigned long len)
{
    char *p = (char *)buf;
    unsigned long off = 0;

    while (off < len)
    {
        long r;

        errno = 0;
        r = read(fd, p + off, len - off);
        if (r == 0)
            return -1; /* EOF：对端全关 */
        if (r < 0)
        {
            if (errno == EAGAIN)
            {
                sleep(2);
                continue;
            }
            return -1;
        }
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

#define COMP_RBUFSZ 2048

typedef struct comp_reader
{
    uint8_t buf[COMP_RBUFSZ];
    unsigned long n; /* 已缓存字节数 */
} comp_reader_t;

static inline void comp_reader_init(comp_reader_t *r)
{
    r->n = 0;
}

/* 非阻塞拉一帧：1=拿到，0=稍后重试，-1=对端关闭 */
/* payload 超 cap 时整帧丢弃 */
static inline int comp_reader_feed(comp_reader_t *r, int fd, comp_hdr_t *hdr_out,
                                   uint8_t *payload_out, unsigned long payload_cap)
{
    unsigned long i;
    unsigned long off;
    long rr;
    comp_hdr_t h;
    unsigned long total;

    /* 先把本次可读的全吞进来 */
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
        /* 缓冲已满还无魔数时直接清空 */
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
