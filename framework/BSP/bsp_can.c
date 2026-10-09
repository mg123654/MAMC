/**
  * @file    bsp_can.c
  * @brief   板级 CAN —— TEC/REC 读取实现
  * @note    背景与用途见 bsp_can.h
  */
#include "bsp_can.h"

#include "can.h" /* hcan1 */

bool
bsp_can_error_counters(uint8_t* tec, uint8_t* rec) {
    if ((tec == 0) || (rec == 0)) {
        return false;
    }

    /* ESR 寄存器一次读完，取两个计数器：
     *   bit16..23 TEC，bit24..31 REC。掩码后即为 0..255 的计数。
     * 这是 bxCAN 的寄存器布局，STM32F407 用 bxCAN（不是 FDCAN），适用。 */
    const uint32_t esr = hcan1.Instance->ESR;

    *tec = (uint8_t)((esr >> 16) & 0xFFu);
    *rec = (uint8_t)((esr >> 24) & 0xFFu);

    return true;
}
