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
	: active(false), fstOffset(0)
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
}

} // namespace Riivo
