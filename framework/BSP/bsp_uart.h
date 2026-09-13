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
  *         115200-8N1。当前板子上 CH340 USB-TTL 接在 USART1(PA9/PA10)，
  *         故选 huart1；改这一行即可切换。
  */
#define BSP_DEBUG_UART_HANDLE   huart1

/**
  * @brief  是否真的往串口发字节
  * @note   置 0 时 printf 只进 RAM 环形缓冲（framework/BSP/diag_log.c），
  *         不发串口。什么时候需要关：
  *           - 板上没接 USB-TTL 时。115200 下每字节约 87µs，一行 60 字的日志
  *             要**阻塞 5ms**，而 main.c 的 CAN 帧日志每秒能打几十行 ——
  *             纯属白白拖慢主循环，还把时序搅乱。
  *        默认 1（保持原有行为）：本工程板子上 CH340 接在 USART1，
  *        留着串口日志。只有在明确不需要串口时才置 0。
  */
#define BSP_DEBUG_UART_ENABLE   1

#endif /* BSP_UART_H */
