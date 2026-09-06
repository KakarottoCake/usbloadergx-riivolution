/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * See RiivoModuleInstall.hpp.
 ***************************************************************************/
#include "RiivoModuleInstall.hpp"
#include "RiivoModuleBlob.hpp"
#include "RiivoMem2Reserve.hpp"

namespace Riivo
{
	//! 'RIIO', the first word of the module's parameter block. Checked so that
	//! a blob and a set of offsets from different builds cannot be combined:
	//! the offsets would land mid-code and the module would branch through
	//! whatever happened to be there.
	static const u32 RIIVO_MODULE_MAGIC = 0x5249494Fu;

	//! Big-endian, because that is how a big-endian ARM reads its own memory.
	static u32 Rd32(const u8 *p)
	{
		return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
	}

	static void Wr32(u8 *p, u32 v)
	{
		p[0] = (u8) (v >> 24);
		p[1] = (u8) (v >> 16);
		p[2] = (u8) (v >> 8);
		p[3] = (u8) v;
	}

	u32 ModuleFootprint()
	{
		return RIIVO_MODULE_CODE_LEN + RIIVO_MODULE_BSS_LEN;
	}

	bool BuildModuleImage(u32 at, const ModuleParams &p, ModulePlan &plan,
						  std::string &why)
	{
		plan = ModulePlan();
		why.clear();

		if (RIIVO_MODULE_CODE_LEN == 0)
		{
			why = "no module was built into this loader";
			return false;
		}
		//! The module holds buffers the SD and USB engines DMA into, and those
		//! want a cache line. Placing the blob unaligned makes every one of
		//! them unaligned too.
		if (at & (MEM2_RESERVE_ALIGN - 1))
		{
			why = "module address is not cache-line aligned";
			return false;
		}
		const u32 foot = ModuleFootprint();
		if (at < MEM2_BASE || at >= MEM2_TOP || (u64) at + foot > MEM2_TOP)
		{
			why = "module does not fit in MEM2 where it was placed";
			return false;
		}

		//! Offsets must be inside the bytes we carry, or they came from a
		//! different build than the blob did.
		if (RIIVO_MODULE_PARAMS_OFF + 32 > RIIVO_MODULE_CODE_LEN
			|| RIIVO_MODULE_ENTRY_OFF >= RIIVO_MODULE_CODE_LEN)
		{
			why = "module offsets do not match the module bytes";
			return false;
		}
		if (RIIVO_MODULE_ENTRY_OFF & 1)
		{
			why = "module entry is odd; the Thumb bit must not be in the offset";
			return false;
		}

		//! Every address the module will branch through has to be present.
		//! A zero here is a branch to zero on the first read.
		if (!p.table || !p.tableLen)
		{
			why = "no redirect table to hand the module";
			return false;
		}
		if (!p.readA || !p.readB || !p.config)
		{
			why = "storage routines were not found; nothing to hand the module";
			return false;
		}

		std::vector<u8> img(RIIVO_MODULE_CODE,
							RIIVO_MODULE_CODE + RIIVO_MODULE_CODE_LEN);

		if (Rd32(&img[RIIVO_MODULE_PARAMS_OFF]) != RIIVO_MODULE_MAGIC)
		{
			why = "module magic is not where the offsets say; blob and offsets "
				  "are from different builds";
			return false;
		}

		//! Relocate to the PHYSICAL address, not the one the loader uses.
		//!
		//! `at` is where the PowerPC writes the blob, in the PowerPC's view of
		//! MEM2 (0x93xxxxxx). Starlet runs the module and dereferences these
		//! words at 0x13xxxxxx. Relocating to the PowerPC's view would leave
		//! every pointer inside the module 0x80000000 too high - a data abort
		//! on the first read, which is a freeze with nothing on screen.
		//!
		//! The hook's call to the module is unaffected: a Thumb BL is
		//! pc-relative, so the distance is the same in either view.
		const u32 phys = at & 0x3FFFFFFFu;
		const u32 delta = phys - RIIVO_MODULE_LINK_BASE;
		for (u32 i = 0; i < RIIVO_MODULE_RELOC_NUM; ++i)
		{
			const u32 off = RIIVO_MODULE_RELOCS[i];
			if (off + 4 > RIIVO_MODULE_CODE_LEN)
			{
				why = "a relocation points outside the module";
				return false;
			}
			Wr32(&img[off], Rd32(&img[off]) + delta);
		}

		//! Fill the parameter block. Field order matches riivo_ios.h; the
		//! counters after it are left zero for the module to write.
		u8 *q = &img[RIIVO_MODULE_PARAMS_OFF];
		Wr32(q + 0, RIIVO_MODULE_MAGIC);
		Wr32(q + 4, p.table);
		Wr32(q + 8, p.tableLen);
		Wr32(q + 12, p.partLba);
		Wr32(q + 16, p.readA);
		Wr32(q + 20, p.readB);
		Wr32(q + 24, p.config);
		Wr32(q + 28, p.sync);

		plan.addr = at;
		plan.physAddr = phys;
		plan.entry = at + RIIVO_MODULE_ENTRY_OFF;
		plan.params = at + RIIVO_MODULE_PARAMS_OFF;
		plan.codeLen = RIIVO_MODULE_CODE_LEN;
		plan.bssLen = RIIVO_MODULE_BSS_LEN;
		plan.footprint = foot;
		plan.image.swap(img);
		plan.ok = true;
		return true;
	}
}
