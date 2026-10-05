#!/usr/bin/env python3
"""EFR32 Series 2 (xG21) DCI driver over a CMSIS-DAP probe via pyOCD.

Ports the knieriem OpenOCD efm32s2 DCI routines:
  - status : print lock state + Secure Engine info, and (if the core is
             reachable) full chip identity incl. the unique EUI-64.
             Non-destructive.
  - erase  : device erase -- wipes all flash+RAM and clears the debug lock.
             DESTRUCTIVE.

Usage: dci.py [status|erase]
"""
import sys, time

try:
    from pyocd.core.session import Session
    from pyocd.probe.aggregator import DebugProbeAggregator
    from pyocd.probe.debug_probe import DebugProbe
    from pyocd.coresight.dap import DebugPort
except ImportError as e:
    sys.exit("ERROR: pyOCD is not installed in this environment (%s).\n"
             "  Run from the project venv, e.g.  .venv/bin/python3 dci.py" % e)

AP = 1  # APB-AP index = DCI mailbox

# DEVINFO (device information page) base and field offsets, from the
# SiliconLabs EFR32MG21 DFP header efr32mg21_devinfo.h.
DEVINFO   = 0x0FE08000
DI_INFO   = DEVINFO + 0x0C
DI_PART   = DEVINFO + 0x10
DI_PKGINFO= DEVINFO + 0x1C
DI_EUI64L = DEVINFO + 0x54
DI_EUI64H = DEVINFO + 0x58
SCB_CPUID = 0xE000ED00

# --------------------------------------------------------------------------
# Connection, with meaningful errors for the two common failure modes.
# --------------------------------------------------------------------------
def find_probe():
    try:
        probes = DebugProbeAggregator.get_all_connected_probes()
    except Exception as e:
        sys.exit("ERROR: could not enumerate USB debug probes: %s" % e)
    if not probes:
        sys.exit(
            "ERROR: No CMSIS-DAP debug probe found over USB.\n"
            "  - Is the Raspberry Pi Debug Probe (Pico) plugged in?\n"
            "  - Is another program (pyocd gdbserver, openocd, ...) already using it?\n"
            "  - Try re-seating the USB cable.")
    return probes[0]

def connect():
    """Return a connected (probe, port), or exit with a helpful message."""
    probe = find_probe()
    try:
        Session(probe)                    # attaches probe.session, no target init
        probe.open()
        probe.set_clock(1_000_000)
        probe.connect(DebugProbe.Protocol.SWD)
    except Exception as e:
        sys.exit("ERROR: found a probe but could not open it: %s" % e)

    print("Probe: %s" % probe.description)

    class Shim:
        session = probe.session
    port = DebugPort(probe, Shim())
    try:
        port.connect()                    # SWD line reset + read DPIDR
    except Exception as e:
        sys.exit(
            "ERROR: the probe is connected, but no SWD target responded.\n"
            "  - Check the SWD wiring: GND, SWDIO, SWCLK (and RESET if used).\n"
            "  - Make sure the RODRET is powered (battery in, or 3V3 on VCC);\n"
            "    the Pico probe does NOT power the target.\n"
            "  - The chip may be asleep in a low-power mode; press a button or\n"
            "    hold RESET while connecting.\n"
            "  (underlying error: %s)" % e)
    return probe, port

# --------------------------------------------------------------------------
# Raw ADIv5 helpers (pyOCD manages DP SELECT from the (apsel<<24)|reg address
# and handles the posted-read / RDBUFF dance for MEM-AP reads).
# --------------------------------------------------------------------------
def apw(port, reg, val): port.write_ap((AP << 24) | reg, val)
def apr(port, reg):      return port.read_ap((AP << 24) | reg)
def dpw(port, reg, val): port.write_dp(reg, val)

def mem_read(port, addr):           # DCI mailbox read (via APB-AP MEM-AP)
    apw(port, 0x04, addr)           # TAR
    return apr(port, 0x0C)          # DRW
