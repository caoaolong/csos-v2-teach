#ifndef CSOS_PIT_H
#define CSOS_PIT_H

#include <stdint.h>
#include <idt.h>

#define PIT_DEFAULT_HZ 100

extern volatile uint64_t jiffies;

/* 成功返回 0；freq_hz==0 返回 -1 且不写端口 */
int init_pit(uint32_t freq_hz);
void handler_timer(exception_frame_t *frame);

#endif /* CSOS_PIT_H */
