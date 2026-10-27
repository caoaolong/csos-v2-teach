#include <config.h>
#include <memory/pmm_allocator.h>
#include <ata/ata.h>
#include <serial.h>
#include <string.h>

boot_config_t g_boot_config;

/* 与 EfiBoot BOOT_CONFIG_MAX_SIZE 一致 */
#define CFG_MAX_SIZE 16384ULL
#define CFG_IDENTITY_LIMIT (512ULL * 1024 * 1024)

static int cfg_is_ws(char c)
{
    return c == ' ' || c == '\t';
}

/* 拷贝 token（[s, s+n)），保证 NUL 结尾，超长截断 */
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
    kernel_strncpy(g_boot_config.init, "/ext4/usr/local/bin/gcc",
                   sizeof(g_boot_config.init));
    kernel_strncpy(g_boot_config.debug, "serial", sizeof(g_boot_config.debug));
    g_boot_config.timeout = 3;
}

static uint32_t cfg_tok_len(const char *s, const char *end)
{
    uint32_t n = 0;

    while (s + n < end && !cfg_is_ws(s[n]) && s[n] != '\r')
        n++;
    return n;
}

/* 解析 "mount <dev> <mp> <fstype>" 的参数区 */
static void cfg_parse_mount(const char *s, const char *end)
{
    const char *p = s;
    uint32_t n;
    config_mount_t *m;

    if (g_boot_config.mount_count >= BOOT_CONFIG_MAX_MOUNTS)
        return;
    m = &g_boot_config.mounts[g_boot_config.mount_count];

    /* dev */
    while (p < end && cfg_is_ws(*p))
        p++;
    n = cfg_tok_len(p, end);
    if (n == 0)
        return;
    cfg_copy(m->dev, sizeof(m->dev), p, n);
    p += n;

    /* mountpoint */
    while (p < end && cfg_is_ws(*p))
        p++;
    n = cfg_tok_len(p, end);
    if (n == 0 || p[0] != '/')
        return;
    cfg_copy(m->mountpoint, sizeof(m->mountpoint), p, n);
    p += n;

    /* fstype */
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

/* 解析 "key=value"，s..end 为去首尾空白后的一行 */
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
    /* 未知 key：忽略（向前兼容） */
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

            /* 行 [line, ls)，去掉尾部 \r */
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

    /*
     * 应用：目前 pmm 只有 bitmap、disk 只有 ata-pio，
     * buddy/slab、ata-dma 实现后在此按 g_boot_config 选择。
     * 传 NULL 即恢复各自的默认后端；未知值同样回退默认，
     * 由 boot_config_summary() 统一告警。
     */
    pmm_allocator_set(NULL);
    ata_set(NULL);

    /*
     * debug=serial（默认，含无配置时）才初始化串口；
     * 其他值（如 debug=none）保持静默启动。
     * 此前 serial_init() 是 kernel_main 第一件事，
     * 挪到此处时序等价。
     */
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

    fput_string("[CFG] pmm=%s disk=%s root=%s init=%s debug=%s timeout=%d mounts=%d\n",
                g_boot_config.pmm, g_boot_config.disk,
                g_boot_config.root[0] ? g_boot_config.root : "(none)",
                g_boot_config.init, g_boot_config.debug,
                g_boot_config.timeout, g_boot_config.mount_count);
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
