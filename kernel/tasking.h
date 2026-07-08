// tasking.h — cooperative AND preemptive multitasking, unified.
//
// PREVIOUS MILESTONE'S MECHANISM (now replaced, not extended): a
// dedicated switch_to() in assembly, saving/restoring 6 callee-saved
// registers and using `ret` to resume. That mechanism fundamentally
// cannot be triggered from inside a hardware interrupt, because the
// CPU's own interrupt-entry/exit (which uses `iretq`, and which restores
// a FULL register-and-flags frame, not just 6 registers) and switch_to's
// `ret`-based resumption are two different, incompatible disciplines.
// Mixing them — e.g. trying to call switch_to() from inside an interrupt
// handler — leaves you with no correct way to ever get back to `iretq`.
//
// THIS MILESTONE'S MECHANISM: there is only ONE way any task is ever
// saved or resumed, and it's shaped exactly like "this task is paused
// inside an interrupt, about to pop its registers and iretq." A task
// that's voluntarily yielding gets there by executing `int 0x80` — a
// SOFTWARE-triggered interrupt — which lands in the EXACT SAME stub
// (irq_common_stub in isr_stubs.asm) that a hardware timer tick would.
// A task that's being preempted gets there because the timer IRQ
// genuinely interrupted it. Either way, by the time C++ code
// (tasking_maybe_switch, below) gets a chance to decide whether to
// switch tasks, the current task's FULL state already looks identical
// regardless of which path led there — so there's exactly one shape of
// "saved task" to ever reason about, not two that have to be kept
// consistent with each other.
//
// HOW A SWITCH ACTUALLY HAPPENS: tasking_maybe_switch (called from
// irq_common_stub, see isr_stubs.asm) is handed the CURRENT stack
// pointer (pointing at the full saved frame described above). It saves
// that pointer into the current task's struct, picks a different READY
// task, and returns THAT task's previously-saved stack pointer instead.
// The assembly stub then does `mov rsp, <returned pointer>` and falls
// into the normal pop-registers-and-iretq tail — which, because every
// task's saved frame has the identical shape, works correctly no matter
// which task's pointer was returned.

#pragma once
#include "heap.h"
#include "interrupts.h" // for InterruptFrame's exact layout
#include <stdint.h>
#include <stddef.h>

