/**
  * @file    canopen_master.c
  * @brief   CANopen 主站侧 SDO 客户端事务与 NMT 命令的实现
  */
#include "canopen_master.h"

#include "main.h" /* HAL_GetTick() */

#include <string.h>

/* ── 私有状态 ─────────────────────────────────────────────────── */

/* 结果缓冲区大小。SDO 客户端内部 FIFO 是 CO_CONFIG_SDO_CLI_BUFFER_SIZE(32)
 * 字节，本模块按同样的量级开缓冲，够读 0x1018 这种 4×4 字节的数组；
 * 再长的对象（如 0x1003 错误历史、字符串）需要分块读，本模块不支持。 */
#define CO_MASTER_MAX_DATA 32U

typedef enum {
    XFER_IDLE = 0, /**< 没有事务在跑 */
    XFER_READ,     /**< SDO 上传（读远端）进行中 */
    XFER_WRITE     /**< SDO 下载（写远端）进行中 */
} xfer_dir_t;

static CO_t* s_co = NULL;
static uint8_t s_localNodeId = 0;

static xfer_dir_t s_dir = XFER_IDLE;
static uint32_t s_lastTick = 0;
static uint32_t s_abort = 0;
static co_master_result_t s_result = CO_MASTER_OK;

static uint8_t s_buf[CO_MASTER_MAX_DATA];
static size_t s_len = 0;

static co_master_start_err_t s_startErr = CO_MASTER_START_OK;

/* 事务只在主循环上下文里推进（co_master_start_* 与 co_master_poll 都在
 * 主循环调用），CAN 中断只负责把报文塞进协议栈，不碰这些变量。 */

/* ── 内部工具 ─────────────────────────────────────────────────── */

/** @brief 结束当前事务：关掉 SDO 客户端、释放事务占用 */
static void
xfer_finish(co_master_result_t result, uint32_t abortCode) {
    /* 关客户端是必需的：CO_SDOclientClose() 会把状态置回 IDLE，而
     * CO_SDOclient_receive() 只在状态非 IDLE 时才收报文（见上游实现），
     * 所以这一步等于关掉该 SDO 通道的接收，避免关事务后还有报文写进来。 */
    CO_SDOclientClose(&s_co->SDOclient[0]);
    s_dir = XFER_IDLE;
    s_result = result;
    s_abort = abortCode;
}

/** @brief 发起事务前的公共检查与客户端配置 */
static bool_t
xfer_begin(uint8_t nodeId, xfer_dir_t dir) {
    if (s_co == NULL) {
        s_startErr = CO_MASTER_START_NOT_INIT;
        return false;
    }
    if (s_dir != XFER_IDLE) {
        s_startErr = CO_MASTER_START_BUSY;
        return false; /* 上一次事务还没结束 */
    }
    if ((nodeId == 0U) || (nodeId > 127U)) {
        s_startErr = CO_MASTER_START_BAD_NODE;
        return false; /* 节点号非法（0 是广播，不能用来做 SDO） */
    }
    if (nodeId == s_localNodeId) {
        s_startErr = CO_MASTER_START_BAD_NODE;
        /* 目标是自己。此时 SDO 请求发到自己头上、由本地 SDO 服务器处理，
         * 但本工程的 bxCAN 没开回环（见 can.c 的 MX_CAN1_Init），自己发的
         * 帧自己收不到，应答永远等不来。宁可在这里直接失败也不要空等超时。 */
        return false;
    }

    /* 按目标节点重配 SDO 客户端 CAN 通道：C2S = 0x600+nodeId，S2C = 0x580+nodeId。
     * 本工程未开 CO_CONFIG_FLAG_OD_DYNAMIC，所以 setup() 不做「参数没变就跳过」
     * 的优化，一定会重新登记 CAN 收/发缓冲 —— 这正是需要的：换轴读写时不会
     * 残留上一轴的接收缓冲。（顺带也说明 0x1280 不用配，见该函数实现。） */
    if (CO_SDOclient_setup(&s_co->SDOclient[0], (uint32_t)CO_CAN_ID_SDO_CLI + nodeId,
                           (uint32_t)CO_CAN_ID_SDO_SRV + nodeId, nodeId) != CO_SDO_RT_ok_communicationEnd) {
        s_startErr = CO_MASTER_START_SETUP_FAIL;
        return false;
    }

    /* 超时时间不在这里传：它属于「这次传输」的参数，由调用方在
     * CO_SDOclientUploadInitiate()/DownloadInitiate() 里给。 */

    s_dir = dir;
    s_abort = 0;
    s_result = CO_MASTER_OK;
    s_len = 0;
    s_startErr = CO_MASTER_START_OK;
    s_lastTick = HAL_GetTick();
    return true;
}

