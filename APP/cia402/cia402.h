/**
  * @file    cia402.h
  * @brief   CiA 402 驱动 profile 的状态机（主站侧）
  *
  * 本模块做三件事，都在主循环里非阻塞推进：
  *
  *   1. 【链路自检】用 NMT 把从站启动到 Operational，然后读一次
  *      设备类型 0x1000 和每转脉冲 0x2001。读通了就说明 SDO 通道、
  *      节点号、波特率全对 —— 不用等使能超时才能下这个结论。
  *
  *   2. 【使能】用 SDO 反复读写控制字 0x6040 / 状态字 0x6041，
  *      按 CiA 402 的状态图把驱动器从 Switch On Disabled 一路推到
  *      Operation Enabled。
  *
  *   3. 【运动】使能之后执行运动脚本（cia402.c 的 s_motion[]）：
  *      先走一段**有界**的位置（相对 1 圈），再切速度模式连续转，
  *      最后停车。运动参数在文件末尾那组 CIA402_*_MILLI_RPS 宏里调。
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

/* ── 运动相关对象索引 ─────────────────────────────────────────────
 * 全部出自 DM556-CAN 手册 4.5.2「模式及控制」表（第 11 页）。
 * 手册没写 0x60FF 的单位，但同表的 0x606C（实际速度）明确是 **p/s**，
 * 故按 p/s 处理 —— 并留了运行时校验，见 cia402.c 的转速校验。 */

#define CIA402_OD_MODES_OF_OPERATION      0x6060U /**< 运行模式写入口，手册：1=位置 3=速度 6=回零 */
#define CIA402_OD_MODES_OF_OPERATION_DISP 0x6061U /**< 运行模式回读口 —— 写 0x6060 后要读它确认 */
#define CIA402_OD_TARGET_POSITION         0x607AU /**< 目标位置（位置模式），4 字节有符号 */
#define CIA402_OD_POSITION_ACTUAL         0x6064U /**< 实际位置，4 字节有符号 */
#define CIA402_OD_PROFILE_VELOCITY        0x6081U /**< 位置模式下的速度上限，4 字节（速度模式不用它）*/
#define CIA402_OD_TARGET_VELOCITY         0x60FFU /**< 目标速度（速度模式），4 字节有符号 */
#define CIA402_OD_VELOCITY_ACTUAL         0x606CU /**< 实际速度，4 字节有符号，**单位 p/s** */

/* ── 厂商对象（DM556-CAN 手册 4.5.1「常用对象列表」）────────────
 * 注意是厂商私有区，换驱动器型号就不适用。 */
#define CIA402_OD_PULSES_PER_REV          0x2001U /**< 每转脉冲，用于 p/s ↔ 圈/秒 换算 */

/* ── 运行模式取值（写 0x6060 / 读 0x6061）手册 4.5.2 ───────────── */
#define CIA402_MODE_POSITION 1 /**< 位置模式 */
#define CIA402_MODE_VELOCITY 3 /**< 速度模式 */

/* ── 控制字附加位（叠加在 CIA402_CW_ENABLE_OPERATION 之上）─────── */
#define CIA402_CW_NEW_SETPOINT 0x0010U /**< 位置模式：置 1 把 0x607A 的新设定点锁存进去 */
#define CIA402_CW_RELATIVE     0x0040U /**< 位置模式：1 = 0x607A 是**相对**位移，0 = 绝对位置 */
#define CIA402_CW_HALT         0x0100U /**< 按各自的减速斜坡停住（模式无关，比急停温和）*/

/* ── 状态字位 ──────────────────────────────────────────────────── */
#define CIA402_SW_BIT_TARGET_REACHED 0x0400U /**< bit10：位置模式下「已到位」*/

/* ── 运动参数：要调就只调这一段 ─────────────────────────────────
 * 用「千分之一」的整数表示，不用浮点：本工程挂了 -specs=nano.specs，
 * printf 的 %f 默认不可用，而且换算本来就不需要小数。
 * 实际值 = ppr × 系数 / 1000。 */

#define CIA402_POS_TEST_REVS_MILLI   1000U /**< 位置段走多远，千分之一圈。1000 = 正好 1 圈 */
#define CIA402_POS_SPEED_MILLI_RPS   500U  /**< 位置段速度，千分之一圈/秒。500 = 0.5 圈/秒 */
#define CIA402_VEL_SPEED_MILLI_RPS   2500U /**< 速度段速度，千分之一圈/秒。2500 = 2.5 圈/秒 */

/**
  * @brief 速度段跑多久后自动停，0 = 一直转
  * @note  **默认不是 0 是有意的。** 调试器一 `halt`，主循环就停摆，
  *        但驱动器收到的是「持续转速」指令 —— **电机会一直转下去**，
  *        这时唯一能自动停下来的就是这里的超时。要长时间连续转再改成 0。
  */
#define CIA402_VELOCITY_RUN_MS 15000U

/**
  * @brief 目标转速硬上限（p/s），超过直接拒绝下发
  * @note  0x60FF 的单位手册没写。万一它不是 p/s 而是别的量纲，
  *        一个「看起来正常」的数值可能对应高得多的实际转速。
  *        这道钳位 + cia402.c 里写完后的**实测回读校验**，两层兜底。
  */
#define CIA402_VELOCITY_MAX_PS 100000U

/** @brief 0x2001 读不到时的兜底每转脉冲数（DM556 常见值，仅用于换算显示）*/
#define CIA402_PPR_FALLBACK 10000U

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
    CIA402_STATE_ENABLED,  /**< 已在 Operation Enabled，正在跑运动脚本（含跑完停住）*/
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
  * @brief  最近一次读到的实际速度（0x606C），单位 p/s
  * @return 速度；还没读到过或读失败时为 0
  * @note   速度段每 200ms 刷新一次。调试器也可以直接读 cia402.c 里的
  *         s_velActualPs，效果一样 —— 这个函数是留给以后 ROS 层用的。
  */
int32_t cia402_get_actual_velocity_ps(void);

/** @brief 目标从站的每转脉冲数（0x2001），读不到时为 CIA402_PPR_FALLBACK */
uint32_t cia402_get_pulses_per_rev(void);

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
