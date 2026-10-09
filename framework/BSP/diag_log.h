/**
  * @file    diag_log.h
  * @brief   RAM 日志环形缓冲 —— 没有串口线时的观测通道
  *
  * 为什么要这个东西：本工程的 printf 走 USART1(PA9/PA10)，板上要接一根
  * USB-TTL 才能看到日志。bring-up 时线经常不在手边，而 CANopen 的调试
  * **几乎全靠日志**（SDO 的 abort 码、状态字、错误计数器都只能从日志里看）。
  *
  * 做法是在 __io_putchar() 里加一路旁路：每个字符除了发串口，再写进这里的
  * 环形缓冲。**单点修改**就捕获了全部 printf，包括 CANopenNode 自己的
  * log_printf —— 不用去改散落各处的日志语句。
  *
  * 读取方式：用调试器（ST-Link + gdb）直接读下面三个全局量，见
  * tools/dump_ramlog.gdb。它们刻意**不加 static**，就是为了让 gdb 能直接看到。
  *
  * ── 为什么是环形而不是线性 ────────────────────────────────────────
  * 线性缓冲写满就停，而这段固件是**长期运行**的：真正的证据（电机转起来之后
  * 的实测速度）出现在启动几秒之后，线性缓冲会被前面那些启动日志填满。
  * 环形则永远保留**最近** DIAG_LOG_SIZE 字节，那正是我们想看的。
  *
  * ── 注意 ──────────────────────────────────────────────────────────
  * 与 bsp_uart.c 一样，只在主循环上下文使用，**不要在中断里调**：
  * 这里没有任何并发保护，也没有做原子性处理。
  */
#ifndef DIAG_LOG_H
#define DIAG_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 环形缓冲容量。必须是 2 的幂 —— 取模用掩码实现，省一次除法 */
#define DIAG_LOG_SIZE 4096u

/**
  * @brief 日志环形缓冲本体
  * @note  volatile：编译器的 -Og 优化下不加这个，写入可能被优化掉或缓存，
  *        调试器读到的就不是最新的内容。只读调试用，不参与逻辑。
  */
extern volatile char g_diagLog[DIAG_LOG_SIZE];

/** @brief 下一个待写入位置（0..DIAG_LOG_SIZE-1）。**最新一行的开头就在这里** */
extern volatile uint32_t g_diagLogHead;

/**
  * @brief 累计写入的总字节数，只增不减
  * @note  用它判断缓冲区绕过一圈没有：
  *        < DIAG_LOG_SIZE → 还没绕圈，有效数据是 g_diagLog[0..head)
  *        >= DIAG_LOG_SIZE → 已绕圈，顺序是 [head..END) + [0..head)
  *        gdb 脚本靠这个决定拼接顺序。
  */
extern volatile uint32_t g_diagLogTotal;

/** @brief 清空缓冲并复位指针。在 main() 里、任何 printf 之前调用 */
void diag_log_init(void);

/**
  * @brief 写入一个字符，满了就覆盖最旧的
  * @param  c 待写入字符
  * @note   由 bsp_uart.c 的 __io_putchar() 调用，一般不直接调
  */
void diag_log_putc(char c);

/**
  * @brief  把环形缓冲按时间顺序拷成线性字符串（供将来做 host 测试用）
  * @param  out     输出缓冲
  * @param  outSize 输出缓冲容量
  * @return 实际写入的字节数（不含结尾的 '\0'）
  * @note   调试器读内存时用不到这个函数（gdb 不会去执行目标代码）。
  *         它存在是为了能在 host 上用 gcc 单独测环形逻辑。
  */
uint32_t diag_log_dump(char* out, uint32_t outSize);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_LOG_H */
