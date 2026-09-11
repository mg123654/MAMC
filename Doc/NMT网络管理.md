# NMT 网络管理笔记

记录 CANopen 的 NMT（Network Management）机制：状态机、命令、**各状态允许哪些通信**，
以及本工程主站视角下的用法与陷阱。

- 协议栈版本：CANopenNode 锚定提交 `9b8beed`（v4.1 之后）
- 整理日期：2026-09-11
- 行号引用对应本仓库当前状态
- 配套文档：`Doc/OD对象字典架构.md`（对象字典）、`Doc/canopen_driver_notes.md`（驱动收发）

---

## 一、NMT 是什么

**NMT 是整个 CANopen 里唯一的总线广播命令对象。**

| 特性 | 说明 |
| --- | --- |
| COB-ID | **`0x000`** —— 全总线最高优先级（仲裁必胜） |
| 长度 | **固定 2 字节** |
| 方向 | 主站 → 广播（协议层无回应） |
| 作用 | **改变从站的运行状态** |

它没有对象字典条目 —— 是协议层的命令，由 `CO_NMT_receive()` 直接处理。

---

## 二、状态机

```
              上电 / RESET_NODE(129) / RESET_COMM(130)
                            │
                            ▼
                  ┌───────────────────┐
                  │  INITIALISING (0) │ ← 自动发一帧 Boot-up 心跳
                  └─────────┬─────────┘
                            │ 初始化完成（自动，无需命令）
                            ▼
          ┌─────────────────────────────────────┐
   128 ──▶│       PRE-OPERATIONAL  (127)        │◀── 128
 (0x80)   │   SDO ✅      PDO ❌                │   (0x80)
          └──────────────┬──────────────────────┘
                         │ 1 (0x01)
                         ▼
          ┌─────────────────────────────────────┐
          │        OPERATIONAL  (5)             │
          │   SDO ✅      PDO ✅                │
          └──────────────┬──────────────────────┘
                         │ 2 (0x02)
                         ▼
          ┌─────────────────────────────────────┐
          │         STOPPED  (4)                │
          │   只有 NMT 和心跳还活着              │
          └─────────────────────────────────────┘
                         │ 128 (0x80) 可回到 PRE-OP
                         └──────────▶
```

状态值定义在 `301/CO_NMT_Heartbeat.h:72-75`。

**状态值不是随便定的** —— `4` / `5` / `127` **同时就是心跳报文里 `data[0]` 的值**。
所以主站看心跳就能知道从站当前状态，无需额外查询。

---

## 三、能力矩阵（本节核心）

| 状态 | NMT | 心跳 | **SDO** | **PDO** | SYNC/TIME | EMCY |
| --- | --- | --- | --- | --- | --- | --- |
| **Initialising** (0) | 收 | 发 Boot-up | ❌ | ❌ | ❌ | ❌ |
| **Pre-operational** (127) | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **Operational** (5) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Stopped** (4) | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ |

**三条要记住的线：**

1. **SDO 在 Pre-op 就能用，PDO 只能在 Operational** ← 新手最大的坑
2. **Stopped 下连 SDO 都没有** —— 别想着「停下来配一下参数」
3. **只有 NMT 和心跳在所有状态都工作** —— 它们是「救命通道」，状态机本身靠它们驱动

### 为什么这么设计

**Operational 是一个「全网同步启动」的信号。** 所有轴都进了 Operational 才开始交换
过程数据 —— 保证多轴**同时**开始工作，不会有的先动有的后动。

所以标准流程永远是：

```
上电 → Pre-op（配参数，走 SDO）→ 全部配好 → NMT 广播进 Operational → 同时开跑
```

---

## 四、代码证据：状态是被「喂」给每个模块的

能力矩阵**不是文档约定，是 `CO_process()` 里实打实的布尔值传参**
（`CANopenNode/CANopen.c:1315-1497`）：

```c
/* ① 算出宽判据 */
bool_t NMTisPreOrOperational = ((NMTstate == CO_NMT_PRE_OPERATIONAL) || (NMTstate == CO_NMT_OPERATIONAL));
/*     └── Pre-op 或 Operational ──┘ */                                    /* :1315 */

/* ② 先跑 NMT，状态可能在本轮刚变 */
reset = CO_NMT_process(co->NMT, &NMTstate, timeDifference_us, timerNext_us); /* :1368 */

/* ③ 【重新计算】—— 因为上面可能刚改了状态 */
NMTisPreOrOperational = ((NMTstate == CO_NMT_PRE_OPERATIONAL) || (NMTstate == CO_NMT_OPERATIONAL));  /* :1370 */

/* ④ 分发给各模块 */
CO_SDOserver_process(&co->SDOserver[i], NMTisPreOrOperational, ...);   /* :1374 */
CO_HBconsumer_process(co->HBcons,       NMTisPreOrOperational, ...);
CO_TIME_process(co->TIME,               NMTisPreOrOperational, ...);
CO_SYNC_process(co->SYNC,               NMTisPreOrOperational, ...);

/* ⑤ PDO 用【更严】的判据 */
bool_t NMTisOperational = CO_NMT_getInternalState(co->NMT) == CO_NMT_OPERATIONAL;  /* :1441 / :1474 */
CO_RPDO_process(..., NMTisOperational, syncWas);
CO_TPDO_process(..., NMTisOperational, syncWas);
```

