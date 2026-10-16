#include <devices.h>
#include <serial.h>
#include <ata/ata_dev.h>

void init_devices()
{
    init_serial_dev();
    init_ata_dev();
}
