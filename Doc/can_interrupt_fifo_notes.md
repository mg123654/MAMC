# bxCAN 中断与 FIFO 配置笔记

记录 STM32F407 bxCAN 的**接收 FIFO 结构**、**中断使能的三层链**，以及本工程
CAN1 中断配置中已存在的隐患。供后续启用双 FIFO 或排查收不到帧时查阅。

- 芯片：STM32F407VET6
- 整理日期：2026-09-10
- 行号引用对应本仓库当前状态
- 配套文档：`Doc/canopen_driver_notes.md`（CANopenNode 驱动的收发机制）

---

## 一、FIFO0 / FIFO1 不是两个外设

**它们是同一个 CAN 外设内部的两个接收缓冲区。** 证据在寄存器映射里 ——
`stm32f407xx.h:253-273` 的 `CAN_TypeDef` 是**单个外设**的结构体，两个 FIFO 都在里面：

```c
typedef struct
{
  __IO uint32_t              MCR;                 /* 主控制                     :254 前 */
  __IO uint32_t              TSR;                 /* 发送状态                            */
  __IO uint32_t              RF0R;                /* FIFO0 控制寄存器           :254 */
  __IO uint32_t              RF1R;                /* FIFO1 控制寄存器           :255 */
  __IO uint32_t              IER;                 /* 中断使能（两个 FIFO 共用）           */
  __IO uint32_t              BTR;                 /* 位时序 ← can.c 配的波特率           */
  CAN_TxMailBox_TypeDef      sTxMailBox[3];       /* 3 个发送邮箱               :260 */
  CAN_FIFOMailBox_TypeDef    sFIFOMailBox[2];     /* 2 个接收 FIFO              :261 */
  CAN_FilterRegister_TypeDef sFilterRegister[28]; /* 28 组滤波器                :272 */
} CAN_TypeDef;                                    /*                            :273 */
```

真正的「两个外设」是 **CAN1 和 CAN2**，各有独立基址：

```c
#define CAN1_BASE   (APB1PERIPH_BASE + 0x6400UL)   /* stm32f407xx.h:956 */
#define CAN2_BASE   (APB1PERIPH_BASE + 0x6800UL)   /* stm32f407xx.h:957 */
```

| | 中断号 |
| --- | --- |
| CAN1_TX / CAN1_RX0 / **CAN1_RX1** / CAN1_SCE | 19 / 20 / **21** / 22 |
| CAN2_TX / CAN2_RX0 / CAN2_RX1 / CAN2_SCE | 63 / 64 / 65 / 66 |

本工程只用 CAN1，CAN2 完全未启用。

### 内部结构

```
CAN1 外设（一个地址块）
├── 位时序 BTR              ← can.c 的 Prescaler/BS1/BS2
├── 发送  sTxMailBox[3]     ← 3 个邮箱
└── 接收  sFIFOMailBox[2]   ← 两个 FIFO，各深 3 帧，互相独立
│        ├── FIFO0  ──┐
│        └── FIFO1  ──┤  一帧进哪个，由【滤波器】静态决定
└── 滤波  sFilterRegister[28] + FFA1R
```

**每个 FIFO 各深 3 帧**，各有自己的状态标志、中断和 pending 计数。不是一个大池子
的两半。

---

## 二、进哪个 FIFO 由滤波器静态决定

`FFA1R`（Filter FIFO Assignment Register）**每条滤波规则一个 bit**，配置时就钉死
了这条规则命中的帧去 FIFO0 还是 FIFO1：

```c
/* CO_driver_STM32.c:161 */
FilterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;
```

一帧报文只会进其中一个，**永远不会「FIFO0 满了溢到 FIFO1」**。

### 滤波器优先级规则（重要的坑）

多条滤波器同时命中时：

1. 32 位滤波器优先于 16 位
2. 同宽度时，Mask 模式优先
3. 同宽度同模式时，**滤波器编号小的赢**

**第 3 条决定了**：本工程 Bank 0 是一条「ID 与 Mask 全 0」的全通过规则
（`CO_driver_STM32.c:147-163`）。若保留它不动，再加任何指向 FIFO1 的规则，
**那些规则永远命中不了** —— 全被 Bank 0 截胡。想用 FIFO1 必须先重构滤波器。

### 本工程现状

只配了 Bank 0 一条全通过规则 → 全部报文进 FIFO0。**FIFO1 从头到尾是空的。**

---

## 三、中断使能的三层链

「开了中断」是三层，缺一不可：

