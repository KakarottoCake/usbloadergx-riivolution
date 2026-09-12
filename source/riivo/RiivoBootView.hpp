/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Patched boot view: loader-owned PPC overlay serving coherent boot
 * metadata + file-table bytes to boot-time readers.
 *
 * Dolphin contract (DirectoryBlobPartition::BuildFST): rebuilt table +
 * header 0x424 (FST offset) / 0x428 (FST size) / 0x42c (FST max size) in one
 * coherent view; boot consumers read that view from the start. GX's
 * stock-view/late-table mismatch (apploader sees stock, table installed
 * afterward) drove per-game reservation experiments. This overlay is the
 * general fix's first half: header + FST coherence before the apploader.
 *
 * Lifetime explicit: Activate after the plan is built (PrepareFileRedirects),
 * consulted only by WDVD_Read during Apploader_Run, Deactivate immediately
 * after. Never a filesystem dependency after devices shut down; runtime file
 * reads use the IOS hook/manifest, not this. Inactive => Serve returns false
 * for every request (stock behavior preserved bit-for-bit).
 *
 * PPC wiring status: host-tested module only in this slice. WDVD_Read
 * consultation awaits the apploader-read survey (which reads it makes, which
 * addresses it writes, how it reserves the table, whether writes overlap live
 * GX memory). Do not activate in production until that measurement lands;
 * MEM1-install + verification remains the production boot path with the
 * in-place-only policy.
 *
 * Scope this slice: header (0x0-0x440, partition-byte offsets) + FST bytes.
 * Executable/content serving (main.dol, file data during apploader) and
 * apploader-allocation measurement remain for the emulator-measured slice
 * (see DOLPHIN-GX-CANDIDATE-SWEEP acceptance 3-5). Boot-file patches refuse
 * until then; partial file data during apploader is stock (whole-file mods
 * only in this slice).
 *
 * Pure logic, no console calls; host tests exercise Serve + header patching
 * byte-exactly. Checked u64 arithmetic, explicit units (partition bytes).
 ***************************************************************************/
#ifndef RIIVO_BOOT_VIEW_HPP_
#define RIIVO_BOOT_VIEW_HPP_

#include <gctypes.h>
#include <string>
#include <vector>

namespace Riivo
{

//! Partition header covering the FST words. Wii shift is 2 (word->byte).
static const u32 BOOTVIEW_HEADER_BYTES = 0x440;
static const u32 BOOTVIEW_FST_OFF = 0x424;
static const u32 BOOTVIEW_FST_SIZE = 0x428;
static const u32 BOOTVIEW_FST_MAX = 0x42c;
static const u32 BOOTVIEW_SHIFT = 2;

//! Read a big-endian u32 (disc header encoding) without alignment assumptions.
inline u32 BootViewReadBE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

//! Write a big-endian u32.
inline void BootViewWriteBE32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)((v >> 16) & 0xff);
	p[2] = (u8)((v >> 8) & 0xff);
	p[3] = (u8)(v & 0xff);
}

//! Patch a stock header image with rebuilt FST placement (offset/size/max
//! in words, i.e. bytes>>SHIFT). Returns false + why when the stock image
//! is too short or the rebuilt values overflow the word field.
bool PatchBootHeader(const u8 *stockHeader, u32 stockLen,
					 u64 fstOffsetBytes, u32 fstSizeBytes, u32 fstMaxBytes,
					 std::vector<u8> &outPatched, std::string &why);

//! Loader-owned overlay state. Activate copies header + FST (bounded: header
//! 0x440 + table bytes, never whole-mod payloads). Serve answers only
//! fully-contained header/FST reads; all else returns false (stock path).
class BootView
{
public:
	BootView();

	//! Activate with stock header (for fallback bytes outside 0x424-42f),
	//! patched header, patched FST + its partition-byte offset. Copies bytes
	//! (bounded). Returns false + why on bad inputs (empty table, overflow,
	//! header too short). Deactivate on failure (remains inactive).
	bool Activate(const u8 *stockHeader, u32 stockLen,
				  const std::vector<u8> &patchedHeader,
				  u64 fstOffsetBytes,
				  const std::vector<u8> &patchedFst,
				  std::string &why);

	//! True when active (consulted). False => stock behavior, Serve refuses.
	bool Active() const { return active; }

	//! Partition-byte offset of the served FST (0 when inactive).
	u64 FstOffset() const { return fstOffset; }

	//! Serve a partition-byte read [offset, offset+length) into `buffer`.
	//! Returns true + copies when the request lies fully inside the header
	//! or FST coverage; false leaves `buffer` untouched (caller does stock).
	//! Zero-length reads return false (nothing to serve). No partial splits:
	//! crossing requests fall back to stock (runtime splitter owns those).
	bool Serve(u64 offset, u8 *buffer, u32 length) const;

	//! Forget everything (explicit lifetime end). Idempotent.
	void Deactivate();

private:
	bool active;
	std::vector<u8> header; // BOOTVIEW_HEADER_BYTES (patched)
	u64 fstOffset;
	std::vector<u8> fst;    // patched table bytes
};

} // namespace Riivo

#endif
