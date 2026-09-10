# CANopen —— 协议栈

本目录是 **CANopen 协议栈的总目录**，放协议栈本体及其移植层。

## 现状

已引入 [CANopenNode](https://github.com/CANopenNode/CANopenNode)（锚定提交
`9b8beed`，v4.1 之后）及其官方 [STM32 移植层](https://github.com/CANopenNode/CanOpenSTM32)，
**协议栈已按主站配置接入并可构建**。

```
framework/canopen/
├── CANopenNode/   301(核心) 303(指示灯) 304(GFC/SRDO) 305(LSS) 309(网关)
│                  storage extra + CANopen.c/h + LICENSE
│                  注：CANopen.h 无条件包含上述全部目录的头，必须完整保留
├── port/          移植层
│   ├── CO_driver_STM32.c    驱动（上游原样，未改）
│   ├── CO_driver_target.h   目标配置（上游原样，未改）
│   ├── CO_driver_custom.h   ← 我们的编译期功能配置（主站开关）
│   ├── CO_app_STM32.h       精简为只保留 CANopenNodeSTM32 结构体
│   └── PATCHES.md           ⚠ 相对上游的改动记录，改本目录前先读它
├── OD/            OD.c, OD.h, DS301_profile.eds（通用 DS301 profile）
└── canopen_app.c/h          ← 我们写的启动/喂栈胶水
```

**分层**：驱动（`port/`）负责 CANopenNode 规定的 8 个接口函数、滤波器、
中断接线、发送队列；`canopen_app.c/h` 负责启动序列与周期喂栈；`APP/` 只调
`canopen_app.c/h` 暴露的三个函数，不直接接触 CANopenNode 内部。

**CAN 底层不需要另写 BSP 层** —— 驱动在 `CO_CANmodule_init()` 内部完成
滤波器配置、`HAL_CAN_Start()`、RX 中断通知。`framework/BSP` 目前只负责
`printf` 的串口重定向。

**1ms 时基由 SysTick 提供**，占用 HAL 默认的 1ms 滴答，不新增硬件 TIM。

**已启用主站能力**（`port/CO_driver_custom.h`）：NMT 主站、SDO 客户端、
心跳消费多节点回调。但**主站业务逻辑尚未实现** —— 即
「发 NMT 让某轴进 Operational」「用 SDO 读写某轴对象字典」「多轴编排」
这些调用还没写，属于 `APP/` 的职责。

---

## 原始设计意图（保留备查）

```
framework/canopen/
├── CANopenNode/            ← 协议栈本体（上游代码，尽量不改）
│   ├── 301/                ← CiA 301 核心：NMT / SDO / PDO / SYNC / EMCY / 心跳
│   ├── 303/ 304/ 305/ 309/ ← 指示灯 / 网络变量 / LSS / 参数组
│   └── storage/            ← 对象字典掉电保存
├── port/                   ← 移植层：与 STM32 HAL 对接
│   └── CO_driver_STM32.*   ← CAN 收发(含过滤器/Start) + 临界区
└── OD/                     ← 对象字典（.eds 源文件 + 生成的 .c/.h）
```

> 更正：编译期功能裁剪开关在上游的 `CANopenNode/301/CO_config.h`
> （`CO_CONFIG_NMT_MASTER`、`CO_CONFIG_SDO_CLI_ENABLE` 等），不在 port 目录。
> 覆盖这些开关应放在我们自己的配置头里，不改上游文件。

## 已知的构建约束

Makefile 采用 `notdir` + `vpath`，**所有 `.o` 平铺在 `build/`**，因此
不同子目录下的同名 `.c` 会互相覆盖。引入协议栈时要检查 basename 是否唯一。

## 与其它层的关系

- 向上给 `APP` 暴露对象字典读写和 PDO 数据，协议栈细节不外泄；`APP` 不直接碰 HAL
- **CAN 底层由本层的移植层自己完成**（`CO_driver_STM32.c` 调 `HAL_CAN_ConfigFilter` /
  `HAL_CAN_Start` / `HAL_CAN_ActivateNotification`），不经过 `framework/BSP`。
  BSP 目前只负责 `printf` 的串口重定向。
- 与 `framework/Core` 的耦合点只有两个：`main.c` 调 `canopen_app_init/process()`，
  `stm32f4xx_it.c` 的 `SysTick_Handler` 调 `canopen_app_interrupt()`