def mem_write(port, addr, val):
    apw(port, 0x04, addr)
    apw(port, 0x0C, val)

# AHB-AP (AP#0) memory access, used for CPUID/DEVINFO. On an unlocked part the
# first transaction after a fresh connect can transiently FAULT; clear the sticky
# error and retry (this is what pyOCD's heavier init does for us). A genuinely
# locked core faults every time -- but we never guess that here: the caller
# already knows the real lock state from the DCI SESTATUS.
def _clear_sticky(port):
    try: port.write_dp(0x00, 0x1E)     # DP ABORT: *STKERRCLR etc.
    except Exception: pass

def ahb_read(port, addr, tries=5):
    err = None
    for _ in range(tries):
        try:
            port.write_ap((0 << 24) | 0x04, addr)      # TAR
            return port.read_ap((0 << 24) | 0x0C)      # DRW
        except Exception as e:
            err = e
            _clear_sticky(port)
    raise err

def ahb_setup(port):
    # Ensure 32-bit access. We set TAR before every read, so address auto-inc
    # doesn't matter; the default CSW is already word-sized on these parts.
    for _ in range(5):
        try:
            csw = port.read_ap((0 << 24) | 0x00)
            if (csw & 0x7) != 0x2:
                port.write_ap((0 << 24) | 0x00, (csw & ~0x37) | 0x02)
            return
        except Exception:
            _clear_sticky(port)

# --------------------------------------------------------------------------
# DCI protocol
# --------------------------------------------------------------------------
def dci_connect(port):
    idr = port.dpidr.idr
    if idr != 0x6BA02477:
        sys.exit("ERROR: unexpected DP IDCODE 0x%08X -- is this really an "
                 "EFR32 Series 2 part?" % idr)
    dpw(port, 0x00, 0x1E)              # ABORT: clear sticky errors
    dpw(port, 0x04, 0x50000000)        # CTRL/STAT: power up debug + system
    dpw(port, 0x08, 0x01000000)        # SELECT: APSEL=1 (APB-AP), bank 0
    apw(port, 0x00, 0x22000002)        # CSW: 32-bit access
    dciid = mem_read(port, 0x10FC)
    if dciid != 0xDC11D:
        sys.exit("ERROR: could not reach the DCI (got DCIID 0x%X, expected "
                 "0xDC11D)." % dciid)

def dci_status(port):
    return mem_read(port, 0x1008)

def dci_write_cmd(port, word):
    for _ in range(100):
        s = dci_status(port)
        if s & 1:            # WPENDING
            time.sleep(0.01); continue
        if s & 0x100:        # RDATAVALID -- can't write
            sys.exit("ERROR: DCI RDATAVALID set, cannot write command.")
        mem_write(port, 0x1000, word)
        return
    sys.exit("ERROR: DCI write timeout.")

def dci_read_response(port):
    for _ in range(100):
        if dci_status(port) & 0x100:    # RDATAVALID
            return mem_read(port, 0x1004)
        time.sleep(0.01)
    sys.exit("ERROR: DCI read timeout.")

def read_se_status_words(port):
    dci_connect(port)
    dci_write_cmd(port, 8)              # length
    dci_write_cmd(port, 0xFE010000)     # cmd: read SE status
    recvlen = dci_read_response(port)
    if recvlen & 0xFFFF0000:
        sys.exit("ERROR: SE status command failed (response 0x%X)." % recvlen)
    words = []
    n = recvlen - 4
    while n > 0:
        n -= 4
        words.append(dci_read_response(port))
    # long xG21 format: 9 words, lock flags in word[7]; else word[3]
    lock_idx = 7 if recvlen == 0x28 else 3
    return recvlen, words, lock_idx