/* ── 对外接口 ─────────────────────────────────────────────────── */

void
co_master_init(CO_t* co, uint8_t localNodeId) {
    s_co = co;
    s_localNodeId = localNodeId;
    s_dir = XFER_IDLE;
    s_result = CO_MASTER_OK;
    s_abort = 0;
    s_len = 0;
}

bool_t
co_master_start_read(uint8_t nodeId, uint16_t index, uint8_t subIndex, uint16_t timeout_ms) {
    if (!xfer_begin(nodeId, XFER_READ)) {
        return false;
    }

    if (CO_SDOclientUploadInitiate(&s_co->SDOclient[0], index, subIndex, timeout_ms, false)
        != CO_SDO_RT_ok_communicationEnd) {
        s_dir = XFER_IDLE;
        s_startErr = CO_MASTER_START_INITIATE_FAIL;
        return false;
    }

    return true;
}

bool_t
co_master_start_write(uint8_t nodeId, uint16_t index, uint8_t subIndex, const void* data, size_t size,
                      uint16_t timeout_ms) {
    if ((data == NULL) || (size == 0U) || (size > CO_MASTER_MAX_DATA)) {
        s_startErr = CO_MASTER_START_BAD_ARG;
        return false;
    }
    if (!xfer_begin(nodeId, XFER_WRITE)) {
        return false;
    }

    CO_SDOclient_t* sdo = &s_co->SDOclient[0];

    /* sizeIndicated 传真实长度：≤4 字节走快速传输（请求里带上长度和值，
     * 一条报文完事）；>4 字节自动转分段传输（CO_CONFIG_SDO_CLI_SEGMENTED 已开）。 */
    if (CO_SDOclientDownloadInitiate(sdo, index, subIndex, size, timeout_ms, false) != CO_SDO_RT_ok_communicationEnd) {
        s_dir = XFER_IDLE;
        s_startErr = CO_MASTER_START_INITIATE_FAIL;
        return false;
    }

    /* 数据一次全塞进客户端 FIFO。CO_MASTER_MAX_DATA(32) 与客户端 FIFO 同量级，
     * 所以这里必然装得下，Download() 的 bufferPartial 可以恒传 false。 */
    if (CO_SDOclientDownloadBufWrite(sdo, (const uint8_t*)data, size) != size) {
        xfer_finish(CO_MASTER_ERR_ARG, 0);
        return false;
    }

    return true;
}

