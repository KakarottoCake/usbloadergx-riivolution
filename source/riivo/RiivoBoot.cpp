/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogcsys.h>

#include "RiivoBoot.hpp"
#include "RiivoConfig.hpp"
#include "RiivoFst.hpp"
#include "RiivoFile.hpp"
#include "RiivoFstBuild.hpp"
#include "RiivoFstInstall.hpp"
#include "RiivoIosProbe.hpp"
#include "RiivoDiPatch.hpp"
#include "usbloader/wdvd.h"
#include "system/IosLoader.h"
#include "gecko.h"

//! Built once at startup by IosLoader::GetD2XInfo().
extern std::vector<struct d2x> d2x_list;

namespace Riivo
{
	// --------------------------------------------------------------------
	// Boot context. BootPartition is a static with a fixed signature and is
	// called exactly once per boot, so parking the context here is simpler
	// than threading two more arguments through it.
	// --------------------------------------------------------------------

	static const ResolvedPatchSet *bootSet = 0;
	static std::string bootDevice;
	static std::string bootLogPath;

	//! Size of the table PrepareFileRedirects worked out, carried across to
	//! ReportFstPlacement - which runs later, after the apploader, and needs to
	//! know how much room the rebuilt table wants.
	static u32 plannedFstSize = 0;

	void SetBootContext(const ResolvedPatchSet *set, const std::string &device,
						const std::string &logPath)
	{
		bootSet = set;
		bootDevice = device;
		bootLogPath = logPath;
	}

	void AppendLog(const std::string &text)
	{
		if (bootLogPath.empty() || text.empty())
			return;
		FILE *f = fopen(bootLogPath.c_str(), "a");
		if (!f)
		{
			gprintf("Riivo: log append failed (%s)\n", bootLogPath.c_str());
			return;
		}
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
	}

	//! Small printf-into-std::string helper; the reports are short.
	static void Addf(std::string &out, const char *fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		out += buf;
	}

	// --------------------------------------------------------------------
	// 1. cIOS survey
	// --------------------------------------------------------------------

	void ReportCios()
	{
		std::string out;
		out += "\n\ncIOS survey\n-----------\n";
		Addf(out, "running under: IOS%d (rev %d)\n",
			 (int) IOS_GetVersion(), (int) IOS_GetRevision());
		out += "That is the cIOS this game was told to use, and the one the\n"
			   "file-replacement read hook will have to patch.\n\n";

		//! What the loader itself found at startup. GetD2XInfo() builds this with
		//! ISFS up, so it is the authoritative answer to "which slots hold a d2x".
		out += "d2x cIOS the loader detected at startup:\n";
		if (d2x_list.empty())
			out += "  (none - the loader found no d2x cIOS in any slot)\n";
		for (size_t i = 0; i < d2x_list.size(); ++i)
			Addf(out, "  slot %3d : d2x, base IOS%d%s\n",
				 (int) d2x_list[i].slot, (int) d2x_list[i].base,
				 d2x_list[i].slot == IOS_GetVersion() ? "   <== in use" : "");

		out += "\nEvery slot that publishes a cIOS info block:\n";

		//! GetIOSInfo reads the info block out of NAND, so ISFS has to be up -
		//! GetD2XInfo() does exactly the same around its own loop. Without it
		//! every slot silently returns NULL and the survey reports nothing.
		ISFS_Initialize();
		int found = 0;
		for (s32 slot = 200; slot <= 254; ++slot)
		{
			iosinfo_t *info = IosLoader::GetIOSInfo(slot);
			if (!info)
				continue;

			//! name/versionstring are fixed-size fields, not guaranteed terminated.
			char name[0x11], vers[0x11];
			memcpy(name, info->name, 0x10);          name[0x10] = 0;
			memcpy(vers, info->versionstring, 0x10); vers[0x10] = 0;

			Addf(out, "  slot %3d : %-8s v%-3u base IOS%-3u  %s%s\n",
				 (int) slot, name, (unsigned) info->version, (unsigned) info->baseios, vers,
				 slot == IOS_GetVersion() ? "   <== in use" : "");
			free(info);
			++found;
		}

		ISFS_Deinitialize();

		if (found == 0)
			out += "  (none - no slot in 200-254 carries a cIOS info block)\n";

		out += "\nA slot with no line above either holds no title at all, or holds a cIOS\n"
			   "that does not publish the d2x info block - a Hermes cIOS or a custom\n"
			   "build, say. Those are the ones the read hook may not recognise.\n";

		AppendLog(out);
		gprintf("Riivo: cIOS survey written (%d slot(s) with info)\n", found);
	}

