/**
  * @file    diag_log.c
  * @brief   RAM 日志环形缓冲 —— 实现
  * @note    设计说明见 diag_log.h
  */
#include "diag_log.h"

/* 非 static 是有意的：调试器要按名字直接找到它们，见 tools/dump_ramlog.gdb。
 * 加 volatile 是因为本模块的写入点（printf）与读取点（调试器）之间没有
 * 任何编译器可见的同步，不加的话 -Og 下写入可能被合并或缓存。 */
volatile char     g_diagLog[DIAG_LOG_SIZE];
volatile uint32_t g_diagLogHead;
volatile uint32_t g_diagLogTotal;

void
diag_log_init(void) {
    for (uint32_t i = 0u; i < DIAG_LOG_SIZE; i++) {
        g_diagLog[i] = '\0';
    }
    g_diagLogHead = 0u;
    g_diagLogTotal = 0u;
}

void
diag_log_putc(char c) {
    /* DIAG_LOG_SIZE 是 2 的幂，取模换成与运算 —— 每字符省一次除法。
     * 这个函数在每次 printf 时逐字符调用，值得。 */
    g_diagLog[g_diagLogHead] = c;
    g_diagLogHead = (g_diagLogHead + 1u) & (DIAG_LOG_SIZE - 1u);
    g_diagLogTotal++;
}

uint32_t
diag_log_dump(char* out, uint32_t outSize) {
    if ((out == 0) || (outSize == 0u)) {
        return 0u;
    }

    const uint32_t head = g_diagLogHead;
    const uint32_t total = g_diagLogTotal;

    /* 还没绕圈：有效数据是 [0, head)，且 head == total。
     * 已绕圈：  最新的数据从 head 开始，顺序是 [head, END) 再 [0, head)。 */
    uint32_t avail;
    uint32_t n = 0u;

    if (total < DIAG_LOG_SIZE) {
        avail = head;
        for (uint32_t i = 0u; (i < avail) && (n + 1u < outSize); i++) {
            out[n++] = g_diagLog[i];
        }
    } else {
        avail = DIAG_LOG_SIZE;
        for (uint32_t i = 0u; (i < DIAG_LOG_SIZE) && (n + 1u < outSize); i++) {
            out[n++] = g_diagLog[(head + i) & (DIAG_LOG_SIZE - 1u)];
        }
    }

    out[n] = '\0';
    return n;
}
