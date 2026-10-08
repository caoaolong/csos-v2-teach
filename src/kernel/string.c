#include <string.h>

void kernel_strcpy(char *dst, const char *src)
{
    if (!dst || !src)
        return;

    while (*src)
        *dst++ = *src++;

    *dst = '\0';
}

void kernel_strncpy(char *dst, const char *src, uint32_t size)
{
    uint32_t i = 0;

    if (!dst || !src || !size)
        return;

    /* 安全语义：最多拷贝 size-1 字节，恒以 '\0' 结尾 */
    while (i + 1 < size && src[i])
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int kernel_strcmp(const char *str1, const char *str2)
{
    if (str1 == str2)
        return 0;
    if (!str1)
        return -1;
    if (!str2)
        return 1;

    while (*str1 && *str1 == *str2)
    {
        str1++;
        str2++;
    }

    return (int)(uint8_t)*str1 - (int)(uint8_t)*str2;
}

int kernel_strncmp(const char *str1, const char *str2, uint32_t size)
{
    if (size == 0)
        return 0;
    if (str1 == str2)
        return 0;
    if (!str1)
        return -1;
    if (!str2)
        return 1;

    while (size > 0)
    {
        if (*str1 != *str2)
            return (int)(uint8_t)*str1 - (int)(uint8_t)*str2;
        if (*str1 == '\0')
            return 0;
        str1++;
        str2++;
        size--;
    }

    return 0;
}

uint32_t kernel_strlen(const char *str)
{
    if (!str)
        return 0;

    const char *c = str;
    uint32_t length = 0;
    while (*c++)
        length++;

    return length;
}

void kernel_memcpy(void *dst, void *src, uint32_t size)
{
    if (!dst || !src || !size)
        return;

    uint8_t *s = (uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    while (size--)
        *d++ = *s++;
}

void kernel_memset(void *dst, uint8_t value, uint32_t size)
{
    if (!dst || !size)
        return;
    uint8_t *d = (uint8_t *)dst;
    while (size--)
        *d++ = value;
}

int kernel_memcmp(void *v1, void *v2, uint32_t size)
{
    uint8_t *p1 = (uint8_t *)v1;
    uint8_t *p2 = (uint8_t *)v2;

    if (size == 0)
        return 0;
    if (!v1 || !v2)
        return -1;

    while (size--)
    {
        if (*p1 != *p2)
            return (int)*p1 - (int)*p2;
        p1++;
        p2++;
    }

    return 0;
}