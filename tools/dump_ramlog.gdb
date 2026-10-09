# tools/dump_ramlog.gdb —— 用调试器把板子 RAM 里的日志读出来
#
# 用途：板上没接 USB-TTL 时，代替串口看 printf 输出。日志由
#       framework/BSP/diag_log.c 的环形缓冲保存，__io_putchar() 每字符写一份。
#
# 单独用（openocd 已经在跑 gdb server）：
#     arm-none-eabi-gdb -q -batch -x tools/dump_ramlog.gdb build/multi-axis-canopen-master.elf
#
# 省事的用法（自动起 openocd）：
#     bash tools/read_log.sh
#
# ── 注意 ─────────────────────────────────────────────────────────────
# `target remote` 会**暂停 CPU**。暂停期间主循环停摆，但速度模式下
# 驱动器收到的是「持续转速」指令，**电机会继续转**。读完 `detach`
# 目标即恢复运行。要真正停下来得给驱动器断电或写 0x60FF=0。

set pagination off
set confirm off

target remote localhost:3333

python
import gdb

SIZE = 4096  # 与 diag_log.h 的 DIAG_LOG_SIZE 一致


def _dump_ramlog():
    try:
        addr = int(gdb.parse_and_eval("(unsigned int)&g_diagLog"))
        head = int(gdb.parse_and_eval("g_diagLogHead"))
        total = int(gdb.parse_and_eval("g_diagLogTotal"))
    except gdb.error as exc:
        print("!! 找不到 g_diagLog / g_diagLogHead / g_diagLogTotal：%s" % exc)
        print("!! 多半是 elf 没加载或符号被优化掉了，检查 -x 后面的 elf 路径。")
        return

    # 一次读完整个缓冲。逐字节 parse_and_eval 要跑 4096 次远程读，慢得离谱。
    raw = bytes(gdb.selected_inferior().read_memory(addr, SIZE))

    if total < SIZE:
        # 还没绕圈：有效数据是从 0 开始、长度为 head 的一段（此时 head == total）
        data = raw[:head]
        state = "未绕圈"
    else:
        # 已绕圈：最新数据从 head 开始，先读到尾，再从头读到 head
        data = raw[head:] + raw[:head]
        state = "已绕圈（下面按时间顺序拼接）"

    text = data.decode("latin-1", errors="replace").replace("\r\n", "\n")

    print("")
    print("=================== RAM 日志 ===================")
    print("累计写入 %d 字节 | %s | 有效 %d 字节" % (total, state, len(data)))
    print("-----------------------------------------------")
    print(text)
    print("================= 日志结束 ====================")


_dump_ramlog()
end

# detach 而不是 kill：让目标恢复运行（CPU 重新跑主循环）
detach
quit
