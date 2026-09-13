/**
  * @file    cia402.c
  * @brief   CiA 402 使能状态机 + 周期读设备类型
  *
  * 状态迁移的骨架（每一格都是一次 SDO 事务，不是一次函数调用）：
  *
  *   IDLE ──NMT Start Remote Node──▶ 等 100ms
  *     ▼
  *   读 0x6041 ─▶ 解码 ─▶ 按状态选控制字 ─▶ 写 0x6040 ─▶ 等 50ms ─┐
  *     ▲                                                          │
  *     └──────────────────────────────────────────────────────────┘
  *     │ 直到解出 Operation Enabled
  *     ▼
  *   ENABLED：每 1s 读一次 0x1000 并 printf
  */
#include "cia402.h"

#include "canopen_master.h"
#include "main.h" /* HAL_GetTick() */

#include <stdio.h>

/* ── 时间参数 ─────────────────────────────────────────────────── */

#define SDO_TIMEOUT_MS         500U  /**< 单次 SDO 事务等从站应答的超时 */
#define NMT_SETTLE_MS          100U  /**< 发完 NMT 后等从站进 Operational */
#define STATE_SETTLE_MS        50U   /**< 写完控制字后等驱动器状态过渡 */
#define ENABLE_TIMEOUT_MS      5000U /**< 单轮使能过程的总超时 */
#define DEVICE_READ_PERIOD_MS  1000U /**< 周期读设备类型的间隔 */
#define DEVICE_READ_MAX_ERRORS 3U    /**< 连续失败几次判定从站掉线 */
#define FAULT_RESET_MAX_TRIES  3U    /**< 故障复位最多试几次 */
#define SDO_MAX_IN_ATTEMPT     3U    /**< 同一轮里 SDO 连续失败几次就放弃这一轮 */

/**
  * @brief 一轮失败后歇多久再从头来过
  * @note  不退避而是永远重试：驱动器晚上电、线没插好、上位机没打开，
  *        这些都不是「今天不会再好」的错误。重新上电才能恢复的状态机
  *        对 bring-up 是灾难 —— 总线上会安静得看不出任何线索。
  */
#define ATTEMPT_RETRY_DELAY_MS 2000U


/* ── 私有状态 ─────────────────────────────────────────────────── */

/** @brief 内部执行步骤。比对外的 cia402_state_t 细，因为一次状态迁移
  *        要拆成「发起 SDO → 等结果 → 等过渡」好几个主循环周期。 */
typedef enum {
    STEP_IDLE = 0,
    STEP_NMT_WAIT,  /**< 等 NMT 生效 */
    STEP_SW_START,  /**< 发起读 0x6041 */
    STEP_SW_WAIT,   /**< 等读结果 */
    STEP_DECIDE,    /**< 解码状态字并算出下一个控制字（不耗时） */
    STEP_CW_START,  /**< 发起写 0x6040 */
    STEP_CW_WAIT,   /**< 等写结果 */
    STEP_SETTLE,    /**< 等驱动器状态过渡 */
    STEP_ENABLED,   /**< 已使能，等下一次周期读的时机，到了就发起读 0x1000 */
    STEP_DEV_WAIT,  /**< 等读结果 */
    STEP_FAULT,     /**< 停在故障态（复位无效，等人工处理） */
    STEP_RETRY_WAIT /**< 本轮失败，退避中，到点后从 STEP_IDLE 重来 */
} cia402_step_t;

static CO_t* s_co = NULL;
static uint8_t s_nodeId = CIA402_DEFAULT_NODE_ID;

static cia402_state_t s_state = CIA402_STATE_OFF;
static cia402_step_t s_step = STEP_IDLE;

static uint32_t s_deadline = 0;         /**< arm() 设的到期时刻 */
static uint32_t s_enableStartTick = 0;  /**< 使能过程起点，用于总超时 */
static uint32_t s_lastDevReadTick = 0;  /**< 上次读设备类型的时刻 */

