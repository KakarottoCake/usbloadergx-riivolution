/* Thumb-1 LOW_READ hook. Assembled separately; never built as PPC code.
 * Placed over a verified dead reader body, which MODE_FRAG never reaches.
 * Its original entry becomes an error stub; the LOW_READ case calls entry + 8.
 *
 * Entry contract, taken from the console's own handler (dipp-93800a1c.asm):
 *   r0 = command struct ([r0,#4] = length in bytes, [r0,#8] = offset in words)
 *   r4 = config ([r4,#0] = mode, [+8]/[+12] = drive offsets, [+16] = error word)
 *   r7 = outbuf. r5/r6 are preserved for the stock paths. r1-r3 are scratch:
 *   the stock paths reload them, so nothing downstream can tell we ran.
 * The four bytes overwritten at the handler (ldr r3,[r4,#0] / lsls r2,r3,#30)
 * are replayed on the pass-through path, so a non-mod read executes its exact
 * original instruction stream from there on.
 *
 * All external addresses arrive as patched words or patched calls, so the IOS
 * virtual to physical mapping never has to be guessed. Word offsets never
 * inherit drive offsets: the two config words are added exactly as the stock
 * read worker adds them.
 */
.syntax unified
.cpu arm926ej-s
.thumb
.section .text
.balign 4
.global redirect_start, redirect_entry, redirect_end
.global redirect_frag_call, redirect_epilogue, redirect_limit
redirect_start:
    movs r0, #0xa0
    lsls r0, r0, #8          /* error return, matches the stock worker */
    bx lr
    nop
redirect_entry:
    push {r0, r3, r4, r6, r7, lr}
    ldr r2, [r0, #8]         /* word offset (inbuf[2]) */
    movs r3, #0xc0
    lsls r3, r3, #23         /* 0x60000000 words = 6 GiB */
    cmp r2, r3
    blo pass
    ldr r3, redirect_limit   /* patched: exclusive end, word units */
    cmp r2, r3
    bhs range_error
    subs r3, r3, r2          /* words remaining; no borrow, offset is below */
    lsls r3, r3, #2          /* bytes remaining, <= 2 GiB, cannot overflow */
    ldr r1, [r0, #4]         /* length in bytes */
    cmp r1, r3
    bhi range_error
    ldr r3, [r4, #0]
    movs r5, #16
    tst r3, r5               /* no fraglist to read without MODE_FRAG */
    beq range_error
    ldr r3, [r4, #8]
    ldr r5, [r4, #12]
    adds r3, r3, r5
    adds r2, r2, r3          /* config offsets, as the stock worker adds them */
    movs r0, r7              /* outbuf; r1 still holds the length */
redirect_frag_call:
    bl redirect_start      /* relocated to the frag-mode chunked reader */
    cmp r0, #0
    bne read_error
    movs r5, r0              /* 0: full transfer, the only success shape */
    b finish
pass:
    pop {r0, r3, r4, r6, r7}
    add sp, #4               /* drop the saved lr slot; lr still holds it */
    ldr r3, [r4, #0]         /* replay of the overwritten pair */
    lsls r2, r3, #30
    bx lr                    /* back to the branch: flags decide, as stock */
range_error:
    ldr r3, block_range
    b store_error
read_error:
    ldr r3, unrecovered_read
store_error:
    str r3, [r4, #16]
    movs r5, #0xa0
    lsls r5, r5, #8          /* error return, matches the stock worker */
finish:
    pop {r0, r3, r4, r6, r7}
    add sp, #4
    ldr r3, redirect_epilogue /* patched: the dispatcher's return sequence */
    bx r3
.balign 4
redirect_limit:
    .word 0                  /* exclusive, padded end in word units */
redirect_epilogue:
    .word 0                  /* the add-sp / mov-r0 / pop sequence address */
block_range:
    .word 0x00052100
unrecovered_read:
    .word 0x00031100
redirect_end:
