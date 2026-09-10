# 对象字典（OD）架构笔记

记录 CANopenNode v4 对象字典的**实现架构**：数据怎么放、描述符怎么组织、
索引和子索引怎么定位到具体成员。供后续写主站业务逻辑时查阅。

- 协议栈版本：CANopenNode 锚定提交 `9b8beed`（v4.1 之后）
- 整理日期：2026-09-11
- 行号引用对应本仓库当前状态
- 配套文档：`Doc/canopen_driver_notes.md`（驱动收发）、`Doc/can_interrupt_fifo_notes.md`（中断与 FIFO）

---

## 一、三层结构总览

```
① 数据层 —— 171 个成员，纯变量，无任何元数据
   OD_PERSIST_COMM_t OD_PERSIST_COMM;     /* .data  504 字节 */
   OD_RAM_t          OD_RAM;              /* .data   56 字节 */
   ↑ 名字（x1017_...）只存在于编译期；运行时只剩偏移量

② 描述符层 —— ODObjs（OD.c:250，一个 static 结构体，所有描述符都在里面）
   ├─ VAR:  1 个 OD_obj_var_t                       （3 字段，无 subIndex）
   ├─ ARR:  1 个 OD_obj_array_t（管 N 个等长元素）   （6 字段，无 subIndex）
   └─ REC:  N 个 OD_obj_record_t（每子索引一个）     （4 字段，【有】subIndex）
   ↑ 每个描述符里【写死】了 dataOrig（编译期常量地址）+ attribute + dataLength

③ 目录层 —— OD->list[]，34 个 OD_entry_t，按索引升序排列
   {index, subEntriesCount, odObjectType, odObject指针, extension}
   ↑ 统一形状，屏蔽②的类型差异，支持二分查找
```

**实测数量**：34 个索引条目 ↔ 171 个独立参数（子索引总数）。

---

## 二、第①层：数据

### 存储与分段

| 结构体 | 大小 | 段 | 内容 |
| --- | --- | --- | --- |
| `OD_PERSIST_COMM` | 504 字节 | `.data` | 声明为"要保存"的参数 |
| `OD_RAM` | 56 字节 | `.data` | 声明为"纯内存"的参数 |

两者都在 **`.data` 而不是 `.bss`** —— 因为都有**非零初值**。`.data` 的代价是
初始值要存一份在 flash 里，启动时拷到 RAM。

### ⚠️ `PERSIST` 是假的

`OD_ATTR_PERSIST_COMM` / `OD_ATTR_RAM` / `OD_ATTR_OD` 三个宏定义在
`OD/OD.h:248-261`，**都是空定义**：

```c
#ifndef OD_ATTR_PERSIST_COMM
#define OD_ATTR_PERSIST_COMM        /* 后面什么都没有 */
#endif
```

它们的设计用途是**把变量钉进指定的链接段**：

```c
/* 应用层在 include OD.h 之前定义，就能覆盖 */
#define OD_ATTR_PERSIST_COMM  __attribute__((section(".persist_comm")))
#include "OD.h"
```

**本工程没有任何人覆盖它们**（全工程搜 `define OD_ATTR_` 只有 `OD.h` 自己），
因为**存储功能是关闭的**（`CO_driver_target.h:48` 有 `#undef CO_CONFIG_STORAGE_ENABLE`，
`CO_storageBlank.c/h` 已删除）。

**结论**：`OD_PERSIST_COMM` 现在只是个**普通全局变量**，掉电后**不保留任何东西**。
运行时改的心跳周期、PDO 配置，断电全部回到 `OD.c` 里的编译期默认值。

### 可直接访问

`OD.h` 用 `extern` 声明，所以工程里任何 `.c` 都能直接读写：

```c
OD_PERSIST_COMM.x1017_producerHeartbeatTime = 1000;   /* 心跳设成 1 秒 */
```

**但有个陷阱**：直接赋值会**绕过 OD 接口**，扩展回调不触发。判据是
"这个对象有没有配套行为"：

| 改法 | 效果 |
| --- | --- |
| `OD_PERSIST_COMM.x1280_...COB_IDClientToServerTx = 0x602;` | ❌ 只改了内存，SDO 客户端**不知道** |
| `CO_SDOclient_setup(sdo, 0x602, 0x582, 2);` | ✅ 改内存**并且**重新配置客户端 |

