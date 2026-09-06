/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Address model (Gate B contract, WP5).
 *
 * Four quantities that must never be mixed:
 *   WholeDiscBytes - encrypted whole-disc byte offset (backup file layout).
 *   PartitionBytes - decrypted partition byte offset (what the game reads).
 *   DiWord         - DI word address (partition bytes / 4 on the DI bus).
 *   PhysSector     - physical sector number on the drive.
 *
 * All arithmetic is 64-bit internally; conversion at hardware interfaces is
 * checked. The current LOW_READ hook (RiivoDiPatch.hpp) routes by synthetic
 * PartitionBytes, not by WholeDiscBytes - see RiivoFragPlan.hpp. Dual-layer
 * images are refused by PlanFragRegion, not re-addressed here; any future
 * layout must prove every chosen address survives FST encoding, DI command
 * encoding, signed comparisons, and cIOS translation before use.
 ***************************************************************************/
#ifndef RIIVO_ADDR_HPP_
#define RIIVO_ADDR_HPP_

#include <gctypes.h>

namespace Riivo
{
	//! Distinct address spaces. Wrappers are deliberately not implicitly
	//! convertible; use the checked helpers below.
	struct WholeDiscBytes { u64 v; explicit WholeDiscBytes(u64 x = 0) : v(x) {} };
	struct PartitionBytes { u64 v; explicit PartitionBytes(u64 x = 0) : v(x) {} };
	struct DiWord { u64 v; explicit DiWord(u64 x = 0) : v(x) {} };
	struct PhysSector { u64 v; explicit PhysSector(u64 x = 0) : v(x) {} };

	//! DI word <-> partition bytes. DI addresses are word units (x4 bytes).
	//! Fails when the byte count is not word-aligned or the word count
	//! overflows 32 bits on the bus.
	inline bool DiWordsToPartitionBytes(DiWord w, PartitionBytes &out)
	{
		if (w.v > 0xFFFFFFFFULL)
			return false;
		out.v = w.v * 4ULL;
		return true;
	}

	inline bool PartitionBytesToDiWords(PartitionBytes b, DiWord &out)
	{
		if (b.v & 3ULL)
			return false;
		out.v = b.v / 4ULL;
		if (out.v > 0xFFFFFFFFULL)
			return false;
		return true;
	}

	//! Sector <-> bytes for a given sector size. Sector size must be a
	//! power of two in [512, 4096] (same rule as PlanFragRegion).
	inline bool SectorSizeValid(u32 sectorSize)
	{
		return sectorSize >= 512 && sectorSize <= 4096 &&
			(sectorSize & (sectorSize - 1)) == 0;
	}

	inline bool PhysSectorToBytes(PhysSector s, u32 sectorSize, u64 &outBytes)
	{
		if (!SectorSizeValid(sectorSize))
			return false;
		outBytes = s.v * (u64) sectorSize;
		// Overflow check: round-trip must hold.
		if (sectorSize && outBytes / sectorSize != s.v)
			return false;
		return true;
	}

	inline bool BytesToPhysSector(u64 bytes, u32 sectorSize, PhysSector &out)
	{
		if (!SectorSizeValid(sectorSize))
			return false;
		if (bytes % sectorSize)
			return false;
		out.v = bytes / sectorSize;
		return true;
	}

	//! One ordered, non-overlapping partition extent (cf. arch 3.2).
	//! Kind matches the manifest (RiivoManifest.hpp); Original gaps between
	//! listed extents are delegated to the original cIOS reader.
	struct PartitionExtent
	{
		u64 start; // PartitionBytes
		u64 len;
		u64 End() const { return start + len; }
	};

	//! True when [off, off+len) lies fully inside [lo, hi).
	inline bool RangeInside(u64 off, u64 len, u64 lo, u64 hi)
	{
		if (len == 0)
			return off >= lo && off <= hi;
		if (off + len < off)
			return false; // wrap
		return off >= lo && off + len <= hi;
	}

	//! Verify a sorted, non-overlapping extent list. Returns false with the
	//! failing index in `failIdx` when out of order, overlapping, or wrapped.
	template <typename T>
	inline bool ExtentsOrdered(const T *items, u32 count, u32 &failIdx,
							   u64 (*getStart)(const T &),
							   u64 (*getLen)(const T &))
	{
		u64 prevEnd = 0;
		for (u32 i = 0; i < count; ++i)
		{
			u64 s = getStart(items[i]);
			u64 l = getLen(items[i]);
			if (l > 0 && s + l < s)
			{
				failIdx = i;
				return false;
			}
			if (i > 0 && s < prevEnd)
			{
				failIdx = i;
				return false;
			}
			u64 e = s + l;
			if (e > prevEnd)
				prevEnd = e;
		}
		return true;
	}

	//! Shared range contract for any future layout (WP5, Gate D).
	//! Checks one partition extent against the boundaries a layout must
	//! survive, without choosing a layout itself:
	//!   dvd5Ceiling  - single-layer read ceiling (bytes).
	//!   dvd9Probe    - dual-layer probe offset (bytes); a declared size
	//!                  reaching it promotes the disc and breaks single-layer
	//!                  anti-piracy reads.
	//!   regionBase   - lowest synthetic byte the hook may serve.
	//!   regionLimit  - one past the highest byte the hook may serve.
	//! Signed-word wrap: DI words are compared signed in places, so anything
	//! at or above 0x80000000 words (8 GiB bytes) is out. Returns true when
	//! the range is placeable; false + a short reason otherwise. Dual-layer
	//! images stay refused by PlanFragRegion; this does not re-admit them.
	enum RangeVerdict
	{
		RANGE_OK = 0,
		RANGE_WRAPS,
		RANGE_BELOW_REGION,
		RANGE_ABOVE_REGION,
		RANGE_HITS_DVD9_PROBE,
		RANGE_HITS_SIGNED_WRAP
	};

	inline RangeVerdict ValidatePartitionRange(u64 start, u64 len,
											   u64 dvd9ProbeBytes,
											   u64 regionBase,
											   u64 regionLimit)
	{
		static const u64 SIGNED_WRAP_BYTES = 0x80000000ULL * 4ULL;
		if (len > 0 && start + len < start)
			return RANGE_WRAPS;
		u64 end = start + len;
		if (start < regionBase)
			return RANGE_BELOW_REGION;
		if (end > regionLimit)
			return RANGE_ABOVE_REGION;
		if (start < dvd9ProbeBytes && end > dvd9ProbeBytes)
			return RANGE_HITS_DVD9_PROBE;
		if (end > SIGNED_WRAP_BYTES)
			return RANGE_HITS_SIGNED_WRAP;
		return RANGE_OK;
	}
}

#endif
