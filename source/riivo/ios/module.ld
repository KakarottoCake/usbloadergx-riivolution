/* The injected module: one contiguous blob the loader copies into the MEM2 it
 * reserved from the game, then relocates.
 *
 * ORIGIN is arbitrary. The blob is linked with --emit-relocs and every
 * absolute word in it is fixed up at load time against wherever it actually
 * landed, because the reservation address depends on the game's arena and
 * cannot be known when this is built. It is kept non-zero so that a word that
 * was never relocated is obvious rather than plausible.
 *
 * .bss is part of the blob's footprint but carries no bytes, so the loader
 * copies `text+data` and then clears `bss` - the module's statics must start
 * zero, and nothing on the console does that for us.
 */
ENTRY(riivo_di_read)

MEMORY
{
	ALL (rwx) : ORIGIN = 0x10000000, LENGTH = 1M
}

SECTIONS
{
	.text : {
		*(.text.riivo_di_read)
		*(.text*)
		*(.rodata*)
		. = ALIGN(32);
	} > ALL

	.data : {
		*(.data*)
		. = ALIGN(32);
	} > ALL

	__riivo_bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
		. = ALIGN(32);
	} > ALL
	__riivo_bss_end = .;

	/DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) *(.ARM.attributes) }
}
