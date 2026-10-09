/**
  * @file    bsp_uart.c
  * @brief   板级串口 —— printf 重定向
  */
#include "bsp_uart.h"
#include "diag_log.h"
#include "usart.h"

/**
  * @brief  重定向 newlib 的字符输出到调试串口
  * @note   覆盖 syscalls.c 中的 weak 版本。阻塞发送，供主循环上下文
  *         (CANopenNode 的 log_printf) 使用，不要在中断里调用。
  *
  *         每个字符**同时**写一路 RAM 环形缓冲（diag_log.c）。放在这里
  *         是因为这是所有 printf 的必经之路 —— 一个点改掉，全工程
  *         （含 CANopenNode 内部的 log_printf）的日志就都能被调试器读到，
  *         不用去改散落各处的日志语句。
  */
int __io_putchar(int ch)
{
    uint8_t byte = (uint8_t)ch;

    /* 先记 RAM：这一步不依赖串口，没接 USB-TTL 也能从调试器里读到日志 */
    diag_log_putc((char)byte);

#if BSP_DEBUG_UART_ENABLE
    HAL_UART_Transmit(&BSP_DEBUG_UART_HANDLE, &byte, 1U, HAL_MAX_DELAY);
#else
    (void)byte;
#endif
    return ch;
}

/**
  * @brief  重定向 newlib 的字符输入
  * @note   当前无输入需求，恒返回 -1(EOF)。提供定义是为了避免 syscalls.c 的
  *         _read() 调用到未定义符号。
  */
int __io_getchar(void)
{
    return -1;
}