**两种判据，精确对应能力矩阵的两档：**

| 判据 | 覆盖模块 | 对应状态 |
| --- | --- | --- |
| `NMTisPreOrOperational` | SDO、心跳、TIME、SYNC、EMCY | Pre-op **或** Operational |
| `NMTisOperational` | **RPDO、TPDO** | **只有** Operational |

模块内部的实际门禁：

```c
/* 301/CO_SDOserver.c:609 */
} else if (!NMTisPreOrOperational || !SDO->valid) {     /* SDO：Pre-op 或 Op 才干活 */

/* 301/CO_PDO.c:754 (TPDO) / :1359 (RPDO) */
if (PDO->valid && NMTisOperational && ...) {            /* PDO：只有 Op 才干活 */
```

> **`CO_NMT_process()` 唯一的作用就是算出这个状态，然后全栈按它行事。**
> 「上层协议怎么知道该不该工作」—— 答案就是这个布尔值。

---

## 五、报文格式

```
ID  = 0x000   （固定）
DLC = 2       （固定）

┌──────────┬──────────────┐
│  命令    │   节点号     │
│ (1 字节) │   (1 字节)   │
└──────────┴──────────────┘
              └─ 0 = 全网所有节点
```

**没有响应帧。** 广播命令，硬件 ACK 之外协议层不做确认。

---

## 六、五种命令

定义在 `301/CO_NMT_Heartbeat.h:83-87`：

| 命令 | 值 | 效果 | 目标状态 |
| --- | --- | --- | --- |
| `CO_NMT_ENTER_OPERATIONAL` | **`0x01`** | 启动 | → Operational |
| `CO_NMT_ENTER_STOPPED` | **`0x02`** | 停止 | → Stopped |
| `CO_NMT_ENTER_PRE_OPERATIONAL` | **`0x80`** | 进配置态 | → Pre-operational |
| `CO_NMT_RESET_NODE` | **`0x81`** | **复位整个设备** | → 重新初始化（＝重启） |
| `CO_NMT_RESET_COMMUNICATION` | **`0x82`** | **只复位通信** | → 重新初始化通信参数 |

状态机分支在 `301/CO_NMT_Heartbeat.c:208-215`。

### `0x81` 与 `0x82` 的区别（重要）

| | RESET_NODE (`0x81`) | RESET_COMM (`0x82`) |
| --- | --- | --- |
| 通信参数（PDO 映射等） | 复位 | 复位 |
| **应用程序状态**（位置、使能…） | **复位** | **保留** |
| 主站侧收到 | `CO_RESET_APP` | `CO_RESET_COMM` |

返回值枚举见 `301/CO_NMT_Heartbeat.h:94-99`。

---

## 七、主站用法

```c
/* 单播：只命令 2 号节点进 Pre-operational */
CO_NMT_sendCommand(CO->NMT, CO_NMT_ENTER_PRE_OPERATIONAL, 2);

/* 广播：命令全网进 Operational（节点号填 0） */
CO_NMT_sendCommand(CO->NMT, CO_NMT_ENTER_OPERATIONAL, 0);
```

接口声明：`301/CO_NMT_Heartbeat.h:283`。

### ⚠️ NMT 是**异步**的 —— 发完必须等

**`CO_NMT_sendCommand()` 只是把帧发出去，从站不会立刻切状态。** 它可能还在处理
上一件事，甚至可能根本没收到（总线错误）。

**怎么知道切好了？看心跳。** 从站进了新状态后，下一帧心跳的 `data[0]` 就是新状态值：

```c
/* 心跳消费者已经在替你跟踪每个节点的状态 */
CO_NMT_internalState_t st;
if (CO_HBconsumer_getNmtState(CO->HBcons, idx, &st) == 0) {   /* 301/CO_HBconsumer.h:272 */
    if (st == CO_NMT_PRE_OPERATIONAL) { /* 可以开始配了 */ }
}
```

**标准流程是三段式：**

```
① 发 NMT 命令
② 轮询心跳，确认状态真的切过去了（带超时）
③ 确认后再进行下一步（配 SDO / 发下一道命令）
```

