# Unlocking and reflashing an IKEA RODRET (EFR32MG21)

This documents how a factory **IKEA RODRET wireless dimmer** (type E2201) was
unlocked over SWD and reflashed with a tiny blinky, using nothing but a
CMSIS-DAP probe (a Raspberry Pi Debugprobe) and [pyOCD](https://pyocd.io/).

The hard part isn't flashing — it's that factory units ship with the debug port
**locked**, and the chip is a Silicon Labs **EFR32 Series 2** part, which needs a
special unlock protocol (the DCI) that stock tools don't speak.

---

## The hardware

| | |
|---|---|
| MCU | **EFR32MG21A010F1024IM32** — ARM Cortex-M33, 1024 KB flash (8 KB pages), 96 KB RAM |
| Debug | SWD only (Series 2 has no JTAG) |
| Probe used | Raspberry Pi Debugprobe (CMSIS-DAP) |
| pyOCD target | `efr32mg21a010f1024im32` (from the `SiliconLabs.GeckoPlatform_EFR32MG21_DFP` pack) |

The RODRET exposes its debug pads through **six holes under the battery cover**,
so you don't have to open the case. Wire the probe to **GND**, **SWDIO**,
**SWCLK**, and **RESET** (go by the labels printed on the PCB — pad order varies),
and make sure the board is **powered** (battery in, or 3V3 on VCC). The Pi
Debugprobe does *not* power the target.

> The chip identifies itself: `CPUID = 0x410FD213` (Cortex-M33), and the DEVINFO
> area at `0x0FE08000` decodes to the MG21 A010 1 MB part. The factory EUI-64 and
> calibration in DEVINFO survive a device erase.

---

## How you tell a locked unit from an unlocked one

Connect with pyOCD and watch what the access ports (APs) do:

```sh
.venv/bin/pyocd commander -t cortex_m -M attach -c "show aps"
```

- **No ACK at all** → nothing is answering on the wire. Wiring, power, or the
  chip is in deep sleep (Series 2 turns the debug port off in EM2/EM4). Retry
  while pressing a button, or use `-M under-reset`.
- **APs enumerate but `FAULT ACK` reading the ROM table, "No cores discovered"**
  → the debug port is alive but the **core is walled off: the chip is locked.**
  This is what a factory RODRET looks like.
- **Core discovered, flash/RAM readable** → unlocked.

On the locked unit we saw:

```
DPIDR     = 0x6BA02477   SW-DPv2 debug port — talks fine
AP0 IDR   = 0x84770001   AHB-AP (Cortex-M33 memory)   <- IDR reads, but memory faults
AP1 IDR   = 0x54770002   APB-AP                        <- this is the DCI mailbox
AP2 IDR   = 0x84770001   AHB-AP (Secure Engine side)
CTRL/STAT = 0xF0000F40   debug+system powered, no stale error
```

Every AP answers its IDR, but any *memory* read through AP0 faults. That mismatch
is the signature of a Secure-Engine debug lock.

---

## The unlock: DCI device erase

Series 2 unlock does **not** use the old Series 0/1 AAP + `0xCFACC118` key. It
uses the **DCI (Debug Challenge Interface)**, a mailbox reached through the
**APB-AP (AP#1)** treated as a MEM-AP:

| DCI register | Address |
|---|---|
| `DCIWDATA`  | `0x1000` |
| `DCIRDATA`  | `0x1004` |
| `DCISTATUS` | `0x1008` |
| `DCIID`     | `0x10FC` (reads `0xDC11D` on a good handshake) |

The protocol (ported from the [knieriem OpenOCD efm32s2
driver](https://github.com/knieriem/openocd-efm32-series2) into `dci.py`):

1. Connect: clear DP sticky errors, power up debug, set the APB-AP CSW to 32-bit,
   and read `DCIID` — must be `0xDC11D`.
2. To send a command, poll `DCISTATUS` (wait for `WPENDING` clear), then write
   words to `DCIWDATA`: a **length** word, then the **command** word.
3. Read responses from `DCIRDATA` when `DCISTATUS` bit `RDATAVALID (0x100)` is set.

Commands used:

| Command word | Meaning |
|---|---|
| `0xFE010000` | read Secure Engine status (non-destructive) |
| `0x430F0000` | **device erase** (wipes all flash + RAM, clears the lock) |

### Always check first (safe)

```sh
.venv/bin/python3 dci.py status
```

You want to see **`Device erase: Enabled`**. If it says *Disabled*, the device
was permanently locked and erase won't rescue it.

`status` prints three groups. The **Lock state** and **Secure Engine** groups
come from the DCI and work even on a locked chip. The **Chip identity** group is
read through the core's memory, so it only appears when the device is *unlocked*
(on a locked part it says so instead). Example from an unlocked unit:

```
Lock state:
  Debug lock (config):    Disabled
  Device erase:           Enabled
  Secure debug:           Disabled
  Debug lock (hw status): Disabled
Secure Engine:
  SE firmware version:    1.2.13 (0x1020D)
  Reset counter (volatile): 24   (counts resets since last power-up; ...)
  SESTATUS raw: 0x0 0x0 0x0 0x18 0x20 0x1020D 0xFFFFFFFF 0x2 0xFFFFFFFF
Chip identity:
  Core:                   Cortex-M33 r0p3 (CPUID 0x410FD213)
  Unique ID (EUI-64):     B0:40:1D:46:11:75:04:A9   [B0401D46117504A9]
  Production revision:    96
  DEVINFO raw: PART=0xFF204D01 INFO=0x00600400 PKGINFO=0x00111112
```

The **Unique ID (EUI-64)** is the reliable way to tell your several RODRETs
apart — it is burned per-chip and survives erasing. On a *locked* factory unit
the first two groups still print (showing `Debug lock (config): Enabled` and
`hw status: Enabled`), and the identity group reports that it can't read the core
until you unlock it.

> The identity group reads the core's memory through the AHB-AP, and on a freshly
> connected unlocked part the **first such transaction can transiently fault**
> (a sticky-error glitch). `dci.py` retries those reads a few times (clearing the
> sticky error between tries) rather than giving up — and it decides "locked vs.
> not" from the SESTATUS bit, never from a read fault. If it still can't read
> after retries it says so as a transient glitch, not a lock. (An earlier version
> gave up on the first fault and wrongly blamed a lock; see JOURNAL §12.)

If nothing is connected you get a plain message instead of a traceback: no probe
on USB ("No CMSIS-DAP debug probe found...") versus a probe present but no target
responding ("the probe is connected, but no SWD target responded" + wiring/power/
sleep checklist).

### Erase (destructive — this wipes IKEA's firmware)

```sh
.venv/bin/python3 dci.py erase
```

Then reset and confirm the core is now reachable:

```sh
.venv/bin/pyocd commander -t efr32mg21a010f1024im32 -M under-reset \
    -c "reset halt" -c "status" -c "read32 0x0 16"
```

A healthy unlocked result: core halts, no ROM-table faults, flash reads `0xFF…`.

> **The reset after erase is not optional.** Immediately after a device-erase the
> core is held in a not-yet-initialized state: the DCI reports the chip unlocked,
> but reads through the core's AHB-AP still fail (even with retries) until you
> reset it. So the order is always **erase → reset → read/flash**. `dci.py status`
> run in that window honestly says "transient glitch, try again" rather than
> "locked" — because it trusts the SESTATUS lock bit, which already reads
> unlocked. One `reset halt` and identity/flash reads work first try.

---

## The blinky

`blinky.bin` is a 68-byte hand-written Thumb program that toggles **PC0** forever.
It needs no clock setup because the MG21 clocks the GPIO block out of reset.

```
vector table:  SP = 0x20018000 (top of 96 KB RAM), Reset = 0x00000009 (+Thumb bit)

  GPIO_PC_MODEL (0x4003C094) = 4          ; PC0 = push-pull output
loop:
  GPIO_PC_DOUTSET (0x4003D0A0) = 1        ; PC0 high   -> delay 0x017D7840 counts
  GPIO_PC_DOUTCLR (0x4003E0A0) = 1        ; PC0 low    -> delay 0x017D7840 counts
  b loop
delay(r5): subs r5,#1; bne delay; bx lr
```

Flash it at address 0 and run:

```sh
.venv/bin/pyocd flash -t efr32mg21a010f1024im32 --base-address 0x0 blinky.bin
.venv/bin/pyocd commander -t efr32mg21a010f1024im32 -M under-reset -c "reset" -c "go"
```

Verify it's running: halt and the PC sits inside the delay loop (~`0x2c`), and
`read32 0x4003C094` reads `4` (PC0 configured as output).

---

## Reproducing on another RODRET

Everything is wrapped in one script:

```sh
./unlock-and-blink.sh
```

It checks the probe, reads lock status, prompts before erasing, erases, confirms
the unlock, flashes `blinky.bin`, and resets to run.

---

## The units worked on so far

All three are the **same part** (EFR32MG21A010F1024IM32, Cortex-M33 `r0p3`), the
**same SE firmware** (1.2.13 / `0x1020D`), and the same `PART`/`INFO`/`PKGINFO`
registers. They differ only by their burned-in **EUI-64** (the reliable identity)
and their history. Read any unit's EUI-64 with `dci.py status`.

| Unit | EUI-64 | Arrived as | Now |
|---|---|---|---|
| #1 | `B0:40:1F:48:11:8C:04:B3` | already unlocked + blinky (done by the owner earlier) | blinky |
| #2 | `B0:40:1D:46:11:75:04:A9` | factory debug-locked | unlocked, blinky |
| #3 | `B0:40:20:47:10:53:04:C1` | brand-new, factory debug-locked | unlocked, blinky |

Notes:
- A factory unit reads `Debug lock (config): Enabled` + `hw status: Enabled` and
  SESTATUS word[7] = `0x23`; after a device-erase it reads `Disabled`/`Disabled`
  and word[7] = `0x2`. All three had **Device erase: Enabled**, so all were
  recoverable.
- The EUI-64s share the top bytes `B0:40:…` (a Silicon Labs block); the per-die
  part is what distinguishes them.

---

## Files

| File | What it is |
|---|---|
| `dci.py` | EFR32 Series 2 DCI driver over pyOCD: `status` (safe) / `erase` (destructive) |
| `blinky.bin` | The 68-byte PC0 blinky image, flashed at `0x0` |
| `unlock-and-blink.sh` | One-shot reproduce: probe → status → erase → flash → run |
| `.venv/` | Python venv with `pyocd` 0.45.1 and the MG21 device pack |

---

## Gotchas

- **Deep sleep kills the debug port.** If `dci.py status` can't connect, the
  firmware may have the chip in EM2/EM4. Hold RESET or press a button while
  connecting, or use `-M under-reset`.
- **The erase is one-way.** It destroys IKEA's signed firmware, which is not
  publicly available — you can't put the stock image back.
- **pyOCD prints one harmless error** on every connect to an MG21:
  `Transfer error ... reading AHB-AP#2 ROM table`. That's the Secure Engine's AP
  refusing normal reads; ignore it.
- **Probe doesn't power the board.** Keep a battery in or feed 3V3.

## References

- [knieriem/openocd-efm32-series2](https://github.com/knieriem/openocd-efm32-series2) — the DCI protocol this is ported from
- [AN1190: EFR32 Series 2 Secure Debug](https://www.silabs.com/documents/public/application-notes/an1190-efr32-secure-debug.pdf)
- [EFR32MG21 datasheet](https://www.silabs.com/documents/public/data-sheets/efr32mg21-datasheet.pdf)
- [basilfx/TRADFRI-Hacking — module pinouts](https://github.com/basilfx/TRADFRI-Hacking/blob/master/MODULES.md)