	// --------------------------------------------------------------------
	// 2. Disc FST, redirect plan and file-table rebuild
	// --------------------------------------------------------------------

	static u32 be32(const u8 *p)
	{
		return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
	}

	//! Read the FST off the currently open partition. Caller frees *outData.
	static bool ReadDiscFst(u8 **outData, u32 *outSize, u32 *outOffset, std::string &err)
	{
		*outData = 0;
		*outSize = 0;
		*outOffset = 0;

		//! boot.bin: 0x420 dol offset, 0x424 FST offset, 0x428 FST size.
		//! All three are stored >>2 on a Wii disc.
		static u8 hdr[0x20] ATTRIBUTE_ALIGN(32);
		s32 ret = WDVD_Read(hdr, sizeof(hdr), 0x420);
		if (ret < 0)
		{
			err = "could not read boot.bin";
			return false;
		}

		const u32 fstOffset = be32(hdr + 0x04) << 2;
		const u32 fstSize = be32(hdr + 0x08) << 2;

		if (fstOffset == 0 || fstSize == 0 || fstSize > 0x00800000)
		{
			err = "boot.bin gave an implausible FST offset/size";
			return false;
		}

		const u32 readSize = (fstSize + 31) & ~31u;
		u8 *fst = (u8 *) memalign(32, readSize);
		if (!fst)
		{
			err = "out of memory for the FST";
			return false;
		}

		ret = WDVD_Read(fst, readSize, fstOffset);
		if (ret < 0)
		{
			free(fst);
			err = "could not read the FST from the disc";
			return false;
		}

		*outData = fst;
		*outSize = fstSize;
		*outOffset = fstOffset;
		return true;
	}

	static bool ExternalFileSize(const std::string &path, u32 *outSize)
	{
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			return false;
		*outSize = (u32) st.st_size;
		return true;
	}

