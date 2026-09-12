/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Boot-view overlay implementation (host-safe, no console calls).
 ***************************************************************************/
#include <string.h>

#include "RiivoBootView.hpp"

namespace Riivo
{

bool PatchBootHeader(const u8 *stockHeader, u32 stockLen,
					 u64 fstOffsetBytes, u32 fstSizeBytes, u32 fstMaxBytes,
					 std::vector<u8> &outPatched, std::string &why)
{
	why.clear();
	outPatched.clear();
	if (!stockHeader || stockLen < BOOTVIEW_HEADER_BYTES)
	{
		why = "stock header too short for FST words";
		return false;
	}
	if ((fstOffsetBytes & ((1ULL << BOOTVIEW_SHIFT) - 1)) != 0)
	{
		why = "FST offset not word-aligned";
		return false;
	}
	u64 offW = fstOffsetBytes >> BOOTVIEW_SHIFT;
	u64 sizeW = (u64)fstSizeBytes >> BOOTVIEW_SHIFT;
	u64 maxW = (u64)fstMaxBytes >> BOOTVIEW_SHIFT;
	// Sizes need not be multiples (Dolphin aligns name table up); words
	// truncate down, matching the on-disc convention (reader shifts back).
	// Refuse only on field overflow, never silently wrap.
	if (offW > 0xFFFFFFFFULL || sizeW > 0xFFFFFFFFULL || maxW > 0xFFFFFFFFULL)
	{
		why = "FST header word overflow";
		return false;
	}
	outPatched.assign(stockHeader, stockHeader + BOOTVIEW_HEADER_BYTES);
	BootViewWriteBE32(&outPatched[BOOTVIEW_FST_OFF], (u32)offW);
	BootViewWriteBE32(&outPatched[BOOTVIEW_FST_SIZE], (u32)sizeW);
	BootViewWriteBE32(&outPatched[BOOTVIEW_FST_MAX], (u32)maxW);
	return true;
}

BootView::BootView()
	: active(false), fstOffset(0),
	  hasDol(false), dolBase(0), dolSize(0)
{
}

bool BootView::Activate(const u8 *stockHeader, u32 stockLen,
						const std::vector<u8> &patchedHeader,
						u64 fstOffsetBytes,
						const std::vector<u8> &patchedFst,
						std::string &why)
{
	why.clear();
	Deactivate();
	if (!stockHeader || stockLen < BOOTVIEW_HEADER_BYTES)
	{
		why = "stock header too short";
		return false;
	}
	if (patchedHeader.size() < BOOTVIEW_HEADER_BYTES)
	{
		why = "patched header too short";
		return false;
	}
	if (patchedFst.empty())
	{
		why = "patched table is empty";
		return false;
	}
	// Bound the copy: header + table only (never whole-mod payloads).
	// Tables are KBs (T0 150 KB, big conversions single-digit MB at most).
	if (patchedFst.size() > 16 * 1024 * 1024)
	{
		why = "patched table exceeds 16 MiB bound";
		return false;
	}
	header.assign(patchedHeader.begin(),
				  patchedHeader.begin() + BOOTVIEW_HEADER_BYTES);
	fstOffset = fstOffsetBytes;
	fst = patchedFst;
	active = true;
	return true;
}

bool BootView::Serve(u64 offset, u8 *buffer, u32 length) const
{
	if (!active || !buffer || length == 0)
		return false;
	// Header coverage [0, 0x440).
	if (offset < BOOTVIEW_HEADER_BYTES)
	{
		u64 end = offset + (u64)length;
		if (end <= BOOTVIEW_HEADER_BYTES)
		{
			memcpy(buffer, &header[(size_t)offset], length);
			return true;
		}
		return false; // crossing: stock path (no split reassembly here)
	}
	// FST coverage [fstOffset, fstOffset+fst.size()).
	if (offset >= fstOffset)
	{
		u64 end = offset + (u64)length;
		u64 fstEnd = fstOffset + (u64)fst.size();
		if (end <= fstEnd && end >= offset)
		{
			memcpy(buffer, &fst[(size_t)(offset - fstOffset)], length);
			return true;
		}
		return false;
	}
	return false;
}

void BootView::Deactivate()
{
	active = false;
	header.clear();
	fstOffset = 0;
	fst.clear();
	hasDol = false;
	dolBase = 0;
	dolSize = 0;
	dol = PlannedFile();
}

//! True when segs tile [0,size) contiguously in order with no gaps,
//! overlaps, or zero-length runs (empty only if size == 0, refused here).
static bool SegmentsTile(const PlannedFile &f, u64 size)
{
	if (size == 0 || f.segs.empty())
		return false;
	u64 pos = 0;
	for (size_t i = 0; i < f.segs.size(); ++i)
	{
		const PlanSegment &s = f.segs[i];
		if (s.length == 0 || s.fileOffset != pos)
			return false;
		pos += s.length;
		if (pos < s.length)
			return false; // u64 wrap (unreachable at u32 lengths, checked)
	}
	return pos == size;
}

static bool RangesOverlap(u64 aLo, u64 aHi, u64 bLo, u64 bHi)
{
	return aLo < aHi && bLo < bHi && aLo < bHi && bLo < aHi;
}

bool BootView::SetDol(u64 base, u64 size, const PlannedFile &dolIn, std::string &why)
{
	why.clear();
	hasDol = false;
	dolBase = 0;
	dolSize = 0;
	dol = PlannedFile();
	if (!dolIn.bootFile)
	{
		why = "not an executable plan";
		return false;
	}
	if (size == 0 || size > 0xFFFFFFFFULL)
	{
		why = "empty or oversized DOL image";
		return false;
	}
	if (dolIn.finalSize != size)
	{
		why = "served image must preserve the stock image size";
		return false;
	}
	if (base < BOOTVIEW_HEADER_BYTES)
	{
		why = "DOL image overlaps the boot header";
		return false;
	}
	if (base + size < base)
	{
		why = "DOL range overflow";
		return false;
	}
	if (!SegmentsTile(dolIn, size))
	{
		why = "executable segments do not tile the image";
		return false;
	}
	if (active && !fst.empty())
	{
		u64 fstEnd = fstOffset + (u64)fst.size();
		if (RangesOverlap(base, base + size, fstOffset, fstEnd))
		{
			why = "DOL image overlaps the served FST range";
			return false;
		}
	}
	dolBase = base;
	dolSize = size;
	dol = dolIn;
	hasDol = true;
	return true;
}

bool BootView::ServeDol(u64 offset, u8 *buffer, u32 length,
						const BootReaders &r, u32 *doneOut) const
{
	if (doneOut)
		*doneOut = 0;
	if (!hasDol || !buffer || length == 0 || !r.stock || !r.fat)
		return false;
	u64 end = offset + (u64)length;
	if (end < offset || offset < dolBase || end > dolBase + dolSize)
		return false;
	u64 rel = offset - dolBase;
	u64 relEnd = rel + length;
	u32 done = 0;
	for (size_t i = 0; i < dol.segs.size() && (u64)done < length; ++i)
	{
		const PlanSegment &s = dol.segs[i];
		u64 sEnd = s.fileOffset + (u64)s.length;
		if (relEnd <= s.fileOffset || rel >= sEnd)
			continue; // no overlap with this run
		u64 p = rel > s.fileOffset ? rel : s.fileOffset;
		u64 q = relEnd < sEnd ? relEnd : sEnd;
		u32 take = (u32)(q - p);
		u8 *dst = buffer + (p - rel);
		bool ok = false;
		if (s.kind == PlanSegment::SEG_ORIGINAL)
			ok = r.stock(dolBase + p, dst, take, r.ctx);
		else if (s.kind == PlanSegment::SEG_EXTERNAL)
			ok = r.fat(s.external, s.srcOffset + (p - s.fileOffset), dst, take, r.ctx);
		else
		{
			memset(dst, 0, take);
			ok = true;
		}
		if (!ok)
		{
			if (doneOut)
				*doneOut = done;
			return false;
		}
		done += take;
	}
	if (doneOut)
		*doneOut = done;
	return done == length;
}

} // namespace Riivo
