/**
  * @file    canopen_app.h
  * @brief   CANopen 协议栈的启动与运行接口
  *
  * 本层是 CANopenNode 与具体工程之间的胶水：
  *   - 启动序列：把协议栈对象建出来、接线到 CAN 外设、让它上线
  *   - 周期喂栈：主循环喂非实时部分，1ms 时基喂实时部分
  *
  * 它取代了上游 CanOpenSTM32 的 CO_app_STM32.c —— 那份是从站架构
  * （调 CO_LSSinit 等主站分配节点号、用默认从站 NMT、没有 SDO 客户端），
  * 本工程是主站，所以重写。
  *
  * 分层：APP/ 只调本文件的三个函数，不直接接触 CANopenNode 内部。
  */
#ifndef CANOPEN_APP_H
#define CANOPEN_APP_H

#include "CO_app_STM32.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  建立协议栈对象并按主站配置完成初始化
  * @param  node  由应用层持有并填好 CANHandle / HWInitFunction /
  *               desiredNodeID / baudrate 的对象
  * @return 0 成功；非 0 为失败步骤编号（见 canopen_app.c 内注释）
  */
int canopen_app_init(CANopenNodeSTM32* node);

/**
  * @brief  通信复位：重新初始化协议栈（NMT reset communication 会触发）
  * @return 0 成功；非 0 失败
  */
int canopen_app_resetCommunication(void);

/**
  * @brief  非实时处理，在主循环里反复调用
  * @note   内部按 HAL_GetTick() 的时间差驱动 CO_process()，
  *         NMT / SDO / 心跳等状态机都在这里推进。
  */
void canopen_app_process(void);

/**
  * @brief  实时处理，在 1ms 时基中断里调用
  * @note   本工程用 SysTick 提供 1ms，见 stm32f4xx_it.c 的 SysTick_Handler。
  *         内部有运行标志守卫，协议栈未就绪或正在重建时直接返回。
  */
void canopen_app_interrupt(void);

#ifdef __cplusplus
}
#endif

#endif /* CANOPEN_APP_H */