static uint16_t s_statusword = 0;
static uint16_t s_lastControlword = 0;  /**< 上次真正写进 0x6040 的值 */
static uint16_t s_nextControlword = 0;  /**< 本次要写的值 */
static cia402_sw_state_t s_lastSwState = CIA402_SW_UNKNOWN;

static uint8_t s_faultResetTries = 0;
static uint8_t s_devReadErrors = 0;
static uint8_t s_inAttemptFails = 0;  /**< 本轮里 SDO 连续失败次数 */
static uint32_t s_attempt = 0;        /**< 第几轮尝试，只用于日志 */
static uint16_t s_lastCanStatus = 0;  /**< 上次的 CANerrorStatus，用于只在变化时打印 */

/* ── 内部工具 ─────────────────────────────────────────────────── */

static void
arm(uint32_t ms) {
    s_deadline = HAL_GetTick() + ms;
}

/** @brief 到期了吗。用有符号差值比较，HAL_GetTick() 回绕（约 49 天）也对 */
static bool_t
elapsed(void) {
    return ((int32_t)(HAL_GetTick() - s_deadline) >= 0) ? true : false;
}

static const char*
result_name(co_master_result_t r) {
    switch (r) {
        case CO_MASTER_OK: return "OK";
        case CO_MASTER_ERR_ARG: return "参数/状态错误";
        case CO_MASTER_ERR_ABORT: return "从站 abort";
        case CO_MASTER_ERR_TIMEOUT: return "超时";
        default: return "?";
    }
}

/** @brief 本轮失败，退避一段时间后从头发起 */
static void
attempt_failed(const char* what) {
    printf("CiA402: 第 %lu 轮失败 —— %s（结果=%s, abort=0x%08lX）；%u ms 后重试\r\n", (unsigned long)s_attempt, what,
           result_name(co_master_get_result()), (unsigned long)co_master_get_abort(), (unsigned)ATTEMPT_RETRY_DELAY_MS);
    s_state = CIA402_STATE_ERROR; /* 对外含义是「当前没使能」，会自动重试 */
    arm(ATTEMPT_RETRY_DELAY_MS);
    s_step = STEP_RETRY_WAIT;
}

/**
  * @brief 单次 SDO 失败的处理：先在本轮里重试几次，太多才放弃整轮
  * @note  单帧丢失是正常的，一失败就从头来过会让状态机永远收敛不了。
  */
static void
in_attempt_fail(const char* what) {
    if (++s_inAttemptFails < SDO_MAX_IN_ATTEMPT) {
        printf("CiA402: %s（本轮第 %u 次，继续重试）\r\n", what, (unsigned)s_inAttemptFails);
        arm(STATE_SETTLE_MS);
        s_step = STEP_SETTLE; /* 歇一下再重读状态字 */
        return;
    }
    attempt_failed(what);
}

/**
  * @brief 发起一次 SDO 事务，失败时按原因处理
  * @param  started  co_master_start_* 的返回值
  * @return true 已发起
  * @note   BUSY 是正常的（上一个事务还没收尾），直接返回等下一轮即可；
  *         其它原因都是真问题（节点号写错、客户端配置不上……），
  *         必须计入失败次数并报出来 —— 否则状态机会以主循环的速度空转，
  *         既不发报文也不打日志，看起来跟死机一样。
  */
static bool_t
try_start(bool_t started) {
    if (started) {
        return true;
    }
    if (co_master_get_start_error() != CO_MASTER_START_BUSY) {
        in_attempt_fail(co_master_start_error_name(co_master_get_start_error()));
    }
    return false;
}