（`0x1280` 挂了自定义 write 钩子，见 `CO_SDOclient.c:260-262`）

---

## 三、第②层：描述符

三种结构定义在 `CO_ODinterface.h:672-699`：

| 结构 | 字段 | 有 `subIndex` 吗 |
| --- | --- | --- |
| `OD_obj_var_t` | `dataOrig`, `attribute`, `dataLength` | ❌ |
| `OD_obj_array_t` | `dataOrig0`, `dataOrig`, `attribute0`, `attribute`, `dataElementLength`, `dataElementSizeof` | ❌ |
| `OD_obj_record_t` | `dataOrig`, **`subIndex`**, `attribute`, `dataLength` | ✅ |

### 字段含义（以 0x1016 为例）

```c
/* OD.c:315-322 */
.o_1016_consumerHeartbeatTime = {
    .dataOrig0         = &OD_PERSIST_COMM.x1016_consumerHeartbeatTime_sub0,  /* → uint8_t */
    .dataOrig          = &OD_PERSIST_COMM.x1016_consumerHeartbeatTime[0],    /* → uint32_t[8] */
    .attribute0        = ODA_SDO_R,                    /* 子0 只读 */
    .attribute         = ODA_SDO_RW | ODA_MB,          /* 元素可读可写 */
    .dataElementLength = 4,                            /* 协议长度 */
    .dataElementSizeof = sizeof(uint32_t)              /* 内存步长 */
},
```

| 字段 | 含义 |
| --- | --- |
| `dataOrig` | 指向**元素数组**的指针 |
| `dataOrig0` | 指向**子索引 0** 的数据（一个 `uint8_t`，存元素个数） |
| `attribute` | **元素**的属性位 |
| `attribute0` | **子索引 0** 的属性位 |
| `dataElementLength` | 单个元素的**协议长度**（字节）→ 用于 `stream->dataLength` |
| `dataElementSizeof` | 单个元素的**内存步长** → 用于**指针算术** |

> **命名规律**：`xxx0` 后缀 = 子索引 0 专用，`xxx` = 元素通用。
> `dataOrig0` 和 `dataOrig` 是一对，**不是**"起始地址"和"长度"。

**为什么 `dataElementLength` 和 `dataElementSizeof` 要分开？**

```c
/* CO_ODinterface.c:216-219 —— ARR 分支 */
stream->dataOrig   = ptr + (odo->dataElementSizeof * (uint8_t)(subIndex - 1U));
/*                          └── 算地址，用内存步长 ──┘ */
stream->dataLength = odo->dataElementLength;
/*                    └── 上报长度，用协议长度 ──┘ */
```

一个管**指针算术**，一个管**SDO 传输字节数**。多数时候相等，概念上是两回事。

---

## 四、第③层：目录

```c
/* CO_ODinterface.h:271-279 */
typedef struct {
    uint16_t index;            /* 索引 */
    uint8_t  subEntriesCount;  /* 子条目总数，【含子索引 0】 */
    uint8_t  odObjectType;     /* ODT_VAR / ODT_ARR / ODT_REC */
    void*    odObject;         /* 指向描述符块 */
    OD_extension_t* extension; /* 应用层钩子，通常 NULL */
} OD_entry_t;
```

### 为什么需要这一层

三种描述符的**字段数不同**（3 / 6 / 4），大小不一，**没法放进同一个数组**。

目录把它抹平：**固定大小 → 能排成数组 → 能二分查找**。

> 这和驱动层 `rxArray` 把各模块回调统一成 `{ident, mask, object, callback}`
> 是同一个套路 —— **用一层间接抹平异构**。

### ⚠️ index **不是**数组下标

```
list[0]  = 0x1000
list[1]  = 0x1001
list[2]  = 0x1003      ← 0x1002 不存在
list[3]  = 0x1005      ← 0x1004 也不存在
...
list[12] = 0x1017      ← 下标 12，索引 0x1017
```

**索引是稀疏的**，拿它当数组下标得开 65536 项。所以 `OD_find` 用**二分查找**
（`CO_ODinterface.c:143-178`），前提是表按索引**升序排列**：

