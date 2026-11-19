#include <kernel/config.h>
#include <kernel/memory/pmm_allocator.h>
#include <kernel/ata/ata.h>
#include <kernel/serial.h>
#include <kernel/string.h>

boot_config_t g_boot_config;

/* 与启动配置大小上限一致 */
#define CFG_MAX_SIZE 16384ULL
#define CFG_IDENTITY_LIMIT (512ULL * 1024 * 1024)

static int cfg_is_ws(char c)
{
    return c == ' ' || c == '\t';
}

/* 拷贝 token，保证 NUL 结尾 */
static void cfg_copy(char *dst, uint32_t dst_size, const char *s, uint32_t n)
{
    uint32_t i;

    if (dst_size == 0)
        return;
    if (n >= dst_size)
        n = dst_size - 1;
    for (i = 0; i < n; i++)
        dst[i] = s[i];
    dst[n] = '\0';
}

static void cfg_set_defaults(void)
{
    kernel_memset(&g_boot_config, 0, (uint32_t)sizeof(g_boot_config));
    kernel_strncpy(g_boot_config.pmm, "bitmap", sizeof(g_boot_config.pmm));
    kernel_strncpy(g_boot_config.disk, "ata-pio", sizeof(g_boot_config.disk));
    kernel_strncpy(g_boot_config.init, "/sbin/init",
                   sizeof(g_boot_config.init));
    kernel_strncpy(g_boot_config.debug, "serial", sizeof(g_boot_config.debug));
    g_boot_config.timeout = 3;
    g_boot_config.disp_w = 0;
    g_boot_config.disp_h = 0;
}

static uint32_t cfg_tok_len(const char *s, const char *end)
{
    uint32_t n = 0;

    while (s + n < end && !cfg_is_ws(s[n]) && s[n] != '\r')
        n++;
    return n;
}

/* 解析 mount 行参数 */
static void cfg_parse_mount(const char *s, const char *end)
{
    const char *p = s;
    uint32_t n;
    config_mount_t *m;

    if (g_boot_config.mount_count >= BOOT_CONFIG_MAX_MOUNTS)
        return;
    m = &g_boot_config.mounts[g_boot_config.mount_count];

    /* 设备名 */
    while (p < end && cfg_is_ws(*p))
        p++;
    n = cfg_tok_len(p, end);
    if (n == 0)
        return;
    cfg_copy(m->dev, sizeof(m->dev), p, n);
    p += n;

    /* 挂载点 */
    while (p < end && cfg_is_ws(*p))
        p++;
    n = cfg_tok_len(p, end);
    if (n == 0 || p[0] != '/')
        return;
    cfg_copy(m->mountpoint, sizeof(m->mountpoint), p, n);
    p += n;

    /* 文件系统类型 */
    while (p < end && cfg_is_ws(*p))
        p++;
    n = cfg_tok_len(p, end);
    if (n == 0)
        return;
    cfg_copy(m->fstype, sizeof(m->fstype), p, n);
    p += n;

    /* 行尾只允许空白 */
    while (p < end && cfg_is_ws(*p))
        p++;
    if (p != end)
        return;

    g_boot_config.mount_count++;
}

/* 解析 key=value 行 */
static void cfg_parse_kv(const char *s, const char *end)
{
    const char *eq = s;
    const char *vs, *ve;
    const char *ks = s;
    const char *ke;
    uint32_t kn;

    while (eq < end && *eq != '=')
        eq++;
    if (eq == end)
        return;

    /* key 去尾空白 */
    ke = eq;
    while (ke > ks && cfg_is_ws(ke[-1]))
        ke--;
    kn = (uint32_t)(ke - ks);

    /* value 去首尾空白 */
    vs = eq + 1;
    while (vs < end && cfg_is_ws(*vs))
        vs++;
    ve = end;
    while (ve > vs && (cfg_is_ws(ve[-1]) || ve[-1] == '\r'))
        ve--;

    if (kn == 3 && kernel_strncmp(ks, "pmm", 3) == 0)
        cfg_copy(g_boot_config.pmm, sizeof(g_boot_config.pmm), vs,
                 (uint32_t)(ve - vs));
    else if (kn == 4 && kernel_strncmp(ks, "disk", 4) == 0)
        cfg_copy(g_boot_config.disk, sizeof(g_boot_config.disk), vs,
                 (uint32_t)(ve - vs));
    else if (kn == 4 && kernel_strncmp(ks, "root", 4) == 0)
        cfg_copy(g_boot_config.root, sizeof(g_boot_config.root), vs,
                 (uint32_t)(ve - vs));
    else if (kn == 4 && kernel_strncmp(ks, "init", 4) == 0)
        cfg_copy(g_boot_config.init, sizeof(g_boot_config.init), vs,
                 (uint32_t)(ve - vs));
    else if (kn == 5 && kernel_strncmp(ks, "debug", 5) == 0)
        cfg_copy(g_boot_config.debug, sizeof(g_boot_config.debug), vs,
                 (uint32_t)(ve - vs));
    else if (kn == 10 && kernel_strncmp(ks, "resolution", 10) == 0)
    {
        /* WxH：如 1280x720；任一非法则整项作废（保持自动） */
        uint32_t w = 0;
        uint32_t h = 0;
        const char *p = vs;

        while (p < ve && *p >= '0' && *p <= '9')
        {
            w = w * 10 + (uint32_t)(*p - '0');
            if (w > 4096)
                break;
            p++;
        }
        if (p < ve && *p == 'x')
        {
            p++;
            while (p < ve && *p >= '0' && *p <= '9')
            {
                h = h * 10 + (uint32_t)(*p - '0');
                if (h > 4096)
                    break;
                p++;
            }
        }
        if (p == ve && w >= 640 && w <= 4096 && h >= 480 && h <= 4096)
        {
            g_boot_config.disp_w = w;
            g_boot_config.disp_h = h;
        }
    }
    else if (kn == 7 && kernel_strncmp(ks, "timeout", 7) == 0)
    {
        int v = 0;
        const char *p = vs;

        while (p < ve && *p >= '0' && *p <= '9')
        {
            v = v * 10 + (*p - '0');
            if (v > 300)
            {
                v = 300;
                break;
            }
            p++;
        }
        g_boot_config.timeout = v;
    }
    /* 未知 key 忽略 */
}

