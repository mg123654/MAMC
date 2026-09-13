/**
  * @file    cia402.c
  * @brief   CiA 402 使能状态机 + 周期读设备类型
  *
  * 状态迁移的骨架（每一格都是一次 SDO 事务，不是一次函数调用）：
  *
  *   IDLE ──NMT Start Remote Node──▶ 等 100ms
  *     ▼
  *   链路自检：读 0x1000 设备类型 + 0x2001 每转脉冲
  *     ▼
  *   读 0x6041 ─▶ 解码 ─▶ 按状态选控制字 ─▶ 写 0x6040 ─▶ 等 50ms ─┐
  *     ▲                                                          │
  *     └──────────────────────────────────────────────────────────┘
  *     │ 直到解出 Operation Enabled
  *     ▼
  *   运动阶段（见下面的「运动脚本」）—— 先位置后速度
  *     ▼
  *   速度段跑满 CIA402_VELOCITY_RUN_MS 后自动停车
  *
  * ── 运动阶段为什么写成脚本表 ────────────────────────────────────
  * 位置段 + 速度段一共要串起十几次 SDO 事务（写模式、写速度、写目标、
  * 触发、等到位、换模式……）。按上面使能状态机那样一格一格手写，
  * 就是近三十个几乎雷同的 case。这里改成一张**顺序脚本表** + 一个小解释器：
  * 「要发生什么」一眼能看全，改顺序就是改表，不用动控制流。
  *
  * 表里少数几项的值要在运行时才知道（0x6081/0x607A/0x60FF 都得用 0x2001
  * 读回来的每转脉冲换算），所以表是**非 const** 的，进入运动阶段时由
  * motion_prepare() 把算好的值填进去。
  */
#include "cia402.h"

#include "bsp_can.h" /* bsp_can_error_counters()：读 TEC/REC */
#include "canopen_master.h"
#include "main.h" /* HAL_GetTick() */

#include <stdio.h>

/* ── 时间参数 ─────────────────────────────────────────────────── */

#define SDO_TIMEOUT_MS         500U  /**< 单次 SDO 事务等从站应答的超时 */
#define NMT_SETTLE_MS          100U  /**< 发完 NMT 后等从站进 Operational */
#define STATE_SETTLE_MS        50U   /**< 写完控制字后等驱动器状态过渡 */
#define ENABLE_TIMEOUT_MS      5000U /**< 单轮使能过程的总超时 */
#define FAULT_RESET_MAX_TRIES  3U    /**< 故障复位最多试几次 */
#define SDO_MAX_IN_ATTEMPT     3U    /**< 同一轮里 SDO 连续失败几次就放弃这一轮 */

/**
  * @brief 一轮失败后歇多久再从头来过
  * @note  不退避而是永远重试：驱动器晚上电、线没插好、上位机没打开，
  *        这些都不是「今天不会再好」的错误。重新上电才能恢复的状态机
  *        对 bring-up 是灾难 —— 总线上会安静得看不出任何线索。
  */
#define ATTEMPT_RETRY_DELAY_MS 2000U

/* ── 运动阶段的时间参数 ───────────────────────────────────────── */

#define MOTION_SETTLE_MS      100U   /**< 写完运动参数后等驱动器消化 */
#define POS_MOVE_TIMEOUT_MS   10000U /**< 位置段从触发出去到「到位」的超时 */
#define VEL_MONITOR_PERIOD_MS 200U   /**< 速度段读 0x606C 的间隔 */
#define VEL_STARTUP_GRACE_MS  500U   /**< 刚下发转速后的起转宽限期，期间不做「零速」判定 */


/* ── 私有状态 ─────────────────────────────────────────────────── */

/** @brief 内部执行步骤。比对外的 cia402_state_t 细，因为一次状态迁移
  *        要拆成「发起 SDO → 等结果 → 等过渡」好几个主循环周期。 */
typedef enum {
    STEP_IDLE = 0,
    STEP_NMT_WAIT,        /**< 等 NMT 生效 */
    STEP_LINK_DEV_START,  /**< 发起读 0x1000 —— 使能前的链路自检 */
    STEP_LINK_DEV_WAIT,
    STEP_LINK_PPR_START,  /**< 发起读 0x2001，取每转脉冲用于转速换算 */
    STEP_LINK_PPR_WAIT,
    STEP_SW_START,        /**< 发起读 0x6041 */
    STEP_SW_WAIT,         /**< 等读结果 */
    STEP_DECIDE,          /**< 解码状态字并算出下一个控制字（不耗时） */
    STEP_CW_START,        /**< 发起写 0x6040 */
    STEP_CW_WAIT,         /**< 等写结果 */
    STEP_SETTLE,          /**< 等驱动器状态过渡 */
    STEP_MOTION_PREP,     /**< 按每转脉冲算好脚本里的值（不耗时）*/
    STEP_MOP_START,       /**< 发起运动脚本里当前这一条 SDO */
    STEP_MOP_WAIT,        /**< 等结果、判条件、决定是否推进到下一条 */
    STEP_MOTION_DONE,     /**< 脚本跑完且已停车 */
    STEP_FAULT,           /**< 停在故障态（复位无效，等人工处理） */
    STEP_RETRY_WAIT       /**< 本轮失败，退避中，到点后从 STEP_IDLE 重来 */
} cia402_step_t;

