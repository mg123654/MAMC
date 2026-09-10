# CANopen —— 协议栈

本目录是 **CANopen 协议栈的总目录**，放协议栈本体及其移植层。

## 现状

**空占位。** 协议栈尚未引入，移植工作在其后的步骤中进行。

## 后续要放什么

预期采用 [CANopenNode](https://github.com/CANopenNode/CANopenNode) v4 及其官方
[STM32 移植层](https://github.com/CANopenNode/CanOpenSTM32)，大致布局：

```
framework/canopen/
├── CANopenNode/            ← 协议栈本体（上游代码，尽量不改）
│   ├── 301/                ← CiA 301 核心：NMT / SDO / PDO / SYNC / EMCY / 心跳
│   ├── 303/ 304/ 305/ 309/ ← 指示灯 / 网络变量 / LSS / 参数组
│   └── storage/            ← 对象字典掉电保存
├── port/                   ← 移植层：与 STM32 HAL 对接
│   ├── CO_driver_STM32.*   ← CAN 收发 + 临界区 + 1ms 定时器钩子
│   └── CO_config.h         ← 编译期功能裁剪（主站/从站、SDO 客户端等开关）
└── OD/                     ← 对象字典（.od 源文件 + 生成的 .c/.h）
```

## 已知的构建约束

Makefile 采用 `notdir` + `vpath`，**所有 `.o` 平铺在 `build/`**，因此
不同子目录下的同名 `.c` 会互相覆盖。引入协议栈时要检查 basename 是否唯一。

## 与其它层的关系

- 不直接访问寄存器，收发报文通过 `framework/BSP` 的板级 CAN 接口
- 向上给 `APP` 暴露对象字典读写和 PDO 数据，协议栈细节不外泄
