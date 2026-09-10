# 对上游移植层所做的改动

本目录文件来自 [CANopenNode/CanOpenSTM32](https://github.com/CANopenNode/CanOpenSTM32)
的 `CANopenNode_STM32/`，**并非原样上游代码**。本文件记录我们的改动，便于日后跟进上游。

- 上游仓库：`https://github.com/CANopenNode/CanOpenSTM32`
- 配套 CANopenNode 提交：`9b8beed8367241e96ac03f916cd5a500bcb2cf23`（v4.1 之后的 master）
- 拉取日期：2026-09-10

所有改动在源码中都用 `[本项目已改]` / `[本项目新增]` 注释标出。

---

## 改动原因：本工程用 SysTick 而非硬件 TIM 提供 1ms 时基

上游移植层要求传入 `TIM_HandleTypeDef*`，并调用 `HAL_TIM_Base_Start_IT()` 启动。
本工程**未启用 HAL TIM 模块**（`stm32f4xx_hal_conf.h` 中 `HAL_TIM_MODULE_ENABLED`
未定义，`stm32f4xx_hal_tim.c/h` 也不在 `framework/Drivers/` 中）。

不引入 TIM 模块的理由：需要与本工程 HAL 版本匹配的 TIM 驱动，而本机没有该文件
（STM32CubeCLT 不带完整 HAL 包），从 CanOpenSTM32 范例里拷贝又会混入版本不同的
HAL 驱动（范例的 `stm32f4xx_hal_can.c` 2462 行 vs 本工程 2466 行，`hal_def.h` /
`hal_can.h` / `hal_rcc.h` 均不同）。

SysTick 本身就是精确 1 ms（HAL 默认，`uwTickFreq = HAL_TICK_FREQ_DEFAULT`），
优先级 15，低于 `CAN1_RX0_IRQn` 的 0 —— 时基中断可被 CAN 接收中断打断、反之不可，
符合 CANopenNode 对中断优先级的要求。

---

## 改动清单

### 1. `CO_app_STM32.h` — `timerHandle` 类型

```diff
-    TIM_HandleTypeDef*
+    void*              /* [本项目已改] */
         timerHandle;
```

不再需要 TIM 句柄，字段保留仅为兼容上游 API，传 `NULL`。

### 2. `CO_app_STM32.c` — 新增运行时标志（**修的是 use-after-free 竞态**）

```diff
+/* [本项目新增] */
+static volatile bool_t canopenRunning = false;
```

上游在 `CO_RESET_COMM` 分支里的顺序是：

```c
HAL_TIM_Base_Stop_IT(timerHandle);      // ① 先停掉 1ms 中断
CO_CANsetConfigurationMode(...);        // ② CAN 转入配置模式
CO_delete(CO);                          // ③ 释放 CO
canopen_app_init(canopenNodeSTM32);     // ④ 重新初始化，新建 CO
```

① 是关键：停掉中断，保证 ③ 之后不会有中断去访问已释放的 `CO`。
SysTick **无法停止**（停掉会连带影响整个 HAL 的 `HAL_GetTick()`），因此改用等价的
软件互斥：③ 之前把标志清 false，④ 成功之后置 true，中断入口判该标志。

### 3. `CO_app_STM32.c:179` — 启动定时器 → 置位标志

```diff
-    HAL_TIM_Base_Start_IT(canopenNodeSTM32->timerHandle);
+    canopenRunning = true;
```

位置仍在该函数末尾、`CO_CANsetNormalMode()` 附近，保证 `CO` 完全就绪后才允许中断访问。

### 4. `CO_app_STM32.c:221` — 停止定时器 → 清位标志

```diff
-    HAL_TIM_Base_Stop_IT(canopenNodeSTM32->timerHandle);
+    canopenRunning = false;
```

### 5. `CO_app_STM32.c` — `canopen_app_interrupt()` 入口守卫

```diff
 void canopen_app_interrupt(void) {
+    if (!canopenRunning) {
+        return;
+    }
     CO_LOCK_OD(CO->CANmodule);
```

---

## 调用侧需要配合的事项

这两条不在本目录，但与本移植层的改动配套，改动时需一并注意：

1. **`framework/Core/Src/stm32f4xx_it.c`** 的 `SysTick_Handler` 中调用
   `canopen_app_interrupt()` —— 替代上游的 TIM 更新中断。
2. **`APP/main.c`** 中 `canopenNodeSTM32.timerHandle = NULL`。

---

## 跟进上游时的注意

上游若修复了 `CO_RESET_COMM` 的竞态（例如把标志内置进移植层），本补丁可以撤销。
除此之外 1、3、4、5 四项都是「没有 TIM 模块」这一个前提的直接后果，
若将来启用了 HAL TIM 模块并拿到匹配版本的驱动，整套补丁都可还原。
