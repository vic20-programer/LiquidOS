; boot.asm — Multiboot2 entry point, 32-bit -> long mode (64-bit) -> jump to kernel
BITS 32

section .multiboot_header
header_start:
    dd 0xe85250d6
    dd 0
    dd header_end - header_start
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))
    dw 0
    dw 0
    dd 8
header_end:

; Exported so kernel C++ code can patch individual page table entries at
; runtime (mark a specific PCI BAR's page uncacheable - see mmio.h's
; mark_uncacheable()) - a PCI BAR's physical address isn't known until PCI
; enumeration runs, well after this file has already built the initial
; identity map, so this can only ever be a runtime patch, not something
; set up here at boot.
global p2_table_0
global p2_table_1
global p2_table_2
global p2_table_3

section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
; 4 p2 tables now instead of 1 - each maps 512 * 2MiB = 1GiB, so 4 of
; them (one per p3_table entry) identity-map the full 4GiB 32-bit
; physical address space instead of just the first 1GiB. Needed for the
; PCI-enumeration milestone's follow-up: a PCI BAR's physical address
; (e.g. an EHCI controller's MMIO registers) can legally be anywhere in
; that 4GiB range - commonly near the TOP of it, well past the 1GiB the
; original single p2_table covered - and this kernel has no on-demand
; page-table-editing mechanism, so whatever isn't identity-mapped here
; at boot simply isn't reachable at all.
p2_table_0:
    resb 4096
p2_table_1:
    resb 4096
p2_table_2:
    resb 4096
p2_table_3:
    resb 4096
stack_bottom:
    resb 16384
stack_top:

section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
BITS 32
global start
extern long_mode_start

start:
    mov esp, stack_top
    mov edi, ebx

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]

    jmp gdt64.code:long_mode_start

    hlt

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp error

set_up_page_tables:
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax

    ; p3_table's 4 entries each point at one of the 4 p2 tables below -
    ; together they identity-map the full 4GiB 32-bit physical address
    ; space (each p3 entry covers 1GiB via its p2 table's 512 * 2MiB
    ; huge pages), instead of just the first 1GiB the original single
    ; p2_table covered. See the .bss comment above for why.
    mov eax, p2_table_0
    or eax, 0b11
    mov [p3_table], eax
    mov eax, p2_table_1
    or eax, 0b11
    mov [p3_table + 8], eax
    mov eax, p2_table_2
    or eax, 0b11
    mov [p3_table + 16], eax
    mov eax, p2_table_3
    or eax, 0b11
    mov [p3_table + 24], eax

    mov esi, 0                ; table_index: which 1GiB region (0..3)
.map_p2_table_outer:
    mov eax, esi
    shl eax, 30                ; ebx = table_index * 1GiB - this table's physical base
    mov ebx, eax

    mov edi, p2_table_0
    cmp esi, 0
    je .have_table
    mov edi, p2_table_1
    cmp esi, 1
    je .have_table
    mov edi, p2_table_2
    cmp esi, 2
    je .have_table
    mov edi, p2_table_3
.have_table:

    mov ecx, 0                 ; entry_index: which 2MiB page within this 1GiB (0..511)
.map_p2_table_inner:
    mov eax, ecx
    shl eax, 21                  ; entry_index * 2MiB
    add eax, ebx                 ; + this table's 1GiB base
    or eax, 0b10000011           ; present, writable, huge page (PS bit)
    mov [edi + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_table_inner

    inc esi
    cmp esi, 4
    jne .map_p2_table_outer

    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ret

error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte  [0xb800a], al
    hlt

BITS 64
section .text
long_mode_start:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top
    mov rdi, rdi

    extern kmain
    call kmain

.hang:
    hlt
    jmp .hang
