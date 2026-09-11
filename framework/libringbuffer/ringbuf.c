/**
  * @file    ringbuf.c
  * @brief   精简环形缓冲区实现
  */
#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

/* 无锁 SPSC 的关键设计：
 *
 * 1) head 只由写方修改，tail 只由读方修改 —— 没有双方共写的变量，
 *    因此不需要任何临界区。
 * 2) 计数器用【单调递增】而非取模后的下标：
 *      实际下标 = 计数器 & mask
 *      已用字节 = head - tail        （无符号回绕，天然正确）
 *    好处是不必"牺牲一个槽位"来区分空/满，RINGBUF_SIZE 字节全部可用。
 *    前提：size <= 2^31，保证 head - tail 不会溢出成错值。这里固定 512。
 * 3) size 是 2 的幂，用掩码代替取模。
 *
 * 内存：结构体与数据区同属一次 malloc，buf 是柔性数组成员紧随其后，
 *       所以 ringbuf_free() 只有一次 free。
 */

/* 编译器屏障：阻止编译器把数据拷贝重排到 head/tail 更新之后。
 * 单核 Cortex-M 无数据缓存，只需编译器屏障，不需要 DMB。 */
#if defined(__GNUC__)
#define RB_BARRIER() __asm volatile("" ::: "memory")
#else
#define RB_BARRIER() do { } while (0)
#endif

bool ringbuf_init(ringbuffer_t **rb)
{
    if (rb == NULL) {
        return false;
    }
    *rb = NULL;   /* 失败路径下出参始终是 NULL，调用方不会拿到野指针 */

    /* 一次 malloc 拿下 结构体 + 数据区 + 预留字节 */
    ringbuffer_t *p = (ringbuffer_t *)malloc(sizeof(*p) + (size_t)RINGBUF_SIZE + (size_t)RINGBUF_SLACK);
    if (p == NULL) {
        return false;
    }

    p->size = RINGBUF_SIZE;
    p->mask = RINGBUF_SIZE - 1u;
    p->head = 0u;
    p->tail = 0u;

    *rb = p;
    return true;
}

void ringbuf_free(ringbuffer_t *rb)
{
    /* 结构体与数据区是同一块，一次 free 即可 */
    free(rb);
}

void ringbuf_clear(ringbuffer_t *rb)
{
    if (rb == NULL) {
        return;
    }
    /* 只把读计数器推到写计数器处，数据内容不动。
     * 单点写 tail —— 因此并发时应由读方调用。 */
    rb->tail = rb->head;
}

size_t ringbuf_write(ringbuffer_t *rb, const void *src, size_t len)
{
    if ((rb == NULL) || (src == NULL) || (len == 0u)) {
        return 0u;
    }

    const uint32_t head = rb->head;
    const uint32_t used = head - rb->tail;   /* 快照 tail：读方可能并发推进 */
    size_t space = (size_t)(rb->size - used);

    if (len > space) {
        len = space;                          /* 无阻塞：装不下的部分不写 */
    }
    if (len == 0u) {
        return 0u;
    }

    const uint32_t idx = head & rb->mask;
    size_t first = (size_t)(rb->size - idx);  /* 从 idx 到缓冲区末尾的连续段 */
    if (first > len) {
        first = len;
    }

    memcpy(&rb->buf[idx], src, first);
    if (len > first) {                        /* 回绕：剩下这段写到开头 */
        memcpy(&rb->buf[0], (const uint8_t *)src + first, len - first);
    }

    RB_BARRIER();                             /* 数据先落地，再更新 head */
    rb->head = head + (uint32_t)len;
    return len;
}

size_t ringbuf_read(ringbuffer_t *rb, void *dst, size_t len)
{
    if ((rb == NULL) || (dst == NULL) || (len == 0u)) {
        return 0u;
    }

    const uint32_t tail = rb->tail;
    const uint32_t avail = rb->head - tail;   /* 快照 head：写方可能并发推进 */
    if (len > (size_t)avail) {
        len = (size_t)avail;                  /* 无阻塞：有多少读多少 */
    }
    if (len == 0u) {
        return 0u;
    }

    const uint32_t idx = tail & rb->mask;
    size_t first = (size_t)(rb->size - idx);
    if (first > len) {
        first = len;
    }

    memcpy(dst, &rb->buf[idx], first);
    if (len > first) {
        memcpy((uint8_t *)dst + first, &rb->buf[0], len - first);
    }

    RB_BARRIER();                             /* 数据读走，再回收空间 */
    rb->tail = tail + (uint32_t)len;
    return len;
}

size_t ringbuf_used(const ringbuffer_t *rb)
{
    if (rb == NULL) {
        return 0u;
    }
    return (size_t)(rb->head - rb->tail);
}

size_t ringbuf_space(const ringbuffer_t *rb)
{
    if (rb == NULL) {
        return 0u;
    }
    return (size_t)(rb->size - (rb->head - rb->tail));
}