```c
/* Fast search in ordered Object Dictionary. If indexes are mixed, this won't work. */
while (min < max) {
    uint16_t cur = (min + max) >> 1;
    OD_entry_t* entry = &od->list[cur];
    if (index == entry->index) return entry;
    if (index < entry->index) max = (cur > 0U) ? (cur - 1U) : cur;
    else                      min = cur + 1U;
}
```

`OD_ENTRY_H1017 = &OD->list[12]` 这类宏是**生成器算好的编译期常量**，
与运行时的二分查找是两回事。

---

## 五、查找全流程

```
SDO 收到「读 0x1018:01」
  │
  ├─ ① OD_find(OD, 0x1018)          二分查找 list[]  →  OD_entry_t*
  │
  ├─ ② OD_getSub(entry, 1, &io)     按 odObjectType 三选一：
  │        VAR → 只有子0，直接取
  │        ARR → base + size×(sub-1)      ← 【算】
  │        REC → 扫描比对 .subIndex       ← 【查】
  │      填出 io.stream.dataOrig / dataLength / attribute
  │
  ├─ ③ 检查 attribute 位是否允许读
  │
  └─ ④ io.read(...)                 memcpy(dataOrig, buf, dataLength)
```

### 两种定位（`CO_ODinterface.c:203-236`）

**ARR —— 指针算术：**

```c
case ODT_ARR: {
    if (subIndex >= entry->subEntriesCount) { ret = ODR_SUB_NOT_EXIST; break; }
    if (subIndex == 0U) {
        stream->dataOrig   = odo->dataOrig0;      /* 子0 单独接走 */
        stream->dataLength = 1;
    } else {
        uint8_t* ptr = odo->dataOrig;
        stream->dataOrig   = ptr + (odo->dataElementSizeof * (uint8_t)(subIndex - 1U));
        stream->dataLength = odo->dataElementLength;
    }
}
```

注意那个 **`- 1`**：子 0 被 `dataOrig0` 接管了，所以子 1 对应数组第 0 个元素。

**REC —— 线性扫描：**

```c
case ODT_REC: {
    CO_PROGMEM OD_obj_record_t* odoArr = entry->odObject;
    CO_PROGMEM OD_obj_record_t* odo = NULL;
    for (uint8_t i = 0; i < entry->subEntriesCount; i++) {
        if (odoArr[i].subIndex == subIndex) { odo = &odoArr[i]; break; }
    }
    stream->dataOrig   = odo->dataOrig;           /* 直接取，零算术 */
    stream->dataLength = odo->dataLength;
}
```

### ⚠️ `dataOrig` 是**存好的常量指针**，不是算出来的

**运行时代码里没有任何 `offsetof`**（`CANopenNode/301/` 全目录搜不到）。

指针是**代码生成器在离线阶段**算好、编译成常量、烧进 flash 的：

```c
/* CANopenEditor 生成 OD.c 时写下的 */
.dataOrig = &OD_PERSIST_COMM.x1018_identity.vendor_ID,
```

**概念上等价于 offsetof，但发生在生成/编译阶段，不在运行时。**

---

## 六、VAR / ARRAY / RECORD 的本质

CiA 301 明文规定的对象码：

| 对象码 | 名称 | 含义 |
| --- | --- | --- |
| `0x07` | **VAR** | 单个值，只有子索引 0 |
| `0x08` | **ARRAY** | 子索引是**同一数据类型** |
| `0x09` | **RECORD** | 子索引**可以是不同数据类型** |

**实测统计**（EDS 与 `OD.c` 完全一致）：

| | EDS | `OD.c` |
| --- | --- | --- |
| ARRAY | 4 个：`1003` `1010` `1011` `1016` | `ODT_ARR` **4** 个 ✅ |
| RECORD | 19 个 | `ODT_REC` **19** 个 ✅ |
| VAR | 11 个索引 | `ODT_VAR` **11** 个 |
| | | 合计 **34** ✅ |

### 本质：不是"类型"，是"地址能不能算"

| | ARRAY | RECORD |
| --- | --- | --- |
| 元素长度 | **等长** | 不等长 |
| 地址 | **可算**：`base + size×(sub-1)` | **不可算** |
| 描述符 | **1 份**（描述所有元素） | **N 份**（每子索引一份） |
| 查找 | O(1) 算术 | O(n) 扫描比对 |

