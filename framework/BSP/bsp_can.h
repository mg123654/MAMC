/**
  * @file    bsp_can.h
  * @brief   板级 CAN —— 直接读控制器的错误计数器
  *
  * ── 为什么需要单独读 TEC/REC ────────────────────────────────────────
  * CANopenNode 的 CO_driver_STM32.c 只把 ESR 的**错误标志位**填进
  * CANerrorStatus，而且 STM32 的 bxCAN 硬件只给一个 EWGF / EPVF，
  * **不区分收发**，所以驱动是这么写的（CO_driver_STM32.c:513-519）：
  *
  *     if (err & CAN_ESR_EWGF) status |= CO_CAN_ERRRX_WARNING | CO_CAN_ERRTX_WARNING;
  *     if (err & CAN_ESR_EPVF) status |= CO_CAN_ERRRX_PASSIVE | CO_CAN_ERRTX_PASSIVE;
  *
  * 收发两侧**永远同时置位**。于是 cia402.c 里那条
  * 「只挂了发送侧 → 本机在发但无人应答」的判据**永远不会成立**，
  * Doc/CiA402主站实现.md 5.4 节那张「分得出是发还是收」的表对本硬件不适用。
  *
  * 但 bxCAN 的 ESR 寄存器**同一次读取**里就带着两个计数器：
  *
  *     bit16..23  TEC  发送错误计数（发一帧没人 ACK 就 +8）
  *     bit24..31  REC  接收错误计数
  *
  * 驱动只是把它们掩掉了。这里补上 —— 有了 TEC/REC，「没人 ACK（从站不在／
  * 波特率不对）」和「从站收到了但不肯回（对象不支持）」就能分开了，
  * 这正是 bring-up 最需要区分的一件事。
  *
  * 放在 framework/BSP 而不是直接让 APP 去碰 HAL：应用层不感知具体硬件，
  * 这是 APP/README.md 的分层原则。
  */
#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  读 CAN 控制器的发送/接收错误计数器
  * @param  tec 输出：发送错误计数 0..255。**每发一帧没人 ACK 就 +8**
  * @param  rec 输出：接收错误计数 0..255
  * @return true 读到了；false 参数为空
  * @note   TEC/REC 的含义（ISO 11898-1）：
  *           0        —— 正常
  *           >= 96    —— 报警阈值（warning），对应 EWGF
  *           >= 128   —— 错误被动（error passive），对应 EPVF
  *           >= 256   —— 总线关闭（bus-off），控制器自己下线
  *         所以 bring-up 时看 TEC 就够了：
  *           TEC 一直涨 → 总线上没有第二个节点在 ACK（从站没上电／没接／波特率不对）
  *           TEC 恒为 0  → 电气层正常，是从站收到了但不应答（节点号不对／对象不支持）
  */
bool bsp_can_error_counters(uint8_t* tec, uint8_t* rec);

#ifdef __cplusplus
}
#endif

#endif /* BSP_CAN_H */
