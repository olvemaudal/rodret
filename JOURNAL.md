# Journal: cracking into a locked RODRET, step by step

This is the honest, chronological story of how I got from "a RODRET and a debug
probe" to "a blinking, unlocked RODRET" — **including the things that didn't
work**, the dead ends in research, and *why* each step made sense. The clean
instructions live in `README.md`; this file is the learning version.

If you want to understand the whole adventure, read top to bottom.

---

## 0. Taking stock

First thing I did was look at what was already on the machine, before touching
any hardware:

```sh
ls -la                      # the project dir
.venv/bin/pyocd --version   # pyOCD 0.45.1 is installed in a venv
.venv/bin/pyocd list        # what probes are plugged in?
```

That last command printed:

```
0   Raspberry Pi Debugprobe on Pico (CMSIS-DAP)   E66368254F64B537   n/a
```

So: a **CMSIS-DAP** class probe (the Pi Debugprobe firmware running on an RP2040).
Good to know, because it rules some things in and out later — notably, it is *not*
a SEGGER J-Link, and Silicon Labs' official unlock tool (Simplicity Commander)
really wants a J-Link.

I also checked which chip families pyOCD already had "packs" for:

```sh
.venv/bin/pyocd pack find efr32mg2
```

The **EFR32MG21** device pack was already installed. That mattered later: pyOCD
can only program flash if it has a flash algorithm for the exact part, which comes
from that pack.

**Lesson:** before connecting to anything, know your probe type and whether your
tool has support for your chip. These two facts shape every later decision.

---

## 1. Research: what *is* a RODRET inside?

I didn't know the exact chip, so I searched the web. The useful findings:

- The RODRET (type **E2201**) is a Zigbee dimmer built on a Silicon Labs
  **EFR32 Series 2** wireless SoC — the same MG21 family used in other IKEA gear.