> **RECORD 是通用形式**（能表达任何情况）；
> **ARRAY 是同构情况的压缩表示**（1 份描述符代替 N 份）。
> **"类型相同"只是"等长"的常见来源，不是定义本身。**

### 反例：0x1600 同构却用 RECORD

`0x1600`（RPDO 映射）的子索引全部等宽：

```
subIndex = 0,  dataLength = 1     ← 子0，计数
subIndex = 1..8, dataLength = 4   ← 全部 uint32，完全同构
```

**它本可以是 ARRAY，但 CiA 301 规定是 RECORD。**
**所以一个对象是哪一类，要去看 EDS，不能靠推。**

### ⚠️ ARRAY 有子索引，但描述符里没有 `subIndex` 字段

这是两回事：

| | ARRAY |
| --- | --- |
| 子索引（CANopen 概念） | ✅ **有**（子0 + 8 元素 = 9 个） |
| `subIndex` 字段（C 成员） | ❌ 没有 |

**因为 ARRAY 的编号和位置恒等，可以直接算出来，不需要存；**
**RECORD 会断号，所以必须把编号记下来供比对。**

实测断号（`0x1400`–`0x1403`、`0x1800`–`0x1803`）：

| 对象 | 子索引 | 断号 |
| --- | --- | --- |
| `0x1018` `0x1200` `0x1280` | 连续 | 无 |
| **`0x1400`–`0x1403`** | `[0,1,2,`**`5`**`]` | **3、4** |
| `0x1600`–`0x1603` `0x1A00`–`0x1A03` | `[0..8]` | 无 |
| **`0x1800`–`0x1803`** | `[0,1,2,3,`**`5,6`**`]` | **4** |

断号来自 EDS —— `DS301_profile.eds` 里就没有 `[1400sub3]` / `[1400sub4]`。

### 两个"数量"别混

```c
{0x1016, 0x09, ODT_ARR, ...}                    /* ← 目录层：subEntriesCount = 9 */
.x1016_consumerHeartbeatTime_sub0 = 0x08,       /* ← 数据层：子0 的值 = 8 */
```

| | 值 | 含义 | 存在哪 | 给谁用 |
| --- | --- | --- | --- | --- |
| `subEntriesCount` | 9 | 一共几个子索引（含子0） | 目录表 | **内部**边界检查 |
| 子索引 0 的值 | 8 | 有几个元素 | 数据区 `uint8_t` | **协议**要求（主站读 `0x1016:00`） |

**9 = 1（子0）+ 8（元素）** —— 对得上。

### 子索引 0 的不对称

| | 子索引 0 怎么存 |
| --- | --- |
| `OD_obj_array_t` | **单开两个字段**：`dataOrig0` + `attribute0` |
| `OD_obj_record_t` | **无需特殊处理** —— 它只是 `odoArr[0]`（`.subIndex = 0`） |

因为 ARRAY 的主字段描述的是**元素**（统一同构），而子 0 是 `uint8_t`（不同类型），
塞不进"统一"里，只能单开。**这就是"1 份描述符管 N 个元素"这个压缩的代价。**

---

## 七、代码生成链路

```
① EDS 文本          DS301_profile.eds          ← 源头，给人看/给工具读
        │  CANopenEditor v4.2.3（离线工具）
        ▼
② OD.h              结构体定义 + OD_ATTR_* + OD_CNT_*
③ OD.c              数据初始化 + ODObjs 描述符 + OD_list 目录表
```

`OD.c` 开头明确写着：

```
This file was automatically generated by CANopenEditor v4.2.3-64-g01b75fc
DON'T EDIT THIS FILE MANUALLY, UNLESS YOU KNOW WHAT YOU ARE DOING !!!!
```

**这是生成物，不是源头。**

### ⚠️ 本工程有一处手工改动

`0x6000 velocity` 存在于 `OD.c:1115-1119` / `OD.h:245`，但**在 `DS301_profile.eds`
里查不到** —— 是有人手工塞进生成物的。不影响功能，但重新生成 OD 时会丢失。

---

## 八、主站视角

### 主站**不会**拿到从站的字典

