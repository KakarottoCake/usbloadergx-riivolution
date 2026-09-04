/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <gccore.h>
#include <ogcsys.h>

#include "RiivoIosProbe.hpp"
#include "RiivoDiPatch.hpp"
#include "libs/libruntimeiospatch/runtimeiospatch.h"
#include "gecko.h"

namespace Riivo
{
	//! MEM2, as the PPC sees it. The cached view is what we scan (fast); the
	//! uncached alias is 0x40000000 higher and is what we read through, because
	//! the Broadway and the Starlet do not snoop each other's caches - anything
	//! IOS wrote after we last touched a line would otherwise come back stale.
	static const u32 MEM2_CACHED   = 0x90000000;
	static const u32 MEM2_END      = 0x94000000;
	static const u32 UNCACHED_BIAS = 0x40000000;

	//! Skip the bottom of MEM2: that is where the loader's own heap lives, and
	//! our own copy of the fragment list and of these very constants sits there.
	//! IOS modules live high. Starting at 12 MB keeps our own data out of the
	//! results without risking the region the plugin could be in.
	static const u32 SCAN_FROM = MEM2_CACHED + 0x00C00000;

	static const u32 DUMP_BEFORE = 0x2000; // 8 KB before the hit
	static const u32 DUMP_AFTER  = 0x6000; // 24 KB after it

	static const size_t MAX_HITS = 24;

	static void Addf(std::string &out, const char *fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		out += buf;
	}

	//! Read one word straight out of RAM, bypassing the data cache.
	static inline u32 ReadUncached(u32 cachedAddr)
	{
		return *(vu32 *) (cachedAddr + UNCACHED_BIAS);
	}

	void ProbeIosPlugin(const std::string &dumpPath, IosProbe &out)
	{
		out = IosProbe();
		out.iosVersion  = (u32) IOS_GetVersion();
		out.iosRevision = (u32) IOS_GetRevision();
		out.ahbprot     = AHBPROT_DISABLED;
		out.scanFrom    = SCAN_FROM;
		out.scanTo      = MEM2_END;

		//! Without AHBPROT the memory protection unit hides IOS from us and every
		//! read comes back as rubbish, so there is nothing to learn here.
		if (!out.ahbprot)
			return;

		//! The values worth looking for. Each is a literal that must exist
		//! somewhere in the module that holds it, and none is a number that
		//! turns up by accident.
		//!
		//! Note the d2x plugin and the stock DI module carry DIFFERENT read
		//! ceilings - d2x's dip.h says 0x46090000/0x7ED38000 while the stock
		//! module's table (the one libruntimeiospatch's di_readlimit patch
		//! rewrites) says 0x460A0000/0x7ED40000. That difference is what tells
		//! the two apart in the results.
		static const int NPAT = 6;
		IosPattern pats[NPAT];
		pats[0].name  = "d2x DVD9";
		pats[0].what  = "d2x dual-layer ceiling, in __DI_CheckOffset's literal pool";
		pats[0].value = 0x7ED38000;
		pats[1].name  = "d2x DVD5";
		pats[1].what  = "d2x single-layer ceiling, same function";
		pats[1].value = 0x46090000;
		pats[2].name  = "DIP thunk";
		pats[2].what  = "LDR r3,[PC,#0] + BX r3, written over the stock DI module";
		pats[2].value = 0x4B004718;
		pats[3].name  = "FRAG_MAX";
		pats[3].what  = "20000, the fragment list's maxnum";
		pats[3].value = 0x00004E20;
		pats[4].name  = "stock DVD9";
		pats[4].what  = "stock DI module's ceiling - locates the ORIGINAL module";
		pats[4].value = 0x7ED40000;
		pats[5].name  = "stock DVD5";
		pats[5].what  = "stock DI module's ceiling, same table";
		pats[5].value = 0x460A0000;

		//! One pass over MEM2 checking all of them, rather than one pass each.
		//! Every read goes through the uncached alias: the Broadway and the
		//! Starlet do not snoop each other, so a cached read could hand back a
		//! line from before IOS last wrote to it.
		u32 words = 0, nonZero = 0;
		for (u32 addr = SCAN_FROM; addr < MEM2_END; addr += 4)
		{
			const u32 v = ReadUncached(addr);
			++words;
			if (v)
				++nonZero;
			for (int i = 0; i < NPAT; ++i)
			{
				if (v != pats[i].value)
					continue;
				if (pats[i].hits.size() < MAX_HITS)
					pats[i].hits.push_back(addr);
			}
		}
		out.words = words;
		out.nonZero = nonZero;
		for (int i = 0; i < NPAT; ++i)
			out.patterns.push_back(pats[i]);

		//! Now the thing that actually matters: the read dispatch itself. The
		//! pattern was taken from a local build of d2x-v11-beta3, so finding it
		//! here proves the running cIOS is that build and the four-byte patch
		//! will land where it is meant to.
		//!
		//! Thumb instructions are halfword-aligned, so step by 2. Compare the
		//! first halfword before reading the rest - that rejects almost every
		//! address without touching memory again.
		{
			const u16 first = ((u16) DI_READ_PATTERN[0] << 8) | DI_READ_PATTERN[1];
			for (u32 addr = SCAN_FROM; addr + DI_READ_PATTERN_LEN < MEM2_END; addr += 2)
			{
				if (*(vu16 *) (addr + UNCACHED_BIAS) != first)
					continue;
				bool all = true;
				for (u32 k = 2; k < DI_READ_PATTERN_LEN && all; ++k)
					if (*(vu8 *) (addr + k + UNCACHED_BIAS) != DI_READ_PATTERN[k])
						all = false;
				if (all && out.patchSites.size() < MAX_HITS)
					out.patchSites.push_back(addr);
			}
		}

		//! DVD9_LENGTH is the best anchor: it is a 32-bit constant unique to the
		//! read-limit check, which sits in the same function we need to patch.
		//! Fall back to DVD5 if the compiler folded DVD9 differently.
		u32 anchor = 0;
		if (!out.patterns[0].hits.empty())
			anchor = out.patterns[0].hits[0];
		else if (!out.patterns[1].hits.empty())
			anchor = out.patterns[1].hits[0];
		if (!anchor)
			return;

		u32 base = anchor > SCAN_FROM + DUMP_BEFORE ? anchor - DUMP_BEFORE : SCAN_FROM;
		u32 size = DUMP_BEFORE + DUMP_AFTER;
		if (base + size > MEM2_END)
			size = MEM2_END - base;

		//! Copy through the uncached alias for the same reason as the scan.
		u8 *buf = (u8 *) malloc(size);
		if (!buf)
			return;
		for (u32 i = 0; i < size; i += 4)
			*(u32 *) (buf + i) = ReadUncached(base + i);

		FILE *f = fopen(dumpPath.c_str(), "wb");
		if (f)
		{
			//! A tiny header so the dump is self-describing: I need to know what
			//! address these bytes came from to disassemble them usefully.
			char hdr[64];
			int n = snprintf(hdr, sizeof(hdr), "RIIVODIP1%08x%08x%08x%08x",
							 (unsigned) base, (unsigned) size,
							 (unsigned) out.iosVersion, (unsigned) out.iosRevision);
			fwrite(hdr, 1, n, f);
			fwrite(buf, 1, size, f);
			fclose(f);
			out.dumpBase = base;
			out.dumpSize = size;
			out.dumpPath = dumpPath;
		}
		free(buf);
	}

