#include <devices.h>
#include <serial.h>
#include <ata/ata_dev.h>
#include <kbd.h>
#include <dev/mem_dev.h>
#include <dev/timer_dev.h>

void init_devices()
{
    init_serial_dev();
    init_ata_dev();
    init_kbd_dev();
    init_mem_dev();
    init_timer_dev();
}
