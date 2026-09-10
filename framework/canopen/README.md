# CANopen —— 协议栈

本目录是 **CANopen 协议栈的总目录**，放协议栈本体及其移植层。

## 现状

已引入 [CANopenNode](https://github.com/CANopenNode/CANopenNode)（锚定提交
`9b8beed`，v4.1 之后）及其官方 [STM32 移植层](https://github.com/CANopenNode/CanOpenSTM32)，
**阶段一（栈移植）已完成并可构建**。

```
framework/canopen/
├── CANopenNode/   301(核心) 303(指示灯) 304(GFC/SRDO) 305(LSS) 309(网关)
│                  storage extra + CANopen.c/h + LICENSE
│                  注：CANopen.h 无条件包含上述全部目录的头，必须完整保留
├── port/          CO_app_STM32.c/h, CO_driver_STM32.c, CO_driver_target.h,
│                  CO_storageBlank.c/h
│                  ⚠ PATCHES.md 记录了我们相对上游的改动（SysTick 时基相关）
└── OD/            OD.c, OD.h, DS301_profile.eds（通用 DS301 profile）
```

移植层内部已完成全部 CAN 底层配置（过滤器 / Start / RX 中断通知），
不需要另写 BSP 的 CAN 层。1ms 时基由 SysTick 提供，不占用硬件 TIM。

**主站功能（阶段二）尚未实现** —— 移植层的应用层是「从站」架构
（`CO_app_STM32.c` 调 `CO_LSSinit()`，且给 `CO_CANopenInit()` 传 `NULL` 用默认从站 NMT），
NMT 主站 / SDO 客户端 / 多轴管理等需要另写。

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
