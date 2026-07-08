CXX := g++
AS  := nasm
LD  := ld

BUILD_DIR := build
ISO_DIR   := $(BUILD_DIR)/isofiles

CXXFLAGS := -m64 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
            -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
            -fno-pic -fno-pie -Wall -Wextra -O2 -std=c++17

ASFLAGS := -f elf64

LDFLAGS := -n -T boot/linker.ld -nostdlib

KERNEL_BIN := $(BUILD_DIR)/kernel.bin
ISO_FILE   := $(BUILD_DIR)/myos.iso
DISK_IMG   := $(BUILD_DIR)/disk.img
DISK2_IMG  := $(BUILD_DIR)/disk2.img
USB_IMG    := $(BUILD_DIR)/usb.img
USBDISK_IMG:= $(BUILD_DIR)/usbdisk.img

# MUST match kernel/fs.h's COMBINED_IMAGE_PARTITION_LBA exactly (65536
# sectors * 512 bytes/sector) - see the $(USB_IMG) recipe below.
BOOT_REGION_BYTES := 33554432

OBJS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/kernel.o

KERNEL_HEADERS := kernel/keyboard.h kernel/allocator.h kernel/heap.h kernel/strutil.h \
                   kernel/idt.h kernel/idt_init.h kernel/interrupts.h \
                   kernel/pic.h kernel/pit.h kernel/irq.h kernel/keyboard_irq.h \
                   kernel/cursor.h kernel/tasking.h kernel/ata.h kernel/fs.h kernel/pci.h kernel/mmio.h kernel/ehci.h kernel/usb.h kernel/msc.h