/**
  * @brief 总线错误状态变化时打一行
  * @note  从站不理你的时候，最早能看出来的其实是 CAN 控制器自己的错误计数器：
  *        发出去的帧没人 ACK，发送错误计数器(TEC)每帧 +8，96 报报警、128 转被动、
  *        256 掉线(bus-off)。把这段打出来就能区分三件事：
  *          - 只有发送侧报警/被动 → 帧发出去了但没人应答（从站不在 / 波特率不对 / 线接错）
  *          - 接收侧也报警        → 总线上有错误帧，多半是波特率不一致
  *          - 一直是 0            → 总线电气没问题，是从站收到了但不肯回（对象不支持等）
  */
static void
report_bus_status(void) {
    uint16_t st = s_co->CANmodule->CANerrorStatus;
    if (st == s_lastCanStatus) {
        return; /* 没变化不重复打印 */
    }
    s_lastCanStatus = st;

    if (st == 0U) {
        printf("CiA402: CAN 总线错误状态已清零\r\n");
        return;
    }

    printf("CiA402: CAN 总线异常 0x%04X:", (unsigned)st);
    if ((st & CO_CAN_ERRTX_WARNING) != 0U) {
        printf(" 发送报警");
    }
    if ((st & CO_CAN_ERRTX_PASSIVE) != 0U) {
        printf(" 发送被动");
    }
    if ((st & CO_CAN_ERRTX_BUS_OFF) != 0U) {
        printf(" 发送掉线(bus-off)");
    }
    if ((st & CO_CAN_ERRRX_WARNING) != 0U) {
        printf(" 接收报警");
    }
    if ((st & CO_CAN_ERRRX_PASSIVE) != 0U) {
        printf(" 接收被动");
    }
    printf("\r\n");

    /* 只挂了发送侧，说明本机在使劲发、但从没人给过 ACK —— 总线上没有能正常
     * 收发的第二个节点。这是「驱动器没上电 / 没接 / 波特率不一致」的典型特征。 */
    if (((st & (CO_CAN_ERRTX_WARNING | CO_CAN_ERRTX_PASSIVE | CO_CAN_ERRTX_BUS_OFF)) != 0U)
        && ((st & (CO_CAN_ERRRX_WARNING | CO_CAN_ERRRX_PASSIVE)) == 0U)) {
        printf("CiA402: → 本机在发但无人应答：检查从站供电、接线、两边波特率\r\n");
    }
}

/* ── 对外接口 ─────────────────────────────────────────────────── */

void
cia402_init(CO_t* co, uint8_t nodeId) {
    s_co = co;
    s_nodeId = (nodeId != 0U) ? nodeId : CIA402_DEFAULT_NODE_ID;

    s_state = CIA402_STATE_OFF;
    s_step = STEP_IDLE;
    s_statusword = 0;
    s_lastControlword = 0;
    s_nextControlword = 0;
    s_lastSwState = CIA402_SW_UNKNOWN;
    s_faultResetTries = 0;
    s_devReadErrors = 0;

    /* 不在这里调 co_master_init()：本机节点号属于协议栈的配置，
     * 由 main.c 初始化底层时一起给（见 co_master_init 的说明）。
     * 本模块只负责「要对哪个从站做什么」。 */
}

cia402_sw_state_t
cia402_decode_statusword(uint16_t statusword) {
    /* 判定顺序照抄 CiA 402 状态表：特征位越多的状态掩码越严，必须排在前面。
     * 0x4F 掩住 bit0~3 与 bit6；0x6F 多掩住 bit5。
     * 顺序一旦调换，比如把 0x27(Operation enabled) 放到 0x07 后面，
     * Quick stop active 就会先被 0x27 那条命中，状态判断全乱。 */
    if ((statusword & 0x4FU) == 0x00U) {
        return CIA402_SW_NOT_READY_TO_SWITCH_ON;
    }
    if ((statusword & 0x4FU) == 0x40U) {
        return CIA402_SW_SWITCH_ON_DISABLED;
    }
    if ((statusword & 0x6FU) == 0x21U) {
        return CIA402_SW_READY_TO_SWITCH_ON;
    }
    if ((statusword & 0x6FU) == 0x23U) {
        return CIA402_SW_SWITCHED_ON;
    }
    if ((statusword & 0x6FU) == 0x27U) {
        return CIA402_SW_OPERATION_ENABLED;
    }
    if ((statusword & 0x6FU) == 0x07U) {
        return CIA402_SW_QUICK_STOP_ACTIVE;
    }
    if ((statusword & 0x4FU) == 0x0FU) {
        return CIA402_SW_FAULT_REACTION_ACTIVE;
    }
    if ((statusword & 0x4FU) == 0x08U) {
        return CIA402_SW_FAULT;
    }
    return CIA402_SW_UNKNOWN;
}

