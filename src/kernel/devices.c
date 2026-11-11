#include <kernel/devices.h>
#include <kernel/serial.h>
#include <kernel/ata/ata_dev.h>
#include <kernel/kbd.h>
#include <kernel/dev/mem_dev.h>
#include <kernel/dev/timer_dev.h>
#include <kernel/dev/console_dev.h>

void init_devices()
{
    init_serial_dev();
    init_console_dev();
    init_ata_dev();
    init_kbd_dev();
    init_mem_dev();
    init_timer_dev();
}