| 层 | 内容 | 谁提供 | 对应什么 |
| --- | --- | --- | --- |
| ① 外设 | `CAN_IER` 的使能位 | `HAL_CAN_ActivateNotification` | 有报文时**举不举手** |
| ② NVIC | `CAN1_RX0_IRQn` / `CAN1_RX1_IRQn` | CubeMX 勾选 / `HAL_NVIC_EnableIRQ` | 举手了 **CPU 理不理** |
| ③ 函数体 | `CAN1_RXx_IRQHandler` | 手写 | 接了活**谁来干** |

### 中断线 ↔ FIFO 对应关系

| 中断线 | 对应 FIFO | `CAN_IER` 位 | CubeMX 里的名字 |
| --- | --- | --- | --- |
| `CAN1_RX0_IRQn` (IRQ 20) | **FIFO0** | `FMPIE0` | CAN1 RX0 interrupt |
| `CAN1_RX1_IRQn` (IRQ 21) | **FIFO1** | `FMPIE1` | CAN1 RX1 interrupt |

`CAN_IT_RX_FIFO0_MSG_PENDING` / `CAN_IT_RX_FIFO1_MSG_PENDING`
（`stm32f4xx_hal_can.h:521,524`）分别对应这两个位。

### 本工程实际状态

| | FIFO0 | FIFO1 |
| --- | --- | --- |
| ① 外设 IER | ✅ `FMPIE0` | ✅ `FMPIE1` |
| ② NVIC | ✅ 已使能 | ❌ **未使能** |
| ③ ISR 函数体 | ✅ `stm32f4xx_it.c:212` | ❌ **未定义** |

其中 ① 是驱动开的，**两个都开了**：

```c
/* CO_driver_STM32.c:183-185 —— 同时激活 FIFO0 与 FIFO1 的 pending 中断 */
HAL_CAN_ActivateNotification(..., CAN_IT_RX_FIFO0_MSG_PENDING
                                | CAN_IT_RX_FIFO1_MSG_PENDING
                                | CAN_IT_TX_MAILBOX_EMPTY)
```

### 陷阱：RX0 的中断会「顺带」搬空 FIFO1

四个 ISR 都调同一个 `HAL_CAN_IRQHandler(&hcan1)`（`stm32f4xx_it.c:212,229,237`），
而该函数会把**整个 `IER` 里所有已使能的中断源都处理一遍**
（`stm32f4xx_hal_can.c:1903` FIFO0、`:1952` FIFO1）。

所以 **RX0 中断触发时会把 FIFO1 也搬空**，容易让人误以为「不开 RX1 也行」。

**但反过来不成立**：若只有 FIFO1 有流量、RX0 一直不触发，FIFO1 就永远没人搬。
**所以 RX1 必须单独使能，不能靠顺带。**

---

## 四、⚠️ 弱别名陷阱：勾了 NVIC 却没写 ISR = 死循环

启动文件里，**所有未实现的 IRQ 都被弱别名到一个死循环**：

```asm
/* startup_stm32f407xx.s:112-114 */
Default_Handler:
Infinite_Loop:
  b  Infinite_Loop

/* :330-331 */
   .weak      CAN1_RX1_IRQHandler
   .thumb_set CAN1_RX1_IRQHandler,Default_Handler
```

向量表槽位早就留好（`:167`），`.weak` 保证「只要 C 里写了同名函数就顶掉别名」——
**不用注册、不用配置**。

### 三种状态

| ② NVIC | ③ ISR 函数体 | FIFO1 来了帧会怎样 |
| --- | --- | --- |
| **关**（现状） | 无 | CPU **永不跳过去**。NVIC pending 位静静置着，无副作用。FIFO1 填满后硬件丢弃 |
| 开 | **无** | ⚠️ 跳进 `Default_Handler` → **死循环，整机卡死** |
| 开 | 有 | ✅ 正常 |

**结论：「不勾 NVIC」是比「勾了不写 ISR」安全得多的失败模式。** 前者静默丢弃，
后者当场卡死。

> 注意：未在 NVIC 使能的中断线，即使外设举手、pending 位置位，CPU 也**永远不会
> 跳过去执行**。isr 槽位存在 ≠ 中断会被响应。

---

## 五、⚠️ 现存隐患：`.ioc` 里没有 TX / SCE

**这是一个当前就存在的雷，与是否启用 FIFO1 无关。**

```
$ grep "^NVIC\." multi-axis-canopen-master.ioc
NVIC.CAN1_RX0_IRQn=true:0:0:...      ← 只有 RX0
NVIC.SysTick_IRQn=true:15:0:...
（其余全是 Fault/PendSV 之类）
```

**没有 `NVIC.CAN1_TX_IRQn`，也没有 `NVIC.CAN1_SCE_IRQn`。**