void parse_apply_boot_config(boot_info_t *boot)
{
    const char *buf;
    const char *end;
    const char *line;
    const char *ls;

    cfg_set_defaults();

    if (boot != NULL && boot->magic == CSOS_BOOT_INFO_MAGIC &&
        boot->config_addr != 0 && boot->config_size != 0 &&
        boot->config_size <= CFG_MAX_SIZE &&
        boot->config_addr + boot->config_size <= CFG_IDENTITY_LIMIT)
    {
        buf = (const char *)(uintptr_t)boot->config_addr;
        end = buf + boot->config_size;

        line = buf;
        for (ls = buf; ls <= end; ls++)
        {
            const char *le;
            const char *p;

            if (ls != end && *ls != '\n')
                continue;

            /* 去掉行尾回车 */
            le = ls;
            if (le > line && le[-1] == '\r')
                le--;

            p = line;
            while (p < le && cfg_is_ws(*p))
                p++;
            if (p == le || *p == '#')
            {
                line = ls + 1;
                continue;
            }

            /* 行尾空白不参与解析 */
            while (le > p && cfg_is_ws(le[-1]))
                le--;

            if ((le - p >= 6 && kernel_strncmp(p, "mount", 5) == 0 && cfg_is_ws(p[5])))
                cfg_parse_mount(p + 5, le);
            else
                cfg_parse_kv(p, le);

            line = ls + 1;
        }

        g_boot_config.present = 1;
    }

    /* 应用：未知后端回退默认 */
    pmm_allocator_set(NULL);
    ata_set(NULL);

    /* 串口输出按 debug 配置决定 */
    if (kernel_strcmp(g_boot_config.debug, "serial") == 0)
        serial_init();
}

void boot_config_summary()
{
    int i;

    if (!g_boot_config.present)
    {
        put_string("[CFG] no csos.cfg, using built-in defaults\n");
        return;
    }

    fput_string("[CFG] pmm=%s disk=%s root=%s init=%s debug=%s timeout=%d mounts=%d",
                g_boot_config.pmm, g_boot_config.disk,
                g_boot_config.root[0] ? g_boot_config.root : "(none)",
                g_boot_config.init, g_boot_config.debug,
                g_boot_config.timeout, g_boot_config.mount_count);
    if (g_boot_config.disp_w != 0 && g_boot_config.disp_h != 0)
        fput_string(" resolution=%ux%u", g_boot_config.disp_w,
                    g_boot_config.disp_h);
    else
        fput_string(" resolution=auto");
    fput_string("\n");
    for (i = 0; i < g_boot_config.mount_count; i++)
    {
        fput_string("[CFG] mount %s %s %s\n",
                    g_boot_config.mounts[i].dev,
                    g_boot_config.mounts[i].mountpoint,
                    g_boot_config.mounts[i].fstype);
    }
    if (kernel_strcmp(g_boot_config.pmm, "bitmap") != 0)
        fput_string("[CFG] unknown pmm '%s', using bitmap\n", g_boot_config.pmm);
    if (kernel_strcmp(g_boot_config.disk, "ata-pio") != 0)
        fput_string("[CFG] unknown disk '%s', using ata-pio\n", g_boot_config.disk);
}
