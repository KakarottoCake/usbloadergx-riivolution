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
	// 2. Disc FST + redirect dry run
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
		out += "\n\nPhase 3 dry run - file/folder replacement\n"
			   "-----------------------------------------\n"
			   "Working out what WOULD be redirected. None of it is applied yet: the\n"
			   "IOS-side disc-read hook that carries it out is still being written.\n\n";

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
		free(fstData);

		if (!parsed)
		{
			out += "\nThe FST could not be parsed, so no redirect could be worked out.\n";
			AppendLog(out);
			return;
		}

		Addf(out, "patches  : %u <file>, %u <folder>\n\n",
			 (unsigned) bootSet->files.size(), (unsigned) bootSet->folders.size());

		FsDirLister lister;
		std::vector<RedirectSpec> redirects;
		std::vector<std::string> created;
		BuildRedirects(fst, *bootSet, bootDevice, &lister, redirects, &created);

		//! Size accounting. This is the question that decides whether plain
		//! redirection can ever work for this mod: a replacement that is bigger
		//! than the file it stands in for cannot just be read in place, because
		//! the disc's file table still advertises the old, smaller length.
		int missing = 0, fits = 0, grows = 0;
		u64 maxDiscOffset = 0;
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
			if (extSize > r.discLength)
			{
				++grows;
				growers.push_back(i);
			}
			else
				++fits;
		}

		Addf(out, "external files seen : %u\n", (unsigned) (redirects.size() + created.size()));
		Addf(out, "  matched on disc   : %u\n", (unsigned) redirects.size());
		Addf(out, "  no disc entry     : %u\n", (unsigned) created.size());
		Addf(out, "  metadata ignored  : %d  (macOS ._ twins, .DS_Store, Thumbs.db)\n\n",
			 lister.skipped);

		out += "Replacement size vs the file it replaces\n";
		out += "---------------------------------------\n";
		Addf(out, "  same size or smaller : %d\n", fits);
		Addf(out, "  LARGER than original : %d\n", grows);
		Addf(out, "  missing from card    : %d\n", missing);
		Addf(out, "  highest disc offset  : 0x%010llx\n\n", (unsigned long long) maxDiscOffset);

		if (grows > 0)
		{
			out += "A replacement bigger than the original cannot simply be read in its\n"
				   "place: the disc's file table still advertises the old length, so the\n"
				   "game would only ever ask for that many bytes. Making these work needs\n"
				   "the in-RAM file-table rebuild (Phase 4), not just read redirection.\n\n";
			out += "biggest growers:\n";
			for (size_t n = 0; n < growers.size() && n < 15; ++n)
			{
				const RedirectSpec &r = redirects[growers[n]];
				u32 extSize = 0;
				ExternalFileSize(r.external, &extSize);
				Addf(out, "  disc %8u -> ext %8u  %s\n",
					 (unsigned) r.discLength, (unsigned) extSize, r.external.c_str());
			}
			if (growers.size() > 15)
				Addf(out, "  ... and %u more\n", (unsigned) (growers.size() - 15));
			out += "\n";
		}

		out += "Sample of the redirects that would be applied:\n";
		for (size_t i = 0; i < redirects.size() && i < 25; ++i)
		{
			const RedirectSpec &r = redirects[i];
			u32 extSize = 0;
			const bool have = ExternalFileSize(r.external, &extSize);
			Addf(out, "  disc 0x%010llx len %-8u <- %-8u %s%s\n",
				 (unsigned long long) r.discOffset, (unsigned) r.discLength, (unsigned) extSize,
				 r.external.c_str(), have ? "" : "   [MISSING ON CARD]");
		}
		if (redirects.size() > 25)
			Addf(out, "  ... and %u more\n", (unsigned) (redirects.size() - 25));

		if (!created.empty())
		{
			Addf(out, "\n%u external file(s) have no entry on the disc at all - these are\n"
					  "files the mod ADDS. Adding files needs the Phase 4 file-table\n"
					  "rebuild, so they would be ignored by redirection alone:\n",
				 (unsigned) created.size());
			for (size_t i = 0; i < created.size() && i < 15; ++i)
				Addf(out, "  %s\n", created[i].c_str());
			if (created.size() > 15)
				Addf(out, "  ... and %u more\n", (unsigned) (created.size() - 15));
		}

		if (missing > 0)
			Addf(out, "\nWARNING: %d redirect target(s) are not on the card. Check that the\n"
					  "mod's files were copied to the same device as the XML.\n", missing);

		if (redirects.empty() && created.empty())
			out += "\nNothing resolved. Either the mod's disc paths do not match this game,\n"
				   "or its files are not where the XML expects them.\n";
		else
			out += "\nThe loader side works. What is still missing is the IOS-side read hook\n"
				   "that would make the console fetch those ranges from the card at runtime.\n";

		AppendLog(out);
		gprintf("Riivo: dry run - %u redirect(s), %u new, %d grow, %d missing\n",
				(unsigned) redirects.size(), (unsigned) created.size(), grows, missing);
	}
}