而 `can.c:110-113` 里那四行 TX/SCE 的 NVIC 使能，位置在
`/* USER CODE BEGIN CAN1_MspInit 1 */`（**第 114 行**）**之前** —— 也就是落在
**CubeMX 的生成区**里。

### 后果

**下次在 CubeMX 里点「Generate Code」，这四行会被直接抹掉。**

抹掉之后的症状是自己注释里写的那个（`can.c:100-103`）：

> 若该中断收不到，3 个邮箱占满后发送会永久停摆，表现为「少量报文能发、连续发送卡死」

因为 `CAN_IT_TX_MAILBOX_EMPTY` 是驱动激活的，而 `CO_CANinterrupt_TX` 是唯一清除
`bufferFull` 的地方。

### 建议

**进 CubeMX 时把 CAN1 的四个中断一起勾上**（RX0 / RX1 / TX / SCE），一次性生成。
好处：

- TX / SCE 从「手改、会被覆盖」变成「CubeMX 管理、永不丢失」
- 顺带解决 RX1 的 NVIC 使能（前提是同时补 ISR，见第四节）

---

## 六、⚠️ RX0 与 RX1 必须同抢占优先级

若给 RX0 和 RX1 设**不同的**抢占优先级，会踩到一个真实竞态。看驱动这段：

```c
/* CO_driver_STM32.c:573-580 */
static CAN_RxHeaderTypeDef rx_hdr;                          /* static！不是栈变量 */
if (HAL_CAN_GetRxMessage(hcan, fifo, &rx_hdr, rcvMsg.data) != HAL_OK) return;
rcvMsg.ident = rx_hdr.StdId | ...;                          /* 用的还是那个 static */
rcvMsg.dlc   = rx_hdr.DLC;
```

`rx_hdr` 是 **`static` 存储**，两个 ISR 共用同一份。若 RX0 抢占 RX1，在 575 行与
579 行之间插进来，`rx_hdr` 会被覆盖 → **帧的 ID 和长度张冠李戴，路由到错误的对象**。

**解法：RX0 与 RX1 设成相同的抢占优先级（都是 0）。** NVIC 同级不抢占，竞态不存在。
反正按 ID 位分出的两半是对等的，没有谁更紧急。

### 建议优先级

| 中断 | 抢占优先级 | 理由 |
| --- | --- | --- |
| CAN1_RX0 / CAN1_RX1 | **0 / 0** | 同级不互相抢占；接收最紧急（FIFO 只有 3 层） |
| CAN1_TX / CAN1_SCE | 1 / 1 | 可让位给接收，晚几微秒无所谓 |
| SysTick | 15 | 最低 |

---

## 七、双 FIFO 能不能当「扩容」用？

**结论：不能当共享缓冲，只能当「另一半专用缓冲」。**

### 为什么

- 每个 FIFO 各深 3，独立
- 进哪个由滤波器静态决定，**不存在「满了溢到另一个」**
- 总容量不是 6，是 **3+3 且互不通用**

所以要让 FIFO1 有东西，必须把一部分 COB-ID 划给它 —— 而那部分 ID 就同时失去了
FIFO0 的容量。**这不是「增大缓冲区」，是「把缓冲区一分为二」。**

### 收益取决于突发是否天然分裂

真正的压力源是**多轴同步突发**：一次 SYNC 后 N 根轴同时回 TPDO，ID 是
`0x281`–`0x286`，在 500 kbit/s 上约 1.4 ms 连续到达（每帧 ~230 µs）。

| 方案 | 结果 |
| --- | --- |
| 现状（全进 FIFO0） | FIFO0 装 3 帧，**后 3 帧丢失** |
| 按节点号拆两半 | 3 帧进 FIFO0、3 帧进 FIFO1，**6 帧全收** |

拆分方法：利用**节点号的 bit2**（节点号在 ID 低 7 位内）

```
节点 1,2,3 = 0b001,0b010,0b011 → bit2 = 0
节点 4,5,6 = 0b100,0b101,0b110 → bit2 = 1
```

两条规则即可**完整二分**整个 11 位 ID 空间（并集 = 全部 ID，不会有帧被硬件丢弃）：

| Bank | `FilterIdHigh` | `FilterMaskIdHigh` | 命中 | → FIFO |
| --- | --- | --- | --- | --- |
| 0 | `0x000 << 5` | `0x004 << 5` | ID bit2 = 0（节点 0,1,2,3,8…） | FIFO0 |
| 1 | `0x004 << 5` | `0x004 << 5` | ID bit2 = 1（节点 4,5,6,7…） | FIFO1 |

