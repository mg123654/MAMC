#!/usr/bin/env bash
#
# tools/read_log.sh —— 一条命令把板子 RAM 里的日志打出来
#
# 做三件事：
#   1. 若 3333 端口上没有 gdb server，就起一个 openocd（后台）
#   2. 用 arm-none-eabi-gdb 跑 tools/dump_ramlog.gdb 读 RAM 环形缓冲
#   3. 若这次是自己起的 openocd，收摊时把它关掉
#
# 依赖 STM32CubeCLT 把 openocd / arm-none-eabi-gdb 放进 PATH（见 .vscode/settings.json）。
#
# 注意：脚本执行期间 CPU 会被暂停，读完自动恢复。**速度模式下电机不会因为
#       调试器暂停而停下**（驱动器收到的是持续转速指令），见 dump_ramlog.gdb 顶部说明。

set -u

cd "$(dirname "$0")/.." || exit 1

ELF="build/multi-axis-canopen-master.elf"
PORT=3333

if [ ! -f "$ELF" ]; then
    echo "找不到 $ELF —— 先 make -j" >&2
    exit 1
fi

started_openocd=0
if ! (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    echo "==> 起 openocd (gdb server :$PORT) ..."
    openocd -f interface/stlink.cfg -f target/stm32f4x.cfg >/tmp/mamc-openocd.log 2>&1 &
    ocd_pid=$!
    started_openocd=1

    # 等端口就绪，最多 10 秒
    for _ in $(seq 1 20); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
            break
        fi
        sleep 0.5
    done

    if ! (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        echo "openocd 没起来，日志：" >&2
        cat /tmp/mamc-openocd.log >&2
        kill "$ocd_pid" 2>/dev/null
        exit 1
    fi
else
    echo "==> 复用已在运行的 openocd (:$PORT)"
fi

arm-none-eabi-gdb -q -batch -x tools/dump_ramlog.gdb "$ELF"
rc=$?

if [ "$started_openocd" -eq 1 ]; then
    kill "$ocd_pid" 2>/dev/null
    wait "$ocd_pid" 2>/dev/null
fi

exit $rc
