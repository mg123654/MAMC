# 移植层：上游来源与本项目的改动

本目录是 CANopenNode 的 STM32 移植层。**不是全部原样上游代码**，本文件记录差异，
便于日后跟进上游版本。

- 上游仓库：`https://github.com/CANopenNode/CanOpenSTM32`
- 配套 CANopenNode 提交：`9b8beed8367241e96ac03f916cd5a500bcb2cf23`（v4.1 之后的 master）
- 引入日期：2026-09-10

---

## 文件清单

| 文件 | 来源 | 状态 |
| --- | --- | --- |
| `CO_driver_STM32.c` | 上游 | **原样，一行未改** |
| `CO_driver_target.h` | 上游 | **原样，一行未改** |
| `CO_driver_custom.h` | — | **本项目新增**：编译期功能配置 |
| `CO_app_STM32.h` | 上游 | 精简为只保留结构体，声明部分删除 |
| `CO_app_STM32.c` | 上游 | **已删除** —— 从站架构的应用层 |
| `CO_storageBlank.c/h` | 上游 | **已删除** —— 存储已关闭，无人引用 |

应用层的替代实现位于 `framework/canopen/canopen_app.c/h`（不在本目录）。

---

## 为什么重写应用层

上游 `CO_app_STM32.c` 是**从站**架构：

```c
CO_LSSinit(CO, &lssAddress, &desiredNodeID, &baudrate);   // 等 LSS 主站分配节点号
CO_CANopenInit(CO, NULL, NULL, OD, ...);                  // 传 NULL → 用默认【从站】NMT
```

它没有 SDO 客户端、没有心跳消费回调、没有 NMT 主站对象。本工程是**主站**，
同样的职责（启动序列 + 主循环喂栈 + 1ms 喂栈）需要按主站组织，因此重写为
`framework/canopen/canopen_app.c`。

`CO_driver_STM32.c`（驱动，750 行）是通用的，直接复用 —— 它负责那 8 个
`CO_driver.h` 规定的接口函数、滤波器配置、中断接线、发送队列。

---

## 改动明细

### 1. `CO_app_STM32.h` —— 精简为只保留结构体

上游此文件还声明了 `canopen_app_init/process/interrupt`，随 `.c` 一起删除。

**保留结构体的原因**：上游驱动 `CO_driver_STM32.c:32` 直接 include 它，并把
`CANptr` 强转后取字段：

```c
HAL_CAN_Start(((CANopenNodeSTM32*)CANmodule->CANptr)->CANHandle)
```

为了不改驱动，结构体定义留在原文件。

**结构体字段也做了裁剪**（实测驱动只用到 `CANHandle` 与 `HWInitFunction`）：

| 字段 | 处置 | 原因 |
| --- | --- | --- |
| `CANHandle` | 保留 | 驱动使用 |
| `HWInitFunction` | 保留 | 驱动使用 |
| `desiredNodeID` / `activeNodeID` / `baudrate` / `canOpenStack` | 保留 | `canopen_app.c` 使用 |
| `timerHandle` | **删除** | 驱动不使用；本工程用 SysTick，无 TIM 模块 |
| `outStatusLEDGreen` / `outStatusLEDRed` | **删除** | 无 LED 硬件，无引用者 |

### 2. `CO_driver_custom.h` —— 新增，功能配置

上游官方预留的覆盖钩子：`CO_driver_target.h:51` 在 `#ifdef CO_DRIVER_CUSTOM` 下
include 本文件。各模块头文件的默认值都写在 `#ifndef` 保护里，include 顺序为：

```
CANopen.h → 301/CO_driver.h → CO_config.h（只定义“位”宏）
                           → CO_driver_target.h → CO_driver_custom.h   ← 覆盖在此生效
之后才是 CO_NMT_Heartbeat.h / CO_SDOclient.h / CO_HBconsumer.h 的 #ifndef 默认值
```

所以在这里 `#undef` + `#define` 即可覆盖，**不需要改任何上游文件**。
生效前提：编译时定义 `CO_DRIVER_CUSTOM`（见 Makefile 的 `C_DEFS`）。

配置内容（主站）：

| 宏 | 上游默认 | 本项目 | 原因 |
| --- | --- | --- | --- |
| `CO_CONFIG_NMT` | `CALLBACK_PRE\|TIMERNEXT`（纯从站） | 追加 `CO_CONFIG_NMT_MASTER` | 否则 `CO_NMT_sendCommand()` 根本不编译 |
| `CO_CONFIG_SDO_CLI` | `0`（关闭） | `ENABLE\|SEGMENTED` | 主站读写从站对象字典 |
| `CO_CONFIG_FIFO` | `0` | `ENABLE` | SDO 客户端分段传输依赖；`CO_SDOclient.c:31` 有 `#error` 强制检查 |
| `CO_CONFIG_HB_CONS` | 已含 `ENABLE` | 追加 `CALLBACK_MULTI` | 按轴分别处理心跳超时（与 `CALLBACK_CHANGE` 互斥） |

新增配置项时注意检查依赖链：例如打开 SDO 客户端会连带要求 FIFO。

### 3. `CO_app_STM32.c` / `CO_storageBlank.c/h` —— 删除

- `CO_app_STM32.c`：由 `framework/canopen/canopen_app.c` 取代
- `CO_storageBlank.c/h`：仅被上面那个文件引用；且上游在
  `CO_driver_target.h:48` 已 `#undef CO_CONFIG_STORAGE_ENABLE`，存储功能是关闭的

---

## 本项目对 SysTick 时基的处理

上游应用层要求传入 `TIM_HandleTypeDef*` 并调 `HAL_TIM_Base_Start_IT()`。
本工程**未启用 HAL TIM 模块**，1ms 时基由 SysTick 提供（`stm32f4xx_it.c` 的
`SysTick_Handler` 调 `canopen_app_interrupt()`）。

由此产生一个必须处理的点：`CO_RESET_COMM` 路径下，协议栈会被销毁再重建
（`CO_delete()` → `canopen_app_init()`）。上游靠 `HAL_TIM_Base_Stop_IT()` 停掉
1ms 中断来避开「CO 已释放、尚未重建」的窗口。SysTick 停不掉（会连带影响整个
HAL 的 `HAL_GetTick()`），因此在 `canopen_app.c` 中用 `running` 标志做等价互斥：

- `CO_delete()` **之前**置 `false`
- 重建完成后置 `true`
- `canopen_app_interrupt()` 入口检查该标志，未就绪直接返回

**注意**：该检查必须放在 `CO_LOCK_OD` **之前**。`CO_LOCK_OD` 内部是关中断，
若在加锁后提前 `return`，中断会被永久关闭。

---

## 跟进上游时的注意

- 驱动（`CO_driver_STM32.c` / `CO_driver_target.h`）可直接用上游新版本替换，
  本项目未改这两个文件
- 替换后需重新核对 `CO_app_STM32.h` 里的结构体字段是否与驱动期望一致
- `CO_driver_custom.h` 的配置项对应上游 `301/CO_config.h` 的宏定义，
  升级时核对宏名是否有变