	void PrepareFileRedirects()
	{
		if (!bootSet)
			return;
		//! Nothing to do for a mod that only uses <memory>/<savegame>.
		if (bootSet->files.empty() && bootSet->folders.empty())
			return;

		std::string out;
		out += "\n\nPhase 3 - file/folder replacement plan\n"
			   "--------------------------------------\n"
			   "Worked out against the real disc, then discarded. Nothing below is\n"
			   "applied yet - this run is checking that the pieces line up on your\n"
			   "console before anything is switched on. The note at the end says what\n"
			   "is left.\n\n";

		u8 *fstData = 0;
		u32 fstSize = 0, fstOffset = 0;
		std::string err;
		if (!ReadDiscFst(&fstData, &fstSize, &fstOffset, err))
		{
			Addf(out, "FAILED: %s\n", err.c_str());
			AppendLog(out);
			return;
		}

		Fst fst;
		const bool parsed = fst.Parse(fstData, fstSize, true);
		Addf(out, "disc FST : offset 0x%08x, %u bytes, %s, %u file(s)\n",
			 fstOffset, fstSize, parsed ? "parsed OK" : "PARSE FAILED",
			 (unsigned) fst.FileCount());

		if (!parsed)
		{
			free(fstData);
			out += "\nThe FST could not be parsed, so no plan could be worked out.\n";
			AppendLog(out);
			return;
		}

		Addf(out, "patches  : %u <file>, %u <folder>\n\n",
			 (unsigned) bootSet->files.size(), (unsigned) bootSet->folders.size());

		FsDirLister lister;
		std::vector<RedirectSpec> redirects;
		std::vector<CreatedFile> created;
		BuildRedirects(fst, *bootSet, bootDevice, &lister, redirects, &created);

		//! Size accounting. A replacement bigger than the file it stands in for
		//! cannot be served by redirection alone: the file table still advertises
		//! the old length, so the game never asks for the extra bytes.
		int missing = 0, fits = 0, grows = 0;
		u64 maxDiscOffset = 0, modBytes = 0;
		std::vector<size_t> growers;

		for (size_t i = 0; i < redirects.size(); ++i)
		{
			const RedirectSpec &r = redirects[i];
			if (r.discOffset > maxDiscOffset)
				maxDiscOffset = r.discOffset;

			u32 extSize = 0;
			if (!ExternalFileSize(r.external, &extSize))
			{
				++missing;
				continue;
			}
			modBytes += extSize;
			if (extSize > r.discLength)
			{
				++grows;
				growers.push_back(i);
			}
			else
				++fits;
		}

		Addf(out, "external files seen : %u\n",
			 (unsigned) (redirects.size() + created.size()));
		Addf(out, "  matched on disc   : %u\n", (unsigned) redirects.size());
		Addf(out, "  no disc entry     : %u  (files the mod ADDS)\n",
			 (unsigned) created.size());
		Addf(out, "  metadata ignored  : %d  (macOS ._ twins, .DS_Store, Thumbs.db)\n\n",
			 lister.skipped);

		out += "Replacement size vs the file it replaces\n";
		out += "---------------------------------------\n";
		Addf(out, "  same size or smaller : %d\n", fits);
		Addf(out, "  LARGER than original : %d\n", grows);
		Addf(out, "  missing from card    : %d\n", missing);
		Addf(out, "  highest disc offset  : 0x%010llx\n\n",
			 (unsigned long long) maxDiscOffset);

		if (grows > 0)
		{
			out += "biggest growers:\n";
			for (size_t n = 0; n < growers.size() && n < 10; ++n)
			{
				const RedirectSpec &r = redirects[growers[n]];
				u32 extSize = 0;
				ExternalFileSize(r.external, &extSize);
				Addf(out, "  disc %8u -> ext %8u  %s\n",
					 (unsigned) r.discLength, (unsigned) extSize, r.external.c_str());
			}
			if (growers.size() > 10)
				Addf(out, "  ... and %u more\n", (unsigned) (growers.size() - 10));
			out += "\n";
		}

		if (missing > 0)
			Addf(out, "WARNING: %d target(s) are not on the card. Check that the mod's\n"
					  "files were copied to the same device as the XML.\n\n", missing);

		// ------------------------------------------------------------------
		// The rebuilt file table. Every route to working replacement needs
		// this, so measure it against the real disc rather than guessing.
		// ------------------------------------------------------------------
		out += "Rebuilt file table\n";
		out += "------------------\n";

		FstBuilder builder;
		if (!builder.Parse(fstData, fstSize, true))
		{
			out += "  the FST would not parse into an editable tree.\n";
			free(fstData);
			AppendLog(out);
			return;
		}
		free(fstData);
		fstData = 0;

		u32 planned = 0, rejected = 0;
		bool isNew = false;
		for (size_t i = 0; i < redirects.size(); ++i)
		{
			u32 extSize = 0;
			if (!ExternalFileSize(redirects[i].external, &extSize))
				continue;
			if (builder.AddOrReplace(redirects[i].disc, extSize, &isNew))
				++planned;
			else
				++rejected;
		}
		for (size_t i = 0; i < created.size(); ++i)
		{
			u32 extSize = 0;
			if (!ExternalFileSize(created[i].external, &extSize))
				continue;
			modBytes += extSize;
			if (builder.AddOrReplace(created[i].disc, extSize, &isNew))
				++planned;
			else
				++rejected;
		}

		//! The mod region has to start at 4 GiB, because that is the only
		//! threshold the four-byte read patch can express - see RiivoDiPatch.hpp.
		//! Everything below it stays real, decrypted, hash-checked game data.
		const u64 extent = builder.OriginalExtent();
		const u64 region = RIIVO_REGION_BYTES;
		const bool extentFits = extent < region;
		builder.Layout(region, 0x800);

		std::vector<u8> newFst;
		builder.Serialize(newFst, true);
		const FstBuildStats &st = builder.Stats();
		plannedFstSize = st.fstSize;

		Addf(out, "  entries planned    : %u  (%u rejected)\n", planned, rejected);
		Addf(out, "  replaced / added   : %u / %u  (+%u new directories)\n",
			 st.replaced, st.added, st.addedDirs);
		Addf(out, "  table entries      : %u  (disc listed %u file(s))\n",
			 st.entryCount, (unsigned) fst.FileCount());
		Addf(out, "  table size         : %u bytes, was %u  (%+d)\n",
			 st.fstSize, fstSize, (int) st.fstSize - (int) fstSize);
		Addf(out, "  disc data ends at  : 0x%010llx  (%s)\n",
			 (unsigned long long) extent,
			 extentFits ? "below the 4 GiB line, good"
						: "AT OR ABOVE 4 GiB - THIS GAME CANNOT BE PATCHED THIS WAY");
		Addf(out, "  mod relocated to   : 0x%010llx .. 0x%010llx\n",
			 (unsigned long long) region, (unsigned long long) st.highestOffset);
		Addf(out, "  headroom below     : %llu bytes spare before the line\n",
			 (unsigned long long) (extentFits ? region - extent : 0));
		Addf(out, "  mod payload        : %llu bytes\n", (unsigned long long) modBytes);

		//! d2x refuses reads past the disc-type limit (dip.h). Those constants are
		//! word offsets, hence the <<2 here.
		const u64 dvd5 = 0x46090000ULL << 2;
		const u64 dvd9 = 0x7ED38000ULL << 2;
		Addf(out, "  DVD5 read ceiling  : 0x%010llx  %s\n", (unsigned long long) dvd5,
			 st.highestOffset <= dvd5 ? "(fits)" : "(EXCEEDED)");
		Addf(out, "  DVD9 read ceiling  : 0x%010llx  %s\n", (unsigned long long) dvd9,
			 st.highestOffset <= dvd9 ? "(fits)" : "(EXCEEDED)");

		//! FRAG_MAX in the cIOS is 20000 fragments for the whole virtual disc,
		//! shared with the game image itself. A contiguous external file costs one
		//! fragment; a fragmented one costs more.
		Addf(out, "  fragment budget    : %u file(s) need at least %u of 20000 slots\n",
			 planned, planned);
		if (planned > 15000)
			out += "  WARNING: that is close to the cIOS fragment limit.\n";

		//! Go looking for the cIOS code that has to be patched. This is the one
		//! input that cannot be worked out anywhere but on the console itself.
		{
			std::string dumpPath = bootDevice + "/riivolution/usbloadergx_riivo_dip.bin";
			IosProbe probe;
			ProbeIosPlugin(dumpPath, probe);
			out += DescribeProbe(probe);
		}

		out += "\nWhat is left\n";
		out += "------------\n";
		out += "The fragment list the loader gives the cIOS serves the RAW backup, which\n"
			   "keeps the game partition encrypted exactly as it was pressed, and the IOS\n"
			   "disc code decrypts whatever comes back. A fragment aimed at a plaintext\n"
			   "file on the card would decrypt into noise - which is why this needs a\n"
			   "change inside IOS rather than only in the loader.\n\n"
			   "That change is now known, and it is four bytes: the read handler's test\n"
			   "for 'is this image already decrypted' becomes a test for 'is this read at\n"
			   "or above 4 GiB'. Mod files live above that line and are served raw from\n"
			   "the fragment list, with no decryption and no hash check. Everything below\n"
			   "the line is real game data and is read exactly as it always was.\n\n"
			   "Still to build: extending the fragment list to cover the relocated files,\n"
			   "and writing the rebuilt table into the game's memory. Both are measured\n"
			   "above. Nothing is written to your console by this build.\n";

		AppendLog(out);
		gprintf("Riivo: plan - %u redirect, %u new, %u entries, fst %u bytes\n",
				(unsigned) redirects.size(), (unsigned) created.size(),
				st.entryCount, st.fstSize);
	}

