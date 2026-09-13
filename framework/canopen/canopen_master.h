/**
  * @file    canopen_master.h
  * @brief   CANopen 主站侧的对象字典访问接口：SDO 客户端事务 + NMT 命令
  *
  * 上游的 SDO 客户端是【非阻塞】的：调用者发起后必须反复调
  * CO_SDOclientUpload()/Download()，直到它返回 <= 0 才算结束。本模块把这套
  * 「发起 → 轮询 → 取结果」的流程包成一次**事务（transaction）**：
  *
  *     co_master_start_read(2, 0x1000, 0, 1000);   // 发起，立刻返回
  *     ...                                        // 期间继续跑主循环
  *     if (!co_master_busy()) {                   // 事务结束了
  *         if (co_master_get_result() == CO_MASTER_OK) {
  *             uint32_t v; co_master_get_u32(&v);
  *         }
  *     }
  *
  * 同一时刻只允许一次事务；busy 期间再调 start_* 会直接返回 false。
  *
  * ── 为什么不做成阻塞函数 ────────────────────────────────────────────
  * SDO 报文的**接收**在 CAN 中断里完成（CO_SDOclient_receive 是注册给驱动的
  * 回调），但**状态推进**要靠调用者周期性调 CO_SDOclientUpload/Download。
  * 如果在这里死等，主循环就停了，canopen_app_process() 不再执行 —— NMT、
  * SDO 服务器、心跳全都停摆，事务反而等不到结果。所以必须非阻塞。
  *
  * ── 层归属 ──────────────────────────────────────────────────────────
  * 这是 CANopen 协议栈的通用主站能力，不含任何业务语义，因此放在
  * framework/canopen/ 而不是 APP/（见 APP/README.md 的分层原则）。
  */
#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include "CANopen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一次 SDO 事务的最终结果 */
typedef enum {
    CO_MASTER_OK = 0,     /**< 成功结束 */
    CO_MASTER_ERR_ARG,    /**< 参数非法，或 SDO 客户端不可用、上一次事务还没结束 */
    CO_MASTER_ERR_ABORT,  /**< 从站返回了 SDO abort（错误码见 co_master_get_abort） */
    CO_MASTER_ERR_TIMEOUT /**< 从站不应答，超时（abort 码为 0x05040000） */
} co_master_result_t;

/**
  * @brief  co_master_start_read/write 返回 false 的原因
  * @note   有了它，发起失败就不再是静默的 —— 状态机死循环重试却说不出为什么，
  *         是 bring-up 阶段最难查的一类问题。
  */
typedef enum {
    CO_MASTER_START_OK = 0,       /**< 发起成功 */
    CO_MASTER_START_NOT_INIT,     /**< co_master_init 没调，协议栈没起来 */
    CO_MASTER_START_BUSY,         /**< 上一次事务还没结束 */
    CO_MASTER_START_BAD_NODE,     /**< 节点号非法，或等于本机节点号 */
    CO_MASTER_START_BAD_ARG,      /**< 数据为空或超长 */
    CO_MASTER_START_SETUP_FAIL,   /**< CO_SDOclient_setup 失败（CAN 缓冲区登记不上）*/
    CO_MASTER_START_INITIATE_FAIL /**< SDO 客户端拒绝发起（通道无效等）*/
} co_master_start_err_t;

/**
  * @brief  绑定协议栈对象，必须在 canopen_app_init() 成功之后调用一次
  * @param  co           协议栈对象（即 CANopenNodeSTM32.canOpenStack）
  * @param  localNodeId  本节点自己的节点号。用于拦住「SDO 目标就是自己」这种
  *                      配置错误 —— 本工程没开 CAN 回环，自己发给自己收不到应答
  */
void co_master_init(CO_t* co, uint8_t localNodeId);

/**
  * @brief  发起一次 SDO 上传（读远端对象）
  * @param  nodeId      目标从站节点号（1..127）
  * @param  index       对象索引
  * @param  subIndex    子索引
  * @param  timeout_ms  等待从站应答的超时，一般给 500~1000
  * @return true 已发起（接下来轮询 co_master_poll）；false 没发起成，事务未占用
  * @note   读回的数据在事务结束后用 co_master_get_* 取
  */
bool_t co_master_start_read(uint8_t nodeId, uint16_t index, uint8_t subIndex, uint16_t timeout_ms);

/**
  * @brief  发起一次 SDO 下载（写远端对象）
  * @param  data 待写数据，长度即对象长度（本模块上限 CO_MASTER_MAX_DATA 字节）
  * @param  size 字节数；写 0x6040 控制字这类 2 字节对象就传 2
  * @return true 已发起；false 没发起成
  * @note   最大 4 字节的对象会走「快速传输」，一次报文写完；
  *         超过 4 字节自动走分段传输（CO_CONFIG_SDO_CLI_SEGMENTED 已开）
  */
bool_t co_master_start_write(uint8_t nodeId, uint16_t index, uint8_t subIndex, const void* data, size_t size,
                             uint16_t timeout_ms);

/**
  * @brief  推进当前事务，非阻塞。在主循环里反复调用
  * @note   内部有 1ms 节流：同一毫秒内重复调用直接返回。
  *         这是必需的 —— SDO 超时计时器按「两次调用的时间差」累加，
  *         若同一毫秒内反复调用，时间几乎不前进，从站不应答时就等不到超时。
  */
void co_master_poll(void);

/** @brief 是否还有事务在进行中 */
bool_t co_master_busy(void);

/** @brief 取最近一次 co_master_start_* 返回 false 的原因 */
co_master_start_err_t co_master_get_start_error(void);

/** @brief 发起失败原因转可读字符串，用于 printf */
const char* co_master_start_error_name(co_master_start_err_t err);

/** @brief 取最近一次事务的结果（事务结束后才有效） */
co_master_result_t co_master_get_result(void);

/** @brief 取最近一次事务的 SDO abort 码；结果不是 ERR_ABORT/ERR_TIMEOUT 时为 0 */
uint32_t co_master_get_abort(void);

/** @brief 取最近一次【读】事务收到的字节数 */
size_t co_master_get_size(void);

/**
  * @brief  把最近一次【读】的结果按小端解释成整数
  * @return true 成功；false 当前没有数据或长度不足
  */
bool_t co_master_get_u32(uint32_t* out);
bool_t co_master_get_u16(uint16_t* out);
bool_t co_master_get_u8(uint8_t* out);

/**
  * @brief  发送 NMT 主站命令（需要 CO_CONFIG_NMT_MASTER，本项目已开）
  * @param  command  CO_NMT_command_t，常用：
  *                  CO_NMT_ENTER_OPERATIONAL(1) 启站
  *                  CO_NMT_STOPPED(2)           停站
  *                  CO_NMT_PRE_OPERATIONAL(128) 退回预操作
  *                  CO_NMT_RESET_NODE(129)      复位节点
  *                  CO_NMT_RESET_COMMUNICATION(130) 复位通信
  * @param  nodeId   目标节点号；0 表示广播（所有从站）
  * @return true 已交给 CAN 发送队列；false 参数非法或发送缓冲满
  * @note   NMT 是【无应答】的单向报文，返回 true 只代表发出去了，
  *         不代表从站真的执行了 —— 要确认状态得读从站的 0x6041 之类
  */
bool_t co_master_nmt_send(CO_NMT_command_t command, uint8_t nodeId);

#ifdef __cplusplus
}
#endif

#endif /* CANOPEN_MASTER_H */
