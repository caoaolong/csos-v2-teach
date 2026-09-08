#ifndef CSOS_LAPIC_H

void init_lapic();

void lapic_start_ap(uint32_t apic_id, uint8_t vector_page);

#endif /*CSOS_LAPIC_H*/