细节：`FilterMaskIdLow` 还需补 `0x0006`，把 IDE 和 RTR 位也钉成 0，
否则标准帧的滤波器会连扩展帧一起放进来。

### 但收益存疑 —— 时间余量测算

| 项 | 数值 |
| --- | --- |
| 8 字节标准帧位数 | 108 位（无填充）~ 132 位（满填充） |
| 500 kbit/s 一帧时间（1 位 = 2 µs） | **216 ~ 264 µs** |
| ISR 处理一帧（取帧 + 扫 17 条 rxArray + 回调） | ~3–6 µs |
| 3 层 FIFO 提供的余量 | ~690 µs |

**ISR 比帧到达快约 40–70 倍。** 只有当有东西长时间关中断（flash 擦写、长临界区）时
才可能不够。当前 RX 负载几乎为零（无 PDO 业务、心跳几百毫秒一次、SDO 是主站自己
发起的串行请求）。

**若要真正提升 RX 抗过载能力，更对症的是加软件队列**（ISR 只入队、主循环出队处理），
那才把缓冲从「3 帧」扩到任意深，而不是换来第二个 3 帧。

**当前建议：不改。** 等真上了多轴 PDO 业务、并实测到溢出（观察 `RF0R.FOVR`）再动。

---

## 八、将来启用 FIFO1 的成套清单

**必须三件一起做，缺一不可：**

```
① 改滤波器      FilterFIFOAssignment = CAN_RX_FIFO1（或加第二个 Bank，注意 Bank 0 会截胡）
② 开 NVIC       CubeMX 勾 CAN1 RX1（同时补 TX/SCE，见第五节）＋ 优先级与 RX0 相同（见第六节）
③ 写 ISR        stm32f4xx_it.c 里加 CAN1_RX1_IRQHandler
```

| 只做了 | 结果 |
| --- | --- |
| ① | 帧进去出不来，**静默丢弃** |
| ① ② | ⚠️ **死循环** |
| ① ② ③ | ✅ 通 |

### 关于 `FMPIE1` 要不要关掉

**不建议关。** 理由：

- 今天关不关**行为完全一样** —— FIFO1 永远收不到帧，`FMPIE1` 永不举手，
  它是个「永远不会响的闹钟」
- 要关就得改 `CO_driver_STM32.c`（又一处偏离上游），且只能关在驱动里 ——
  放 `main.c` 里调一次会被 `CO_RESET_COMM` 重新初始化时又打开
- 关掉是**用「一个更隐蔽的坑」换「一个无害的待命状态」**：将来有人改滤波器、
  开 NVIC、写 ISR 三件都做齐了，**还是收不到**，因为少了一个没人记得的位

`HAL_CAN_DeactivateNotification(hcan, CAN_IT_RX_FIFO1_MSG_PENDING)` 是存在的
（`stm32f4xx_hal_can.h:708`），但没必要用。

---

## 九、一句话概括

> **FIFO0 / FIFO1 是同一外设内的两个各深 3 帧的独立缓冲，进哪个由滤波器静态决定，
> 不是共享池。** 要用 FIFO1 必须「滤波器 + NVIC + ISR」三件成套，且 RX0/RX1 同优先级。
> 不开 NVIC 是安全的（静默丢弃），**勾了 NVIC 却不写 ISR 会死循环**。

---

## 相关文件

| 文件 | 说明 |
| --- | --- |
| `framework/Core/Src/can.c` | CAN1 外设与 NVIC 配置（`:107-113` 含 TX/SCE 隐患） |
| `framework/Core/Src/stm32f4xx_it.c` | `CAN1_RX0/TX/SCE_IRQHandler`（`:212/229/237`） |
| `startup_stm32f407xx.s` | 向量表与弱别名（`:167` 槽位，`:330-331` 别名，`:112-114` 死循环） |
| `framework/canopen/port/CO_driver_STM32.c` | 滤波器配置（`:145-168`）、中断激活（`:183-185`）、`static rx_hdr`（`:573`） |
| `framework/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_can.c` | `HAL_CAN_IRQHandler` 的 FIFO0/FIFO1 分支（`:1903` / `:1952`） |
| `framework/Drivers/CMSIS/Device/ST/STM32F4xx/Include/stm32f407xx.h` | `CAN_TypeDef`（`:253-273`）、CAN1/CAN2 基址（`:956-957`） |
| `multi-axis-canopen-master.ioc` | CubeMX 配置（`:39` 仅有 RX0，缺 TX/SCE） |
| `Doc/canopen_driver_notes.md` | CANopenNode 驱动的 rxArray / txArray 收发机制 |
