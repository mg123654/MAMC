/*hook模块：为CAN接收帧提供旁路钩子功能，也可扩展其他钩子*/


#ifndef _HOOK_H_
#define _HOOK_H_
#include "ringbuf.h"
#include "stdio.h"
#include "string.h"

/* ═══════════════════════════════════════════════════════════════════
 *  接收帧旁路钩子开关 —— 改这一个数字即可
 *
 *    1 = 启用   每收到一帧回调一次 CO_CANrxCaptureHook()
 *    0 = 关闭   连调用都不编译，零开销
 *
 *  钩子是弱符号，应用层在 APP/main.c 里给出强定义覆盖下面的空壳。
 * ═══════════════════════════════════════════════════════════════════ */
#define CO_CAN_RX_CAPTURE_HOOK 1

typedef struct {
    uint16_t ident;   /* 11 位标准 ID */
    uint8_t  dlc;     /* 0..8 */
    uint8_t  data[8]; /* 未用到的字节补 0 */
} can_frame_t;

extern ringbuffer_t      *g_canLog;
extern volatile uint32_t  g_canLogDrops;  
extern uint32_t           g_dumpTick; 

void can_log_dump(void);
void CO_CANrxCaptureHook(uint32_t ident, uint8_t dlc, const uint8_t *data);

#endif /* _HOOK_H_ */
