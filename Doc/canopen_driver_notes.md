# CANopenNode 驱动层机制笔记

记录 CANopenNode 移植层（`framework/canopen/port/CO_driver_STM32.c`）的收发机制，
供后续编写主站业务逻辑时查阅。

- 协议栈版本：CANopenNode 锚定提交 `9b8beed`（v4.1 之后）
- 移植层来源：`https://github.com/CANopenNode/CanOpenSTM32`
- 整理日期：2026-09-10
- 行号引用对应本仓库当前状态

---

## 分层与调用方向

```
               ┌─────────────────────────────────────────────┐
   上层协议    │ NMT │ SDO服务器 │ SDO客户端 │ PDO │ EMCY │ 心跳 │
               └──────┬───────────────────────────┬──────────┘
                      │ 初始化时登记 rxArray       │ 要发帧时 CO_CANsend
                      ▼                           ▼
               ┌─────────────────────────────────────────────┐
   移植层      │  rxArray[]  路由表   │   txArray[]  数据槽    │
   (驱动)      │  CO_CANrxBufferInit  │   CO_CANtxBufferInit   │
               └──────┬───────────────────────────┬──────────┘
                      │ 中断里派发                 │ prv_send_can_message
                      ▲                           ▼
               ┌──────┴───────────────────────────┴──────────┐
   硬件        │   RX FIFO0 (3深度)   │   TxMailbox 0/1/2     │
               └─────────────────────────────────────────────┘
```

**两半契约**（本工程自己写的应用层见 `framework/canopen/canopen_app.c`）：

- `301/CO_driver.h` 规定 8 个函数由移植层实现：`CO_CANmodule_init` /
  `_disable` / `CO_CANsetConfigurationMode` / `CO_CANsetNormalMode` /
  `CO_CANrxBufferInit` / `CO_CANsend` / `CO_CANclearPendingSyncPDOs` /
  `CO_CANmodule_process`
- `CO_driver_target.h` 规定要定义的类型与临界区宏
- **收发数组本体由 CANopenNode 静态分配**（`CANopen.c:770-771`），
  移植层不分配任何缓冲

---

## 一、接收：路由表机制

### 数据结构

`rxArray[]` —— **一维数组，每项是「规则」不是「数据」**：

```c
typedef struct {
    uint16_t ident;                          /* 匹配基准 */
    uint16_t mask;                           /* 哪些位必须一致 */
    void*    object;                         /* 派发目标 */
    void   (*pCANrx_callback)(void*, void*); /* 处理函数 */
} CO_CANrx_t;                                /* ← 没有 data 字段 */
```

### 三个阶段

**① 登记（初始化，一次）**

各协议模块调 `CO_CANrxBufferInit()` 把自己的条目写进数组：

```c
/* SDO 服务器 CO_SDOserver.c:171 */
CO_CANrxBufferInit(CANdevRx, idx, idC2S, 0x7FF, false, (void*)SDO, CO_SDO_receive);
/*                               ↑ident  ↑mask        ↑object     ↑callback */
```

实测 17 处登记，mask 只用了两种：

| mask | 处数 | 匹配粒度 | 谁在用 |
| --- | --- | --- | --- |
| `0x7FF` | 15 | 功能码**+节点号**全匹配 | SDO、PDO、心跳消费者、NMT、SYNC、TIME、LSS、GFC、SRDO |
| `0x780` | 2 | 只匹配功能码（节点随意） | EMCY、节点守护主站 |

CANopen COB-ID 结构决定了两者的差别：

```
COB-ID (11 位) = [功能码 4 位][节点号 7 位]
mask = 0x7FF → 覆盖低 7 位 → 筛节点
mask = 0x780 → 不覆盖低 7 位 → 不筛节点
```

**对主站而言筛节点是核心**：心跳消费者给每个被监视节点各登记一条
（`ident = 0x700 + 从站号`，`object = &monitoredNodes[idx]`），
靠精确匹配区分是哪根轴掉线。EMCY 则相反，一条 `0x780` 收下全网所有节点的
紧急报文，由回调再解析来源。

**② 匹配（运行时，中断内）**

```c
/* prv_read_can_received_msg, CO_driver_STM32.c:595-601 */
buffer = CANModule_local->rxArray;
for (index = CANModule_local->rxSize; index > 0U; --index, ++buffer) {
    if (((rcvMsgIdent ^ buffer->ident) & buffer->mask) == 0U) {
        messageFound = 1;
        break;                    /* ← 只派发给【第一条】命中的 */
    }
}
```

> **注意**：同一帧若被多条规则匹配，只有索引最小的那条收到。各模块靠
> COB-ID 段天然错开（0x000 / 0x080 / 0x180 / 0x580 / 0x600 / 0x700 …）。
> 自行新增登记项时要留意别写出重叠的规则。

**③ 派发**

