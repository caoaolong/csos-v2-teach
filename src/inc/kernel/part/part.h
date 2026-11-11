#ifndef CSOS_PART_PART_H
#define CSOS_PART_PART_H

#include <stdint.h>

#define PART_NAME_MAX 16

void init_partitions();

int part_count();

int part_name(int idx, char *buf, uint32_t size);

#endif /* CSOS_PART_PART_H */
