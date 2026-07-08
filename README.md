# LiquidOS
Operating system made from scratch
This is an operating system I have been making from scratch.

## Running LiquidOS in Qemu
### You will need:
-The .zip (the one from releases or source code doesn't matter)  
-Make command line tool
### Windows
¯\_(ツ)_/¯
### Linux
You should have make already installed if you do unzip the zip file cd into the folder and run "make run"  
If you dont have make and your on a debian based distro run "sudo apt install build-essential" otherwise ¯\_(ツ)_/¯
### macOS
Same instructions as linux but if you have not downloaded make already you need to run "xcode-select --install"

## Running LiquidOS on real hardware
### You will need:
-Something to store the image like a usb flash drive (any size the image is less than a gigabyte right now)
-A computer (The official testing computer is a HP ProBook 6450b)
### Installation
Do the same steps for running LiquidOS in Qemu for your operating system then do these steps:  
1. after you run LiquidOS in Qemu go to the folder containing the makefile then cd into the build folder
2. Flash the .iso image in the folder onto your usb flash drive
3. Once it is flashed boot off the flash drive on your computer
4. If you want to install LiquidOS to the internal storage drive follow these steps again but flash to the drive as (currently) there is no way to install LiquidOS to your internal storage drive without flashing to it directly.

#NEWEST MILESTONE

# LiquidOS — NIC MAC address read milestone

Milestone 3 of the networking series. Builds on milestone 2's confirmed
real-hardware BAR-parsing success (see below): adds a read of this
NIC's own MAC address (register `RAL0`/`RAH0`), still entirely
read-only, ahead of the actually state-changing reset/link-bring-up
step that comes next.

## Milestone 2 real-hardware result (why this one stays read-only too)

`nicprobe` on the actual ProBook 6450b reported `BAR0 MEM`,
`CTRL=0x00100240`, `STATUS=0x00080600`, link down. This was a genuine
validation: real MMIO access + PCI Bus Master enable + uncacheable-
marking all worked cleanly against a SECOND, DIFFERENT Intel chip on
the exact hardware that took 13 rounds to never fully resolve EHCI's
DMA (no Master Abort, no garbage/`0xFFFFFFFF` readback, no hang).
Decoding `STATUS` against Linux's real `e1000e` driver source (fetched
directly, not worked from memory — `drivers/net/ethernet/intel/
e1000e/defines.h` on GitHub) confirmed bit 10, `PHYRA` (PHY Reset
Asserted), was set — which fully and correctly explains the observed
link-down: milestone 2 never asserts Set-Link-Up or resets the
controller, so the PHY was never brought up. Nothing wrong, nothing
guessed.

Given how expensive guessing was on this exact machine last time, this
milestone continues pulling register definitions from Linux's actual
`e1000e`/`e1000` driver source (`defines.h`/`regs.h`/`hw.h`) rather
than from memory, before writing any more register-touching code.

## What's new

`kernel/nic.h`'s `nic::probe()` now also reads `RAL0`/`RAH0` (Receive
Address slot 0, offsets `0x5400`/`0x5404` — verified against Linux's
`e1000e/regs.h`, stable across the whole e1000/e1000e family) and
decodes this NIC's own MAC address, loaded by hardware from the EEPROM
at power-on. `RAH0`'s Address-Valid bit (`RAH_AV`, bit 31) gates
whether the slot is treated as populated. `nicprobe` now prints it,
plus (when link is down) an explicit "PHY Reset Asserted - expected"
note referencing the `STATUS.PHYRA` bit, so a future real-hardware run
doesn't have to re-derive that explanation from raw hex.

Still entirely read-only — no register writes, no state change to the
controller at all. This is the last free read-only checkpoint before
the next milestone has to start writing to hardware registers (a
reset, at minimum) to make any further progress.

## Build & run

```bash
sudo apt install -y build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3
make run-net
```

At the shell: `nicprobe`.

## Verified

QEMU-tested with `make run-net -device e1000,...,mac=52:54:00:12:34:56`
(explicit MAC so the readback could be checked byte-for-byte): `nicprobe`
printed `MAC address: 52:54:00:12:34:56` — exact match, confirming the
`RAL0`/`RAH0` byte-order decoding is correct. Re-ran with the default
(link-up) `make run-net` too: `Link UP, full duplex, 1000Mb/s` still
prints correctly (the new PHYRA-explanation branch only fires when link
is down, so this exercises the untouched link-up path). `heapstats`
afterward still shows the pristine baseline (`Free blocks: 1, Free
bytes: 4194272 / 4194304`) — no regression.

**Not yet tested on the ProBook 6450b.** Expect the MAC address printed
to be this NIC's real, factory-assigned MAC (first 3 bytes an Intel OUI
in the `00:xx:xx` / vendor-specific range) — a link-down PHYRA note is
still expected on this milestone, not a new problem.

## Known limitations of this milestone

- **Still no chip programming.** No reset, no Set-Link-Up, no TX/RX
  descriptor rings.
- **Only reads RAL/RAH slot 0.** The other 15 slots (used for multiple
  MAC addresses / multicast filtering on real OSes) are irrelevant
  until this project has an actual RX path to filter for.

## Suggested next milestone

1. **Run `nicprobe` on the ProBook 6450b** — confirms the MAC address
   read also transfers cleanly to real hardware, and gives the real MAC
   for later use.
2. **Register-level bring-up** — the first state-changing milestone in
   this series: global reset (`CTRL.RST`, bit 26, self-clearing),
   Set-Link-Up (`CTRL.SLU`, bit 6 — already confirmed asserted by
   default in milestone 2's real-hardware read, interestingly, despite
   link being down), and waiting for `STATUS.LU` to actually assert.
   Given this project's real-hardware history, treat this as its own
   careful step, not something to rush through in one shot.
3. **Raw frame TX/RX** via descriptor rings once link is confirmed up.
4. **A trivial custom transfer protocol** over raw frames — still the
   fastest path to solving the original file-transfer problem.
