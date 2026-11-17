#ifndef CSOS_INPUT_H
#define CSOS_INPUT_H

#include <stdint.h>

/*
 * 统一输入事件
 *
 * 鼠标：每组 PS/2 包展开为 [REL_X?] [REL_Y?] [BTN_* 变化…] SYN_REPORT。
 * 键盘：每次按键展开为 EV_KEY（value 1=按下 0=抬起）+ SYN_REPORT。
 */

typedef struct input_event
{
    uint64_t time_ms; /* 事件入队时的 uptime_ms */
    uint16_t type;    /* EV_SYN / EV_KEY / EV_REL */
    uint16_t code;    /* SYN_REPORT / KEY_* / BTN_* / REL_* */
    int32_t value;    /* SYN 恒 0；REL 为位移；KEY 为 1 按下 / 0 抬起 */
} input_event_t;

/* 事件类型（与 Linux EV_* 同值） */
#define INPUT_EV_SYN 0x00
#define INPUT_EV_KEY 0x01
#define INPUT_EV_REL 0x02

/* SYN 编码 */
#define INPUT_SYN_REPORT 0

/* 相对位移编码 */
#define INPUT_REL_X 0x00
#define INPUT_REL_Y 0x01

/*
 * 按键编码
 */
#define INPUT_BTN_MOUSE 0x110
#define INPUT_BTN_LEFT 0x110
#define INPUT_BTN_RIGHT 0x111
#define INPUT_BTN_MIDDLE 0x112

#define INPUT_KEY_ESC 1
#define INPUT_KEY_1 2
#define INPUT_KEY_2 3
#define INPUT_KEY_3 4
#define INPUT_KEY_4 5
#define INPUT_KEY_5 6
#define INPUT_KEY_6 7
#define INPUT_KEY_7 8
#define INPUT_KEY_8 9
#define INPUT_KEY_9 10
#define INPUT_KEY_0 11
#define INPUT_KEY_BACKSPACE 14
#define INPUT_KEY_TAB 15
#define INPUT_KEY_Q 16
#define INPUT_KEY_W 17
#define INPUT_KEY_E 18
#define INPUT_KEY_R 19
#define INPUT_KEY_T 20
#define INPUT_KEY_Y 21
#define INPUT_KEY_U 22
#define INPUT_KEY_I 23
#define INPUT_KEY_O 24
#define INPUT_KEY_P 25
#define INPUT_KEY_ENTER 28
#define INPUT_KEY_LEFTCTRL 29
#define INPUT_KEY_A 30
#define INPUT_KEY_S 31
#define INPUT_KEY_D 32
#define INPUT_KEY_F 33
#define INPUT_KEY_G 34
#define INPUT_KEY_H 35
#define INPUT_KEY_J 36
#define INPUT_KEY_K 37
#define INPUT_KEY_L 38
#define INPUT_KEY_LEFTSHIFT 42
#define INPUT_KEY_Z 44
#define INPUT_KEY_X 45
#define INPUT_KEY_C 46
#define INPUT_KEY_V 47
#define INPUT_KEY_B 48
#define INPUT_KEY_N 49
#define INPUT_KEY_M 50
#define INPUT_KEY_SPACE 57
#define INPUT_KEY_LEFT 105
#define INPUT_KEY_RIGHT 106
#define INPUT_KEY_UP 103
#define INPUT_KEY_DOWN 108

/* 按键按下值（与 Linux 同值） */
#define INPUT_KEY_PRESS 1
#define INPUT_KEY_RELEASE 0

/*
 * keycode → ASCII
 */
static inline char input_key_to_ascii(uint16_t code)
{
    switch (code)
    {
    case INPUT_KEY_1:
        return '1';
    case INPUT_KEY_2:
        return '2';
    case INPUT_KEY_3:
        return '3';
    case INPUT_KEY_4:
        return '4';
    case INPUT_KEY_5:
        return '5';
    case INPUT_KEY_6:
        return '6';
    case INPUT_KEY_7:
        return '7';
    case INPUT_KEY_8:
        return '8';
    case INPUT_KEY_9:
        return '9';
    case INPUT_KEY_0:
        return '0';
    case INPUT_KEY_Q:
        return 'q';
    case INPUT_KEY_W:
        return 'w';
    case INPUT_KEY_E:
        return 'e';
    case INPUT_KEY_R:
        return 'r';
    case INPUT_KEY_T:
        return 't';
    case INPUT_KEY_Y:
        return 'y';
    case INPUT_KEY_U:
        return 'u';
    case INPUT_KEY_I:
        return 'i';
    case INPUT_KEY_O:
        return 'o';
    case INPUT_KEY_P:
        return 'p';
    case INPUT_KEY_A:
        return 'a';
    case INPUT_KEY_S:
        return 's';
    case INPUT_KEY_D:
        return 'd';
    case INPUT_KEY_F:
        return 'f';
    case INPUT_KEY_G:
        return 'g';
    case INPUT_KEY_H:
        return 'h';
    case INPUT_KEY_J:
        return 'j';
    case INPUT_KEY_K:
        return 'k';
    case INPUT_KEY_L:
        return 'l';
    case INPUT_KEY_Z:
        return 'z';
    case INPUT_KEY_X:
        return 'x';
    case INPUT_KEY_C:
        return 'c';
    case INPUT_KEY_V:
        return 'v';
    case INPUT_KEY_B:
        return 'b';
    case INPUT_KEY_N:
        return 'n';
    case INPUT_KEY_M:
        return 'm';
    case INPUT_KEY_SPACE:
        return ' ';
    case INPUT_KEY_ENTER:
        return '\n';
    case INPUT_KEY_BACKSPACE:
        return '\b';
    case INPUT_KEY_TAB:
        return '\t';
    case INPUT_KEY_ESC:
        return 0x1B;
    default:
        return 0;
    }
}

#endif /* CSOS_INPUT_H */