```c
if (messageFound && buffer->CANrx_callback != NULL) {
    buffer->CANrx_callback(buffer->object, (void*)&rcvMsg);   /* :606 */
}
```

### 四个特性

1. **硬件全通过，筛选全在软件** —— 硬件滤波器（`CO_driver_STM32.c:145-168`）
   配的是 ID / Mask 全 0，唯一作用是清 `CAN_FMR.FINIT` 把接收打开，不筛任何东西
2. **mask 控制粒度，object 控制去向** —— 两件事正交，各模块自由组合
3. **没命中就静默丢弃** —— `messageFound` 保持 0，回调不执行
4. **全程在 ISR 内同步跑完** —— 无队列、无缓冲，从总线到协议模块一步到位

### 为什么不用硬件滤波

- bxCAN 硬件滤波器只能做「通过 / 丢弃」，**做不到「按 ID 分派到不同对象」**，
  而 CANopen 恰恰需要后者
- 硬件滤波格式各芯片不同（bxCAN / FDCAN），软件滤波一套代码通用
- 代价是每帧都要进中断 + 遍历比较；代价可接受时优先软件方案

---

## 二、发送：槽 + 邮箱两级管理

### 两级结构

```
txArray[N]        每个发送对象一个固定槽，自带 data[8]     ← 一级（软件）
    ↓  prv_send_can_message() 把数据拷进去
TxMailbox 0/1/2   硬件真正发送的地方，只有 3 个            ← 二级（硬件）
```

### 数据结构

```c
typedef struct {
    uint32_t ident;
    uint8_t  DLC;
    uint8_t  data[8];              /* ← 数据内嵌，不是指针 */
    volatile bool_t bufferFull;    /* 这一项还占着，等待续发 */
    volatile bool_t syncFlag;
} CO_CANtx_t;
```

### 生命周期

**① 占槽（初始化，一次，永久持有）**

```c
/* CO_SDOserver.c:174 */
SDO->CANtxBuff = CO_CANtxBufferInit(module, idx, idS2C, false, 8, false);
```

**② 填数据 + 发起**

```c
/* CO_SDOserver.c:1372-1375 —— 唯一的发送入口，没有分支 */
SDO->CANtxBuff->data[0] = 0xA1;
CO_CANsend(SDO->CANdevTx, SDO->CANtxBuff);
```

**上层没有「选择权」**：数据永远写在对象自己的槽里，`CO_CANsend` 是唯一入口。
走立即发还是挂起，由驱动根据邮箱有无空位决定。

**③ 试发**（调用者上下文，通常是主循环）

```c
/* prv_send_can_message, CO_driver_STM32.c:264-346 */
uint8_t success = 0;                                 /* :267 默认失败 */
if (HAL_CAN_GetTxMailboxesFreeLevel(...) > 0) {      /* :326 有空邮箱才尝试 */
    success = HAL_CAN_AddTxMessage(...) == HAL_OK;
}
return success;
```

**④ 挂起**（没空邮箱时）

```c
/* CO_CANsend 内，被 CO_LOCK_CAN_SEND 包着 */
} else {
    if (!buffer->bufferFull) {
        buffer->bufferFull = true;
        CANmodule->CANtxCount++;
    }
}
```

**⑤ 续发**（TX 中断内）

```c
/* CO_CANinterrupt_TX, CO_driver_STM32.c:702-735 */
if (CANmodule->CANtxCount > 0U) {
    buffer = &CANmodule->txArray[0];              /* 从索引 0 开始 */
    for (i = CANmodule->txSize; i > 0U; --i, ++buffer) {
        if (buffer->bufferFull) {
            if (prv_send_can_message(CANmodule, buffer)) {   /* ← 同一个函数 */
                buffer->bufferFull = false;
                CANmodule->CANtxCount--;
            } else {
                break;                             /* 又满了，留给下次中断 */
            }
        }
    }
}
```

### 中断触发条件

硬件层面是 `CAN_TSR.RQCPx`（Request Completed）置位。HAL 把它分成四种结局：

| TSR 标志 | 含义 | HAL 动作 | 会走到 `CO_CANinterrupt_TX` 吗 |
| --- | --- | --- | --- |
| `TXOKx` | 发送**成功** | `TxMailboxxCompleteCallback` | ✅ 会 |
| `ALSTx` | 仲裁丢失 | 只置 errorcode | ❌ 不会 |
| `TERRx` | 发送出错 | 只置 errorcode | ❌ 不会 |
| 其它 | 被中止 | `TxMailboxxAbortCallback` | ❌ 不会 |

**所以严格说只有「发送成功」才触发续发。** `ALST`/`TERR` 靠 `can.c` 里配的
`AutoRetransmission = ENABLE` 由硬件自动重传兜底，最终等到 `TXOK`。

> 这也是当初那两项配置必须从 CubeMX 默认的 `DISABLE` 改成 `ENABLE` 的原因之一 ——
> 若关闭自动重传，一次仲裁丢失就可能让积压永远等不到唤醒。