namespace tasking {

constexpr size_t STACK_SIZE = 16 * 1024;
constexpr int MAX_TASKS = 8;

enum class TaskState {
    UNUSED,
    READY,
    RUNNING,
};

using TaskEntryFn = void(*)();

struct Task {
    uint64_t saved_rsp;  // valid when state != RUNNING - where to resume
    void* stack_base;    // heap-allocated stack memory, for cleanup later
    TaskState state;
    int id;
};

inline Task g_tasks[MAX_TASKS];
inline int g_current_task = -1;
inline int g_task_count = 0;

// Set to false to temporarily disable preemption from the timer (e.g.
// while the scheduler itself is doing bookkeeping that must not be
// interrupted mid-update). Cooperative yield() is unaffected by this -
// it's only consulted on TIMER-driven switch attempts.
inline bool g_preemption_enabled = true;

inline void task_entry_wrapper(); // forward decl, defined after make_task

// Storage for each task's real entry point (read by task_entry_wrapper
// once it starts running). Indexed in parallel with g_tasks rather than
// folded into the Task struct, purely so Task itself stays focused on
// scheduler bookkeeping vs. "how a task starts." Declared here, before
// make_task(), since make_task() writes to it.
inline TaskEntryFn g_task_entry_fns[MAX_TASKS];

// Registers the CALLER's currently-running flow as task slot 0. Must be
// called exactly once, before any make_task()/yield() calls, and before
// interrupts are enabled (sti) - see kernel.cpp for ordering.
inline void init_main_task() {
    g_tasks[0].state = TaskState::RUNNING;
    g_tasks[0].id = 0;
    g_tasks[0].stack_base = nullptr;
    g_current_task = 0;
    g_task_count = 1;
}

// Builds a brand-new task by fabricating a stack that LOOKS exactly like
// a task paused inside irq_common_stub, mid-interrupt, about to pop its
// registers and iretq. The very first time this task is switched to,
// nothing can tell the difference between "genuinely resuming" and "this
// was hand-built five minutes ago" - which is the entire point.
//
// Field order must exactly match interrupts::InterruptFrame (see that
// struct's definition) since that's the shape irq_common_stub's pop
// sequence + iretq expects to unwind.
inline bool make_task(TaskEntryFn entry) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) { // slot 0 reserved for the main task
        if (g_tasks[i].state == TaskState::UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;

    void* stack_mem = heap2::alloc(STACK_SIZE);
    if (stack_mem == nullptr) return false;

    uint8_t* stack_top = reinterpret_cast<uint8_t*>(stack_mem) + STACK_SIZE;

    // We need a small landing area for task_entry_wrapper to read the
    // real entry function from - simplest robust approach is the same
    // one used last milestone: store it in the Task struct, but since
    // task_entry_wrapper has no parameters (it's reached via iretq, not
    // a call), it reads g_tasks[g_current_task] same as before. The
    // entry pointer itself is stashed in a small per-task field.
    g_task_entry_fns[slot] = entry;

    // Reserve space for the full InterruptFrame shape. The fabricated
    // values for general-purpose registers are irrelevant (0 is fine) -
    // they'll be overwritten by real values the first time this task
    // actually runs and gets interrupted/yields again. What MATTERS is
    // RIP (must point at task_entry_wrapper), CS/SS (must be valid
    // kernel segment selectors), and RFLAGS (must have interrupts
    // enabled, bit 9, or this task would run with interrupts disabled
    // forever once resumed).
    interrupts::InterruptFrame* frame =
        reinterpret_cast<interrupts::InterruptFrame*>(stack_top) - 1;

    frame->r15 = 0; frame->r14 = 0; frame->r13 = 0; frame->r12 = 0;
    frame->r11 = 0; frame->r10 = 0; frame->r9  = 0; frame->r8  = 0;
    frame->rbp = 0; frame->rdi = 0; frame->rsi = 0; frame->rdx = 0;
    frame->rcx = 0; frame->rbx = 0; frame->rax = 0;
    frame->vector_number = 0;
    frame->error_code = 0;
    frame->rip = reinterpret_cast<uint64_t>(task_entry_wrapper);
    frame->cs = 0x08;       // kernel code segment, matches idt.h's KERNEL_CODE_SELECTOR
    frame->rflags = 0x202;  // bit 1 (reserved, always 1) + bit 9 (IF, interrupts enabled)
    // frame->rsp and frame->ss below are DEAD FIELDS in this kernel: iretq
    // only pops RSP/SS off the stack when the target CS represents a
    // DIFFERENT privilege level than the current one (a real ring
    // transition, e.g. ring0->ring3). Since this kernel has no user mode
    // yet and CS is always 0x08 (ring 0) both before and after, every
    // iretq here is a same-privilege return, which pops only RIP/CS/
    // RFLAGS and leaves RSP wherever it naturally ends up after those
    // three pops. These two fields are filled in anyway, purely so the
    // frame is fully-formed and obviously intentional to read later -
    // not because iretq will ever actually consume them as written.
    frame->rsp = reinterpret_cast<uint64_t>(stack_top);
    frame->ss = 0x00;

    g_tasks[slot].saved_rsp = reinterpret_cast<uint64_t>(frame);
    g_tasks[slot].stack_base = stack_mem;
    g_tasks[slot].state = TaskState::READY;
    g_tasks[slot].id = slot;

    g_task_count++;
    return true;
}

// Called via iretq landing here (RIP was set to this in make_task()), NOT
// via a normal C++ call - there is no caller stack frame. This is why a
// task function returning needs special handling: falling off the end
// of task_entry_wrapper would try to `ret` into garbage, since nothing
// ever `call`ed it in the normal sense.
inline void yield(); // forward declaration, used below and by kernel.cpp

inline void task_entry_wrapper() {
    TaskEntryFn entry = g_task_entry_fns[g_current_task];
    entry();

    // The task function returned. Mark this task UNUSED and switch away
    // from it ONE FINAL TIME via yield() - but unlike every earlier
    // yield in this task's life, this is the last time this stack will
    // ever be read from again. tasking_maybe_switch (below) specifically
    // checks for UNUSED before deciding what to do with "the task we're
    // switching away from": for any READY task that means "mark it READY
    // again so it gets its turn later," but for an UNUSED one it means
    // "this task is done forever - free its stack now, while we're
    // already in the middle of switching to something else's stack and
    // about to stop touching this memory for good."
    //
    // We still need SOMETHING to keep this stack minimally alive long
    // enough for the final int $0x80 instruction and the resulting
    // interrupt entry to finish pushing their frame onto it - freeing
    // happens AFTER that point, from inside tasking_maybe_switch, not
    // here. This call genuinely never returns: there is no scenario
    // where this task gets scheduled again once UNUSED.
    g_tasks[g_current_task].state = TaskState::UNUSED;
    yield();

    // UNREACHABLE. If we ever get here, something is badly wrong with
    // the scheduler (it switched back to a task it should have known
    // was finished) - loop on hlt rather than running off into garbage,
    // since there's nowhere meaningful to go from here.
    while (true) {
        asm volatile("hlt");
    }
}

// Cooperative yield: deliberately triggers the SAME interrupt path a
// timer tick would, via a software interrupt instruction. This is what
// guarantees a cooperatively-yielding task's saved state has the exact
// same shape as a preempted task's — there's only one code path that
// ever saves a task's state at all, and this is how non-timer code
// enters it.
inline void yield() {
    if (g_task_count <= 1) return;
    asm volatile("int $0x80");
}

} // namespace tasking

