# BSP —— 板级驱动

本目录放**与具体开发板相关**的代码，即「换一块板子就要改」的部分。

## 职责边界

| 层 | 目录 | 关心什么 |
| --- | --- | --- |
| 芯片级 | `framework/Core` | 时钟、中断向量、启动、HAL 配置、syscalls |
| 板级 | `framework/BSP`（本目录） | 板上有什么器件、接在哪个引脚、怎么初始化 |
| 协议 | `framework/canopen` | CANopen 协议栈本身，与硬件无关 |
| 应用 | `APP` | 业务逻辑，不直接碰寄存器 |

## 后续要放什么

- **`bsp_can.c/.h`** —— CAN 收发器使能引脚（若有）、`HAL_CAN_ConfigFilter()` 过滤器
  配置、`HAL_CAN_Start()`、接收通知注册。CANopen 协议栈只通过这一层收发报文，
  不直接调 HAL。
- **`bsp_uart.c/.h`** —— `__io_putchar()` / `__io_getchar()` 实现（`framework/Core/Src/syscalls.c`
  里只有弱声明，未定义，目前直接调 `printf` 会跳空指针），用于调试串口输出。
- **`bsp_led.c/.h`** —— 状态指示灯，可映射 CANopen 的 `outStatusLEDGreen` / `outStatusLEDRed`。
- 其它板上外设（EEPROM、看门狗等）。

## 现状

**空占位。** 目前 `gpio.c` / `can.c` / `usart.c` 等 CubeMX 生成的外设初始化代码
仍在 `framework/Core/Src/` 下，等真正写板级代码时再按上面的划分迁进来，
不为了填满目录而写没有实际需求的文件。