static CO_t* s_co = NULL;
static uint8_t s_nodeId = CIA402_DEFAULT_NODE_ID;

static cia402_state_t s_state = CIA402_STATE_OFF;
static cia402_step_t s_step = STEP_IDLE;

static uint32_t s_deadline = 0;         /**< arm() 设的到期时刻 */
static uint32_t s_enableStartTick = 0;  /**< 使能过程起点，用于总超时 */

static uint16_t s_statusword = 0;
static uint16_t s_lastControlword = 0;  /**< 上次真正写进 0x6040 的值 */
static uint16_t s_nextControlword = 0;  /**< 本次要写的值 */
static cia402_sw_state_t s_lastSwState = CIA402_SW_UNKNOWN;

static uint8_t s_faultResetTries = 0;
static uint8_t s_inAttemptFails = 0;  /**< 本轮里 SDO 连续失败次数 */
static uint32_t s_attempt = 0;        /**< 第几轮尝试，只用于日志 */
static uint16_t s_lastCanStatus = 0;  /**< 上次的 CANerrorStatus，用于只在变化时打印 */
static uint8_t s_lastVerdict = 0xFFU; /**< 上次的总线判读结论；初值 0xFF 保证首轮必打印 */

/* ── 运动阶段的状态 ───────────────────────────────────────────── */

static uint32_t s_ppr = 0;      /**< 从站 0x2001 每转脉冲；0 = 还没读到 */
static uint8_t s_mopIdx = 0;    /**< 运动脚本执行到第几条 */
/* 长度写死 2 而不是用 SLOT_COUNT：motion_slot_t 在本文件靠下的运动脚本
 * 那一节里才定义，这里还看不见。改槽位数时两处一起改。 */
static int32_t s_slot[2] = {0}; /**< 脚本里 MOP_READ 读回来的值，索引见 motion_slot_t */

static int32_t s_velActualPs = 0;     /**< 最近一次读到的 0x606C 实际速度（p/s）*/
static uint32_t s_targetVelPs = 0;    /**< 本次下发的目标速度（p/s），用于校验 */
static uint32_t s_velRunStart = 0;    /**< 速度段起点，用于 CIA402_VELOCITY_RUN_MS 超时 */
static uint8_t s_velPhase = 0;        /**< 速度段交替读 0x6041/0x606C 的相位 */
static uint32_t s_mopDeadline = 0;    /**< MOP_WAIT_STATE / MOP_VEL_RUN 的到期时刻 */
static uint8_t s_velZeroReads = 0;    /**< 速度段连续读到几次零速（判「没转起来」）*/
static uint32_t s_lastVelMonTick = 0; /**< 上次读 0x606C 的时刻，用于限流 */

/* ── 运动脚本 ───────────────────────────────────────────────────
 *
 * 一张顺序表 + 一个小解释器。每一条对应一次 SDO 事务（或一段等待），
 * 解释器在 STEP_MOP_START/STEP_MOP_WAIT 里推进 s_mopIdx。
 *
 * 为什么不用手写 case：位置段加速度段要串十几次 SDO，按使能状态机那样
 * 一格一格写就是近三十个雷同分支。表写法让「到底要发生什么」在一屏内看全，
 * 改顺序 / 加一条都只是改表。
 *
 * 表里有三项的值要等 0x2001 读回来才能算（0x6081/0x607A/0x60FF 都是
 * 圈/秒 → p/s 的换算结果），所以表**不是 const**，进入运动阶段时由
 * motion_prepare() 填进去。 */

/** @brief 脚本支持的动作 */
typedef enum {
    MOP_END = 0,    /**< 脚本结束 */
    MOP_WRITE,      /**< 写对象（value/size 有效）*/
    MOP_READ,       /**< 读对象，结果存进 slot */
    MOP_CHECK_MODE, /**< 读 0x6061 并与 value 期望的模式比对，不一致算本轮失败 */
    MOP_WAIT_STATE, /**< 反复读 0x6041，等 value 里的位变 1（带超时）*/
    MOP_DELAY,      /**< 纯等待 value 毫秒 */
    MOP_VEL_RUN     /**< 速度段：交替读 0x6041/0x606C 并校验，跑到超时自动推进 */
} motion_op_t;