	std::string DescribeProbe(const IosProbe &p)
	{
		std::string out;
		out += "\n\ncIOS plugin probe\n-----------------\n";

		if (!p.ahbprot)
		{
			out += "AHBPROT is not open, so IOS memory is hidden from the loader and\n"
				   "nothing could be read. This is the one thing the read hook cannot\n"
				   "work without. Launch the loader from the Homebrew Channel (not from\n"
				   "a forwarder that reloads IOS) so it keeps hardware access.\n";
			return out;
		}

		Addf(out, "AHBPROT   : open, IOS memory is readable\n");
		Addf(out, "running   : IOS%u (rev %u)\n", p.iosVersion, p.iosRevision);
		Addf(out, "scanned   : %08x .. %08x  (%u words, %u non-zero)\n",
			 p.scanFrom, p.scanTo, p.words, p.nonZero);

		//! If almost everything read back as zero we were not actually seeing
		//! IOS, and every "0 hits" below means nothing.
		if (p.words && p.nonZero < p.words / 100)
		{
			out += "\nWARNING: nearly every word read back as zero. That is not what a\n"
				   "running IOS looks like, so these reads were probably refused rather\n"
				   "than empty, and the hit counts below are meaningless.\n";
		}
		out += "\n";

		for (size_t i = 0; i < p.patterns.size(); ++i)
		{
			const IosPattern &pat = p.patterns[i];
			Addf(out, "%-12s %08x  %u hit(s)   %s\n",
				 pat.name, pat.value, (unsigned) pat.hits.size(), pat.what);
			for (size_t k = 0; k < pat.hits.size(); ++k)
				Addf(out, "                 %08x\n", pat.hits[k]);
		}

		//! The headline result.
		out += "\nThe read dispatch that has to be patched\n";
		out += "---------------------------------------\n";
		if (p.patchSites.size() == 1)
		{
			Addf(out, "  FOUND, exactly once, at %08x.\n\n", p.patchSites[0]);
			out += "  That is the answer I was hoping for. The running cIOS is the same\n"
				   "  d2x build the patch was worked out against, so the four bytes go\n"
				   "  exactly where they are meant to. Nothing was written this time.\n";
		}
		else if (p.patchSites.empty())
		{
			out += "  NOT FOUND.\n\n"
				   "  The patch was derived from d2x v11 beta3. Either this cIOS is a\n"
				   "  different build, or its compiler laid the code out differently.\n"
				   "  The dump below is what is needed to re-derive it.\n";
		}
		else
		{
			Addf(out, "  found %u times - ambiguous, so it must not be applied blind:\n",
				 (unsigned) p.patchSites.size());
			for (size_t i = 0; i < p.patchSites.size(); ++i)
				Addf(out, "    %08x\n", p.patchSites[i]);
		}

		if (p.dumpSize)
		{
			Addf(out, "\nWrote %u bytes of the surrounding code from %08x to:\n  %s\n",
				 p.dumpSize, p.dumpBase, p.dumpPath.c_str());
			out += "That file is the missing input. With it the read hook can be written\n"
				   "against the real instructions instead of guessed at. Please send it\n"
				   "along with this log.\n";
		}
		else
		{
			out += "\nNo anchor constant was found, so nothing was dumped. Either this\n"
				   "cIOS is not a d2x, or its read-limit check is built differently.\n";
		}

		return out;
	}
}
