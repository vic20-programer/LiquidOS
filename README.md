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
1.go to the folder containing the makefile and run "make usb"
2.then cd into the build folder
3. Flash the usb.img image in the folder onto your usb flash drive
4. Once it is flashed boot off the flash drive on your computer
5. If you want to install LiquidOS to the internal storage drive follow these steps again but flash to the drive as (currently) there is no way to install LiquidOS to your internal storage drive without flashing to it directly.

# NEWEST MILESTONE

# LiquidOS — usbmsc real-hardware fix round

A four-part fix round for the ProBook 6450b's unresolved EHCI failure
(the Master Abort that survived 13 diagnostic rounds), produced by
auditing this driver line-by-line against the EHCI 1.0 spec and Linux's
reference handoff code (`drivers/usb/host/pci-quirks.c`, fetched
directly, per this project's don't-work-from-memory convention).

## Fix 1: the USBLEGSUP semaphore bits were SWAPPED (ehci.h)

The big one. `USBLEGSUP_BIOS_OWNED` was defined as bit 24 and
`USBLEGSUP_OS_OWNED` as bit 16 — but the EHCI spec (section 2.1.7) and
Linux both define bit 16 as the BIOS semaphore and bit 24 as the OS
semaphore. Consequence on the ProBook, whose BIOS demonstrably runs USB
keyboard emulation through SMM on this exact controller: the set BIOS
semaphore (bit 16) was read through the wrong mask, `bios_handoff()`
concluded "already OS-owned", NEVER REQUESTED HANDOFF AT ALL, and then
reset a controller the BIOS's SMM code was still actively driving. That
is a recipe for exactly the observed failure: DMA-level chaos at
addresses unrelated to any structure this driver owns, immune to every
place we moved our own structures. Worse, every "handoff outcome"
diagnostic printed through the same swapped constants — the tooling
built to debug this bug was reporting the handshake as fine when it had
never happened. QEMU's emulated EHCI never sets either semaphore, so no
amount of QEMU testing could ever expose this.

## Fix 2: BIOS SMIs were never disabled (ehci.h, pci.h)

Even with the semaphore handshake fixed, Linux's reference handoff does
one more thing this driver never did: it ALWAYS zeroes USBLEGCTLSTS
(the legacy-support control/status dword at capability offset +4),
whose low 16 bits individually enable BIOS SMIs on controller events
(port change, transfer completion, errors, ownership change...). The
comment in Linux is simply "just in case, always disable EHCI SMIs".
On a machine whose BIOS services the keyboard through this controller,
those SMIs are demonstrably live — each one runs BIOS code against a
controller whose schedules this driver has since reset out from under
it. `bios_handoff()` now zeroes it unconditionally, requests handoff
with a single BYTE write to the OS-semaphore byte (matching Linux —
a dword read-modify-write writes the BIOS's own semaphore byte back at
it), and force-clears the BIOS semaphore if the BIOS never releases
within the timeout (also matching Linux). `pci.h` gained
`write_config_byte()` for this.

## Fix 3: overlay priming wrote the "go" signal FIRST (ehci.h)

