#!/usr/bin/env python3
"""Đọc SCDC của màn hình HDMI từ userspace, không đụng gì tới driver.

Chạy TRÊN BOARD, bằng root. Dùng để trả lời một câu hỏi: khi màn hình nháy,
sink có báo lỗi link nào không?

    scdc-dump.py                 xem byte nào thay đổi, poll mỗi 1 giây
    scdc-dump.py --once          dump một lần rồi thoát
    scdc-dump.py -i 0.2          poll nhanh hơn
    scdc-dump.py --log FILE      ghi ra file để đối chiếu sau

SCDC nằm ở địa chỉ I2C 0x54 trên bus DDC. Bản đồ thanh ghi cho chế độ FRL thì
tài liệu công khai không rõ ràng, và driver của Rockchip cũng không đọc chúng —
nên công cụ này **không đoán**: nó dump cả dải và để bạn thấy byte nào nhúc
nhích. Byte nào đếm lên khi link chạy thì đó là bộ đếm lỗi, bất kể spec nói gì.

Hai điều cần biết:

  - Một số bộ đếm lỗi SCDC là **read-to-clear**. Đọc bằng công cụ này có thể
    xoá số mà thứ khác đang đợi đọc. Driver trong repo này không đọc chúng nên
    không xung đột, nhưng đừng chạy song song với công cụ khác cũng đọc SCDC.
  - Mỗi lần poll là thêm lưu lượng trên bus DDC, mà bus này đã chứng minh là
    mong manh trên phần cứng đây. Giữ nhịp poll vừa phải; mặc định 1 giây.
"""

import argparse
import ctypes
import fcntl
import os
import sys
import time

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001
SCDC_ADDR = 0x54


class i2c_msg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class i2c_rdwr_ioctl_data(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(i2c_msg)),
        ("nmsgs", ctypes.c_uint32),
    ]


# Chỉ những trường đã chắc chắn. Phần còn lại để nguyên dạng thô.
KNOWN = {
    0x01: "sink_version",
    0x02: "source_version",
    0x10: "update_0     bit4=FRL_START bit5=FLT_update",
    0x20: "tmds_config",
    0x21: "scrambler_status",
    0x30: "config_0",
    0x31: "config_1    FRL_rate | FFE<<4",
    0x40: "status_0    bit6=FLT_ready",
    0x41: "status_1    LTP req ln0/ln1",
    0x42: "status_2    LTP req ln2/ln3",
}


def candidate_adapters():
    """Các bus DDC của HDMI, tìm theo tên adapter chứ không dò mù.

    RK3588 khai báo nhiều controller HDMI nên có thể ra nhiều bus; chỉ bus nào
    thật sự có màn hình mới trả lời SCDC. Tên adapter khác nhau giữa kernel
    upstream ("DesignWare HDMI QP") và BSP, nên khớp lỏng theo "hdmi".
    """
    found = []

    for entry in sorted(os.listdir("/sys/class/i2c-dev")):
        try:
            with open(f"/sys/class/i2c-dev/{entry}/name") as fh:
                name = fh.read().strip()
        except OSError:
            continue

        low = name.lower()
        if "hdmi" in low or "designware" in low or "ddc" in low:
            found.append((int(entry.removeprefix("i2c-")), name))

    return found


def all_adapters():
    """Liệt kê mọi adapter i2c, để chọn tay khi khớp theo tên thất bại."""
    lines = []

    for entry in sorted(os.listdir("/sys/class/i2c-dev")):
        try:
            with open(f"/sys/class/i2c-dev/{entry}/name") as fh:
                lines.append(f"  {entry}  {fh.read().strip()}")
        except OSError:
            continue

    return "\n".join(lines) or "  (không có /dev/i2c-* nào)"