	// --------------------------------------------------------------------
	// 3. Where the rebuilt table would go in the game's memory
	// --------------------------------------------------------------------

	void ReportFstPlacement()
	{
		if (!bootSet || plannedFstSize == 0)
			return;

		const ArenaInfo arena = ReadArenaInfo();
		const FstPlacement place = PlaceFst(arena, plannedFstSize, 32);

		std::string out;
		out += "\n\nWhere the rebuilt table would go\n";
		out += "--------------------------------\n";
		out += "Read from the boot-info block the apploader has just filled in.\n\n";

		Addf(out, "  arena low    : %08x\n", arena.arenaLo);
		Addf(out, "  arena high   : %08x\n", arena.arenaHi);
		Addf(out, "  table now at : %08x, %u bytes reserved\n",
			 arena.fstAddr, arena.fstMaxSize);
		Addf(out, "  rebuilt size : %u bytes\n\n", plannedFstSize);

		if (!place.ok)
		{
			Addf(out, "  REFUSED: %s\n", place.why.c_str());
			out += "\n  Nothing would be written. Refusing is the right outcome here -\n"
				   "  a wrong address writes over the running game and shows up as a\n"
				   "  hang with nothing on screen.\n";
		}
		else if (place.inPlace)
		{
			Addf(out, "  fits in the room the apploader already set aside (%u bytes spare)\n",
				 arena.fstMaxSize - plannedFstSize);
			out += "  Nothing would move and the game's heap would be untouched.\n";
		}
		else
		{
			Addf(out, "  would go at  : %08x  (extended downwards)\n", place.fstAddr);
			Addf(out, "  arena high   : %08x -> %08x\n", arena.arenaHi, place.newArenaHi);
			Addf(out, "  taken from the game's heap : %u KB\n", place.reserved / 1024);
			Addf(out, "  heap the game still has    : %u MB\n",
				 place.heapLeft / (1024 * 1024));
		}

		AppendLog(out);
		gprintf("Riivo: placement %s (%08x, %u bytes)\n",
				place.ok ? "ok" : "refused", place.fstAddr, plannedFstSize);
	}
}