const char*
cia402_sw_state_name(cia402_sw_state_t state) {
    switch (state) {
        case CIA402_SW_NOT_READY_TO_SWITCH_ON: return "Not ready to switch on";
        case CIA402_SW_SWITCH_ON_DISABLED: return "Switch on disabled";
        case CIA402_SW_READY_TO_SWITCH_ON: return "Ready to switch on";
        case CIA402_SW_SWITCHED_ON: return "Switched on";
        case CIA402_SW_OPERATION_ENABLED: return "Operation enabled";
        case CIA402_SW_QUICK_STOP_ACTIVE: return "Quick stop active";
        case CIA402_SW_FAULT_REACTION_ACTIVE: return "Fault reaction active";
        case CIA402_SW_FAULT: return "Fault";
        default: return "Unknown";
    }
}

cia402_state_t
cia402_get_state(void) {
    return s_state;
}

/** @brief 解码当前状态字，算出下一个要写的控制字；或直接切到终态 */
static void
decide(void) {
    cia402_sw_state_t st = cia402_decode_statusword(s_statusword);

    if (st != s_lastSwState) {
        printf("CiA402: 状态字 0x%04X → %s\r\n", (unsigned)s_statusword, cia402_sw_state_name(st));
        s_lastSwState = st;
    }

    switch (st) {
        case CIA402_SW_OPERATION_ENABLED:
            printf("CiA402: 节点 %u 已进入 Operation Enabled（用时 %lu ms）\r\n", (unsigned)s_nodeId,
                   (unsigned long)(HAL_GetTick() - s_enableStartTick));
            s_state = CIA402_STATE_ENABLED;
            /* 把上次读取时刻往前挪一个周期，让第一次设备类型读取立刻发生，
             * 不用再干等 1 秒。 */
            s_lastDevReadTick = HAL_GetTick() - DEVICE_READ_PERIOD_MS;
            s_step = STEP_ENABLED;
            return;

        case CIA402_SW_FAULT:
            if (s_faultResetTries >= FAULT_RESET_MAX_TRIES) {
                printf("CiA402: 故障复位试了 %u 次仍无效，停在故障态\r\n", (unsigned)s_faultResetTries);
                s_state = CIA402_STATE_FAULT;
                s_step = STEP_FAULT;
                return;
            }
            /* 故障复位靠控制字 bit7 的 0→1 边沿触发。若上一次已经发过 0x0080，
             * 必须先写回 0 把 bit7 拉低，否则这次再写 0x0080 没有上升沿，
             * 驱动器根本不会理。这就是下面这个交替的原因。 */
            if ((s_lastControlword & CIA402_CW_FAULT_RESET) != 0U) {
                s_nextControlword = CIA402_CW_DISABLE_VOLTAGE;
            } else {
                s_nextControlword = CIA402_CW_FAULT_RESET;
                s_faultResetTries++;
                printf("CiA402: 检测到故障，发故障复位（第 %u 次）\r\n", (unsigned)s_faultResetTries);
            }
            break;

        case CIA402_SW_SWITCH_ON_DISABLED:
            s_nextControlword = CIA402_CW_SHUTDOWN; /* 0x0006 */
            break;

        case CIA402_SW_READY_TO_SWITCH_ON:
            s_nextControlword = CIA402_CW_SWITCH_ON; /* 0x0007 */
            break;

        case CIA402_SW_SWITCHED_ON:
        case CIA402_SW_QUICK_STOP_ACTIVE:
            /* Quick stop active 下 bit2=0（急停生效）。0x000F 会把 bit2 置 1，
             * 等于解除急停并直接使能，这正是 CiA 402 状态表里
             * Quick stop active → Operation enabled 那一格。 */
            s_nextControlword = CIA402_CW_ENABLE_OPERATION; /* 0x000F */
            break;

        case CIA402_SW_NOT_READY_TO_SWITCH_ON:
        case CIA402_SW_FAULT_REACTION_ACTIVE:
        case CIA402_SW_UNKNOWN:
        default:
            /* 过渡态：驱动器自己在往下一个状态走，这时候写什么控制字都
             * 没用（写了也不会被接受），等一会儿直接重读状态字更省事。 */
            arm(STATE_SETTLE_MS);
            s_step = STEP_SETTLE;
            return;
    }

    /* 算出来的控制字和上次写下去的一样，说明状态没往前走。这时重复写同一个
     * 值不会改变什么，只会刷屏总线，所以退化成「等一会再读一次」，
     * 让 5 秒的总超时去兜底。 */
    if ((s_nextControlword == s_lastControlword) && (s_lastControlword != 0U)) {
        arm(STATE_SETTLE_MS);
        s_step = STEP_SETTLE;
        return;
    }

    s_step = STEP_CW_START;
}