/** @brief MOP_READ 的结果存哪个槽
  * @note  只有**需要前后对照**的量才配一个槽（正是那两个位置）。
  *        速度段每 200ms 刷一次，存槽没意义，直接进 s_velActualPs。 */
typedef enum {
    SLOT_POS_BEFORE = 0, /**< 运动前位置 0x6064 */
    SLOT_POS_AFTER,      /**< 运动后位置 0x6064 —— 两者之差就是「电机真的动了」的证据 */
    SLOT_COUNT
} motion_slot_t;

/** @brief 运动脚本的一条 */
typedef struct {
    uint8_t     op;
    uint16_t    index;
    uint8_t     sub;
    uint8_t     size;   /**< MOP_WRITE：2 或 4 字节 */
    uint32_t    value;  /**< MOP_WRITE 的写入值 / MOP_WAIT_STATE 要等的位 /
                             MOP_CHECK_MODE 期望模式 / MOP_DELAY 的毫秒数 */
    uint8_t     slot;   /**< MOP_READ 存到哪个槽 */
    const char* name;   /**< 日志里打这句，出错时能直接看出卡在哪一条 */
} motion_cmd_t;

/** @brief 脚本条目的下标。给 motion_prepare() 用来定位那几个要算的条目 */
enum {
    MOPI_POS_SNAP = 0,
    MOPI_POS_MODE,
    MOPI_POS_MODE_CHK,
    MOPI_POS_PROFVEL,
    MOPI_POS_TARGET,
    MOPI_POS_GO,
    MOPI_POS_CLEAR,
    MOPI_POS_WAIT,
    MOPI_POS_AFTER,
    MOPI_POS_SETTLE,
    MOPI_VEL_HALT,
    MOPI_VEL_SETTLE,
    MOPI_VEL_MODE,
    MOPI_VEL_MODE_CHK,
    MOPI_VEL_UNHALT,
    MOPI_VEL_GO,
    MOPI_VEL_RUN,
    MOPI_VEL_STOP,
    MOPI_VEL_FINAL_HALT,
    MOPI_END
};

static motion_cmd_t s_motion[] = {
    /* ── 位置段：有界的一段位移，先把整条链路验证掉 ────────── */
    { MOP_READ, CIA402_OD_POSITION_ACTUAL, 0, 0, 0, SLOT_POS_BEFORE, "读运动前位置 0x6064" },
    { MOP_WRITE, CIA402_OD_MODES_OF_OPERATION, 0, 1, CIA402_MODE_POSITION, 0, "写 0x6060=1 位置模式" },
    { MOP_CHECK_MODE, CIA402_OD_MODES_OF_OPERATION_DISP, 0, 0, CIA402_MODE_POSITION, 0,
      "回读 0x6061 确认位置模式" },
    { MOP_WRITE, CIA402_OD_PROFILE_VELOCITY, 0, 4, 0 /*motion_prepare 填*/, 0, "写 0x6081 位置段速度" },
    { MOP_WRITE, CIA402_OD_TARGET_POSITION, 0, 4, 0 /*motion_prepare 填*/, 0, "写 0x607A 目标位置（相对）" },
    { MOP_WRITE, CIA402_OD_CONTROLWORD, 0, 2,
      CIA402_CW_ENABLE_OPERATION | CIA402_CW_NEW_SETPOINT | CIA402_CW_RELATIVE, 0, "写 0x6040=0x005F 触发相对定位" },
    { MOP_WRITE, CIA402_OD_CONTROLWORD, 0, 2, CIA402_CW_ENABLE_OPERATION | CIA402_CW_RELATIVE, 0,
      "写 0x6040=0x004F 清 bit4" },
    { MOP_WAIT_STATE, CIA402_OD_STATUSWORD, 0, 0, CIA402_SW_BIT_TARGET_REACHED, 0, "等 0x6041 bit10 到位" },
    { MOP_READ, CIA402_OD_POSITION_ACTUAL, 0, 0, 0, SLOT_POS_AFTER, "读运动后位置 0x6064" },
    { MOP_DELAY, 0, 0, 0, MOTION_SETTLE_MS, 0, "位置段收尾" },

    /* ── 切速度模式：CiA402 不保证使能状态下能改模式，先停稳 ── */
    { MOP_WRITE, CIA402_OD_CONTROLWORD, 0, 2, CIA402_CW_ENABLE_OPERATION | CIA402_CW_HALT, 0,
      "写 0x6040=0x010F 暂停" },
    { MOP_DELAY, 0, 0, 0, MOTION_SETTLE_MS, 0, "等停稳" },
    { MOP_WRITE, CIA402_OD_MODES_OF_OPERATION, 0, 1, CIA402_MODE_VELOCITY, 0, "写 0x6060=3 速度模式" },
    { MOP_CHECK_MODE, CIA402_OD_MODES_OF_OPERATION_DISP, 0, 0, CIA402_MODE_VELOCITY, 0,
      "回读 0x6061 确认速度模式" },
    { MOP_WRITE, CIA402_OD_CONTROLWORD, 0, 2, CIA402_CW_ENABLE_OPERATION, 0, "写 0x6040=0x000F 解除暂停" },

    /* ── 速度段：转起来并盯着 ──────────────────────────────── */
    { MOP_WRITE, CIA402_OD_TARGET_VELOCITY, 0, 4, 0 /*motion_prepare 填*/, 0, "写 0x60FF 目标速度" },
    { MOP_VEL_RUN, 0, 0, 0, 0, 0, "速度段运行 + 监控" },

    /* ── 停车 ──────────────────────────────────────────────── */
    { MOP_WRITE, CIA402_OD_TARGET_VELOCITY, 0, 4, 0, 0, "写 0x60FF=0 停车" },
    { MOP_WRITE, CIA402_OD_CONTROLWORD, 0, 2, CIA402_CW_ENABLE_OPERATION | CIA402_CW_HALT, 0,
      "写 0x6040=0x010F 暂停" },
    { MOP_END, 0, 0, 0, 0, 0, "脚本结束" },
};

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