### 三条不变量

| # | 不变量 | 意义 |
| --- | --- | --- |
| 1 | `bufferFull=true` ⟹ 置位时三个邮箱全满 ⟹ **必有在途传输** ⟹ 必被唤醒 | 置位条件本身保证自举，**不会死锁** |
| 2 | 数据始终在 `txArray` 槽里，直到拷进邮箱 | `bufferFull` 期间**内容不许改**（上游注释明确要求） |
| 3 | 多个挂起时**按索引顺序**发送 | 遍历从 `txArray[0]` 开始 |

### 关于不变量 1 的论证

`prv_send_can_message` 返回 0 有两种可能：

1. `GetTxMailboxesFreeLevel() == 0` —— **正常路径**，邮箱全满意味着必有帧在途，
   其完成必然触发 `TXOK` 中断。积压不可能脱离在途传输单独存在。
2. `HAL_CAN_AddTxMessage` 失败 —— **理论异常**。但该函数在有空邮箱时的实现就是
   「挑一个空邮箱写进去」，不会失败；上游注释也写着 `/* Should not fail */`。

**首次发送**（邮箱全空）会直接成功，根本不置位、不需要中断 —— 自举无问题。

### `CO_LOCK_CAN_SEND` 保护的其实不只是数据

```c
CO_LOCK_CAN_SEND(CANmodule);
    ... 判断邮箱满 + 置位 bufferFull ...
CO_UNLOCK_CAN_SEND(CANmodule);
```

若不加锁会有竞态：主循环判断「邮箱满」→ 尚未置位 → 中断恰好此刻触发、
续发完毕、发现 `CANtxCount == 0` 直接返回 → 主循环这才置位 →
**这一帧被永久遗忘在数组里**。

所以这个锁保护的是「**判断-置位序列的原子性**」，不只是内存一致性。

### 为什么「立即发」和「续发」是同一个函数

「把一帧塞进硬件邮箱」只有一种做法，第一次试和第一百次试没有区别。
所以驱动里没有两个「管理员」，只有：

```c
CO_CANtx_t::bufferFull       /* 这一项还占着 */
CO_CANmodule_t::CANtxCount   /* 一共积压几项（0 则什么都不用做） */
```

> **续发不是异常补救，而是负载高时的常态。** 总线密集时邮箱经常是满的，
> 相当大比例的帧都经中断送出。因此该 ISR 必须保持轻量。

---

## 三、收发对照

| | 接收 | 发送 |
| --- | --- | --- |
| 驱动侧结构 | `rxArray[]` | `txArray[]` |
| 存业务数据吗 | **否**（规则表） | **是**（`data[8]` 内嵌） |
| 登记接口 | `CO_CANrxBufferInit` | `CO_CANtxBufferInit` |
| 操作接口 | ——（自动匹配派发） | `CO_CANsend` |
| 硬件侧对应 | 3 深度 RX FIFO | 3 个 TX 邮箱 |
| 在中断里做 | **全部**（取帧 + 匹配 + 派发） | **只有续发**（首次尝试在调用者上下文） |
| 满了/没命中怎么办 | 没命中静默丢弃；FIFO 溢出丢帧 | 挂起排到 `txArray`，中断续发 |
| 谁分配数组 | CANopenNode 静态分配 | 同左 |

**为什么收发不对称**：

| | 接收 | 发送 |
| --- | --- | --- |
| 触发者 | 总线（外部，不可预测） | 协议栈自己（可预测） |
| 中断里的工作量 | 全部 | 只在「邮箱满」时续发 |
| 原因 | 必须尽快腾空 FIFO，否则溢出丢帧 | 有空位就顺手发了，不必绕一圈中断 |

---

## 四、一句话概括

> **收**：硬件全收 → 软件按 `(ident, mask)` 路由到 `object` → 中断里同步完成。
>
> **发**：对象往自己的槽写数据 → 发起时试塞邮箱 → 塞不进就挂 `bufferFull`
> 排队 → 每次硬件发完一帧腾出位置，中断回来把积压的往前推。

---

## 相关文件

| 文件 | 说明 |
| --- | --- |
| `framework/canopen/port/CO_driver_STM32.c` | 驱动实现（上游原样，未改） |
| `framework/canopen/port/CO_driver_target.h` | 目标类型与临界区宏（上游原样，未改） |
| `framework/canopen/port/CO_driver_custom.h` | 本项目的编译期功能配置（主站开关） |
| `framework/canopen/port/PATCHES.md` | 相对上游的改动记录 |
| `framework/canopen/canopen_app.c` | 本项目的启动 / 喂栈胶水 |
| `framework/canopen/CANopenNode/301/CO_driver.h` | 移植接口契约（8 个函数） |
| `Doc/port_note.md` | CAN1 外设配置记录 |