class Scdc:
    def __init__(self, bus):
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)

    def read(self, reg):
        """Một giao dịch write-offset rồi read, đúng cách SCDC yêu cầu."""
        out = ctypes.c_uint8(reg)
        val = ctypes.c_uint8(0)

        msgs = (i2c_msg * 2)(
            i2c_msg(SCDC_ADDR, 0, 1, ctypes.pointer(out)),
            i2c_msg(SCDC_ADDR, I2C_M_RD, 1, ctypes.pointer(val)),
        )
        data = i2c_rdwr_ioctl_data(msgs, 2)

        try:
            fcntl.ioctl(self.fd, I2C_RDWR, data)
        except OSError:
            return None

        return val.value


def sample(scdc, regs):
    return {r: scdc.read(r) for r in regs}


def fmt(reg, val):
    label = KNOWN.get(reg, "")
    shown = "--" if val is None else f"{val:02x}"

    return f"0x{reg:02x}={shown}" + (f"  {label}" if label else "")


def open_scdc(explicit=None):
    """Mở bus có màn hình thật, chứ không phải bus HDMI đầu tiên gặp được."""
    if explicit is not None:
        return Scdc(explicit), explicit

    cands = candidate_adapters()
    if not cands:
        sys.exit("Không nhận ra bus DDC của HDMI theo tên. Chọn tay bằng --bus "
                 "trong danh sách sau:\n" + all_adapters())

    first = None
    for bus, name in cands:
        try:
            scdc = Scdc(bus)
        except OSError:
            continue

        if first is None:
            first = (scdc, bus)

        # sink_version khác 0 nghĩa là có màn hình đang trả lời SCDC ở bus này.
        if scdc.read(0x01):
            print(f"# adapter: i2c-{bus} \"{name}\"")
            return scdc, bus

    if first is None:
        sys.exit("Không mở được bus DDC nào. Chạy bằng root?")

    return first


# Xác định bằng thực nghiệm trên Orange Pi 5B + màn hình đang dùng, ở FRL6:
# 0x51/0x53/0x55/0x58 đứng yên ở 0x80 (bit7 = valid, số đếm = 0) → đó là byte
# cao của bốn bộ đếm lỗi ký tự theo lane. 0x59/0x5a thì đổi mỗi lần đọc →
# read-to-clear, nên mỗi mẫu là số lỗi phát sinh kể từ lần đọc trước.
COUNTERS = [
    ("ln0", 0x50, 0x51),
    ("ln1", 0x52, 0x53),
    ("ln2", 0x54, 0x55),
    ("ln3", 0x57, 0x58),
    ("fec", 0x59, 0x5A),
]


def read_dpms():
    """DPMS của connector HDMI, để tách link đứt thật khỏi màn hình tắt chủ động."""
    import glob

    for f in glob.glob("/sys/class/drm/card*-HDMI-A-*/dpms"):
        try:
            with open(f) as fh:
                return fh.read().strip()
        except OSError:
            pass

    return "?"


def counter(scdc, lo_reg, hi_reg):
    """Cặp L/H, bit7 của byte cao là cờ valid. None nếu sink không trả lời."""
    lo = scdc.read(lo_reg)
    hi = scdc.read(hi_reg)

    if lo is None or hi is None:
        return None
    if not hi & 0x80:
        return None

    return ((hi & 0x7F) << 8) | lo