DISK_FILES  := $(wildcard diskfiles/*)
DISK2_FILES := $(wildcard diskfiles2/*)
DISK3_FILES := $(wildcard diskfiles3/*)

.PHONY: all run usb run-usb run-net clean

all: $(ISO_FILE) $(DISK_IMG) $(DISK2_IMG)

$(BUILD_DIR)/boot.o: boot/boot.asm
	@mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/isr_stubs.o: kernel/isr_stubs.asm
	@mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: kernel/kernel.cpp $(KERNEL_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJS) boot/linker.ld
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

$(ISO_FILE): $(KERNEL_BIN) grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR) 2>&1

# The LiquidFS data disks: two separate drives from the GRUB boot ISO,
# each built fresh from its diskfiles*/ directory whenever a file in
# there changes. disk.img is the one the kernel auto-mounts at boot;
# disk2.img exists purely so the storage-device-switching milestone has
# a genuinely different disk to `mount` at runtime.
$(DISK_IMG): tools/mkfs.py $(DISK_FILES)
	@mkdir -p $(BUILD_DIR)
	python3 tools/mkfs.py diskfiles $(DISK_IMG)

$(DISK2_IMG): tools/mkfs.py $(DISK2_FILES)
	@mkdir -p $(BUILD_DIR)
	python3 tools/mkfs.py diskfiles2 $(DISK2_IMG)

# usbdisk.img: a THIRD, separate LiquidFS image - not attached as an
# IDE drive at all, but as a USB mass-storage device (see run-usb,
# below), for testing fs.h's `mount usb` and mount_any()'s USB fallback
# without needing real USB hardware.
$(USBDISK_IMG): tools/mkfs.py $(DISK3_FILES)
	@mkdir -p $(BUILD_DIR)
	python3 tools/mkfs.py diskfiles3 $(USBDISK_IMG)

# -cdrom is the boot device; QEMU always attaches it at the secondary
# IDE channel's MASTER position (index=2), regardless of what else is
# attached. index=0 pins disk.img to the PRIMARY master (0x1F0, drive
# select 0xE0) — the exact bus ata.h's PIO driver reads from by default.
# index=3 pins disk2.img to the SECONDARY SLAVE position (0x170, drive
# select 0xF0) — the one still-free slot, and the one that exercises
# BOTH a different I/O port base and the slave select bit at once, so
# `mount 3` is a meaningful test of ata.h's drive-switching, not just a
# second copy of the same address. format=raw because both images ARE
# the raw sector layout fs.h expects, byte for byte, no container
# format wrapping either.
run: $(ISO_FILE) $(DISK_IMG) $(DISK2_IMG)
	qemu-system-x86_64 -cdrom $(ISO_FILE) \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-drive file=$(DISK2_IMG),format=raw,if=ide,index=3 \
		-serial stdio

# run-usb: everything `run` does, PLUS an EHCI controller with a
# LiquidFS-formatted USB mass-storage device (usbdisk.img, built from
# diskfiles3/) attached - the one-command way to test `mount usb` and
# mount_any()'s USB fallback (try it with disk.img/disk2.img's
# `-drive` lines removed entirely, to see the USB-only-storage boot
# path the ProBook 6450b this series has been motivated by would hit).
# Kept as a separate target from `run` so the default, fastest test
# loop doesn't pay for USB controller emulation when nothing's testing
# it.
run-usb: $(ISO_FILE) $(DISK_IMG) $(DISK2_IMG) $(USBDISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO_FILE) \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-drive file=$(DISK2_IMG),format=raw,if=ide,index=3 \
		-device usb-ehci,id=ehci \
		-drive if=none,id=usbstick,file=$(USBDISK_IMG),format=raw \
		-device usb-storage,drive=usbstick,bus=ehci.0 \
		-serial stdio

# run-net: everything `run` does, PLUS an emulated Intel e1000 NIC - the
# one-command way to test `lspci`'s Ethernet-controller detection (this
# milestone) and, later, an actual NIC driver. e1000 chosen because it's
# extremely well-supported by QEMU and a genuinely common real chip
# family too, unlike some of QEMU's more toy-only NIC models.
run-net: $(ISO_FILE) $(DISK_IMG) $(DISK2_IMG)
	qemu-system-x86_64 -cdrom $(ISO_FILE) \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-drive file=$(DISK2_IMG),format=raw,if=ide,index=3 \
		-netdev user,id=n0 -device e1000,netdev=n0 \
		-serial stdio

# usb.img: ONE image combining the GRUB-bootable ISO (padded out to a
# fixed BOOT_REGION_BYTES) with a LiquidFS region (disk.img, reused
# as-is) appended right after it — meant to be written to a real USB
# stick so a single drive is both bootable AND holds LiquidFS data,
# for real hardware with no second physical drive to attach. Whether
# the kernel can actually READ that LiquidFS region back once booted
# depends on the machine's BIOS keeping the USB stick reachable through
# the same legacy-IDE ports past boot — NOT guaranteed on all hardware,
# see the storage-device-switching milestone's README. QEMU's `run`
# target above is unaffected; it keeps testing with two separately
# `-drive`-attached images, which QEMU always makes reachable.
$(USB_IMG): $(ISO_FILE) $(DISK_IMG)
	@mkdir -p $(BUILD_DIR)
	@size=$$(stat -c%s $(ISO_FILE)); \
	if [ $$size -gt $(BOOT_REGION_BYTES) ]; then \
		echo "error: $(ISO_FILE) is $$size bytes, exceeds BOOT_REGION_BYTES ($(BOOT_REGION_BYTES)) - bump both this and fs.h's COMBINED_IMAGE_PARTITION_LBA"; \
		exit 1; \
	fi
	cp $(ISO_FILE) $(USB_IMG)
	truncate -s $(BOOT_REGION_BYTES) $(USB_IMG)
	cat $(DISK_IMG) >> $(USB_IMG)

usb: $(USB_IMG)
	@echo "Wrote $(USB_IMG) - write it to a USB stick in DD/raw mode (NOT 'ISO mode' - that discards the boot structure), e.g.:"
	@echo "  sudo dd if=$(USB_IMG) of=/dev/sdX bs=4M status=progress && sync"

clean:
	rm -rf $(BUILD_DIR)