# --------------------------------------------------------------------------
# Pretty printers
# --------------------------------------------------------------------------
def print_lock(words, lock_idx):
    dl = words[lock_idx]
    print("  Debug lock (config):    %s" % ("Enabled" if dl & 0x01 else "Disabled"))
    print("  Device erase:           %s" % ("Enabled" if dl & 0x02 else "Disabled"))
    print("  Secure debug:           %s" % ("Enabled" if dl & 0x04 else "Disabled"))
    print("  Debug lock (hw status): %s" % ("Enabled" if dl & 0x20 else "Disabled"))
    locked = bool(dl & 0x20)
    if locked:
        print("  -> This device is DEBUG-LOCKED. Run `dci.py erase` to unlock "
              "(wipes flash).")
    return locked

def print_se(recvlen, words):
    if recvlen == 0x28 and len(words) >= 6:
        fw = words[5]
        print("  SE firmware version:    %d.%d.%d (0x%X)" %
              ((fw >> 16) & 0xFF, (fw >> 8) & 0xFF, fw & 0xFF, fw))
        print("  Reset counter (volatile): %d   "
              "(counts resets since last power-up; not a lifetime/identity value)"
              % words[3])
    print("  SESTATUS raw: %s" % " ".join("0x%X" % w for w in words))

def eui_str(hi, lo):
    b = ((hi << 32) | lo).to_bytes(8, "big")
    return ":".join("%02X" % x for x in b)

def print_chip_identity(port, locked):
    """DEVINFO + CPUID. The core's memory is only reachable when unlocked."""
    if locked:
        print("  (unavailable: the device is debug-locked -- run `dci.py erase` "
              "to unlock, then re-read.)")
        return
    try:
        ahb_setup(port)
        cpuid = ahb_read(port, SCB_CPUID)
        part  = ahb_read(port, DI_PART)
        info  = ahb_read(port, DI_INFO)
        pkg   = ahb_read(port, DI_PKGINFO)
        e64h, e64l = ahb_read(port, DI_EUI64H), ahb_read(port, DI_EUI64L)
    except Exception as e:
        _clear_sticky(port)
        print("  (could not read the core's memory after retries -- likely a "
              "transient SWD/wiring glitch; try `dci.py status` again.)")
        print("   underlying error: %s" % e)
        return

    cores = {0xC20: "Cortex-M0", 0xC60: "Cortex-M0+", 0xC23: "Cortex-M3",
             0xC24: "Cortex-M4", 0xD21: "Cortex-M33"}
    pn = (cpuid >> 4) & 0xFFF
    print("  Core:                   %s r%dp%d (CPUID 0x%08X)" %
          (cores.get(pn, "0x%X" % pn), (cpuid >> 20) & 0xF, cpuid & 0xF, cpuid))
    print("  Unique ID (EUI-64):     %s   [%08X%08X]" %
          (eui_str(e64h, e64l), e64h, e64l))
    print("  Production revision:    %d" % ((info >> 16) & 0xFF))
    print("  DEVINFO raw: PART=0x%08X INFO=0x%08X PKGINFO=0x%08X" %
          (part, info, pkg))

# --------------------------------------------------------------------------
def cmd_status(port):
    recvlen, words, lock_idx = read_se_status_words(port)
    print("Lock state:")
    locked = print_lock(words, lock_idx)
    print("Secure Engine:")
    print_se(recvlen, words)
    print("Chip identity:")
    print_chip_identity(port, locked)

def cmd_erase(port):
    dci_connect(port)
    dci_write_cmd(port, 8)
    dci_write_cmd(port, 0x430F0000)     # cmd: device erase
    time.sleep(2.0)
    print("Device erase command sent. Reset the device; debug should now be "
          "available and flash is blank.")

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd not in ("status", "erase"):
        sys.exit("usage: dci.py [status|erase]")
    probe, port = connect()
    try:
        if cmd == "status":
            cmd_status(port)
        else:
            cmd_erase(port)
    finally:
        try:
            probe.disconnect(); probe.close()
        except Exception:
            pass

if __name__ == "__main__":
    main()