| 常见误解 | 实际 |
| --- | --- |
| 主站需要从站的 OD.c | ❌ 跨设备不共享代码，**不要编译进主站** |
| 通过结构体反推 index/subindex | ❌ index/subindex 是**开发者写在代码里的常量** |
| 读回的数据放进"从站的字典内存区" | ❌ 放进**主站自己的变量** |

**主站持有的不是字典，是一张「我知道要读写哪些地址」的清单：**

```c
#define CIA402_CONTROLWORD   0x6040
#define CIA402_STATUSWORD    0x6041
#define CIA402_TARGET_POS    0x607A
```

**整条链路上，字典只存在于两端各自的固件里；中间流动的只有
`(index, subindex, data)` 三元组。**

### EDS 的双重身份

| | 从站 | 主站 |
| --- | --- | --- |
| EDS 的角色 | **源代码** | **文档** |
| 流向 | `EDS → 生成器 → OD.c/.h → 编译进固件` | 人读了写 `#define`；工具读了生成配置流程 |

**要改从站的 SDO/PDO 参数？运行时发 SDO 写过去，EDS 一个字都不用动。**

### ⚠️ 从机的 OD.c 不能进主站

1. **符号冲突** —— `OD_PERSIST_COMM` / `OD_RAM` / `OD` / `ODObjs` / `OD_list`
   主站已经有一份，重复定义直接链接失败
2. **概念错误** —— 就算改名硬塞也没用，主站的 SDO 客户端拿 index/subindex
   **发到总线上**，从不查本地字典
3. **本工程特有**：Makefile 用 `notdir` 把 `.o` 拍平到 `build/`，
   两个 `OD.c` 都产出 `build/OD.o` → **静默覆盖，不报错**

---

## 九、易错点速查

| # | 易错 | 正确 |
| --- | --- | --- |
| 1 | 拿 index 当数组下标 | index 稀疏，`OD_find` 走**二分查找** |
| 2 | `dataOrig0` 是"起始地址"，`attribute0` 是"长度" | 都是**子索引 0 专用**：数据指针 + 属性位 |
| 3 | ARRAY 没有子索引 | ARRAY **有**子索引，只是**没有 `subIndex` 字段** |
| 4 | `dataOrig` 是运行时算出来的 | 是**生成器算好的编译期常量指针** |
| 5 | 运行时会用 `offsetof` | 运行时**零 offsetof**，偏移只在生成期算 |
| 6 | `subEntriesCount` = 元素个数 | = **子索引总数（含子0）**，元素数在子索引 0 里 |
| 7 | `PERSIST` 会自动持久化 | **不持久化**，`OD_ATTR_*` 是空的，存储功能已关闭 |
| 8 | ARRAY/RECORD 可按需选 | **看 EDS**，标准也会拍板（`0x1600` 就是反例） |

---

## 十、一句话概括

> **三层结构**：数据（171 个裸变量）→ 描述符（ODObjs，类型各异）→ 目录
> （34 条 `OD_entry_t`，统一形状，二分可查）。
>
> **定位靠两段**：`index` 走二分查找定条目，`subindex` 走「ARR 算 / REC 查」定成员。
>
> **所有地址都是编译期常量** —— 运行时只做加法和 memcpy。

---

## 相关文件

| 文件 | 说明 |
| --- | --- |
| `framework/canopen/OD/OD.c` | 生成物：数据初始化（`:21` `:194`）+ 描述符（`:250`）+ 目录表 |
| `framework/canopen/OD/OD.h` | 生成物：结构体定义、`OD_CNT_*`、`OD_ATTR_*`（`:248-261`） |
| `framework/canopen/OD/DS301_profile.eds` | **源头**，EDS 文本 |
| `framework/canopen/CANopenNode/301/CO_ODinterface.h` | `OD_find`（`:315`）、`OD_getSub`（`:333`）、三种描述符（`:672-699`） |
| `framework/canopen/CANopenNode/301/CO_ODinterface.c` | 查找与定位实现（`:143-178` / `:181-243`） |
| `Doc/canopen_driver_notes.md` | 驱动层收发机制（rxArray / txArray） |
| `Doc/can_interrupt_fifo_notes.md` | bxCAN 中断三层链与 FIFO0/FIFO1 |
