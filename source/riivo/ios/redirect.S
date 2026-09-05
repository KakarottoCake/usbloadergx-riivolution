/* Thumb-1 LOW_READ hook. Assembled separately; never built as PPC code.
 * Placed over a verified DI_ReadDvd body, which MODE_FRAG does not use.
 * Its original entry becomes an error stub; the dispatch calls entry + 8.
 * r5 = dipConfig, r6 = command, r7 = output, [sp] = output capacity.
 * All external calls are patched as PC-relative BLs, so the IOS virtual to
 * physical mapping does not have to be guessed.
 */
.syntax unified
.cpu arm926ej-s
.thumb
.section .text
.balign 4
.global redirect_start, redirect_entry, redirect_end
.global redirect_frag_call, redirect_stock_call, redirect_raw_call
.global redirect_limit
redirect_start:
    movs r0, #0xa0
    lsls r0, r0, #8
    bx lr
    nop
redirect_entry:
    push {r4, lr}
    ldr r2, [r6, #8]
    movs r3, #0xc0
    lsls r3, r3, #23         /* 0x60000000 words = 6 GiB */
    cmp r2, r3
    blo stock
    ldr r3, redirect_limit
    cmp r2, r3
    bhs range_error
    subs r3, r3, r2
    lsls r3, r3, #2         /* remaining bytes, <= 2 GiB */
    ldr r1, [r6, #4]
    cmp r1, r3
    bhi range_error
    ldr r3, [sp, #8]        /* caller's output capacity */
    cmp r1, r3
    bhi range_error
    movs r3, #31
    tst r1, r3
    bne range_error
    movs r0, r7
    tst r0, r3
    bne range_error
    ldr r3, [r5]
    movs r4, #16
    tst r3, r4              /* no call to Frag_Read without MODE_FRAG */
    beq range_error
    ldr r3, [r5, #8]
    ldr r4, [r5, #12]
    orrs r3, r4
    bne range_error         /* synthetic offsets never inherit drive offsets */
redirect_frag_call:
    bl redirect_start      /* relocated to Frag_Read */
    cmp r0, #0
    bne read_error
    pop {r4, pc}
stock:
    ldr r3, [r5]
    lsls r3, r3, #30
    bmi raw
    movs r0, r6
    movs r1, r7
    ldr r2, [sp, #8]
redirect_stock_call:
    bl redirect_start      /* relocated to DI_HandleCmd */
    pop {r4, pc}
raw:
    ldr r1, [r6, #4]
    ldr r2, [r6, #8]
    movs r0, r7
redirect_raw_call:
    bl redirect_start      /* relocated to __DI_ReadUnencrypted */
    pop {r4, pc}
range_error:
    ldr r3, block_range
    b error
read_error:
    ldr r3, unrecovered_read
error:
    str r3, [r5, #16]
    movs r0, #0xa0
    lsls r0, r0, #8
    pop {r4, pc}
.balign 4
redirect_limit:
    .word 0                /* exclusive, padded end in word units */
block_range:
    .word 0x00052100
unrecovered_read:
    .word 0x00031100
redirect_end:
