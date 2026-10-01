#include <ata/ata.h>
#include <kernel.h>
#include <serial.h>
#include <spinlock.h>
#include <cpu.h>
#include <string.h>

extern const ata_ops_t ata_pio_ops;

static const ata_ops_t *current_ops;

const ata_ops_t *ata_current(void)
{
    if (current_ops == NULL)
        return &ata_pio_ops;
    return current_ops;
}

void ata_set(const ata_ops_t *ops)
{
    current_ops = ops;
}