// ---------------------------------------------------------------------------
// Called from irq_common_stub (isr_stubs.asm) on EVERY timer tick, EVERY
// keyboard interrupt, AND every cooperative yield() (since all three
// share the same stub). current_rsp points at the full saved frame for
// whatever task is currently running. Returns the rsp to actually resume
// — either current_rsp unchanged (no switch happening) or a different
// task's previously-saved rsp (switch happening).
//
// extern "C" with a stable, unmangled name since isr_stubs.asm calls
// this directly by symbol.
// ---------------------------------------------------------------------------
extern "C" uint64_t tasking_maybe_switch(uint64_t current_rsp) {
    using namespace tasking;

    if (g_current_task == -1 || g_task_count <= 1) {
        return current_rsp; // tasking not initialized yet, or nothing to switch to
    }

    auto* frame = reinterpret_cast<interrupts::InterruptFrame*>(current_rsp);
    bool is_timer_tick = (frame->vector_number == 32);
    bool is_cooperative_yield = (frame->vector_number == 0x80);
    bool previous_task_finished = (g_tasks[g_current_task].state == TaskState::UNUSED);

    // previous_task_finished is checked here too purely as a defensive
    // belt-and-suspenders measure - in practice a finished task always
    // reaches this point via its own final cooperative yield() (see
    // task_entry_wrapper), never via a timer tick, so is_timer_tick would
    // normally be false for it anyway. Kept explicit so this condition
    // can never accidentally strand a finished task if that assumption
    // ever changes.
    if (is_timer_tick && !g_preemption_enabled && !previous_task_finished) {
        return current_rsp; // preemption deliberately paused - resume as-is
    }
    if (!is_timer_tick && !is_cooperative_yield) {
        return current_rsp; // some other IRQ (keyboard) - never switches tasks
    }

    int previous = g_current_task;
    int next = previous;
    for (int attempts = 0; attempts < MAX_TASKS; attempts++) {
        next = (next + 1) % MAX_TASKS;
        if (g_tasks[next].state == TaskState::READY) break;
    }

    bool found_a_ready_task = (next != previous && g_tasks[next].state == TaskState::READY);

    if (!found_a_ready_task) {
        if (previous_task_finished) {
            // Nothing else is ready to run, but the task we'd be
            // "resuming" (because there's nowhere else to go) is the
            // finished one itself - that would mean executing code on a
            // stack we're about to free, or worse, falling through to
            // task_entry_wrapper's unreachable hlt loop. This shouldn't
            // happen in practice (init_main_task's slot 0 is never
            // marked UNUSED, so there's always at least one other valid
            // task to fall back to) - but if it ever did, halting with
            // an honest diagnostic is far better than silently resuming
            // freed/finished memory.
            while (true) asm volatile("hlt");
        }
        return current_rsp; // no switch needed - resume the same task
    }

    // Free the FINISHED task's stack now, if that's what we're switching
    // away from. This is the one safe moment to do it: we are no longer
    // going to read or write that stack's memory (we're about to move rsp
    // to `next`'s saved stack entirely), and this task will never be
    // resumed again since it's leaving the READY rotation for good.
    if (previous_task_finished) {
        if (g_tasks[previous].stack_base != nullptr) {
            heap2::free_ptr(g_tasks[previous].stack_base);
            g_tasks[previous].stack_base = nullptr;
        }
        g_task_count--;
        // Deliberately do NOT set g_tasks[previous].state = READY below -
        // it must stay UNUSED so make_task() can reuse this slot later,
        // and so this scheduler never tries to resume it again. Also
        // deliberately skip saving current_rsp into saved_rsp - that
        // memory is being freed and must never be treated as a valid
        // resume point again.
    } else {
        g_tasks[previous].saved_rsp = current_rsp;
        g_tasks[previous].state = TaskState::READY;
    }

    g_tasks[next].state = TaskState::RUNNING;
    g_current_task = next;

    return g_tasks[next].saved_rsp;
}
