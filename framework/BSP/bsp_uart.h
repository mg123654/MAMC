/**
  * @file    bsp_uart.h
  * @brief   板级串口 —— printf 重定向
  *
  * framework/Core/Src/syscalls.c 的 _write() 会调用 __io_putchar()，但那里
  * 只有 weak 声明、全工程没有定义。若不在此提供强定义，任何 printf 都会跳到
  * 未定义符号导致 HardFault —— CANopenNode 移植层的 log_printf 就会走到这条路。
  */
#ifndef BSP_UART_H
#define BSP_UART_H

/**
  * @brief  调试串口使用的 USART 实例
  * @note   USART1(PA9/PA10) 与 USART2(PA2/PA3) 均已在 CubeMX 中初始化为
  *         115200-8N1。此处默认选 USART1，改这一行即可切换。
  */
#define BSP_DEBUG_UART_HANDLE   huart1

#endif /* BSP_UART_H */
