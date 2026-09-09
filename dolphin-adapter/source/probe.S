// Probe "game": minimal consumer that runs at a jumped-to MEM1 address.
// Pure absolute-address assembly: no TOC (r2 untouched), no stack use,
// nonvolatiles untouched, interrupts untouched. Safe to memcpy anywhere
// executable and enter with mtctr/bctr; returns via blr with the entry
// count in r3 (0 = reject) and a result block at 0x80000200 for GDB.
// (0x80000200 sits clear of the 0x80000100 GDB mailbox, which ends at
// 0x80000194 - an earlier 0x80000180 address collided with it and ate
// the blob-length word, failing loudly in doprod.)
//
// Result block: [0]=magic ('PROB' ok / 'FAIL'), [1]=entry count,
// [2]=CRC32 over [ptr,ptr+max), [3]=arena word. The CRC matches the
// production verify semantics: bytes actually readable through the
// repointed words, by fresh code, after the jump.
	.text
	.globl	probe_start
	.globl	probe_end
	.p2align 2
probe_start:
	lis	3,0x8000
	lwz	4,0x38(3)	// ptr = *0x80000038
	lwz	5,0x3C(3)	// max = *0x8000003C
	lwz	6,0x34(3)	// arena = *0x80000034
	// ptr inside MEM1, max sane (unsigned compares throughout)
	lis	0,0x8000
	cmplw	0,4
	bgt	fail		// ptr < 0x80000000
	lis	0,0x8180
	cmplw	4,0
	bge	fail		// ptr >= 0x81800000
	cmpwi	5,64
	blt	fail		// max < 64
	lis	0,0x0080
	cmplw	5,0
	bge	fail		// max >= 8MB sanity cap
	lwz	7,8(4)		// count = root.next
	cmpwi	7,2
	blt	fail
	mulli	8,7,12
	cmplw	8,5
	bge	fail		// 12*count >= max
	lbz	9,0(4)
	cmpwi	9,1
	bne	fail		// root not a directory
	// CRC32 over the table (IEEE, init -1, poly EDB88320)
	li	10,-1
	add	0,4,5		// end
	mr	8,4		// cursor
1:	cmplw	8,0
	bge	2f
	lbz	9,0(8)
	addi	8,8,1
	xor	10,10,9
	li	11,8
3:	andi.	9,10,1
	srwi	10,10,1
	beq	4f
	lis	9,0xEDB8
	ori	9,9,0x8320
	xor	10,10,9
4:	subi	11,11,1
	cmpwi	11,0
	bne	3b
	b	1b
2:	nor	10,10,10	// not r10 (xori cannot take -1)
	lis	11,0x8000
	ori	11,11,0x0200
	lis	12,0x5052	// 'PR'
	ori	12,12,0x4F42	// 'OB'
	stw	12,0(11)
	stw	7,4(11)
	stw	10,8(11)
	stw	6,12(11)
	mr	3,7		// return count
	blr
fail:
	lis	11,0x8000
	ori	11,11,0x0200
	lis	12,0x4641	// 'FA'
	ori	12,12,0x494C	// 'IL'
	stw	12,0(11)
	stw	7,4(11)
	stw	5,8(11)
	stw	6,12(11)
	li	3,0
	blr
probe_end:
	.long	0
