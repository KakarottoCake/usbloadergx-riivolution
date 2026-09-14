/* Thumb-1 LOW_READ hook for the on-demand design.
 *
 * The shipped hook (redirect.S) answers a read out of d2x's fragment list,
 * which means every mod file had to be resolved to raw sectors before the game
 * started. This one calls a module the loader placed in reserved MEM2, which
 * opens the file by path when the game asks for it. The asm is thinner as a
 * result: it does no range test of its own, because the module returns MISS
 * for anything outside the mod region and that is the same decision made once
 * instead of twice.
 *
 * Entry contract, unchanged from redirect.S and taken from the console's own
 * handler (dipp-93800a1c.asm):
 *   r0 = command struct ([r0,#4] = length in bytes, [r0,#8] = offset in words)
 *   r4 = config ([r4,#0] = mode, [+8]/[+12] = drive offsets, [+16] = error word)
 *   r7 = outbuf. r5/r6 are preserved for the stock paths. r1-r3 are scratch.
 * The four bytes overwritten at the handler (ldr r3,[r4,#0] / lsls r2,r3,#30)
 * are replayed on the pass-through path, so a non-mod read executes its exact
 * original instruction stream from there on.
 *
 * One difference matters and is easy to get wrong: redirect.S reaches its
 * pass-through path BEFORE issuing any call, so lr still holds the return into
 * the handler. Here the call has already happened by the time MISS comes back,
 * so lr has been clobbered and has to be restored off the stack. Returning
 * with the wrong lr lands somewhere inside this routine and loops.
 *
 * Execution publication: the module is PPC-written into a reservation whose
 * address varies per boot, then executed here on Starlet - whose instruction
 * cache persists across game boots (IOS keeps running d2x throughout) and
 * may still hold the last occupant's lines for these addresses. Same
 * reservation address plus different relocated bytes (another loader build,
 * another boot) would otherwise execute stale code with silence. So the
 * first hook hit of each boot publishes the module: an epoch word in module
 * BSS (fixed first word, installer-written per boot and flushed) is compared
 * against the expected boot epoch (second literal); on mismatch the stub
 * stamps it and runs a whole-I-cache invalidate on an ARM island below -
 * once per boot, before the first module fetch, including the module's own
 * prologue, which a later invalidate could not retroactively fix. An epoch
 * (not a boolean) because a cleared-or-stale zero must never read as
 * "already published": only an exact epoch match skips, and every mismatch
 * converges to invalidating, which is always the safe direction.
 * The mcr needs no new privilege: the module already executes data-cache
 * maintenance in this exact context on every init and every armed read.
 * And a fault here (rather than mid-gameplay) can only land inside the
 * pre-launch ack probe, which withholds the boot instead of corrupting it.
 *
 * The module's three arguments are (word offset, length in bytes, destination)
 * and it returns 0 for served, 1 for not ours, negative for failed. It masks
 * the destination to a physical address itself - see riivo_ios.h - because
 * that is where the copy happens.
 */
.syntax unified
.cpu arm926ej-s
.thumb
.section .text
.balign 4
.global redirect2_start, redirect2_entry, redirect2_end
.global redirect2_call, redirect2_epilogue, redirect2_flag
redirect2_start:
    movs r0, #0xa0
    lsls r0, r0, #8          /* error return, matches the stock worker */
    bx lr
    nop
redirect2_entry:
    /* The worker lr is saved BEFORE any branch-and-link: bl clobbers lr,
     * and restoring the island's return address as the worker's would loop
     * inside this routine (the documented redirect.S hazard). */
    push {r0, r3, r4, r6, r7, lr}
    ldr r1, flag_addr       /* BSS epoch word (installer-patched) */
    ldr r2, [r1]
    ldr r3, epoch_val       /* expected boot epoch (installer-patched) */
    cmp r2, r3
    beq published           /* exact match: published already this boot */
    str r3, [r1]            /* stamp BEFORE invalidating: both orders safe */
    bl iinv_island          /* publish the module's bytes to the I-cache */
published:
    ldr r1, [r0, #4]        /* length in bytes */
    ldr r2, [r0, #8]         /* word offset (inbuf[2]) */
    ldr r3, [r4, #8]
    ldr r0, [r4, #12]
    adds r3, r3, r0
    adds r2, r2, r3          /* config offsets, as the stock worker adds them */
    movs r0, r2              /* arg0 = word offset */
    movs r2, r7              /* arg2 = destination, in the PPC's view */
redirect2_call:
    bl redirect2_start       /* relocated to the module's riivo_di_read */
    cmp r0, #1
    beq pass                 /* not ours: run the stock read unchanged */
    cmp r0, #0
    bne read_error
    movs r5, r0              /* 0: full transfer, the only success shape */
    b finish
pass:
    pop {r0, r3, r4, r6, r7}
    pop {r1}                 /* the saved lr; r1 is scratch by contract */
    mov lr, r1
    ldr r3, [r4, #0]         /* replay of the overwritten pair */
    lsls r2, r3, #30
    bx lr                    /* back to the branch: flags decide, as stock */
read_error:
    ldr r3, unrecovered_read
    str r3, [r4, #16]
    movs r5, #0xa0
    lsls r5, r5, #8          /* error return, matches the stock worker */
finish:
    pop {r0, r3, r4, r6, r7}
    add sp, #4
    ldr r3, redirect2_epilogue /* patched: the dispatcher's return sequence */
    bx r3
/* ARM island: whole-I-cache invalidate plus drain, then back to Thumb.
 * Entered by bl, so lr carries the Thumb bit and bx lr interworks home.
 * r1/r2 are scratch (the flag address and value are dead past the store);
 * r0/r3/r4/r7 and the flags the body needs are reloaded fresh below, and
 * the worker lr already sits on the stack. */
    .balign 4
    .arm
iinv_island:
    mov r2, #0
    mcr p15, 0, r2, c7, c5, 0
    mcr p15, 0, r2, c7, c10, 4
    bx lr
    .thumb
    .balign 4
flag_addr:
    .global redirect2_flag
redirect2_flag:
    .word 0                  /* patched: ARM-view address of the epoch word */
epoch_val:
    .global redirect2_epoch
redirect2_epoch:
    .word 0                  /* patched: expected boot epoch (nonzero) */
redirect2_epilogue:
    .word 0                  /* the add-sp / mov-r0 / pop sequence address */
unrecovered_read:
    .word 0x00031100
redirect2_end:
