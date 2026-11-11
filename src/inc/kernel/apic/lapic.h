#ifndef CSOS_LAPIC_H

#include <stdint.h>

void init_lapic();

void lapic_start_ap(uint32_t apic_id, uint8_t vector_page);

/* AP：启用本核 Local APIC（SVR），不做 PIC/校准 */
void lapic_ap_init();

#endif /*CSOS_LAPIC_H*/