/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoOnDemand.hpp.
 ***************************************************************************/
#include "RiivoOnDemand.hpp"
#include "RiivoModuleInstall.hpp"
#include "RiivoRedirectTable.hpp"

#ifdef GEKKO
#include <string.h>
#include <ogc/cache.h>
#include "RiivoIosProbe.hpp"
#include "gecko.h"
#endif

namespace Riivo
{
	bool PlanOnDemand(const Mem2Arena &arena, u32 tableLen,
					  OnDemandLayout &out, u32 genLen)
	{
		out = OnDemandLayout();

		if (tableLen == 0)
		{
			out.why = "no redirect table to place";
			return false;
		}
		const u32 moduleBytes = ModuleFootprint();
		if (moduleBytes == 0)
		{
			out.why = "no module was built into this loader";
			return false;
		}

		//! One reservation for module, table, and staged slices. Rounding
		//! each part up to a cache line keeps the module - which is placed
		//! first - aligned for the DMA buffers inside it, and the store
		//! 32-aligned so the storage layer can DMA straight into it.
		const u32 align = MEM2_RESERVE_ALIGN;
		const u32 modulePart = (moduleBytes + align - 1) & ~(align - 1);
		if (modulePart < moduleBytes)
		{
			out.why = "module size overflows when aligned";
			return false;
		}
		const u32 tablePart = (tableLen + align - 1) & ~(align - 1);
		if (tablePart < tableLen)
		{
			out.why = "table size overflows when aligned";
			return false;
		}
		const u32 genPart = (genLen + align - 1) & ~(align - 1);
		if (genLen > 0 && genPart < genLen)
		{
			out.why = "slice store size overflows when aligned";
			return false;
		}
		u64 total = (u64) modulePart + tablePart + genPart;
		if (total > 0xFFFFFFFFULL)
		{
			out.why = "reservation overflows";
			return false;
		}

		Mem2Reservation r = ReserveMem2(arena, (u32) total, align);
		if (!r.ok)
		{
			out.why = r.why;
			return false;
		}

		out.moduleAddr = r.addr;
		out.tableAddr = r.addr + modulePart;
		out.tableLen = tableLen;
		out.genAddr = genLen ? r.addr + modulePart + tablePart : 0;
		out.genLen = genLen;
		out.newArenaHi = r.newArenaHi;
		out.reserved = r.reserved;
		out.heapLeft = r.heapLeft;

		//! All three must sit inside what was actually reserved.
		//! ReserveMem2 has already proved the block fits; this proves the
		//! split of it does, which is the part that would silently overlap.
		if (out.tableAddr < out.moduleAddr
			|| (u64) out.tableAddr + tableLen > (u64) r.addr + r.reserved)
		{
			out.why = "table does not fit alongside the module";
			return false;
		}
		if (genLen > 0
			&& (out.genAddr < out.tableAddr + tablePart
				|| (u64) out.genAddr + genLen > (u64) r.addr + r.reserved))
		{
			out.why = "slice store does not fit alongside the table";
			return false;
		}
		if ((u64) out.moduleAddr + moduleBytes > out.tableAddr)
		{
			out.why = "module and table overlap";
			return false;
		}
		if (out.moduleAddr & (align - 1))
		{
			out.why = "module would not be cache-line aligned";
			return false;
		}

		out.ok = true;
		return true;
	}

#ifdef GEKKO

	bool InstallOnDemand(u32 site, const std::vector<u8> &table, u32 partLba,
						 const OnDemandMeta &meta,
						 OnDemandLayout &layout, std::string &why,
						 u32 genLen)
	{
		layout = OnDemandLayout();
		why.clear();

		if (table.empty())
		{
			why = "no redirect table to install";
			return false;
		}

		const Mem2Arena arena = ReadMem2Arena();
		if (!PlanOnDemand(arena, (u32) table.size(), layout, genLen))
		{
			why = layout.why;
			return false;
		}

		//! Lower the boundary FIRST. Everything written after this goes above
		//! it, and if the install fails from here on the only cost is that the
		//! game has slightly less MEM2 than it might have - which is harmless,
		//! where writing the table into memory the game still owns is not.
		Mem2Reservation r;
		r.ok = true;
		r.addr = layout.moduleAddr;
		r.newArenaHi = layout.newArenaHi;
		r.reserved = layout.reserved;
		r.heapLeft = layout.heapLeft;
		if (!CommitMem2Reservation(r))
		{
			why = "could not reserve MEM2 for the module";
			return false;
		}

		//! The table, where the module will read it. IOS reads this across the
		//! bus, so it has to reach memory rather than sit in the PPC's cache.
		memcpy((void *) layout.tableAddr, &table[0], table.size());
		DCFlushRange((void *) (layout.tableAddr & ~31u),
					 (u32) table.size() + 64);

		ModuleParams p;
		p.table = layout.tableAddr;
		p.tableLen = layout.tableLen;
		p.partLba = partLba;
		p.tableKind = meta.kind;
		p.genBase = layout.genAddr;
		p.genSize = layout.genLen;
		p.declLo = (u32) (meta.declSize & 0xFFFFFFFFULL);
		p.declHi = (u32) (meta.declSize >> 32);
		p.expDiscId = meta.discId;
		p.expPartIdx = meta.partIdx;
		p.epoch = meta.epoch;
		//! Activation state at install: armed only when nothing remains to
		//! fill. A pending slice store arms late after FillGenStore proves
		//! it; until then every read MISSES without initializing, so the
		//! module cannot serve - or cache - a half-staged contract.
		p.armed = (genLen == 0) ? 1 : 0;
		//! Left zero deliberately: ApplyDiPatchOnDemand finds the real
		//! os_sync_after_write in the running plugin and overwrites this. A
		//! guess here would be called on every single read.
		p.sync = 0;

		OnDemandInstall inst;
		if (!ApplyDiPatchOnDemand(site, layout.moduleAddr, p, inst))
		{
			why = inst.why;
			return false;
		}

		gprintf("Riivo: on-demand ready - table %08x (%u bytes), module %08x, "
				"arena2Hi -> %08x, %u bytes left to the game\n",
				layout.tableAddr, (unsigned) layout.tableLen,
				layout.moduleAddr, layout.newArenaHi,
				(unsigned) layout.heapLeft);
		if (layout.genLen > 0)
			gprintf("Riivo: on-demand slice store %08x (%u bytes, fill pending)\n",
					layout.genAddr, (unsigned) layout.genLen);
		return true;
	}

#else

	//! Host build: the layout arithmetic is what the tests exercise. Writing
	//! to IOS and to low memory is target-only by nature.
	bool InstallOnDemand(u32, const std::vector<u8> &, u32, const OnDemandMeta &,
						 OnDemandLayout &, std::string &why, u32)
	{
		why = "on-demand install is target-only";
		return false;
	}

#endif
}
