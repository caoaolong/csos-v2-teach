#include <vdev.h>
#include <spinlock.h>
#include <string.h>

static vdev_t *g_devs;
static vdev_listener_t *g_listeners;
static spinlock_t g_vdev_lock = SPINLOCK_INIT;

void init_vdev()
{
    uint64_t flags = spin_lock_irqsave(&g_vdev_lock);
    g_devs = NULL;
    g_listeners = NULL;
    spin_unlock_irqrestore(&g_vdev_lock, flags);
}

int vdev_register(vdev_t *dev)
{
    vdev_t *it;
    vdev_listener_t *l;
    uint64_t flags;

    if (!dev || dev->name[0] == '\0')
        return VFS_ERR_INVAL;
    if (dev->type != VDEV_CHAR || !dev->ops || !dev->ops->read)
        return VFS_ERR_INVAL;

    flags = spin_lock_irqsave(&g_vdev_lock);
    for (it = g_devs; it; it = it->next)
    {
        if (kernel_strcmp(it->name, dev->name) == 0)
        {
            spin_unlock_irqrestore(&g_vdev_lock, flags);
            return VFS_ERR_EXIST;
        }
    }
    dev->next = g_devs;
    g_devs = dev;
    for (l = g_listeners; l; l = l->next)
        if (l->added)
            l->added(l->ctx, dev);
    spin_unlock_irqrestore(&g_vdev_lock, flags);
    return VFS_OK;
}

int vdev_unregister(vdev_t *dev)
{
    vdev_t **pp;
    vdev_listener_t *l;
    uint64_t flags;

    if (!dev)
        return VFS_ERR_INVAL;

    flags = spin_lock_irqsave(&g_vdev_lock);
    pp = &g_devs;
    while (*pp && *pp != dev)
        pp = &(*pp)->next;
    if (!*pp)
    {
        spin_unlock_irqrestore(&g_vdev_lock, flags);
        return VFS_ERR_NOENT;
    }
    *pp = dev->next;
    dev->next = NULL;
    for (l = g_listeners; l; l = l->next)
        if (l->removed)
            l->removed(l->ctx, dev);
    spin_unlock_irqrestore(&g_vdev_lock, flags);
    return VFS_OK;
}

vdev_t *vdev_find(const char *name)
{
    vdev_t *it;
    uint64_t flags;

    if (!name)
        return NULL;
    flags = spin_lock_irqsave(&g_vdev_lock);
    for (it = g_devs; it; it = it->next)
        if (kernel_strcmp(it->name, name) == 0)
            break;
    spin_unlock_irqrestore(&g_vdev_lock, flags);
    return it;
}

uint32_t vdev_count()
{
    vdev_t *it;
    uint32_t n = 0;
    uint64_t flags = spin_lock_irqsave(&g_vdev_lock);

    for (it = g_devs; it; it = it->next)
        n++;
    spin_unlock_irqrestore(&g_vdev_lock, flags);
    return n;
}

void vdev_listen(vdev_listener_t *l)
{
    vdev_t *it;
    uint64_t flags;

    if (!l)
        return;

    l->next = NULL;
    flags = spin_lock_irqsave(&g_vdev_lock);

    for (it = g_devs; it; it = it->next)
        if (l->added)
            l->added(l->ctx, it);
    l->next = g_listeners;
    g_listeners = l;
    spin_unlock_irqrestore(&g_vdev_lock, flags);
}

void vdev_unlisten(vdev_listener_t *l)
{
    vdev_listener_t **pp;
    uint64_t flags;

    if (!l)
        return;

    flags = spin_lock_irqsave(&g_vdev_lock);
    pp = &g_listeners;
    while (*pp && *pp != l)
        pp = &(*pp)->next;
    if (*pp)
        *pp = l->next;
    spin_unlock_irqrestore(&g_vdev_lock, flags);

    l->next = NULL;
}