def watch_counters(scdc, emit, interval, fast):
    """Bộ đếm mỗi `interval`, còn status0 thì lấy mẫu dày hơn nhiều.

    Nháy chỉ kéo dài vài trăm mili giây, lấy mẫu 1 giây một lần thì phần lớn
    là trượt. Bộ đếm là read-to-clear nên không đọc dày được (sẽ băm nhỏ số
    liệu), nhưng status0 đọc bao nhiêu lần cũng vô hại — nên chỉ dồn nhịp cho
    riêng nó.
    """
    emit("# ln0-3 = lỗi ký tự theo lane, fec = khối RS-FEC sửa được.")
    emit(f"# Bộ đếm read-to-clear, mỗi dòng là {interval}s vừa qua. "
         f"status0 lấy mẫu mỗi {fast}s, chỉ in khi đổi.")
    emit("# dpms=Off nghĩa là màn hình tắt chủ động, không phải link đứt.")

    last_st = None

    while True:
        vals = [(name, counter(scdc, lo, hi)) for name, lo, hi in COUNTERS]
        st0 = scdc.read(0x40)
        dp = read_dpms()
        up = int(float(open("/proc/uptime").read().split()[0]))

        emit(f"[{time.strftime('%T')} uptime={up}s] " +
             " ".join(f"{n}={'--' if v is None else v}" for n, v in vals) +
             f"  status0={'--' if st0 is None else f'0x{st0:02x}'} dpms={dp}")

        last_st = st0

        # Phần còn lại của chu kỳ dành cho việc canh status0.
        deadline = time.monotonic() + interval
        while time.monotonic() < deadline:
            time.sleep(fast)
            st = scdc.read(0x40)

            if st != last_st:
                emit(f"[{time.strftime('%T')}.{int(time.time() * 1000) % 1000:03d}]"
                     f" !! status0 {'--' if last_st is None else f'0x{last_st:02x}'}"
                     f" -> {'--' if st is None else f'0x{st:02x}'}"
                     f"  dpms={read_dpms()}")
                last_st = st



def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--bus", type=int, help="số hiệu bus i2c, mặc định tự tìm")
    ap.add_argument("-i", "--interval", type=float, default=1.0, help="giây giữa hai lần poll")
    ap.add_argument("--once", action="store_true", help="dump một lần rồi thoát")
    ap.add_argument("--counters", action="store_true",
                    help="chỉ theo dõi bộ đếm lỗi, đã giải mã")
    ap.add_argument("--fast", type=float, default=0.05,
                    help="nhịp lấy mẫu riêng cho status0, giây")
    ap.add_argument("--log", help="ghi thêm ra file")
    ap.add_argument("--range", default="0x00-0x5f", help="dải thanh ghi, ví dụ 0x40-0x5f")
    args = ap.parse_args()

    lo, _, hi = args.range.partition("-")
    regs = list(range(int(lo, 16), int(hi, 16) + 1))

    scdc, bus = open_scdc(args.bus)
    log = open(args.log, "a", buffering=1) if args.log else None

    def emit(line):
        print(line, flush=True)
        if log:
            log.write(line + "\n")

    emit(f"# bus i2c-{bus}, dải 0x{regs[0]:02x}-0x{regs[-1]:02x}, "
         f"poll {args.interval}s, bắt đầu {time.strftime('%F %T')}")

    if not scdc.read(0x01):
        emit("# CẢNH BÁO: sink không trả lời SCDC (0x01 rỗng). Sai bus, "
             "hoặc màn hình đang ngủ?")

    prev = sample(scdc, regs)

    emit("# dump đầu tiên:")
    for r in regs:
        if prev[r] not in (None, 0):
            emit("  " + fmt(r, prev[r]))

    if args.once:
        return

    if args.counters:
        watch_counters(scdc, emit, args.interval, args.fast)
        return

    emit("# từ đây chỉ in những byte THAY ĐỔI. Im lặng = link không báo gì.")

    while True:
        time.sleep(args.interval)
        cur = sample(scdc, regs)

        changed = [r for r in regs if cur[r] != prev[r]]
        if changed:
            up = int(float(open("/proc/uptime").read().split()[0]))
            emit(f"[{time.strftime('%T')} uptime={up}s] " +
                 "  ".join(f"0x{r:02x}: {prev[r]:02x}->{cur[r]:02x}"
                           if prev[r] is not None and cur[r] is not None
                           else f"0x{r:02x}: {prev[r]}->{cur[r]}"
                           for r in changed))
            for r in changed:
                if r in KNOWN:
                    emit(f"          {KNOWN[r]}")

        prev = cur


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
