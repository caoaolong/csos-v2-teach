#ifndef CSOS_MINPUT_H
#define CSOS_MINPUT_H

#include <stdint.h>

/* 鼠标事件：/dev/mouse0 的 read() 一次返回整数个，跨端对齐按此结构大小取整。
 * dx/dy 为有符号相对位移（屏坐标：右/下为正）；buttons bit0=左键 bit1=右键 bit2=中键。 */
typedef struct mouse_event
{
    int32_t dx;
    int32_t dy;
    uint32_t buttons;
} mouse_event_t;

#endif /* CSOS_MINPUT_H */