- It has **six holes under the battery cover** exposing labeled debug pads, so you
  can reach SWD without opening the case. (Confirmed by a
  [hackaday.io project](https://hackaday.io/project/194841-converting-ikea-rodret-into-a-switch-digitizer).)
- For *comparison*, the [basilfx TRADFRI module
  list](https://github.com/basilfx/TRADFRI-Hacking/blob/master/MODULES.md) gives
  SWD pinouts for related modules (e.g. the MGM210L uses PA1=SWCLK, PA2=SWDIO).

### Dead ends in research

Not everything panned out:

- I chased the `MattWestb/EFR32-FW` repo expecting RODRET flashing notes. It only
  mentioned IKEA in passing (a different module). **Nothing useful.**
- Several teardown pages (trmm.net, all-about-circuits) covered *older* TRÅDFRI
  remotes, not the RODRET. **Wrong generation.**
- I tried to list the MattWestb repo tree with `gh`, but `gh` wasn't authenticated
  in this environment, so that failed. I switched to the plain GitHub REST API
  over `curl` instead, which needs no auth for public repos.

**Lesson:** a lot of "IKEA hacking" material online is about Series 0/1 TRÅDFRI
devices from years ago. The RODRET is newer (Series 2), and — as you'll see — that
distinction is the whole ballgame for unlocking.

---

## 2. First unit: the one that turned out to be yours

### A false start (my fault, not the hardware's)

Before you had wired the probe to the pads, I ran:

```sh
.venv/bin/pyocd commander -t cortex_m -M attach -c "show map"
```

and got:

```
Error: ... SWD/JTAG communication failure (No ACK)
```

I slowed the clock to 100 kHz and tried connect-under-reset — same "No ACK". I
reasoned (correctly) that **"No ACK" means literally nothing is answering on the
wire** — so either it wasn't wired yet, unpowered, or asleep. I asked you to wire
it up. You did.

### It connects

```sh
.venv/bin/pyocd commander -t cortex_m -M attach -c "read32 0xE000ED00"
```

```
e000ed00:  410fd213     <- CPUID
```

`0x410FD213` is an **ARM Cortex-M33**. Then I read the chip's DEVINFO block:

```sh
-c "read32 0x0FE08000 0x60"
```

The bytes there decode to **EFR32MG21A010, 1024 KB flash, 96 KB RAM**. Now I knew
the exact pyOCD target: `efr32mg21a010f1024im32`.

> One error always printed on connect:
> `Transfer error ... reading AHB-AP#2 ROM table ... FAULT ACK`.
> I noted it but didn't panic — AP#2 is the Secure Engine's access port, which
> refuses ordinary reads. The *core* was found fine. (This detail becomes the key
> clue on the second unit.)

### Reading the flash

I dumped the first words of flash and the vector table. Only the **first 68 bytes**
of the whole 1 MB were non-`0xFF`. I disassembled them with
`arm-none-eabi-objdump -b binary -m arm -M force-thumb` and found a minimal
loop: configure PC0 as output, then toggle it high/low forever with a software
delay. A blinky.

I dumped all 1 MB to `rodret_flash.bin` just to be safe, and confirmed only page 0
had content.

Then you told me: this was **your** RODRET — you'd already erased it and
hand-written that blinky. So this unit was never the challenge; it was the warm-up
that taught us what an *unlocked* MG21 looks like.

**Lesson / the mental model we now have:** on an open MG21, the DP responds, the
core's AHB-AP ROM table reads cleanly, the core is discovered, and flash is
readable. Hold that picture.

---

## 3. Second unit: the locked factory one

You swapped in a factory RODRET and said "you can try." Same connect command:

```sh
.venv/bin/pyocd commander -t cortex_m -M attach -c "show aps"
```

```
Transfer error ... reading AHB-AP#0 ROM table ... FAULT ACK
Transfer error ... reading AHB-AP#2 ROM table ... FAULT ACK
Error: No cores were discovered!
```

**This is different from the first unit in a specific, diagnostic way.** On the
blinky unit, only AP#2 faulted and the core was found. Here **AP#0 faults too**,
and no core is discovered. But notice it's **`FAULT ACK`, not `No ACK`** — the
debug port *is* answering; it's the step *after* (reading through the core's
access port) that's being refused.

That pattern — the access ports are present but memory access through them faults
— is the fingerprint of a **debug lock** enforced by the chip's Secure Engine.

### Proving it: a read-only AP scan (and a comedy of API errors)

I wanted to read the raw DP and AP identification registers to confirm the APs
existed. This took **four tries** because I was fighting pyOCD's internal API, not
the hardware:

1. **`probe.open()` → `AssertionError: self.session`.**
   The CMSIS-DAP probe object refuses to open until it has a `Session` attached.

2. **`Session(probe); session.open()` → `DebugError: No cores were discovered!`**
   `Session.open()` runs the *full* target init, which is exactly the thing that
   fails on a locked chip. I needed to init the debug port *without* the
   core-discovery step.

3. **`DebugPort(probe, None)` → `AttributeError: 'NoneType' has no 'session'`.**
   `DebugPort` wants a "target" argument that carries a `.session`.

4. **`DebugPort(probe, Shim()).init()` → `has no attribute 'init'`.**
   Wrong method name. Introspecting with `dir(DebugPort)` showed the method is
   **`connect()`**, not `init()`.

The combination that finally worked: construct a `Session` just to attach it to the
probe (so `open()` is happy), but **never call `Session.open()`**; instead drive
`DebugPort.connect()` directly and read registers by hand:

```python
probe = DebugProbeAggregator.get_all_connected_probes()[0]
Session(probe)                         # attaches probe.session, no target init
probe.open(); probe.set_clock(1_000_000)
probe.connect(DebugProbe.Protocol.SWD)
class Shim: session = probe.session
port = DebugPort(probe, Shim())
port.connect()
```

Result:

```
DPIDR     = 0x6BA02477   SW-DPv2 debug port (standard for Series 2)
AP0 IDR   = 0x84770001   a valid MEM-AP (AHB-AP) — IDR reads fine...
AP1 IDR   = 0x54770002   a valid MEM-AP (APB-AP)
AP2 IDR   = 0x84770001   a valid MEM-AP (AHB-AP)
CTRL/STAT = 0xF0000F40   debug+system powered up, no stale sticky error
```

So the APs **enumerate** (their IDRs read), but reading *memory* through AP0
faults. That nails it: **debug-locked, not broken, not asleep.**

**Lesson:** when a high-level tool aborts on a locked chip, you often have to drop
to the raw debug-port layer to even observe what's going on. And half the battle
with an unfamiliar library is finding which object owns which method — `dir()` on
the class is your friend.

---

## 4. Research round 2: how do you unlock a *Series 2* part?

My first instinct was the classic Silicon Labs unlock I'd seen referenced: write
the magic key **`0xCFACC118`** to `AAP_CMDKEY`, then set `DEVICEERASE` in
`AAP_CMD`. I searched to confirm the exact sequence — and that's where I caught my
own mistake:

> That AAP + `0xCFACC118` sequence is for **EFM32/EFR32 Series 0 and 1**. Our chip
> is **Series 2**. Series 2 replaced the AAP with a different mechanism: the **DCI
> (Debug Challenge Interface)**.

A second search for "Series 2 recover without J-Link" pointed at the key resource:
the [**knieriem/openocd-efm32-series2**](https://github.com/knieriem/openocd-efm32-series2)
project — a community OpenOCD driver that implements the Series 2 DCI, and which
people have driven with cheap CMSIS-DAP probes.

### Getting the source (more 404s)

I tried to fetch the driver assuming a normal OpenOCD layout:

```
.../master/src/flash/nor/efm32s2.c   -> 404
.../main/src/flash/nor/efm32.c       -> 404
```

All 404. So I asked the GitHub API what the repo *actually* contains:

```sh
curl -s https://api.github.com/repos/knieriem/openocd-efm32-series2 \
  | ... default_branch           # -> "main"
curl -s ".../git/trees/HEAD?recursive=1" | grep efm32
```

That revealed the real paths: `efm32s2/efm32s2.c` and **`efm32s2/efm32s2.cfg`** —
a standalone driver, not a full OpenOCD tree. The `.cfg` file was the jackpot: it
contains the entire DCI protocol written in readable Tcl.

**Lesson:** don't assume a repo's file layout — ask the API for the tree. And when
you find a reference implementation, the config/script file often spells out the
protocol more clearly than the C.

---

## 5. Understanding the DCI protocol (from the OpenOCD Tcl)

Reading `efm32s2.cfg`, the DCI turned out to be refreshingly simple once you see
it. The trick: the **APB-AP (AP#1)** is used as a MEM-AP whose "memory" is a small
set of DCI mailbox registers:

| Register | Address | Role |
|---|---|---|
| `DCIWDATA`  | `0x1000` | write command/data words here |
| `DCIRDATA`  | `0x1004` | read response words here |
| `DCISTATUS` | `0x1008` | bit0 `WPENDING`, bit8 `RDATAVALID` |
| `DCIID`     | `0x10FC` | reads `0xDC11D` when you've connected right |

The handshake:

1. Clear DP sticky errors (`ABORT = 0x1E`), power up debug
   (`CTRL/STAT = 0x50000000`), set the APB-AP `CSW = 0x22000002` (32-bit access).
2. Read `DCIID`; if it's `0xDC11D`, you're talking to the DCI.
3. **To send a command:** poll `DCISTATUS` until `WPENDING` is clear, then write a
   **length** word and then a **command** word to `DCIWDATA`.
4. **To read a response:** poll `DCISTATUS` until `RDATAVALID` is set, then read
   `DCIRDATA`.

The command words I cared about:

| Command | Meaning |
|---|---|
| `0xFE010000` | read Secure Engine status (safe, read-only) |
| `0x430F0000` | **device erase** (wipes flash+RAM, clears lock) |
| `0x430C0000` | set debug lock (the reverse — not used here) |

I ported these routines from Tcl into Python (`dci.py`), driving the same raw
`read_ap`/`write_ap`/`read_dp` primitives I'd gotten working in step 3. One
convenience: pyOCD's `read_ap()` already handles the SWD "posted read + RDBUFF"
dance internally, so each DCI register read is a single call.

---

## 6. Checking the lock status *before* erasing

The responsible move is to ask the chip whether erase-to-recover is even allowed,
*before* doing anything destructive:

```sh
.venv/bin/python3 dci.py status
```

```
DCI connected (DCIID=0xDC11D)
SESTATUS words: ['0x1','0x0','0x0','0xd','0x20','0x1020d','0x1090002','0x23','0xffffffff']
  Debug lock (config):   Enabled
  Device erase:          Enabled
  Secure debug:          Disabled
  Debug lock (hw status):Enabled
```

### A subtle decode bug I had to notice

The OpenOCD code picks *which* word in the response holds the lock bits based on
the response length: index 3 normally, but **index 7 if the length word is `0x28`**
(the longer xG21 format). My first mental check used word[3] = `0xd`, whose bits
*didn't* match the printed lines. The printed lines only make sense if the lock
word is **word[7] = `0x23`**:

- `0x23` bit0 = 1 → debug lock (config) **Enabled** ✓
- `0x23` bit1 = 1 → device erase **Enabled** ✓
- `0x23` bit2 = 0 → secure debug **Disabled** ✓
- `0x23` bit5 = 1 → debug lock (hw) **Enabled** ✓

So the code correctly took the `0x28`-length branch (index 7), and the chip is
telling us plainly: it's locked, **but device-erase recovery is enabled.** If that
line had said *Disabled*, the part would have been permanently locked and we'd have
been stuck. We weren't.

**Lesson:** read the status, and actually reconcile every bit against what the tool
prints. The "which word" detail is exactly the kind of thing that silently gives
you a wrong answer.

---

## 7. The erase, and the moment of truth

With your go-ahead (it's destructive — it kills IKEA's firmware for good):

```sh
.venv/bin/python3 dci.py erase     # sends length=8, cmd=0x430F0000, waits 2s
```

Then I reset the chip and tried a **normal** connect — the real test:

```sh
.venv/bin/pyocd commander -t efr32mg21a010f1024im32 -M under-reset \
    -c "reset halt" -c "status" -c "read32 0x0 16"
```

```
Successfully halted device on reset
Core 0 (Cortex-M33):  Halted [Secure]
00000000:  ffffffff ffffffff ffffffff ffffffff
```

**That's success.** Compare to step 3: no more AP#0 ROM-table fault, the **core is
now discovered and halts**, and flash reads `0xFF` — fully erased and blank. The
lock is gone.

---

## 8. Flashing the blinky and confirming it runs

Rather than write new code, I reused the exact 68-byte blinky from your first unit
(I still had it in `rodret_flash.bin`). I sliced out the first 68 bytes as
`blinky.bin` and flashed it at address 0:

```sh
head -c 68 rodret_flash.bin > blinky.bin
.venv/bin/pyocd flash -t efr32mg21a010f1024im32 --base-address 0x0 blinky.bin
```

```
Erased 8192 bytes (1 sector), programmed 8192 bytes (1 page) ... 36.58 kB/s
```

Reset and run, then halt briefly to check it's actually executing:

```sh
.venv/bin/pyocd commander -t efr32mg21a010f1024im32 -M under-reset \
    -c "reset" -c "halt" -c "reg pc" -c "read32 0x4003C094" -c "go"
```

```
pc = 0x0000002c          <- inside the delay loop
4003c094:  00000004      <- GPIO_PC_MODEL = PC0 push-pull output
```

PC0 configured as output, program counter spinning in the delay loop. **Blinking.**

---

## 9. Comparing two units — what the SE status words really mean

With both units flashed, the question became: are they actually different, and
how would you tell? I reconnected unit #1 and re-read it — its DEVINFO (including
the unique ID region) and flash were byte-for-byte what I'd recorded earlier, so
it was definitely the same physical chip. Then I swapped in unit #2 and compared.

**Same silicon, same firmware:** both read CPUID `0x410FD213`, the same `PART`
register, the same SE firmware version (`0x1020D` = 1.2.13), and both were now
unlocked. No meaningful difference there.

**One field that genuinely differs — the unique ID.** The words at DEVINFO
offset `0x54`/`0x58` differed between the chips, while `PART` (offset `0x10`) was
identical. That's exactly what you'd expect if `0x54/0x58` is the per-chip
**EUI-64** and `0x10` is the part-type code. So the EUI-64 is the reliable way to
tell your several RODRETs apart.

**A counter I first misread.** On unit #1 I watched one status word climb
`0x77 → 0x78 → 0x79`, once per reset, and I called it a "reset/boot counter."
Then unit #2 read `0x1` — but I'd recorded it at `0x62` in an earlier session.
A lifetime counter can't go *down*. That contradiction forced the correct reading:
the word is a **volatile** reset/activity counter living in Secure-Engine RAM —
it survives a CPU reset (hence the `0x77→0x79` climb) but is cleared when the chip
loses power (hence `0x62 → 0x1` after I physically unplugged and re-powered it).
It reflects "activity since last power-up," not identity or wear.

**Lesson:** two readings that disagree are a gift — they tell you a field is
volatile/stateful, which you'd never learn from a single snapshot. And always let
the *unique* ID (EUI-64), not a convenient-looking counter, be your notion of
"which chip is this."

---

## 10. Making `status` tell the whole story

You then asked for all the relevant info folded into `dci.py status`. Two things
mattered here.

**Don't guess register offsets — get them from the vendor.** Rather than eyeball
the DEVINFO dump, I pulled the real layout from the installed device pack:

```sh
PK=".../SiliconLabs/GeckoPlatform_EFR32MG21_DFP/2025.12.1.pack"
unzip -p "$PK" Device/SiliconLabs/EFR32MG21/Include/efr32mg21_devinfo.h
```

That header defines `DEVINFO_TypeDef` field-by-field, so I could read off exact
offsets (`INFO`@`0x0C`, `PART`@`0x10`, `EUI64L/H`@`0x54/0x58`, …) and the
bitfield masks for things like CPUID. I then **confirmed them empirically**: PART
was identical across both chips (right — same part), EUI-64 differed (right —
unique). I dropped the EUI-48 field because it reads as unprogrammed `0xFF` junk
on these parts and would only mislead.

**A real architectural constraint: identity needs an unlocked core.** The lock
status comes from the DCI (the APB-AP, AP#1), which answers even on a locked chip.
But CPUID and DEVINFO live in the Cortex-M33's memory, reached through the
**AHB-AP (AP#0)** — the very port that *faults* when the device is locked. So
`status` is built in two tiers: the DCI-based lock/SE info always prints, and the
chip-identity block is wrapped in a `try` that, on a fault, clears the sticky
error and prints "identity unavailable: the device is debug-locked" instead of
crashing. You always get the lock verdict; you get the EUI-64 only once it's open.

**Lesson:** the installed toolchain packs are a precise, offline source of truth
for register maps — better than guessing or trusting a forum post. And design a
status tool so the *always-available* facts don't depend on the *sometimes-locked*
ones.

---

## 11. Failing gracefully (and testing it for real)

The last request was that missing hardware produce a sentence, not a stack trace.
There are two distinct failure modes, and they want different advice:

- **No probe on USB** — `get_all_connected_probes()` returns an empty list, so
  indexing `[0]` would throw. An explicit check turns that into: "No CMSIS-DAP
  debug probe found… is the Pico plugged in?"
- **Probe present, but nothing on the SWD wires** — the probe opens fine, but
  `DebugPort.connect()` (which does the line reset and reads DPIDR) raises
  `No ACK`. That becomes: "the probe is connected, but no SWD target responded,"
  followed by the wiring / power / asleep checklist.

I first proved the locked-core and no-probe paths by *simulating* them (monkey-
patching an empty probe list and a faulting AHB read) so I didn't have to touch
your hardware. Then you tested both for real: pulling the SWD jumpers produced the
"no target responded (No ACK)" message, and unplugging the Pico produced the
"no probe found" message with exit code 1. No tracebacks either way.

**Lesson:** distinguish failures by *where* they happen in the connect sequence
(USB enumeration vs. the first SWD transaction) — that's what lets you give the
user the right checklist instead of a generic "it didn't work."

---

## 12. A real bug: a transient fault misreported as a lock

Right after shipping the richer `status`, reconnecting unit #2 produced a wrong
answer: the lock lines all said **Disabled** (unlocked), yet the Chip-identity
block printed *"unavailable… the device is debug-locked."* Those two statements
can't both be true. So this was a genuine bug in my own tool, and worth tracing.

**Confirming the chip was fine.** pyOCD's normal path read the core without
trouble:

```sh
.venv/bin/pyocd commander -t efr32mg21a010f1024im32 -M attach -c "read32 0xE000ED00 4"
# -> e000ed00:  410fd213     (CPUID reads fine)
```

So the core was readable; the fault was in *my* AHB-AP code.

**Instrumenting it.** I reproduced `status`'s exact state (connect → DCI session,
which leaves DP SELECT on AP#1) and then read AP#0 by hand. The first time it
*worked* — CSW `0x03800052` (already 32-bit), CPUID `0x410FD213`. I suspected my
read-modify-write of CSW, so I tested write-path vs no-write-path, twice each:
**all four succeeded.** That was the tell — the failure wasn't deterministic. It
was **intermittent**, hitting only the *first* AHB-AP transaction after a fresh
connect: a sticky-error / pipeline glitch on the AP#1→AP#0 switch that a single
retry clears. pyOCD's normal path never shows it because its heavier init retries
and settles the AP before reading memory.

**Two mistakes in my code, not one:**

1. It **gave up after a single fault** instead of retrying.
2. On that fault it **assumed "locked"** — even though the DCI had *just* reported
   the device unlocked. The lock state was already known; the code threw it away
   and guessed from a symptom.

**The fix.**

- AHB-AP reads now **retry (up to 5×), clearing the DP sticky-error (`ABORT`)
  between attempts**. `ahb_setup` only rewrites CSW if it isn't already word-sized,
  and tolerates a transient fault there too.
- `print_chip_identity` now takes the **real lock bit from SESTATUS**. Locked →
  "run `dci.py erase`". Unlocked but still unreadable after retries → an honest
  "transient SWD/wiring glitch, try again" with the underlying error — never a
  false lock claim.

Verified by running `status` five times back-to-back on #2: identity read every
time (`EUI-64 B0:40:1D:46:11:75:04:A9`).

**Lessons:**
- *A result that contradicts another result you already have is a bug in the tool,
  not a fact about the device.* The DCI said unlocked; the identity block said
  locked; the contradiction was the clue.
- *Intermittent means retry, not diagnose.* Four passing tests after one failure
  is itself the diagnosis: the operation is flaky on first contact, so make it
  robust instead of attaching meaning to a single fault.
- *Don't re-derive what you already know.* The authoritative lock state comes from
  the SESTATUS bit; a read fault is weak, ambiguous evidence by comparison.

---

## 13. A brand-new unit (#3), and the erase→reset→read order

A third RODRET, unpacked and never used, was the clean confirmation that the
tooling now behaves on a genuine factory part. `dci.py status` on it:

```
Debug lock (config):    Enabled      <- IKEA locks them at the factory
Device erase:           Enabled      <- recoverable
Debug lock (hw status): Enabled
SESTATUS raw: 0x1 0x0 0x0 0x1 0x20 0x1020D 0x1090002 0x23 0xFFFFFFFF
Chip identity:
  (unavailable: the device is debug-locked -- run `dci.py erase` ...)
```

That is the canonical factory fingerprint — word[7] = `0x23` (config-lock +
erase-enabled + hw-lock), and word[6] = `0x1090002` (present on locked units;
it becomes `0xFFFFFFFF` after an erase). Importantly, the "identity unavailable"
line here is **correct** — the §12 fix means it only says "locked" when the
SESTATUS lock bit actually says so, and here it does.

Unlocking it taught one more sequencing detail. Straight after `dci.py erase`,
`status` showed the lock cleared (`Disabled`/`Disabled`) **but still couldn't
read the core**, even through the retries:

```
Debug lock (config): Disabled
Debug lock (hw status): Disabled
  (could not read the core's memory after retries -- likely a transient ... glitch)
```

That is not a bug and not a leftover lock. **Immediately after a device-erase the
core is held un-initialized until it is reset** — the DCI (which lives on its own
AP) reports "unlocked," but the core's AHB-AP isn't live yet. A single
`pyocd ... -M under-reset -c "reset halt"` fixed it, and identity then read first
try, revealing #3's unique EUI-64. So the invariant is:

> **erase → reset → read/flash.** Never read the core in the window between erase
> and reset.

`unlock-and-blink.sh` already encodes this (its step 4 is `reset halt` before the
flash in step 5), which is why the scripted path "just works" while doing it by
hand can trip you if you skip the reset. And note the tool told the truth
throughout: it said "transient glitch, try again," not "locked," because it
trusts the SESTATUS bit over a read fault — exactly the §12 lesson paying off.

**Lesson:** a successful unlock and a usable core are two different milestones
separated by a reset. Clearing the lock bit doesn't by itself make the core
respond; the reset is what brings it up.

---

## Inventory: the three units

Same silicon (EFR32MG21A010F1024IM32, Cortex-M33 `r0p3`), same SE firmware
(1.2.13 / `0x1020D`), same `PART`/`INFO`/`PKGINFO`. They differ only by the
burned-in **EUI-64** and their history:

| Unit | EUI-64 | Arrived as | Ended as |
|---|---|---|---|
| #1 | `B0:40:1F:48:11:8C:04:B3` | already unlocked + owner's blinky | blinky |
| #2 | `B0:40:1D:46:11:75:04:A9` | factory debug-locked | unlocked, blinky |
| #3 | `B0:40:20:47:10:53:04:C1` | brand-new, factory debug-locked | unlocked, blinky |

The factory lock is a soft per-device config bit (word[7] `0x23` locked vs `0x2`
unlocked), not a hardware trait — #2 and #3 arrived locked, #1 had already been
unlocked by its owner, and all three are identical parts underneath. The volatile
reset counter (word[3]) and the EUI-64 are the only things that ever distinguished
them; only the EUI-64 is a real, persistent identity.

---

## What worked, what didn't — the short version

**Worked:**
- Reading the probe/chip identity first, and recognizing the MG21 by CPUID + DEVINFO.
- Using the *difference* between the two units (`FAULT ACK` + "no cores" vs. a clean
  connect) to diagnose a debug lock instead of guessing.
- Dropping to pyOCD's raw `DebugPort` layer to observe a locked chip.
- Finding the knieriem OpenOCD driver and reading the **`.cfg`** for the protocol.
- Porting the DCI routines to Python and checking SE status before erasing.
- Reusing the already-proven blinky image instead of writing new code.
- Using EUI-64 (from the DFP header's exact offsets) as the per-chip identity, and
  building `status` so DCI info prints even when the core is locked.
- Distinguishing "no probe" from "no target" so each gives the right checklist.

**Didn't work / detours:**
- `No ACK` on the first unit — it just wasn't wired yet.
- Four failed attempts at the raw pyOCD API (`open()` assertion, `Session.open()`
  full-init abort, `DebugPort` needing a session shim, `init()` vs `connect()`).
- Assuming the **Series 0/1 AAP `0xCFACC118`** unlock would apply — wrong; Series 2
  uses the DCI.
- Guessing the OpenOCD repo's file paths (several 404s) before asking the GitHub
  API for the real tree.
- A momentary mis-decode of the SE status (word[3] vs word[7]) that I caught by
  reconciling the bits against the printout.
- Calling the volatile activity counter a "lifetime reset counter" — corrected only
  when a later reading was *lower* than an earlier one.
- Shipping a `status` that gave up on the first (intermittent) AHB-AP fault and
  misreported an unlocked chip as "debug-locked" — fixed with retries + trusting
  the SESTATUS lock bit (§12).
- Expecting the core to be readable the instant the lock cleared — it isn't until
  a reset; the invariant is erase → reset → read/flash (§13).

**The one fact that made it all possible:** the chip reported **Device erase:
Enabled**. A permanently locked part (that bit Disabled) could not have been
recovered with any probe.