`control_transfer()`/`bulk_transfer()` primed the queue head by writing
`overlay_next_qtd` (the pointer that hands the qTD chain to hardware)
BEFORE zeroing the token/current/alt fields. A real controller
traverses the one-entry async ring continuously and can fetch the chain
the instant that first write lands — then have its live status
writeback clobbered by the CPU's remaining resets. EHCI 1.0 section
4.10.2 is explicit that the Next-qTD-Pointer write is the single
committing store, made LAST. QEMU only advances its schedule in
discrete bottom-half steps long after all CPU writes land, so this race
was invisible under emulation. Both functions now commit last.
Also new in this round: `usb::enumerate()` waits out the USB 2.0
section 9.2.6.3 SET_ADDRESS recovery time (devices get 2ms to start
answering at the new address; QEMU's switch instantly, real ones
don't have to), and `bring_up()` clears the PCI Status register's
latched W1C error bits so a Master Abort found by a later diagnostic
is guaranteed to be fresh evidence, not BIOS-era leftovers.

## Fix 4: hub support — the flash drive was never on the root port (hub.h, new)

The ProBook's 5 Series/Ibex Peak PCH has NO UHCI companion
controllers: every physical port hangs off an internal Rate Matching
Hub (Intel 8087:0020, a real USB 2.0 hub baked into the chipset),
permanently attached to each EHCI controller's root port. So even with
every DMA problem fixed, enumerating the root port can only ever find
the RMH itself — and `usbmsc` would end at "no bulk IN/OUT pair". New
`kernel/hub.h` implements just enough of USB 2.0 chapter 11 (hub class
descriptor, port power, hub-timed port reset, port status) to descend
through hub(s) to the first device with a bulk pair, skipping ports
with non-storage devices (the RMH is shared with things like internal
webcams). `usbenum`, `usbmsc`, and `mount usb` all descend
automatically; a directly-attached device (QEMU) passes through
unchanged.

## Also fixed: usbmsc's write test corrupted LiquidFS (kernel.cpp)

The WRITE(10) round-trip test wrote its A-Z pattern to LBA 1 assuming
scratch space — but LBA 1 is LiquidFS's directory table
(fs.h `DIR_START_LBA`). Running `usbmsc` on a LiquidFS-formatted stick
quietly destroyed the filesystem `mount usb` was about to need
(reproduced under QEMU: `ls` after `usbmsc` showed one garbage
`ABCDEFGH...` entry). The test now saves the sector first and restores
it after, and warns if the restore itself fails.

## Round 2 (after the first ProBook test): ports were scanned before connect status could exist (ehci.h, hub.h)

First real-hardware run of the fixes above: `usbenum`/`usbmsc` reported
"No enabled (high-speed) device found on any port" on both EHCI
controllers, and `mount usb` found nothing. Telling detail: in all 13
PREVIOUS rounds ports DID connect and enable — the failures came later,
during transfers. The one bring-up-stage behavior this round changed is
the handoff: before, the BIOS SMM stayed alive (fix 1's bug) and kept
re-initializing ports behind the driver's back, masking that this
driver scans PORTSC within microseconds of resetting the controller.
`reset_controller()` wipes CONFIGFLAG, so ports aren't even routed to
EHCI again until `start_controller()` re-sets it — and real silicon
then needs time before connect detection re-reports an attached device
(QEMU asserts it in the same microsecond, so this was invisible under
emulation, again). With SMM genuinely dead, LiquidOS finally saw the
raw timing. `bring_up()` now polls up to ~1s for the first port to
show a connection, then waits the USB 2.0 attach-debounce interval
(100ms, TATTDB) before returning — the same settling Linux's hub code
has always done. hub.h's downstream port scan got the same 100ms
debounce after port power. And the "no device found" paths in
`usbenum`/`usbmsc` now dump every port's raw PORTSC, so if it still
fails, the screen distinguishes "never connected" (bit 0 clear) from
"connected but wouldn't enable" (bit 0 set, bit 2 clear) with no
debug build needed.

## Round 3 (after the second ProBook test): the Master Abort's real face — VT-d protected memory (vtd.h, new)

Round 2's settle fix worked: the second ProBook run connected, reset
and enabled a port, brought up the async schedule, and issued the
first real control transfer — which timed out with `USBSTS.
HostSystemError` set, `FRINDEX` frozen, and `PCI Status=0x2290`:
Received Master Abort, GUARANTEED fresh (round 1 added the latched-
status clearing) and post-handoff (the handoff diagnostics finally
tell the truth). Translation: the controller's very first DMA read —
of a qTD at ~0x00122000, ordinary low DRAM — went out and NO ONE
ANSWERED.

Plain DRAM reads from chipset-integrated silicon can't just miss.
Something must be REJECTING device DMA — and this machine class has
exactly one classic suspect: Intel VT-d's protected-memory feature.
On VT-d-capable business laptops (the 6450b is one), the BIOS can
leave the DMA-remapping hardware's Protected Memory Region armed at
OS handoff, blocking ALL device DMA into the protected range as an
anti-DMA-attack measure until an IOMMU-aware OS takes over. Every
blocked read is master-aborted. It would explain all 13 original
rounds in one stroke: the abort followed the driver's structures no
matter where in low memory they moved, because the entire REGION is
protected, not any address in it. Linux disarms this in its IOMMU
init (`iommu_disable_protect_mem_regions()`); nothing in LiquidOS
ever had.

New `kernel/vtd.h`: finds the ACPI RSDP -> RSDT/XSDT -> DMAR table,
walks its DRHD units, and for each one disables firmware-left
translation (GCMD=0) and disarms protected memory (PMEN=0, wait for
the status bit). `ehci::bring_up()` calls it before any controller
DMA. The `[diag] VT-d/DMAR:` line in `usbenum`/`usbmsc` output
reports exactly what was found — on the ProBook, "firmware HAD DMA
blocking armed; disarmed it" would be the thirteen-round mystery's
confession. QEMU can't arm PMR, but `-machine q35 -device
intel-iommu` produces a real DMAR table, and the walker was verified
against it (unit found, correctly reported unarmed, USB stack still
passes end to end).

## Round 4 (after the third ProBook test): VT-d acquitted — closing in on the BIOS's SMM poller (smi.h new, ehci.h, vtd.h, kernel.cpp)

Third ProBook run: `[diag] VT-d/DMAR: no DMAR ACPI table` — VT-d is
off on this machine, protected memory is innocent, and the Master
Abort still stands. With VT-d gone, the suspect list collapses onto
the one party we KNOW is present and armed: the BIOS's SMM USB
stack. Two hard facts place it at the scene. First, this machine
BOOTS from the USB stick — the BIOS's own USB driver is running,
via SMM, right up to the moment the kernel takes over. Second, the
keyboard dies the moment LiquidOS touches USB: that keyboard is
being emulated by the same SMM code through the same controller. A
BIOS like that can service USB from a PERIODIC chipset SMI — no
EHCI-generated SMI needed, so round 1's USBLEGCTLSTS=0 (which only
silences the controller's own SMI sources) never stopped it. An SMM
poller that believes it still owns "its" controller will re-arm
schedules and list pointers over whatever the OS driver set up.

And LiquidOS was inviting exactly that belief: `bios_handoff()`'s
"already OS-owned" path (taken on this machine — its BIOS never sets
the BIOS semaphore) returned WITHOUT ever setting the OS semaphore.
The round-3 failure screen shows it: `OS_OWNED=clear`. The one
architected signal that tells SMM "an OS drives this controller now"
was never sent. Four changes this round:

- **Claim the OS semaphore unconditionally** (ehci.h): even when
  there's no BIOS bit to wait out. Free to do; the failure screens
  now show `OS_OWNED=SET`.
- **New kernel/smi.h**: clears the PCH's GLOBAL legacy-USB SMI
  enables — SMI_EN's LEGACY_USB_EN/LEGACY_USB2_EN bits at
  PMBASE+0x30, found via the LPC bridge at the architecturally fixed
  00:1f.0 (Intel 5 Series datasheet; the documented switch an OS
  throws when assuming direct USB control). Everything else in
  SMI_EN — thermal, power button, TCO — is left strictly alone, and
  the whole thing skips gracefully on non-PCH platforms. The
  `[diag] PCH SMI_EN:` line prints before/after values; on the
  ProBook, "legacy-USB SMIs WERE enabled - now off" would confirm the
  poller was armed all along.
- **vtd.h fallback probe**: "no DMAR table" no longer ends the VT-d
  question — the two well-known register pages of this silicon era
  (0xFED90000/0xFED91000) are probed directly, so even a BIOS that
  hides the table while leaving the engine live gets caught.
- **Failure forensics** (kernel.cpp): `print_enum_failure()` now also
  prints USBCMD, ASYNCLISTADDR (flagged loudly if it no longer points
  at OUR queue head — if SMM hijacked the controller, this is the
  checkmate evidence in one register read), and the PCI Command
  register (did Bus Master Enable survive?).

QEMU-verified on both machine types: default (no DMAR, no LPC — both
modules skip cleanly, full usbmsc/mount pass) and q35+intel-iommu
(DMAR walked, real ICH9 LPC bridge found, SMI_EN read and correctly
left alone, full pass, `OS_OWNED=SET` visible in the diag).

## Round 5 (after the fourth ProBook test): the cache flush that was never there (mmio.h)

Fourth ProBook run, with round 4's forensics on screen: `OS_OWNED=SET`
(the semaphore claim landed), `PCH SMI_EN: legacy-USB SMIs WERE
enabled - now off` (the SMM poller WAS armed all along - disarming it
was necessary and real), and `ASYNCLISTADDR (still OUR queue head)` -
nothing external reprogrammed the controller. And the transfer still
died. With SMM disarmed, VT-d absent, the semaphore set, and the
controller's pointers intact, every external suspect is eliminated -
which finally forced a harder look at our own memory setup. And there
it was.

`mmio::mark_uncacheable()` flips pages to UC and flushes the TLB -
but never flushed the CPU CACHES. The Intel SDM (vol 3, 12.12.4)
makes that flush mandatory when changing a page's memory type, and
on this exact machine the omission is lethal: the DMA structures live
in kernel BSS, which GRUB zeroed through ordinary write-back mappings
at load - so the cache holds stale DIRTY lines, full of ZEROS, for
exactly those addresses. After the switch to UC, every CPU write
bypasses the cache straight to DRAM... but the stale lines stay
valid. When the EHCI's DMA read arrives, the memory controller snoops
the cache first, hits the stale line, and serves the controller ZEROS
instead of the queue head the CPU wrote. The controller reads
horizontal_link=0, walks to physical address 0, chases BIOS-leftover
bytes as if they were queue pointers, and dies with a Master Abort at
some address unrelated to anything this driver owns. Which is: the
thirteen-round signature, verbatim - including why the abort
"followed" the structures to any address in low RAM (all of BSS has
the same dirty-zero-line history) and why QEMU never once reproduced
it (TCG doesn't emulate CPU caches at all - mmio.h's own top comment
warns about exactly this class of bug). The i5-520M's 3MB L3 keeps
the kernel's entire ~100KB resident indefinitely, making the failure
deterministic rather than flaky.

The fix is one instruction: WBINVD (write back and invalidate the
entire cache hierarchy) at the end of mark_uncacheable(), after the
PTE flips. Stale dirty lines get written back (harmlessly - the
structures haven't been initialized yet at that point) and dropped;
from then on DMA snoops find nothing stale and DRAM is the truth.
Also this round: the "[diag] USBSTS right after enabling the async
schedule" line now samples AFTER ~50ms of letting the idle ring run -
the self-linked, transfer-less queue head makes that window a pure
DMA-read test, so on real hardware that single line now separates
"raw schedule fetches die" (HostSystemError=SET) from "fetches fine,
transfer contents wrong" (clear).

## Round 6 (after the fifth ProBook test): the idle ring itself dies — hunting silicon-level differences (ehci.h, pci.h, kernel.cpp)

Fifth ProBook run, WITH the cache flush in place: "USBSTS after ~50ms
of idle async schedule: HostSystemError=SET". That line was designed
to be decisive and it is: during that window the controller does
nothing but repeatedly read ONE self-linked queue head from RAM - no
transfers, no buffers, no USB traffic - and even that read dies. Raw
schedule fetches fail on this machine with SMM disarmed, VT-d absent,
the OS semaphore claimed, ASYNCLISTADDR intact, and caches flushed.

What's left is the register-level state the driver INHERITS instead
of controls. Comparing against the two reference implementations for
this exact chipset (Linux's ehci driver, and coreboot's Ibex Peak
EHCI init - fetched, not recalled) surfaced one real difference:
this driver programmed USBCMD by read-modify-OR, silently inheriting
the silicon's reset defaults - and real Intel EHCI parts power up
with ASYNC SCHEDULE PARK MODE enabled (ASPME bit 11, park count 3),
a schedule-fetch behavior Linux never runs with (it builds USBCMD
from explicit bits, park off) and QEMU doesn't emulate at all, so
the difference was invisible under every emulated test. Changes:

- **USBCMD is now built from zero** (interrupt threshold 8, frame
  list size 1024, Run - park mode OFF), matching Linux exactly.
- **The function is forced to D0** before anything else, via the
  standard PCI Power Management capability (new
  `pci::find_capability()`), with the prior PMCSR kept for
  diagnostics - a function left below full power by firmware can
  answer register reads while its bus-mastering path stays gated.
- **The idle-ring experiment is now airtight**: every W1C USBSTS bit
  is cleared immediately before the schedules are enabled, so a
  HostSystemError seen in the idle window was provably caused by it.
- **Failure dump grew again**: PMCSR-at-bring-up, and the five
  Intel-specific config registers a PCH BIOS programs at POST
  (0x80/0x84/0x88/0xF4/0xFC - coreboot's known-good Ibex Peak values
  are 0x84=0x130c8911, 0x88=0xa0, 0xF4=0x00808588, 0xFC=0x301b1728),
  so HP's BIOS-left values can be compared against a working BIOS's
  on sight.

## Round 7 (after the sixth ProBook test): the real bug — 32-bit qTD/QH layouts on a 64-bit-capable controller (ehci.h, kernel.cpp)

The sixth ProBook run's photographed failure block cleared every
remaining register-level suspect in one shot: PMCSR was already D0,
BusMaster stayed on, ASYNCLISTADDR still pointed at our queue head,
and HP's five Intel POST config registers essentially match
coreboot's known-good values. But one line it printed for the first
time cracked the case:

    HCCPARAMS=0x00036881 (64-bit addressing: yes)

The ProBook's Ibex Peak EHCI is a **64-bit-addressing-capable**
controller — and EHCI 1.0 Appendix B says such a controller *always*
fetches the EXTENDED transfer structures from memory: 52-byte qTDs
and 68-byte queue heads carrying an extra five "buffer pointer high"
dwords each (the upper halves of 64-bit DMA addresses). Setting
CTRLDSSEGMENT to zero does NOT opt software out of this; the spec
offers no way back to the small layouts.

This driver used the plain 32-byte qTD / 48-byte QH. On the ProBook,
every structure fetch therefore ran 20 bytes past the end of our
structs into adjacent kernel BSS, and the controller treated those
garbage bytes as the upper 32 bits of its DMA buffer addresses —
producing memory reads at wild addresses above 4 GiB, which the
chipset answers with exactly the signature seen in every failing
round since round 3: PCI Master Abort, HostSystemError, halted
controller, frozen FRINDEX. QEMU's EHCI model only ever reads the
32-byte layouts even when it advertises 64-bit capability, which is
why ~20 rounds of emulator testing never reproduced any of this.
Linux never hits it because it unconditionally allocates the
extended layouts with the high dwords zeroed (`hw_buf_hi[]` in
drivers/usb/host/ehci.h) on every controller, 64-bit-capable or not.

Round-7 changes, mirroring Linux's approach:

- **`QueueTD` is now the extended Appendix-B layout**: 13 dwords
  (five `buffer_high` dwords appended), padded to 64 bytes so array
  elements stay 64-byte aligned. `zero_qtd()` zeroes the high dwords
  — the load-bearing zeroes that keep all DMA below 4 GiB.
- **`QueueHead` is now the extended layout too**: the transfer
  overlay carries `overlay_buffer_high[5]`, padded to 96 bytes.
  Schedule setup and both overlay-priming sites (control and bulk)
  clear the extended fields before handing the QH to hardware.
- **The failure dump prints the layout in use** ("qTD/QH layout:
  EXTENDED 64-bit (Appendix B) - sizeof 64/96 bytes") so a ProBook
  photo proves which build is running.

Also learned from round 6's screen: the BIOS actively held the
USBLEGSUP semaphore this boot ("BIOS never released it (timed out)")
— the force-clear path did its job, and OS_OWNED ended up SET.

QEMU regression after the change: `usbmsc` full pass + `mount usb` +
`ls` on the pc machine, `usbmsc` full pass on q35 + intel-iommu
(DMAR parsed, LPC bridge probed), and `usb.img` itself boots as an
EHCI stick and mounts. `build/usb.img` rebuilt for the next flash.

## Round 8 (after the seventh ProBook test): DMA fixed — the fight moves up to the hub scan (hub.h, kernel.cpp)

Seventh ProBook run, with the extended structures: "No device with a
bulk IN/OUT pair found". That message is a milestone disguised as an
error — it can only print after the Rate Matching Hub itself FULLY
enumerated (four GET_DESCRIPTORs, SET_ADDRESS, SET_CONFIGURATION, all
with real DMA data stages). The Master Abort / HostSystemError
signature that consumed rounds 3-7 is gone; round 7's extended
qTD/QH layouts were the root cause. What fails now is hub.h's
descent through the RMH to the flash drive — code that has NEVER
executed before this test, because QEMU cannot attach a hub to an
emulated EHCI controller (its usb-hub is full-speed only).

One real bug found by inspection, fixed: a downstream device whose
enumeration failed AFTER its SET_ADDRESS succeeded kept that address
— but the scan handed the SAME address to the next port's device,
putting two devices on one address (bus collisions, both look
broken). On the real machine the RMH fronts internal devices (webcam,
Bluetooth) as well as the flash drive, so one flaky internal device
could poison the stick's enumeration. Addresses are now consumed per
ATTEMPT, not per success.

Robustness: the port's enable/high-speed bits are now re-read AFTER
the post-reset recovery window instead of trusting the status word
captured the instant the reset bit cleared.

Diagnostics (the real payload of this round — the next failure photo
should pinpoint the port): on any hub-scan failure, usbmsc/usbenum
now print the root device's VID:PID, the hub's reported port count,
every port's raw wPortStatus/wPortChange (bit0=connected, bit1=
enabled, bit10=high-speed), which port's downstream enumeration
failed, and that failure's transfer token / timeout verdict. Stale
"enum step" diagnostics from a previous controller's scan are also
cleared so the report can't mislead.

**Pager**: a VGA text console has no scrollback, and the diagnostics
above were scrolling off the top faster than they could be read or
photographed — costing whole debug round-trips. Every shell command's
output now pauses each screenful on a highlighted
`-- more: press any key --` line and waits for a keypress. Typing at
the prompt itself never pages.

**Second real bug, found BECAUSE of the pager test**: exercising the
pager in QEMU with two EHCI controllers (each with a stick) made the
second controller's WRITE round-trip fail. Root cause: this driver
has ONE global queue head, and every USB command loops over ALL
controllers — but moving on to controller B left controller A's
schedules RUNNING and still pointed at that same QH, so two DMA
masters fetched and wrote back the same overlay concurrently. The
ProBook has exactly two EHCI controllers, and whichever one the scan
visits SECOND (potentially the one with the flash drive) hit this
race on every previous round. `enable_async_schedule()` now halts
the previously-active controller (bounded HCHalted wait) before a
new one takes ownership of the shared structures. Verified in QEMU:
the two-controller two-stick config now passes end-to-end on both.

## Round 9 (after the eighth ProBook test): the keyboard is a USB device — auto-advancing pager + handing controllers back to the BIOS (kernel.cpp, keyboard_irq.h, ehci.h, smi.h)

The eighth ProBook run produced the first per-port hub dump — and
solved two mysteries at once. The dump (`hub step: no usable device
on any downstream port`, ports P1..P6 = 0x0100 except P3 = 0x0103)
says: every port powered, exactly one device connected and enabled on
port 3, and that device is FULL-speed (bit10 clear) — which cannot be
the flash drive. It's one of the laptop's internal USB devices, and
together with the user's other observation — *the keyboard stops
working after any USB command* — the picture snapped into focus: the
ProBook's internal keyboard IS a USB device on the Rate Matching Hub,
which the BIOS's SMM code services and presents to the OS as a PS/2
keyboard. Taking over the EHCI controllers (semaphore, reset, SMI
gates) kills that emulation, so: (a) the keyboard dies the moment a
USB command runs, (b) the pager then waits forever for a key that can
never arrive, and (c) the flash drive was never found because it's on
the OTHER controller, whose scan never ran while the output was
stuck. Round-9 changes:

- **The pager auto-advances**: each `-- more --` waits for a key OR
  ~15 seconds (long enough to read/photograph), so a dead keyboard
  can no longer freeze the diagnostics mid-stream.
  (keyboard_irq.h gained a non-blocking `poll_key()`; `read_key()` is
  now just that in a hlt loop.)
- **Controllers are handed BACK to the BIOS when a command is done**
  (`ehci::hand_back_to_bios_all()` + `smi::restore_legacy_usb_smis()`):
  halt + HCRESET each taken controller, restore the BIOS's saved
  USBLEGCTLSTS SMI enables, return the ownership semaphores to
  "BIOS owns it", and re-arm the PCH's legacy-USB SMI gates - HP's
  SMM should then revive the keyboard within a moment. Runs at the
  end of usbup/usbenum/usbmsc, after `mount usb` (keeping only the
  controller the filesystem depends on), after a failed mount, after
  `unmount`, and after boot-time `mount_any()` (which on a
  USB-only-storage machine would otherwise kill the keyboard before
  the first prompt). The next USB command simply takes everything
  over again - verified in QEMU that repeated take/handback cycles
  work, including the "BIOS never released it" force-clear path.
- **Every controller's output block is now labeled**
  (`== EHCI controller at 0:29.7 ==`) so a photo can't be ambiguous
  about which of the ProBook's two EHCIs it shows.

QEMU regression: usbmsc twice in a row (take → hand back → re-take),
mount usb + ls + cat + unmount, dual-EHCI dual-stick, q35 +
intel-iommu twice in a row, and usb.img as the boot stick.

## Round 10: a native USB keyboard driver (usbkbd.h, ehci.h, usb.h, hub.h, msc.h, kernel.cpp)

The ninth ProBook run was the milestone this whole series aimed at:
**the flash drive enumerates and answers SCSI commands on real
hardware.** What remained was the keyboard: it's a USB device the
BIOS's SMM code fakes as PS/2, that emulation dies when the OS takes
the controllers, and round 9's "hand the controllers back" gambit
didn't convince HP's firmware to rebuild it. Depending on any
particular BIOS's SMM behavior is a dead end for an OS meant to run
on arbitrary machines - so LiquidOS now drives USB keyboards itself:

- **Split transactions** (EHCI 1.0 section 4.12): keyboards are full/
  low-speed devices behind the high-speed Rate Matching Hub, reached
  through the hub's Transaction Translator. The EHCI hardware runs
  the actual split protocol; the driver's job is the QH programming -
  real device speed in EPS, TT hub address + port, and the Control
  Endpoint Flag (`ehci::EndpointTarget`, threaded through every
  control-transfer helper). hub.h now enumerates full/low-speed
  downstream devices instead of skipping them.
- **kernel/usbkbd.h**: finds the first HID boot-protocol keyboard
  behind any hub (`hub::find_first_matching()` with a predicate - the
  storage scan is now the same machinery with a bulk-pair predicate),
  puts it in boot protocol, and polls it with HID GET_REPORT control
  transfers (~30ms) from the shell's idle loop - no interrupt-
  endpoint/periodic-schedule machinery needed, and boot-protocol
  devices are required to support it. Decoded keys are translated
  from HID usages to PS/2 set-1 scancodes and injected into the SAME
  ring buffer the keyboard IRQ feeds - shift, arrows, and the line
  editor work unchanged, and a real PS/2 keyboard keeps working in
  parallel.
- **Schedule sharing made safe**: there is still exactly one control
  QH in the system, now shared by a polled keyboard and a mounted USB
  filesystem possibly on DIFFERENT controllers - so every
  `msc::send_command()` re-acquires the schedule for its own
  controller first (`ehci::ensure_schedule()`, with `op_base` recorded
  in `msc::Device`), and every keyboard poll does the same. At any
  instant exactly one controller runs the QH.
- **`usbkbd` shell command**: attaches the driver; once attached, USB
  commands re-attach it automatically when they're done (instead of
  the round-9 BIOS handback, which remains the fallback for machines
  where no USB keyboard is found).

QEMU regression: usbmsc / usbkbd-with-no-keyboard / mount usb / ls on
pc and q35+intel-iommu; boot with a usb-kbd attached doesn't disturb
anything (QEMU can't put a full-speed device on EHCI, so the actual
keyboard path - like the hub path before it - is real-hardware-only).

## Round 11: real sticks say no before they say yes — BOT error recovery (msc.h, usb.h, ehci.h, fs.h, kernel.cpp)

Tenth ProBook run: enumeration finds the flash drive, but TEST UNIT
READY and INQUIRY fail. This is the first time the BULK data path has
spoken on real hardware, and it exposed that the MSC layer had no
error handling at all - because QEMU's emulated stick never errs.
Real flash drives do, by design:

- A real SCSI device queues a **UNIT ATTENTION** condition at power-on
  ("I was reset!") and FAILS the first command(s) on purpose, until a
  REQUEST SENSE reads - and thereby clears - the condition.
- Real sticks take hundreds of milliseconds after enumeration before
  the medium is ready, answering "not ready" meanwhile.
- A real stick's standard way to hard-reject a command is to **STALL
  the bulk endpoint** (BOT section 6.7), after which every transfer
  fails until the host does CLEAR_FEATURE(ENDPOINT_HALT) and resets
  its data-toggle tracking. One rejected command otherwise bricks the
  endpoint for all commands after it - which is exactly the "TEST
  UNIT READY failed AND INQUIRY failed" cascade on the screen.

Round-11 changes, all standard practice in any real initiator:

- **msc.h grew REQUEST SENSE** (parses key/ASC/ASCQ), **stall
  recovery** (detects the Halted bit in the transfer token, clears the
  endpoint halt over the control pipe via usb.h's new
  `clear_endpoint_halt()`, resets the toggle), and **retries**: every
  `send_command()` retries once after an intervening REQUEST SENSE,
  and `test_unit_ready_retry()` polls for up to ~1s the way every OS's
  mount path does. `fs::mount_usb()` runs the spin-up wait before its
  LiquidFS probe reads.
- **Failure forensics**: a failed command now reports which BOT phase
  died (CBW / data / CSW / SCSI status), the raw transfer token
  (STALL vs timeout distinguishable at a glance), the CSW status, and
  the device's own sense data. ehci.h's `bulk_transfer()` records the
  same token/timeout diagnostics `control_transfer()` always did.
- A data-phase STALL still reads the CSW afterward (BOT 6.7.2), so
  the exchange stays in sync instead of the next command's CSW
  answering the previous command's question.

QEMU regression: full usbmsc + mount usb + ls + cat on pc and q35 -
the recovery paths simply never trigger there.

## Round 11 result: SUCCESS on real hardware

Eleventh ProBook run, round-11 image: **everything passes.**
`TEST UNIT READY: ready`, `INQUIRY: vendor="Lexar" product="USB Flash
Drive"`, `READ CAPACITY: 60620800 blocks x 512 bytes`, READ(10)
showing the stick's real boot sector, WRITE(10)+READ(10) round-trip
match - and `[diag] USB keyboard: attached (native driver polling)`.
The mission this whole series started with (usbmsc failing on the
ProBook 6450b while passing in QEMU) is accomplished, through eleven
rounds whose real fixes were: the 64-bit extended qTD/QH layouts
(round 7 - the Master Abort root cause), the shared-QH two-controller
quiesce (round 8), BOT error recovery (round 11), and the native
split-transaction USB keyboard driver (round 10).

One last fix from the success screen itself: on the OTHER controller,
the storage scan had grabbed the internal Bluetooth module - radios
carry bulk endpoints too - and tried SCSI against it. The storage
predicate now also requires the mass-storage interface class (0x08),
so only actual disks get BOT commands.

## Round 12: keyboard and flash drive share one controller — adoption (hub.h, usbkbd.h, kernel.cpp)

Twelfth ProBook run confirmed `mount usb` works on real hardware -
and exposed the last topology wrinkle: the USB keyboard and the flash
drive sit behind the SAME controller's Rate Matching Hub (the success
screen's "address 3" said it all: RMH=1, keyboard=2, stick=3). After
a successful mount, the keyboard re-attach deliberately skipped the
mounted controller (a rescan would reset it and destroy the mount) -
which orphaned the keyboard: not driven natively, and not by the BIOS
either, since the OS holds that controller.

The fix uses what the mount already did: its storage scan fully
enumerated the keyboard on the way to the stick. Every hub scan now
records any boot keyboard it passes (`hub::g_seen_keyboard`), and
`usbkbd::attach()` ADOPTS that already-configured device when it
lives on the do-not-touch controller - no reset, no rescan, just
start polling it. Storage and keyboard then interleave on one
controller through the existing `ehci::ensure_schedule()` handover.

Also: `usbmsc` retries its hub scan once after a 300ms settle - the
same scan that failed on a freshly reset controller succeeded via
`mount usb` seconds later, so one flake shouldn't fail the command.

## Round 13: schedule enables do nothing on a halted controller (ehci.h, kernel.cpp)

The keyboard adoption still failed on hardware ("wouldn't answer"),
and tracing the handshake found a genuine EHCI-layer bug that no QEMU
regression could ever hit: `quiesce_previous_controller()` clears
Run/Stop on whichever controller owned the shared schedule before -
but `enable_async_schedule()` only OR'd the schedule-enable bits back
in, and per EHCI 1.0 section 4.8 those bits do NOTHING while the HC
is halted. So the first cross-controller re-acquire that wasn't
preceded by a fresh `bring_up()` (exactly what `ensure_schedule()`
and the adoption path do) polled a stopped chip until timeout. Every
emulator test only ever re-enabled a controller `bring_up()` had just
started, so Run/Stop was always already set there.

`enable_async_schedule()` now restarts a halted controller (sets
Run/Stop, waits bounded for HCHalted to clear) before enabling the
schedules. Also from the same trace: when a mounted filesystem keeps
a controller, the release path no longer re-arms the PCH's legacy-USB
SMIs - waking the BIOS's SMM poller while this OS still drives a
controller is the exact hazard round 4 shut off.

## Round 14: a halted controller suspends every device on it (usbkbd.h, kernel.cpp)

Adoption still failed on hardware even with round 13's restart fix,
because the failure sat one layer lower still. The re-attach ran its
controller SWEEP first and adoption only as a fallback - and the
sweep's first act (enabling the other controller's schedule) quiesces
the mounted controller for the seconds the sweep takes. A halted EHCI
stops generating SOFs, and per USB 2.0 a device that sees 3ms of bus
idle drops into SUSPEND - so by the time adoption ran, the keyboard
(and the flash drive) were suspended, and restarting the controller
alone does not wake suspended devices (that takes per-port resume
signaling, which this driver doesn't do... yet).

Fix: adoption now runs FIRST. When the keyboard is already known to
live on the mounted controller, it's adopted immediately - the
schedule re-acquire is a no-op right after a mount, no other
controller is touched, nothing halts, nothing suspends. The sweep
remains as the fallback for keyboards on other controllers. The boot
path also stops re-arming the BIOS's legacy-USB SMIs when the boot
mount keeps a controller (the same round-13 guard, previously missed
in kernel_main).

## Build & run

```bash
make run-usb
```

At the shell: `usbmsc`, then `mount usb`, `ls`, `cat usbwelcome.txt`.

To make a real bootable stick: `make usb`, then write **`build/usb.img`**
(NOT `usbdisk.img`, which is a QEMU-only raw LiquidFS image with no
boot sector — flashing it is what a "no partition table" warning from
your imaging tool means) to the stick in raw/dd mode. It boots via the
ISO's hybrid MBR and carries a LiquidFS region `mount usb` finds
automatically.

## Verified

QEMU-tested end to end: `usbmsc` passes every stage (TEST UNIT READY,
INQUIRY, READ CAPACITY, READ(10), WRITE(10)+READ(10) round-trip,
sector restored), then `mount usb` + `ls` + `cat usbwelcome.txt` on the
SAME boot proves the filesystem now survives the write test. `usbenum`
unchanged for a direct-attached device. `heapstats` still shows the
pristine baseline (`Free blocks: 1, Free bytes: 4194272 / 4194304`).

Round 2 re-verified the full QEMU suite (usbmsc all stages, mount usb +
ls + cat after it, usb.img booting AS a USB stick with `mount usb`
working against the boot stick itself) with the settle waits in place.

**ProBook 6450b status:** round 1 booted but found no enabled ports
(fixed by round 2's connect settle — confirmed on hardware). Round 2
hit a fresh post-handoff Master Abort. Round 3 acquitted VT-d. Round
4 disarmed the BIOS SMM poller (confirmed armed) and proved nothing
external reprograms the controller. Round 5 fixed a real cache-flush
spec violation, but the idle ring STILL died — round 6 (above) goes
after the inherited register state: park mode off, D0 forced,
airtight idle test, Intel-register dump. Next run: if it still
fails, photograph the whole failure block — the PCI Status line
(is Received Master Abort still the mechanism?), PMCSR, and the
Intel cfg register values are the three things the next diagnosis
needs.

## Known limitations of this milestone

- **The hub path is real-hardware-only.** QEMU's `usb-hub` is a
  full-speed (USB 1.1) device and cannot attach to an EHCI-only bus at
  all ("speed mismatch" — confirmed directly), so hub.h's descent logic
  compiles and is reachable but cannot be exercised under QEMU. It gets
  its first real test on the ProBook.
- **Full/low-speed devices behind the hub are skipped** — reaching them
  needs EHCI split transactions, which stays out of scope (a flash
  drive is high-speed).
- **No hub status-change polling** — one-shot "what's plugged in right
  now", like every other driver here.

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

# MILESTONE: The full USB stack works on real hardware (2026-07-09)

The goal this whole fix series chased is done. On the HP ProBook
6450b - real silicon, real BIOS, real flash drive - LiquidOS now:

- **boots from a USB stick** it built itself (`make usb`),
- **drives the EHCI controllers natively**: BIOS/SMM handoff,
  chipset-level SMI management, the extended 64-bit qTD/QH layouts a
  64-bit-capable controller demands, and clean sharing of one transfer
  queue across two controllers,
- **walks the chipset's Rate Matching Hub** to find devices, including
  full/low-speed ones via hardware split transactions,
- **mounts LiquidFS from the stick** over its own Bulk-Only
  Transport + SCSI stack, with real-device error recovery (unit
  attention, endpoint stalls, spin-up retries) - `mount usb`, `ls`,
  `cat`, `write` all work against the boot stick,
- **takes its keyboard input through its own USB HID driver**
  (`usbkbd`) - polled boot-protocol reports translated into the same
  input path as PS/2, surviving mounts and USB commands, with zero
  dependence on the BIOS's fragile keyboard emulation.

Fourteen real-hardware debug rounds got here, each documented in the
sections above; the decisive fixes were the extended 64-bit EHCI
structures (round 7), the two-controller queue-head quiesce (round 8),
Bulk-Only Transport error recovery (round 11), and the native
split-transaction keyboard driver with same-controller adoption
(rounds 10-14). Every one of them was invisible in QEMU and found only
by testing on the real machine.