---

## 八、Boot-up 报文

从站上电完成初始化后，会**主动**发一帧心跳：

```
ID   = 0x700 + 节点号
DLC  = 1
data = [0x00]        ← 状态值 0 = INITIALISING
```

**这是「我上线了」的信号。** 主站可以靠它发现在线的轴 —— 比逐个 SDO 探测快得多。

---

## 九、本工程的处理

`framework/canopen/canopen_app.c:135-149` 处理的是**主站自己被复位**的情况：

```c
CO_NMT_reset_cmd_t reset_status = CO_process(CO, false, timeDiff_us, NULL);   /* :135 */

if (reset_status == CO_RESET_COMM) {          /* :137 只复位通信 */
    running = false;                          /* ← 见下 */
    CO_CANsetConfigurationMode((void*)node);
    CO_delete(CO);
    CO = NULL;
    canopen_app_init(node);                   /* 重建 */
} else if (reset_status == CO_RESET_APP) {    /* :146 整机复位 */
    HAL_NVIC_SystemReset();
}
```

**`running = false` 那一行是关键** —— 因为本工程用 SysTick 提供 1ms 时基，停不掉，
所以用标志位代替上游的 `HAL_TIM_Base_Stop_IT()`，堵住「CO 已释放、尚未重建」
窗口里中断解引用野指针（详见 `PATCHES.md`）。

> 主站一般不会收到别人的 NMT 命令（除非总线上还有第二个主站），但**协议栈要求
> 必须处理** —— 而且这个 `CO_RESET_COMM` 路径是真实存在的重入点。

---

## 十、易错点速查

| # | 易错 | 正确 |
| --- | --- | --- |
| 1 | 「SDO 通了说明配置好了，PDO 应该也能动」 | **PDO 只在 Operational 工作**，必须先发 `0x01` |
| 2 | 在 Operational 下改 PDO 参数 | 必须在 **Pre-operational** 下改 |
| 3 | 以为 Stopped 下还能用 SDO | Stopped 下**只有 NMT 和心跳** |
| 4 | 发完 NMT 命令立刻发 SDO | 从站状态切换是**异步**的，要等心跳确认 |
| 5 | 节点号填 0 以为是「本节点」 | 节点号 0 = **全网广播** |
| 6 | 分不清 `0x81` / `0x82` | `0x81` 复位整个设备，`0x82` 只复位通信（应用状态保留） |

---

## 十一、过关标准

1. 能画出四状态图，说出五种命令各自的目标状态
2. **能说出为什么「调试顺序永远是 NMT → SDO → PDO」**
3. 能解释 `NMTisPreOrOperational` 和 `NMTisOperational` 两个判据分别管哪些模块
4. 知道发完 NMT 命令后**必须等心跳确认**才能进行下一步
5. 能说出 `0x81` 和 `0x82` 的区别

---

## 十二、一句话概括

> **NMT 是唯一的总线广播命令（`ID=0x000`，2 字节），把从站在
> Initialising / Pre-op / Operational / Stopped 四态间切换。**
>
> **能力矩阵的实质是：`CO_process()` 把 NMT 状态算成两个布尔值，
> 喂给各协议模块的 `process()`。宽判据（Pre-op 或 Op）管 SDO/心跳/SYNC/TIME/EMCY，
> 窄判据（只有 Op）管 RPDO/TPDO。**
>
> **主站用法：发命令 → 等心跳确认 → 再走下一步。**

---

## 相关文件

| 文件 | 说明 |
| --- | --- |
| `framework/canopen/CANopenNode/301/CO_NMT_Heartbeat.h` | 状态/命令/复位命令枚举（`:72-99`）、`CO_NMT_process`（`:238`）、`CO_NMT_sendCommand`（`:283`） |
| `framework/canopen/CANopenNode/301/CO_NMT_Heartbeat.c` | 状态机实现（`:208-215` 命令分支） |
| `framework/canopen/CANopenNode/CANopen.c` | **两个判据的分发处**（`:1315` `:1368-1374` `:1441` `:1474`） |
| `framework/canopen/CANopenNode/301/CO_SDOserver.c` | SDO 的 NMT 门禁（`:609`） |
| `framework/canopen/CANopenNode/301/CO_PDO.c` | PDO 的 NMT 门禁（`:754` TPDO / `:1359` RPDO） |
| `framework/canopen/CANopenNode/301/CO_HBconsumer.h` | 主站查各节点 NMT 状态（`:272`） |
| `framework/canopen/canopen_app.c` | 本工程对复位命令的处理（`:135-149`） |
| `Doc/OD对象字典架构.md` | 对象字典的三层结构 |