/** @brief 每转脉冲，读不到 0x2001 时用兜底值 */
static uint32_t
effective_ppr(void) {
    return (s_ppr != 0U) ? s_ppr : (uint32_t)CIA402_PPR_FALLBACK;
}

/**
  * @brief 打一行实际速度，附带换算成圈/秒
  * @note  只在日志里做换算，**不参与任何判定** —— 判定一律用原始 p/s，
  *        否则兜底的每转脉冲数会给判据掺进误差。
  */
static void
print_velocity(int32_t ps) {
    const int32_t ppr = (int32_t)effective_ppr();
    const int32_t absPs = (ps < 0) ? -ps : ps;
    const int32_t milliRps = (int32_t)(((int64_t)absPs * 1000) / (int64_t)ppr);

    printf("CiA402: 实测速度 0x606C = %ld p/s（%ld.%03ld 圈/秒%s）\r\n", (long)ps, (long)(milliRps / 1000),
           (long)(milliRps % 1000), (ps < 0) ? "，反向" : "");
}

/**
  * @brief 把 0x2001 的每转脉冲换算成脚本里要用的三个值
  * @note  换算全用整数：系数是「千分之一圈」，所以
  *            p/s = ppr × 千分之一圈每秒 / 1000
  *        不用浮点是因为工程挂了 -specs=nano.specs，printf 的 %f 默认不可用，
  *        而这里的量本来也不需要小数精度。
  *        乘法先升到 uint64_t，ppr 大时（比如 100000）中间结果不会溢出。
  */
static void
motion_prepare(void) {
    const uint32_t ppr = effective_ppr();

    uint32_t posVelPs = (uint32_t)(((uint64_t)ppr * (uint64_t)CIA402_POS_SPEED_MILLI_RPS) / 1000U);
    uint32_t velPs = (uint32_t)(((uint64_t)ppr * (uint64_t)CIA402_VEL_SPEED_MILLI_RPS) / 1000U);
    uint32_t posPulses = (uint32_t)(((uint64_t)ppr * (uint64_t)CIA402_POS_TEST_REVS_MILLI) / 1000U);

    /* 硬上限。0x60FF 的单位手册没写，万一不是 p/s，一个「看着正常」的数值
     * 可能对应高得多的实际转速 —— 先在源头上拦住。 */
    if (velPs > (uint32_t)CIA402_VELOCITY_MAX_PS) {
        printf("CiA402: 目标速度 %lu p/s 超过硬上限 %u，钳到上限\r\n", (unsigned long)velPs,
               (unsigned)CIA402_VELOCITY_MAX_PS);
        velPs = (uint32_t)CIA402_VELOCITY_MAX_PS;
    }

    s_targetVelPs = velPs;

    s_motion[MOPI_POS_PROFVEL].value = posVelPs;
    s_motion[MOPI_POS_TARGET].value = posPulses;
    s_motion[MOPI_VEL_GO].value = velPs;

    printf("CiA402: 每转脉冲 = %lu（%s）\r\n", (unsigned long)ppr,
           (s_ppr != 0U) ? "读自 0x2001" : "0x2001 读不到，用兜底值");
    printf("CiA402: 位置段 = %lu 脉冲 @ %lu p/s；速度段 = %lu p/s\r\n", (unsigned long)posPulses,
           (unsigned long)posVelPs, (unsigned long)velPs);
}

