"""Serial helper that works against either a device node or an rfc2217 proxy.

workbench1's portal owns /dev/ttyACM*, so tests reach the pucks through its
proxy ports instead of taking the device nodes away from it.
"""
import time, serial


def open_dut(target, reset=True, wait_banner=True, banner=b"Newsheen Radio", timeout=30):
    if "://" in target:
        s = serial.serial_for_url(target, baudrate=115200, timeout=0.2, do_not_open=True)
    else:
        s = serial.Serial()
        s.port = target
        s.baudrate = 115200
        s.timeout = 0.2
    # DTR drives GPIO0 on USB-Serial/JTAG: hold it de-asserted or opening the
    # port arms download mode instead of running the app.
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    s.open()
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    if reset:
        try:
            s.rts = True; time.sleep(0.2); s.rts = False
        except Exception:
            pass
    if wait_banner:
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            d = s.read(4096)
            if d:
                buf += d
                if banner in buf:
                    time.sleep(2)
                    break
        s.booted = banner in buf
    else:
        s.booted = None
    try:
        s.reset_input_buffer()
    except Exception:
        pass
    return s


def cmd(s, c, wait=2.0):
    s.write((c + "\n").encode()); s.flush()
    out = b""
    end = time.time() + wait
    while time.time() < end:
        d = s.read(8192)
        if d:
            out += d
    return out.decode("utf-8", "replace")


def read_until(s, seconds, markers=(b"guru", b"stack overflow", b"rebooting", b"backtrace")):
    """Read for `seconds`, returning (text, crashed). Case-insensitive markers."""
    acc = b""
    end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096)
        if d:
            acc += d
            low = acc.lower()
            if any(m in low for m in markers):
                time.sleep(3)
                x = s.read(65536)
                if x:
                    acc += x
                return acc.decode("utf-8", "replace"), True
            if len(acc) > 200000:
                acc = acc[-20000:]
    return acc.decode("utf-8", "replace"), False