void
co_master_poll(void) {
    if ((s_co == NULL) || (s_dir == XFER_IDLE)) {
        return;
    }

    /* 1ms 节流。SDO 超时计时器是按「两次调用的时间差」累加的，若同一毫秒内
     * 反复调用，传入的 timeDifference_us 恒为 0，从站不应答时就永远等不到超时，
     * 事务会卡死。这里保证每次传进去的时间差至少是 1ms。 */
    uint32_t now = HAL_GetTick();
    if (now == s_lastTick) {
        return;
    }
    uint32_t timeDiff_us = (now - s_lastTick) * 1000U;
    s_lastTick = now;

    CO_SDOclient_t* sdo = &s_co->SDOclient[0];
    CO_SDO_abortCode_t abortCode = CO_SDO_AB_NONE;
    CO_SDO_return_t ret;

    if (s_dir == XFER_WRITE) {
        ret = CO_SDOclientDownload(sdo, timeDiff_us, false, false, &abortCode, NULL, NULL);
    } else {
        ret = CO_SDOclientUpload(sdo, timeDiff_us, false, &abortCode, NULL, NULL, NULL);
    }

    if (ret > 0) {
        if (ret == CO_SDO_RT_uploadDataBufferFull) {
            /* 分段读长对象时 FIFO 满了，必须先取走数据才能继续，否则会一直卡在
             * 这个返回值上。取干净后直接 return，下一轮接着推进。 */
            s_len += CO_SDOclientUploadBufRead(sdo, &s_buf[s_len], sizeof(s_buf) - s_len);
            if (s_len >= sizeof(s_buf)) {
                /* 对象比本模块的缓冲还长，放弃。这不是通信故障，是能力上限。 */
                xfer_finish(CO_MASTER_ERR_ARG, 0);
            }
        }
        return; /* 事务进行中，等下一轮 */
    }

    if (ret < 0) {
        xfer_finish((abortCode == CO_SDO_AB_TIMEOUT) ? CO_MASTER_ERR_TIMEOUT : CO_MASTER_ERR_ABORT, (uint32_t)abortCode);
        return;
    }

    /* ret == 0：通信正常结束。读事务此时才把 FIFO 里剩余的数据取出来。 */
    if (s_dir == XFER_READ) {
        s_len += CO_SDOclientUploadBufRead(sdo, &s_buf[s_len], sizeof(s_buf) - s_len);
    }
    xfer_finish(CO_MASTER_OK, 0);
}

bool_t
co_master_busy(void) {
    return (s_dir != XFER_IDLE) ? true : false;
}

co_master_start_err_t
co_master_get_start_error(void) {
    return s_startErr;
}

const char*
co_master_start_error_name(co_master_start_err_t err) {
    switch (err) {
        case CO_MASTER_START_OK: return "OK";
        case CO_MASTER_START_NOT_INIT: return "协议栈未初始化";
        case CO_MASTER_START_BUSY: return "上一次事务未结束";
        case CO_MASTER_START_BAD_NODE: return "节点号非法（0/超范围/等于本机）";
        case CO_MASTER_START_BAD_ARG: return "数据为空或超长";
        case CO_MASTER_START_SETUP_FAIL: return "SDO 客户端配置失败";
        case CO_MASTER_START_INITIATE_FAIL: return "SDO 客户端拒绝发起";
        default: return "?";
    }
}

co_master_result_t
co_master_get_result(void) {
    return s_result;
}

uint32_t
co_master_get_abort(void) {
    return s_abort;
}

size_t
co_master_get_size(void) {
    return s_len;
}

bool_t
co_master_get_u32(uint32_t* out) {
    if ((out == NULL) || (s_len < 4U)) {
        return false;
    }
    /* SDO 线上是小端，CANopenNode 把收到的字节按原序放进缓冲区，
     * 所以这里直接按小端拼。 */
    *out = (uint32_t)s_buf[0] | ((uint32_t)s_buf[1] << 8) | ((uint32_t)s_buf[2] << 16) | ((uint32_t)s_buf[3] << 24);
    return true;
}

bool_t
co_master_get_u16(uint16_t* out) {
    if ((out == NULL) || (s_len < 2U)) {
        return false;
    }
    *out = (uint16_t)((uint16_t)s_buf[0] | ((uint16_t)s_buf[1] << 8));
    return true;
}

bool_t
co_master_get_u8(uint8_t* out) {
    if ((out == NULL) || (s_len < 1U)) {
        return false;
    }
    *out = s_buf[0];
    return true;
}

bool_t
co_master_nmt_send(CO_NMT_command_t command, uint8_t nodeId) {
    if (s_co == NULL) {
        return false;
    }
    return (CO_NMT_sendCommand(s_co->NMT, command, nodeId) == CO_ERROR_NO) ? true : false;
}
