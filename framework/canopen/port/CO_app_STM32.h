/*
 * CO_app_STM32.h
 *
 * 上游原文件（CANopenNode/CanOpenSTM32）里，这个头文件除了下面这个结构体，
 * 还声明了 canopen_app_init / canopen_app_process / canopen_app_interrupt。
 *
 * [本项目已改] 那三个函数是从站架构的实现，已被 framework/canopen/canopen_app.c
 * 取代，声明随之删除。
 *
 * 本文件之所以还保留，是因为上游驱动 CO_driver_STM32.c:32 直接 include 它，
 * 并把 CANptr 强转成 CANopenNodeSTM32* 后取 ->CANHandle / ->HWInitFunction：
 *
 *     HAL_CAN_Start(((CANopenNodeSTM32*)CANmodule->CANptr)->CANHandle)
 *
 * 为了不改上游驱动，结构体定义留在这里。详见 PATCHES.md。
 */

#ifndef CANOPENSTM32_CO_APP_STM32_H_
#define CANOPENSTM32_CO_APP_STM32_H_

#include "CANopen.h"
#include "main.h"

/**
  * @brief  应用层持有、并交给驱动的CANopen节点对象
  * @note   字段是驱动与实际用到的成员的并集：
  *         CANHandle / HWInitFunction 由驱动使用；
  *         其余由 framework/canopen/canopen_app.c 使用。
  *         上游的 timerHandle 与 outStatusLEDGreen/Red 已删除 ——
  *         前者本工程用 SysTick 代替、驱动不使用，后者无 LED 硬件。
  */
typedef struct {
    /* —— 驱动使用（CO_driver_STM32.c）—— */
#ifdef CO_STM32_FDCAN_Driver
    FDCAN_HandleTypeDef* CANHandle;     /* CAN 外设句柄，驱动据此收发 */
#else
    CAN_HandleTypeDef* CANHandle;
#endif
    void (*HWInitFunction)(void);       /* 驱动在 CO_CANmodule_init 里回调它初始化外设 */

    /* —— 应用层使用（canopen_app.c）—— */
    uint8_t desiredNodeID;              /* 本节点想用的 CANopen 节点号 */
    uint8_t activeNodeID;               /* 实际生效的节点号 */
    uint16_t baudrate;                  /* 记录用；真实位时序在 can.c 的 MX_CAN1_Init */
    CO_t* canOpenStack;                 /* CO_new() 建出的协议栈对象 */
} CANopenNodeSTM32;

#endif /* CANOPENSTM32_CO_APP_STM32_H_ */