void
cia402_process(void) {
    if (s_co == NULL) {
        return;
    }

    report_bus_status();

    /* 单轮使能的总超时兜底：驱动器卡在某个中间态、或者控制字被拒，
     * 上面那些「等一会再读」的循环会一直转下去，靠这个断掉。 */
    if ((s_state == CIA402_STATE_ENABLING) && ((HAL_GetTick() - s_enableStartTick) > ENABLE_TIMEOUT_MS)) {
        printf("CiA402: 使能超时 —— %u ms 内没能到 Operation Enabled\r\n", (unsigned)ENABLE_TIMEOUT_MS);
        attempt_failed("使能超时");
        return;
    }

    switch (s_step) {
        case STEP_IDLE:
            /* 起手先发 NMT 把从站从 Pre-Operational 推到 Operational。
             * 严格说 SDO 在 Pre-Operational 就能用，使能本身不依赖 Operational；
             * 但 PDO 和心跳只在 Operational 才工作，而且这是主站的正常起手式，
             * 所以先做。NMT 是无应答报文，发完只能等一会儿再往下走。 */
            if (!co_master_nmt_send(CO_NMT_ENTER_OPERATIONAL, s_nodeId)) {
                return; /* CAN 发送缓冲满，下一轮再试 */
            }
            s_attempt++;
            s_inAttemptFails = 0;
            s_faultResetTries = 0;
            s_lastControlword = 0;
            s_lastSwState = CIA402_SW_UNKNOWN;
            printf("CiA402: 第 %lu 轮 —— 节点 %u ← NMT Start Remote Node\r\n", (unsigned long)s_attempt,
                   (unsigned)s_nodeId);
            s_state = CIA402_STATE_ENABLING;
            s_enableStartTick = HAL_GetTick();
            arm(NMT_SETTLE_MS);
            s_step = STEP_NMT_WAIT;
            return;

        case STEP_RETRY_WAIT:
            if (elapsed()) {
                s_state = CIA402_STATE_OFF;
                s_step = STEP_IDLE; /* 重新发 NMT，从头来过 */
            }
            return;

        case STEP_NMT_WAIT:
            if (elapsed()) {
                s_step = STEP_SW_START;
            }
            return;

        case STEP_SW_START:
            if (co_master_busy()) {
                return;
            }
            if (!try_start(co_master_start_read(s_nodeId, CIA402_OD_STATUSWORD, 0, SDO_TIMEOUT_MS))) {
                return;
            }
            s_step = STEP_SW_WAIT;
            return;

        case STEP_SW_WAIT:
            if (co_master_busy()) {
                return;
            }
            if ((co_master_get_result() != CO_MASTER_OK) || !co_master_get_u16(&s_statusword)) {
                in_attempt_fail("读状态字 0x6041 失败");
                return;
            }
            s_inAttemptFails = 0; /* 通了就清零，计数的是「连续」失败 */
            s_step = STEP_DECIDE;
            return;

        case STEP_DECIDE:
            decide();
            return;

        case STEP_CW_START:
            if (co_master_busy()) {
                return;
            }
            if (!try_start(co_master_start_write(s_nodeId, CIA402_OD_CONTROLWORD, 0, &s_nextControlword,
                                                 sizeof(uint16_t), SDO_TIMEOUT_MS))) {
                return;
            }
            s_lastControlword = s_nextControlword;
            s_step = STEP_CW_WAIT;
            return;

        case STEP_CW_WAIT:
            if (co_master_busy()) {
                return;
            }
            if (co_master_get_result() != CO_MASTER_OK) {
                in_attempt_fail("写控制字 0x6040 失败");
                return;
            }
            s_inAttemptFails = 0;
            arm(STATE_SETTLE_MS);
            s_step = STEP_SETTLE;
            return;

        case STEP_SETTLE:
            if (elapsed()) {
                s_step = STEP_SW_START;
            }
            return;

        case STEP_ENABLED:
            if (co_master_busy()) {
                return;
            }
            if ((HAL_GetTick() - s_lastDevReadTick) < DEVICE_READ_PERIOD_MS) {
                return; /* 还没到下一次读取的时刻 */
            }
            if (!try_start(co_master_start_read(s_nodeId, CIA402_OD_DEVICE_TYPE, 0, SDO_TIMEOUT_MS))) {
                return;
            }
            s_step = STEP_DEV_WAIT;
            return;

        case STEP_DEV_WAIT: {
            if (co_master_busy()) {
                return;
            }
            /* 无论成败都刷新计时，避免失败时疯狂重试刷屏 */
            s_lastDevReadTick = HAL_GetTick();

            uint32_t devType = 0;
            if ((co_master_get_result() != CO_MASTER_OK) || !co_master_get_u32(&devType)) {
                s_devReadErrors++;
                printf("CiA402: 读设备类型 0x1000 失败（连续 %u/%u 次，abort=0x%08lX）\r\n",
                       (unsigned)s_devReadErrors, (unsigned)DEVICE_READ_MAX_ERRORS, (unsigned long)co_master_get_abort());
                if (s_devReadErrors >= DEVICE_READ_MAX_ERRORS) {
                    /* 使能着的时候掉线（拔线、驱动器断电、总线故障），
                     * 退回重试流程：整轮重来，能连上就自动重新使能。 */
                    attempt_failed("连续读不到设备类型，判定从站掉线");
                    return;
                }
            } else {
                s_devReadErrors = 0;
                /* 0x1000 的布局：低 16 位是设备 profile 号（402 = 0x0192，
                 * 即 CiA 402 驱动 profile），高 16 位是附加信息/类型。
                 * 步进驱动器这里通常还会带上厂商自己的编码。 */
                printf("CiA402: [%lu ms] 节点 %u 设备类型 = 0x%08lX（profile = %u，type = %u）\r\n",
                       (unsigned long)HAL_GetTick(), (unsigned)s_nodeId, (unsigned long)devType,
                       (unsigned)(devType & 0xFFFFU), (unsigned)(devType >> 16));
            }
            s_step = STEP_ENABLED;
            return;
        }

        case STEP_FAULT:
        default:
            return; /* 唯一的终态：驱动器报故障且复位无效，要人工处理 */
    }
}