/**
  * @brief 跳到脚本的第 idx 条
  * @note  顺便给「需要计时」的两类条目录起点：
  *        MOP_WAIT_STATE 置到位超时，MOP_VEL_RUN 记速度段起点并复位监控状态。
  *        起点**只在进入这条时设一次** —— 所以不能在 STEP_MOP_START 里设，
  *        那会被 MOP_WAIT_STATE 的每轮重读反复重置，超时就永远到不了。
  */
static void
mop_goto(uint8_t idx) {
    s_mopIdx = idx;

    const motion_cmd_t* c = &s_motion[idx];

    if (c->op == MOP_WAIT_STATE) {
        s_mopDeadline = HAL_GetTick() + POS_MOVE_TIMEOUT_MS;
    } else if (c->op == MOP_VEL_RUN) {
        s_velRunStart = HAL_GetTick();
        s_velPhase = 0u;
        s_velZeroReads = 0u;
        s_lastVelMonTick = HAL_GetTick();
    }

    s_step = STEP_MOP_START;
}

/**
  * @brief 速度段实测速度的校验，防的是「单位判断错」和「根本没转」
  * @param  actualPs 刚读回来的 0x606C
  * @return true 正常；false 必须立刻停车
  * @note  这是整套护栏里最重要的一层。0x60FF 的单位手册没说，只有 0x606C
  *        写了是 p/s。万一 0x60FF 其实是 rpm 之类，发一个「25000」出去，
  *        电机可能以完全不同的转速狂奔。所以下发之后必须**读回来对**，
  *        对不上就当场停车，而不是继续转着看。
  */
static bool_t
velocity_check(int32_t actualPs) {
    /* 刚下发的头几百毫秒电机还在起转，这期间判定没有意义 */
    if ((HAL_GetTick() - s_velRunStart) < VEL_STARTUP_GRACE_MS) {
        return true;
    }

    const int32_t want = (int32_t)s_targetVelPs;
    if (want <= 0) {
        return true;
    }

    if (actualPs > (want * 3)) {
        printf("CiA402: !! 实测 %ld p/s 超过目标 %ld 的 3 倍 —— 0x60FF 的单位多半不是 p/s，立即停车\r\n",
               (long)actualPs, (long)want);
        return false;
    }

    if (actualPs == 0) {
        /* 单次读到 0 可能是采样正好落在换向/整定上，连续几次才判定 */
        if (++s_velZeroReads >= 5U) {
            printf("CiA402: !! 下发了 %ld p/s 但实测一直是 0 —— 电机没转（没接电机 / 抱死 / 失步）\r\n",
                   (long)want);
            return false;
        }
    } else {
        s_velZeroReads = 0;
    }

    return true;
}

/**
  * @brief 总线状态变化时打一行，并给出可操作的判读
  * @note  从站不理你的时候，最早能看出来的是 CAN 控制器自己的错误计数器：
  *        发出去的帧没人 ACK，发送错误计数(TEC)每帧 +8，过 96 报警、128 转被动、
  *        256 掉线(bus-off)。
  *
  *        ⚠️ 判读**不能**用 CANerrorStatus 里的发送/接收位 —— STM32 的 bxCAN
  *        只有一个 EWGF / EPVF，**不区分收发**，所以 CO_driver_STM32.c:513-519
  *        把收发两侧**永远同时置位**。早先这里那条「只挂了发送侧 → 无人应答」
  *        的判据在本硬件上永远不成立，等于没写。
  *
  *        能分清的是 ESR 里的 TEC/REC 两个计数（bsp_can.h）。判读：
  *          REC > 0            → 总线上有**错误帧**：两边波特率不一致 / 线太长 / 缺终端电阻
  *          REC == 0, TEC > 0  → 本机在发但**没人 ACK**：从站没上电 / 没接 / 波特率不对
  *          两个都是 0          → 电气层正常：从站**收到了但不回**，查节点号 / 对象是否支持
  */
