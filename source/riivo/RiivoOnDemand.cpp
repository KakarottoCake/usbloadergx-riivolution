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
					  OnDemandLayout &out, u32 genLen,
					  bool tableOnStorage)
	{
		out = OnDemandLayout();

		if (tableLen == 0 && !tableOnStorage)
		{
			out.why = "no redirect table to place";
			return false;
		}
		if (tableOnStorage)
			out.tableOnStorage = true;
		const u32 moduleBytes = ModuleFootprint();
		if (moduleBytes == 0)
		{
			out.why = "no module was built into this loader";
			return false;
		}

		//! One reservation for module, table, and staged slices - or the
		//! module alone when the table lives as a file. Rounding each
		//! part up to a cache line keeps the module - which is placed
		//! first - aligned for the DMA buffers inside it, and the store
		//! 32-aligned so the storage layer can DMA straight into it.
		const u32 align = MEM2_RESERVE_ALIGN;
		const u32 modulePart = (moduleBytes + align - 1) & ~(align - 1);
		if (modulePart < moduleBytes)
		{
			out.why = "module size overflows when aligned";
			return false;
		}
		const u32 tablePart = tableOnStorage
							  ? 0 : (tableLen + align - 1) & ~(align - 1);
		if (!tableOnStorage && tablePart < tableLen)
		{
			out.why = "table size overflows when aligned";
			return false;
		}
		const u32 genPart = (tableOnStorage || genLen == 0)
							? 0 : (genLen + align - 1) & ~(align - 1);
		if (!tableOnStorage && genLen > 0 && genPart < genLen)
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
		out.tableAddr = tableOnStorage ? 0 : r.addr + modulePart;
		out.tableLen = tableOnStorage ? 0 : tableLen;
		out.genAddr = (!tableOnStorage && genLen) ? r.addr + modulePart + tablePart : 0;
		out.genLen = tableOnStorage ? 0 : genLen;
		out.newArenaHi = r.newArenaHi;
		out.reserved = r.reserved;
		out.heapLeft = r.heapLeft;

		//! All parts must sit inside what was actually reserved.
		//! ReserveMem2 has already proved the block fits; this proves the
		//! split of it does, which is the part that would silently overlap.
		//! Storage-backed layouts hold the module alone (no table/store
		//! addresses to check).
		if (!tableOnStorage)
		{
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

		//! Paged tables live as files (staged by the caller before this
		//! runs); the reservation holds the module alone and nothing is
		//! copied. Every other kind still carries its table bytes here.
		const bool paged = (meta.kind == RIIVO_TABLEKIND_PAGED);
		if (table.empty() && !paged)
		{
			why = "no redirect table to install";
			return false;
		}

		const Mem2Arena arena = ReadMem2Arena();
		if (!PlanOnDemand(arena, paged ? 0 : (u32) table.size(), layout,
						  paged ? 0 : genLen, paged))
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
		//! Paged tables skip the copy (the module pages the staged file);
		//! the reservation holds the module alone.
		if (!paged)
		{
			memcpy((void *) layout.tableAddr, &table[0], table.size());
			DCFlushRange((void *) (layout.tableAddr & ~31u),
						 (u32) table.size() + 64);
		}

		ModuleParams p;
		p.table = paged ? 0 : layout.tableAddr;
		p.tableLen = paged ? 0 : layout.tableLen;
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
		//! stage. A pending MEM2 slice store arms late after FillGenStore
		//! proves it; a paged contract arms late after its files land
		//! (FillGenFile + ArmModule in Activate). Until then every read
		//! MISSES without initializing, so the module cannot serve - or
		//! cache - a half-staged contract.
		p.armed = (!paged && genLen == 0) ? 1 : 0;
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
		//! Whether the module can publish its acknowledgment where the
		//! probe can see it. Without the maintenance hook its counters
		//! stay dirty in Starlet's cache and the ack gate below withholds.
		layout.syncFound = (inst.sync != 0);

		gprintf("Riivo: on-demand ready - module %08x, arena2Hi -> %08x, "
				"%u bytes left to the game\n",
				layout.moduleAddr, layout.newArenaHi,
				(unsigned) layout.heapLeft);
		if (paged)
			gprintf("Riivo: on-demand table lives as a file (no MEM2 copy)\n");
		else
			gprintf("Riivo: on-demand table %08x (%u bytes, MEM2 copy)\n",
					layout.tableAddr, (unsigned) layout.tableLen);
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
