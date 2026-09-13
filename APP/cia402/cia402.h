/**
  * @file    cia402.h
  * @brief   CiA 402 驱动 profile 的状态机（主站侧）
  *
  * 本模块做两件事，都在主循环里非阻塞推进：
  *
  *   1. 【使能】用 NMT 把从站启动到 Operational，再用 SDO 反复读写
  *      控制字 0x6040 / 状态字 0x6041，按 CiA 402 的状态图把驱动器
  *      从 Switch On Disabled 一路推到 Operation Enabled。
  *
  *   2. 【周期读】进入 Operation Enabled 之后，每秒用 SDO 读一次
  *      设备类型 0x1000 并 printf 出来。这一次读同时兼作 SDO 通道的
  *      在线检测 —— 连续几次读不到就判定掉线。
  *
  * ── 为什么是「读状态字 → 写控制字 → 再读」的循环 ──────────────────
  * CiA 402 的使能不是一个可以盲发一串控制字的开环过程：驱动器当前在哪个
  * 状态决定了下一步该发什么，而状态只体现在 0x6041 里。所以每一步都是
  * 「先读 0x6041 看在哪，再决定发 0x6040 的哪个命令」，驱动器中途报警、
  * 掉使能、被别的节点改状态，这个循环都能自己纠正回来。
  *
  * ── 依赖 ──────────────────────────────────────────────────────────
  * 用 framework/canopen/canopen_master.h 的 SDO 事务接口，不直接碰
  * CANopenNode 内部，也不直接调 HAL（见 APP/README.md 的分层原则）。
  */
#ifndef CIA402_H
#define CIA402_H

#include "CANopen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  目标从站的节点号
  * @note   DM556-CAN 的节点号由驱动器的 SW1~SW5 拨码设定（低 5 位，
  *         全 on 是广播地址不能用），改完要重新上电才生效。
  *         SW6/SW7 设波特率：500 kbit/s 对应 SW6=on、SW7=off。
  *         本工程主站跑 500 kbit/s，两边必须一致。
  */
#define CIA402_DEFAULT_NODE_ID 2U

/* ── CiA 402 对象索引 ─────────────────────────────────────────── */

#define CIA402_OD_DEVICE_TYPE  0x1000U /**< 设备类型，4 字节（profile 号在低 16 位） */
#define CIA402_OD_ERROR_REG    0x1001U /**< 错误寄存器，1 字节 */
#define CIA402_OD_CONTROLWORD  0x6040U /**< 控制字，2 字节，本模块写它 */
#define CIA402_OD_STATUSWORD   0x6041U /**< 状态字，2 字节，本模块读它 */

/* ── CiA 402 控制字命令（写 0x6040 的值）──────────────────────── */

#define CIA402_CW_DISABLE_VOLTAGE   0x0000U /**< 断开电压 */
#define CIA402_CW_QUICK_STOP        0x0002U /**< 快速停止 */
#define CIA402_CW_SHUTDOWN          0x0006U /**< 关闭：→ Ready to switch on */
#define CIA402_CW_SWITCH_ON         0x0007U /**< 接通：→ Switched on */
#define CIA402_CW_ENABLE_OPERATION  0x000FU /**< 使能运行：→ Operation enabled */
#define CIA402_CW_FAULT_RESET       0x0080U /**< 故障复位（上升沿有效）*/

/** @brief 模块对外可见的运行状态 */
typedef enum {
    CIA402_STATE_OFF = 0,  /**< 还没启动 / 已停止 */
    CIA402_STATE_ENABLING, /**< 正在推状态机走向 Operation Enabled */
    CIA402_STATE_ENABLED,  /**< 已在 Operation Enabled，进入周期读设备类型 */
    CIA402_STATE_FAULT,    /**< 驱动器报故障且复位无效（唯一不会自动恢复的状态）*/
    CIA402_STATE_ERROR     /**< 本轮没成功（SDO 失败 / 使能超时 / 掉线）。会自动退避重试 */
} cia402_state_t;

/** @brief CiA 402 状态字 0x6041 解出来的驱动器状态 */
typedef enum {
    CIA402_SW_UNKNOWN = 0,
    CIA402_SW_NOT_READY_TO_SWITCH_ON,
    CIA402_SW_SWITCH_ON_DISABLED,
    CIA402_SW_READY_TO_SWITCH_ON,
    CIA402_SW_SWITCHED_ON,
    CIA402_SW_OPERATION_ENABLED,
    CIA402_SW_QUICK_STOP_ACTIVE,
    CIA402_SW_FAULT_REACTION_ACTIVE,
    CIA402_SW_FAULT
} cia402_sw_state_t;

/**
  * @brief  绑定协议栈对象并启动状态机
  * @param  co     协议栈对象（CANopenNodeSTM32.canOpenStack）
  * @param  nodeId 目标从站节点号，一般传 CIA402_DEFAULT_NODE_ID
  * @note   必须在 canopen_app_init() 成功之后调用
  */
void cia402_init(CO_t* co, uint8_t nodeId);

/**
  * @brief  推进状态机，非阻塞。在主循环里反复调用
  * @note   调用顺序要和 co_master_poll() 挨着，且 co_master_poll() 在前 ——
  *         本函数靠 co_master_* 取事务结果，先让底层把事务推进完再判断。
  */
void cia402_process(void);

/** @brief 取当前状态 */
cia402_state_t cia402_get_state(void);

/**
  * @brief  把 0x6041 状态字解成 CiA 402 状态
  * @note   判定顺序照抄 CiA 402 规范的状态表：先看特征位更少（掩码更严）的
  *         状态，否则会被更宽松的掩码提前命中。对外暴露是为了方便单独测。
  */
cia402_sw_state_t cia402_decode_statusword(uint16_t statusword);

/** @brief 状态枚举转可读字符串，用于 printf */
const char* cia402_sw_state_name(cia402_sw_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_H */
