#include "hook.h"

/* 生产者在 CAN1_RX0 中断里（CO_CANrxCaptureHook），消费者是本文件的
 * while 主循环 —— 单生产者单消费者，ringbuf 内部无锁即可安全并发。 */


  /* CAN 帧日志缓冲区。必须先于 canopen_app_init() 建好：
   * 接收是在 canopen_app_init() 内部的 CO_CANmodule_init() 配好滤波器
   * 之后才真正打开的，此后第一帧就可能触发钩子。
   * 512 字节 / 11 字节每条 ≈ 46 条记录。 */
 ringbuffer_t      *g_canLog      = NULL;
 
 
 volatile uint32_t  g_canLogDrops = 0;  /* 装不下而整条丢弃的帧数 */
 uint32_t           g_dumpTick    = 0;  /* 上次输出时刻 */

/* ── CAN 帧日志 ─────────────────────────────────────────────────────────
 * 由 CO_driver_STM32.c 的 CO_CANrxCaptureHook 驱动。
 * 开关在 framework/canopen/port/CO_driver_STM32.c 顶部：
 *     #define CO_CAN_RX_CAPTURE_HOOK 1
 * 关掉时驱动侧不再回调，本段代码成为无人引用的死代码，
 * 会被链接器的 --gc-sections 裁掉。 */

/* 一条 CAN 帧的日志记录：ident + dlc + data。
 * 定长 11 字节 —— 保证它总被【整条】写入或【整条】丢弃，不会写一半，
 * 否则消费端按记录长度解析就会错位。 */

void can_log_dump(void)
{
    if (g_canLog == NULL) {
        return;
    }

    const size_t   pending = ringbuf_used(g_canLog) / sizeof(can_frame_t);
    const uint32_t drops   = g_canLogDrops;
    g_canLogDrops = 0;   /* 每轮清零，"丢帧数"指本轮周期内的 */

    if ((pending == 0u) && (drops == 0u)) {
        return;   /* 这一秒一帧都没收到，不输出，避免刷屏 */
    }

    printf("---- CAN log: %u frame(s)", (unsigned)pending);
    if (drops != 0u) {
        printf(", %u dropped (buffer full)", (unsigned)drops);
    }
    printf(" ----\r\n");

    for (size_t i = 0; i < pending; i++) {
        can_frame_t f;
        if (ringbuf_read(g_canLog, &f, sizeof(f)) != sizeof(f)) {
            break;
        }
        printf("  ID=0x%03X  DLC=%u  ", (unsigned)f.ident, (unsigned)f.dlc);
        for (uint8_t k = 0u; k < f.dlc; k++) {
            printf("%02X ", (unsigned)f.data[k]);
        }
        printf("\r\n");
    }
}

/**
  * @brief  接收帧旁路钩子 —— 覆盖 CO_driver_STM32.c 里的 __weak 默认实现
  * @note   在 CAN1_RX0 中断上下文被调用，只做「整条塞进缓冲区」这一件事，
  *         不做任何阻塞或耗时操作。
  */
void CO_CANrxCaptureHook(uint32_t ident, uint8_t dlc, const uint8_t *data)
{
  if ((g_canLog == NULL) || (data == NULL) || (dlc > 8u)) {
    return;
  }

  /* 空间不够容纳【整条】记录就整条丢弃，绝不写半条 —— 否则消费端会解析错位。
   * 单生产者（只在本中断里写），检查与写入之间不会被其它生产者插入；
   * 消费者只会让空间变大，所以这个检查是安全的。 */
  if (ringbuf_space(g_canLog) < sizeof(can_frame_t)) {
    g_canLogDrops++;
    return;
  }

  can_frame_t f = {0};
  f.ident = (uint16_t)(ident & 0x7FFu);
  f.dlc   = dlc;
  memcpy(f.data, data, dlc);

  (void)ringbuf_write(g_canLog, &f, sizeof(f));
}
/* USER CODE END 0 */
