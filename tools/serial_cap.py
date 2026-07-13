#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""串口日志采集：读 ESP32-P4 的 USB JTAG 串口，每行加相对时间戳 [+SS.mmm]，
便于对齐「某功能运行到第 N 秒出问题」这类时序排查（例如 TTS 播到一定时长掉声）。

健壮性：断链（设备重启 / 烧录 / 拔插 / 端口暂时消失）不会退出，而是自动等待
端口回来并重连，持续抓取；断连/重连点在日志里标注。**只有 Ctrl-C 才停止。**
时间戳基于脚本启动，跨重连连续——设备重启表现为「时间戳连续但设备侧 uptime
归零」，便于看出重启点。

依赖：pyserial —— 由文件顶部 PEP 723 内联元数据声明，`uv run` 会自动装进隔离
环境，无需手动 pip install。

用法：
    uv run tools/serial_cap.py                 # 默认端口，实时打到 stdout
    OUT=serial.log uv run tools/serial_cap.py  # 同时写文件
    PORT=/dev/cu.xxx uv run tools/serial_cap.py
    ./tools/serial_cap.py                      # 已 chmod +x，shebang 直接走 uv run
输出到文件用 OUT=（serial*.log 已被 .gitignore 忽略）。断链自动重连，仅 Ctrl-C 停止。

注意：P4 固件口是 “USB JTAG/serial debug unit”（默认 /dev/cu.usbmodem11101），
不要与 idf.py monitor 同时占用同一个串口。
"""
import os
import sys
import time

try:
    import serial
    from serial import SerialException
except ImportError:
    sys.stderr.write("缺少 pyserial。推荐用 `uv run tools/serial_cap.py`"
                     "（PEP 723 内联声明，自动装依赖），或 `pip install pyserial`。\n")
    sys.exit(1)

PORT = os.environ.get("PORT", "/dev/cu.usbmodem11101")  # P4 USB JTAG/serial
OUT = os.environ.get("OUT", "")  # 空 = 只打 stdout；非空 = 同时写该文件
BAUD = int(os.environ.get("BAUD", "115200"))
RECONNECT_DELAY = 0.5  # 秒；断链/端口不可用时的重试间隔


def main():
    out = open(OUT, "w", buffering=1) if OUT else None
    t0 = time.time()

    def emit(text):
        row = f"[+{time.time() - t0:9.3f}] {text}\n"
        sys.stdout.write(row)
        sys.stdout.flush()
        if out:
            out.write(row)

    emit(f"# capture start {time.strftime('%Y-%m-%d %H:%M:%S')} "
         f"port={PORT} baud={BAUD} (Ctrl-C 停止)")

    ser = None
    buf = b""
    waiting_announced = False  # 端口不可用时只提示一次，避免刷屏
    try:
        while True:
            # —— 未连接：尝试（重新）打开串口 ——
            if ser is None:
                try:
                    ser = serial.Serial(PORT, BAUD, timeout=1)
                except (SerialException, OSError) as e:
                    if not waiting_announced:
                        emit(f"--- 等待串口 {PORT} … ({e}) ---")
                        waiting_announced = True
                    time.sleep(RECONNECT_DELAY)
                    continue
                waiting_announced = False
                buf = b""  # 丢弃上一段连接的残留半行
                emit(f"--- 串口已连接 {PORT} ---")

            # —— 已连接：持续读取 ——
            try:
                data = ser.read(4096)
            except (SerialException, OSError) as e:
                emit(f"--- 串口断开，自动重连中 … ({e}) ---")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(RECONNECT_DELAY)
                continue

            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                emit(line.decode("utf-8", "replace").rstrip("\r"))
    except KeyboardInterrupt:
        emit("--- 手动停止 ---")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if out:
            out.close()


if __name__ == "__main__":
    main()
