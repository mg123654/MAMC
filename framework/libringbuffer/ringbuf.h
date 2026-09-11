/**
  * @file    ringbuf.h
  * @brief   精简环形缓冲区（ring buffer）—— 字节流缓存
  *
  * 全部操作【无阻塞】：写不满就少写，读不够就少读，绝不自旋等待。
  * 读写函数返回本次【实际有效】的字节数，调用方据此判断处理了多少。
  *
  * 内存布局：结构体与数据区在【同一次 malloc】里，数据区是紧随结构体之后的
  * 柔性数组成员。因此 ringbuf_free() 只需要 free 一次。
  *
  *     p ──▶ ┌──────────────┬────────────────────────┬───┐
  *           │ ringbuffer_t │   buf[512] 数据区      │+1 │
  *           └──────────────┴────────────────────────┴───┘
  *           ↑ sizeof(ringbuffer_t)                    ↑ 预留 1 字节
  *
  * 线程/中断安全（SPSC）：
  *   写方与读方各只有一个时（典型场景：主循环写 / 中断读，或反之），
  *   本实现无需任何临界区即可安全并发。
  *   若存在【多个】写方或多个读方，需调用方自行加锁。
  */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 数据区容量，字节。必须是 2 的幂（用掩码代替取模） */
#define RINGBUF_SIZE 512u

/** 数据区之后额外预留的字节数 */
#define RINGBUF_SLACK 1u

/**
  * @brief  环形缓冲区
  * @note   buf 是柔性数组成员，必须位于末尾且不参与 sizeof。
  *         整个对象由 ringbuf_init() 一次性堆分配，不要自行定义实例。
  */
typedef struct ringbuffer {
    volatile uint32_t head;  /**< 写计数器，只由生产者递增 */
    volatile uint32_t tail;  /**< 读计数器，只由消费者递增 */
    uint32_t          size;  /**< 容量，= RINGBUF_SIZE */
    uint32_t          mask;  /**< size - 1 */
    uint8_t           buf[]; /**< 数据区，紧随结构体之后 */
} ringbuffer_t;

/**
  * @brief  创建环形缓冲区（单次 malloc：结构体 + 数据区）
  * @param  rb  出参。成功时写入堆块首地址；失败时置为 NULL。
  *             调用完必须检查返回值，失败时 *rb 不可使用。
  * @retval true 申请成功，false 申请失败（*rb 已被置 NULL）
  */
bool ringbuf_init(ringbuffer_t **rb);

/**
  * @brief  销毁缓冲区
  * @param  rb 句柄，可为 NULL（空操作）
  * @note   结构体与数据区是同一次 malloc，故只需释放一次。
  */
void ringbuf_free(ringbuffer_t *rb);

/**
  * @brief  清空缓冲区（丢弃所有未读数据）
  * @param  rb 句柄
  * @note   只把读写计数器归位，不擦除数据内容。
  *         并发场景下应从【读方】调用。
  */
void ringbuf_clear(ringbuffer_t *rb);

/**
  * @brief  写入数据（无阻塞）
  * @param  rb  句柄
  * @param  src 源数据
  * @param  len 期望写入的字节数
  * @retval 实际写入的字节数（0 ~ len）。空间不足时只写装得下的部分。
  */
size_t ringbuf_write(ringbuffer_t *rb, const void *src, size_t len);

/**
  * @brief  读出数据（无阻塞）
  * @param  rb  句柄
  * @param  dst 目标缓冲区，需能容纳 len 字节
  * @param  len 期望读出的字节数
  * @retval 实际读出的字节数（0 ~ len）。数据不足时只读现有的部分。
  */
size_t ringbuf_read(ringbuffer_t *rb, void *dst, size_t len);

/** @brief 当前已缓存字节数 */
size_t ringbuf_used(const ringbuffer_t *rb);

/** @brief 当前剩余可写空间，字节 */
size_t ringbuf_space(const ringbuffer_t *rb);

/** @brief 写单字节。成功返回 true，缓冲区满返回 false */
static inline bool ringbuf_putc(ringbuffer_t *rb, uint8_t byte)
{
    return ringbuf_write(rb, &byte, 1u) == 1u;
}

/** @brief 读单字节。成功返回 true，缓冲区空返回 false */
static inline bool ringbuf_getc(ringbuffer_t *rb, uint8_t *byte)
{
    return ringbuf_read(rb, byte, 1u) == 1u;
}

#endif /* RINGBUF_H */