static void
report_bus_status(void) {
    const uint16_t st = s_co->CANmodule->CANerrorStatus;

    uint8_t tec = 0u;
    uint8_t rec = 0u;
    (void)bsp_can_error_counters(&tec, &rec);

    /* 判读结论：0=正常 1=没人 ACK 2=有错误帧。只在**结论变化**时打印，
     * 否则没人 ACK 时 TEC 每帧都变，日志会被刷屏。 */
    uint8_t verdict;
    if (rec > 0u) {
        verdict = 2u;
    } else if (tec > 0u) {
        verdict = 1u;
    } else {
        verdict = 0u;
    }

    const bool_t flagsChanged = (st != s_lastCanStatus);
    const bool_t verdictChanged = (verdict != s_lastVerdict);

    s_lastCanStatus = st;

    if (flagsChanged && (st == 0U)) {
        printf("CiA402: CAN 总线错误标志已清零\r\n");
    }

    if (!verdictChanged) {
        return;
    }
    s_lastVerdict = verdict;

    printf("CiA402: CAN 计数器 TEC=%u REC=%u | 错误标志 0x%04X\r\n", (unsigned)tec, (unsigned)rec, (unsigned)st);

    switch (verdict) {
        case 1u:
            printf("CiA402: → 本机在发但没人 ACK：查从站供电、接线、两边波特率\r\n");
            break;
        case 2u:
            printf("CiA402: → 总线上有错误帧：多半是两边波特率不一致，或线太长/缺终端电阻\r\n");
            break;
        default:
            printf("CiA402: → 电气层正常（TEC/REC 都是 0）：从站收到了但不应答，"
                   "先核对它的节点号是不是 %u\r\n",
                   (unsigned)s_nodeId);
            break;
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

    /* 运动阶段 */
    s_ppr = 0U;
    s_mopIdx = 0U;
    s_velActualPs = 0;
    s_targetVelPs = 0U;
    s_velZeroReads = 0U;
    s_velPhase = 0U;
    for (uint8_t i = 0U; i < (uint8_t)SLOT_COUNT; i++) {
        s_slot[i] = 0;
    }

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

int32_t
cia402_get_actual_velocity_ps(void) {
    return s_velActualPs;
}

uint32_t
cia402_get_pulses_per_rev(void) {
    return effective_ppr();
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
            /* 使能到手，接下来按脚本下发运动指令。motion_prepare() 在这一步
             * 把 0x2001 换算出来的三个速度/位置值填进脚本表。 */
            printf("CiA402: → 进入运动阶段（先位置后速度）\r\n");
            s_step = STEP_MOTION_PREP;
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
                s_step = STEP_LINK_DEV_START;
            }
            return;

        /* ── 链路自检 ────────────────────────────────────────────────
         * 使能之前先读两个对象，确认 SDO 通道真的通。
         *
         * 这是 Doc/CiA402主站实现.md 7.1 建议的做法。早先 0x1000 只在
         * **使能成功之后**才读，于是「SDO 根本不通」和「使能流程有问题」
         * 这两种排查方向完全相反的故障，在日志里长得一模一样 —— 都得干等
         * 5 秒使能超时才暴露，而且暴露出来的还只是「超时」。
         * 挪到使能之前，SDO 不通会**立刻**停在第一条读上。 */
        case STEP_LINK_DEV_START:
            if (co_master_busy()) {
                return;
            }
            if (!try_start(co_master_start_read(s_nodeId, CIA402_OD_DEVICE_TYPE, 0, SDO_TIMEOUT_MS))) {
                return;
            }
            s_step = STEP_LINK_DEV_WAIT;
            return;

        case STEP_LINK_DEV_WAIT: {
            if (co_master_busy()) {
                return;
            }
            uint32_t devType = 0;
            if ((co_master_get_result() != CO_MASTER_OK) || !co_master_get_u32(&devType)) {
                in_attempt_fail("链路自检：读 0x1000 设备类型失败");
                return;
            }
            s_inAttemptFails = 0;
            printf("CiA402: 链路自检 OK —— 节点 %u 设备类型 0x1000 = 0x%08lX（profile = %u）\r\n",
                   (unsigned)s_nodeId, (unsigned long)devType, (unsigned)(devType & 0xFFFFU));
            s_step = STEP_LINK_PPR_START;
            return;
        }

        case STEP_LINK_PPR_START:
            if (co_master_busy()) {
                return;
            }
            if (!try_start(co_master_start_read(s_nodeId, CIA402_OD_PULSES_PER_REV, 0, SDO_TIMEOUT_MS))) {
                return;
            }
            s_step = STEP_LINK_PPR_WAIT;
            return;

        case STEP_LINK_PPR_WAIT: {
            if (co_master_busy()) {
                return;
            }
            uint32_t ppr = 0;
            if ((co_master_get_result() == CO_MASTER_OK) && co_master_get_u32(&ppr) && (ppr != 0U)) {
                s_ppr = ppr;
            } else {
                /* 读不到**不算失败**：转速换算有兜底值，电机照样能转，
                 * 只是日志里的圈/秒换算不准。为这个把整轮判失败、
                 * 退避 2 秒重来，反而挡住了真正要做的事。 */
                printf("CiA402: 0x2001 每转脉冲读不到（abort=0x%08lX），换算改用兜底值 %u\r\n",
                       (unsigned long)co_master_get_abort(), (unsigned)CIA402_PPR_FALLBACK);
            }
            s_inAttemptFails = 0;
            s_step = STEP_SW_START;
            return;
        }

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

        /* ── 运动阶段：脚本解释器 ──────────────────────────────────
         * 两条步骤轮流跑：STEP_MOP_START 把脚本当前这一条对应的 SDO 事务
         * 发出去，STEP_MOP_WAIT 等结果、判条件、决定推到下一条还是留在原地。
         * 脚本内容见文件上半部分的 s_motion[]。 */

        case STEP_MOTION_PREP:
            motion_prepare();
            mop_goto(0u);
            return;

        case STEP_MOP_START: {
            if (co_master_busy()) {
                return;
            }

            motion_cmd_t* c = &s_motion[s_mopIdx];

            switch (c->op) {
                case MOP_END:
                    printf("CiA402: 运动脚本跑完 —— 节点 %u 仍在 Operation Enabled，已停车\r\n",
                           (unsigned)s_nodeId);
                    s_step = STEP_MOTION_DONE;
                    return;

                case MOP_DELAY:
                    arm(c->value);
                    s_step = STEP_MOP_WAIT;
                    return;

                case MOP_WRITE: {
                    /* 小端铺开，按 c->size 决定发几个字节。
                     * SDO 线上就是小端，co_master_start_write 拿到的字节原样下发；
                     * ≤4 字节走快速传输，一条报文写完。 */
                    uint8_t buf[4];
                    buf[0] = (uint8_t)(c->value & 0xFFu);
                    buf[1] = (uint8_t)((c->value >> 8) & 0xFFu);
                    buf[2] = (uint8_t)((c->value >> 16) & 0xFFu);
                    buf[3] = (uint8_t)((c->value >> 24) & 0xFFu);

                    if (!try_start(co_master_start_write(s_nodeId, c->index, c->sub, buf, c->size,
                                                         SDO_TIMEOUT_MS))) {
                        return;
                    }
                    s_step = STEP_MOP_WAIT;
                    return;
                }

                case MOP_READ:
                case MOP_CHECK_MODE:
                case MOP_WAIT_STATE:
                    if (!try_start(co_master_start_read(s_nodeId, c->index, c->sub, SDO_TIMEOUT_MS))) {
                        return;
                    }
                    s_step = STEP_MOP_WAIT;
                    return;

                case MOP_VEL_RUN:
                    /* 到点就往下走（去停车）。宏为 0 表示一直转。 */
                    if ((CIA402_VELOCITY_RUN_MS != 0U)
                        && ((HAL_GetTick() - s_velRunStart) >= (uint32_t)CIA402_VELOCITY_RUN_MS)) {
                        printf("CiA402: 速度段跑满 %u ms，停车\r\n", (unsigned)CIA402_VELOCITY_RUN_MS);
                        mop_goto((uint8_t)(s_mopIdx + 1u));
                        return;
                    }

                    /* 限流：每 VEL_MONITOR_PERIOD_MS 才读一个对象，
                     * 不占满本来就只有一条的 SDO 通道 */
                    if ((HAL_GetTick() - s_lastVelMonTick) < VEL_MONITOR_PERIOD_MS) {
                        return;
                    }

                    /* 交替读：状态字盯故障，实际速度盯「是不是真的转了」 */
                    const uint16_t idx
                        = ((s_velPhase & 1u) != 0u) ? CIA402_OD_STATUSWORD : CIA402_OD_VELOCITY_ACTUAL;

                    if (!try_start(co_master_start_read(s_nodeId, idx, 0, SDO_TIMEOUT_MS))) {
                        return;
                    }
                    s_step = STEP_MOP_WAIT;
                    return;

                default:
                    s_step = STEP_MOTION_DONE;
                    return;
            }
        }

        case STEP_MOP_WAIT: {
            if (co_master_busy()) {
                return;
            }

            motion_cmd_t* c = &s_motion[s_mopIdx];

            /* MOP_DELAY 没有 SDO 事务，只看时间 */
            if (c->op == MOP_DELAY) {
                if (elapsed()) {
                    mop_goto((uint8_t)(s_mopIdx + 1u));
                }
                return;
            }

            if (co_master_get_result() != CO_MASTER_OK) {
                in_attempt_fail(c->name);
                return;
            }
            s_inAttemptFails = 0;

            switch (c->op) {
                case MOP_WRITE:
                    printf("CiA402: %s —— OK\r\n", c->name);
                    mop_goto((uint8_t)(s_mopIdx + 1u));
                    return;

                case MOP_READ: {
                    uint32_t v = 0;
                    if (!co_master_get_u32(&v)) {
                        in_attempt_fail(c->name);
                        return;
                    }
                    s_slot[c->slot] = (int32_t)v;

                    if (c->index == CIA402_OD_POSITION_ACTUAL) {
                        printf("CiA402: %s = %ld", c->name, (long)(int32_t)v);
                        if (c->slot == (uint8_t)SLOT_POS_AFTER) {
                            /* 这就是「电机到底动没动」最直接的证据：
                             * 下发的位置指令，驱动器有没有真的走完。 */
                            const int32_t delta = s_slot[SLOT_POS_AFTER] - s_slot[SLOT_POS_BEFORE];
                            printf(" —— Δ位置 = %ld 脉冲（%s）", (long)delta,
                                   (delta != 0) ? "电机确实动了" : "!! 一点没动，位置指令没生效");
                        }
                        printf("\r\n");
                    } else {
                        printf("CiA402: %s = %ld\r\n", c->name, (long)(int32_t)v);
                    }
                    mop_goto((uint8_t)(s_mopIdx + 1u));
                    return;
                }

                case MOP_CHECK_MODE: {
                    uint8_t mode = 0;
                    if (!co_master_get_u8(&mode)) {
                        in_attempt_fail(c->name);
                        return;
                    }
                    if (mode != (uint8_t)c->value) {
                        /* 模式没被接受就别往下走了：后面的目标值全是按这个模式
                         * 解释的，模式不对等于下发了一堆没意义的数。 */
                        printf("CiA402: %s —— 驱动器回的是模式 %u，不是期望的 %lu\r\n", c->name, (unsigned)mode,
                               (unsigned long)c->value);
                        in_attempt_fail(c->name);
                        return;
                    }
                    printf("CiA402: %s —— 模式 %u 已确认\r\n", c->name, (unsigned)mode);
                    mop_goto((uint8_t)(s_mopIdx + 1u));
                    return;
                }

                case MOP_WAIT_STATE: {
                    uint16_t sw = 0;
                    if (!co_master_get_u16(&sw)) {
                        in_attempt_fail(c->name);
                        return;
                    }
                    if ((sw & (uint16_t)c->value) != 0U) {
                        printf("CiA402: %s —— 到了（状态字 0x%04X）\r\n", c->name, (unsigned)sw);
                        mop_goto((uint8_t)(s_mopIdx + 1u));
                        return;
                    }
                    if ((int32_t)(HAL_GetTick() - s_mopDeadline) >= 0) {
                        printf("CiA402: %s 超时（状态字一直停在 0x%04X）\r\n", c->name, (unsigned)sw);
                        in_attempt_fail(c->name);
                        return;
                    }
                    s_step = STEP_MOP_START; /* 同一条，再读一次 */
                    return;
                }

                case MOP_VEL_RUN:
                    if ((s_velPhase & 1u) != 0U) {
                        /* 这一轮读的是状态字：只关心有没有报故障 */
                        uint16_t sw = 0;
                        if (co_master_get_u16(&sw)) {
                            s_lastVelMonTick = HAL_GetTick();
                            if ((sw & 0x4FU) == 0x08U) {
                                printf("CiA402: 速度段驱动器报故障（状态字 0x%04X），停车\r\n", (unsigned)sw);
                                mop_goto((uint8_t)MOPI_VEL_STOP);
                                return;
                            }
                        }
                    } else {
                        /* 这一轮读的是实际速度 */
                        uint32_t v = 0;
                        if (co_master_get_u32(&v)) {
                            s_lastVelMonTick = HAL_GetTick();
                            s_velActualPs = (int32_t)v;
                            print_velocity((int32_t)v);

                            if (!velocity_check((int32_t)v)) {
                                /* 护栏拦下来了：立刻跳到脚本的停车段 */
                                mop_goto((uint8_t)MOPI_VEL_STOP);
                                return;
                            }
                        }
                    }
                    s_velPhase++;
                    s_step = STEP_MOP_START;
                    return;

                default:
                    s_step = STEP_MOTION_DONE;
                    return;
            }
        }

        case STEP_MOTION_DONE:
            return; /* 脚本跑完且已停车，停在这儿 */

        case STEP_FAULT:
        default:
            return; /* 唯一的终态：驱动器报故障且复位无效，要人工处理 */
    }
}
