/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
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
#include "RiivoFragPlan.hpp"
#include "RiivoFragBuild.hpp"
#include "usbloader/frag.h"
#include "usbloader/wbfs.h"
#include "settings/CSettings.h"
#include "memory/mem2.h"
#include "usbloader/wdvd.h"
#include "system/IosLoader.h"
#include "libs/libruntimeiospatch/runtimeiospatch.h"
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

	//! Sector size of the drive the backup is on, from SetBootContext.
	static u32 bootSectorSize = 512;

	//! The backup's OWN declared size in sectors, captured before PrepareFragList
	//! inflates it. This has to be kept separately: the inflation raises
	//! frag_list->size all the way to the read ceiling so the cIOS promotes the
	//! disc to dual-layer limits, and reading that field back afterwards would
	//! say the backup fills the entire address space - which is exactly the
	//! condition PlanFragRegion refuses as "dual-layer". Every game would be
	//! refused, single-layer ones included.
	static u32 origImageSectors = 0;

	//! Did the selected options ask for file replacement, and did it actually
	//! happen? A mod that replaces files writes its memory patches on the
	//! assumption that those files are there. Applying the patches without the
	//! files is not a partial success - for a total conversion it is a crash or
	//! an exit to the System Menu, because the patched code goes looking for
	//! assets the disc does not have. If the first is true and the second is
	//! not, the memory patches have to be held back too.
	static bool fileWorkWanted = false;
	static bool fileWorkLive = false;

	//! Set when PrepareFragList deliberately left the fragment list alone, so
	//! the report can say that rather than blaming a missing list.
	static bool fragListUntouched = false;

	//! The cIOS probe and the four-byte patch, both done in SetupDisc rather
	//! than later with everything else.
	//!
	//! Measured on a tester's console: AHBPROT is open when SetupDisc runs and
	//! CLOSED by the time BootPartition does, so a probe from the later window
	//! reads nothing and the patch can never be written. Whatever closes it
	//! between the two, the privileged work has to happen while the access is
	//! still there, so it is done here and the result carried forward.
	//!
	//! Applying the patch before the rest of the plan is known is safe, and is
	//! the same reasoning that already let it be applied before the table was
	//! installed: on its own it only changes how reads at or above 4 GiB are
	//! served, and a game whose file table was never rebuilt does not make any.
	static IosProbe bootProbe;
	static bool patchApplied = false;
	static std::string patchWhy;

	//! Which partition the game is on, looked up in SetupDisc for the same
	//! reason as everything else here.
	//!
	//! WBFS_GetFsInfo goes through WbfsList, and SetupDisc unmounts SD around
	//! set_frag_list - which destroys the Wbfs object for that partition, so
	//! the VALID() test fails afterwards and the lookup returns -1 even though
	//! the game is plainly still there. get_frag_list, a few lines earlier,
	//! uses the identical lookup and succeeds. So ask before the unmount.
	static bool bootFsKnown = false;
	static u8 bootFsType = 0;
	static u32 bootFsLba = 0;

	//! The game's id, needed to ask which partition it lives on.
	static u8 bootGameId[8] = { 0 };

	void SetBootContext(const ResolvedPatchSet *set, const std::string &device,
						const std::string &logPath, u32 sectorSize,
						const u8 *gameId)
	{
		bootSet = set;
		bootDevice = device;
		bootLogPath = logPath;
		bootSectorSize = sectorSize ? sectorSize : 512;
		memset(bootGameId, 0, sizeof(bootGameId));
		if (gameId)
			memcpy(bootGameId, gameId, 6);
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

	static bool ByPlacedOffset(const PlacedFile &a, const PlacedFile &b)
	{
		return a.offset < b.offset;
	}

	//! Ask the builder where each mod file ended up and hand the list back in
	//! ascending offset order, which is what the fragment table and its lookup
	//! index both assume. Files still sitting at their original disc offset were
	//! never placed - their replacement was missing from the card - so they are
	//! left out rather than dragged below the region and tripping the checks.
	static void CollectPlaced(const FstBuilder &builder, u64 region,
							  const std::vector<RedirectSpec> &redirects,
							  const std::vector<CreatedFile> &created,
							  std::vector<PlacedFile> &out)
	{
		out.clear();

		//! Keyed by disc path, because the same disc file can legitimately be
		//! claimed more than once: a mod with overlapping <folder> rules - Newer
		//! SMBW has 38 of them - names some files twice. Both claims resolve to
		//! the same assigned offset, so emitting both put two extents at the
		//! same place and the whole plan was refused with "two files overlap".
		//! Last one wins, matching how AddOrReplace resolved it when the table
		//! was built, so the file that serves the read is the file the game's
		//! table describes.
		std::map<std::string, PlacedFile> byDisc;

		for (size_t i = 0; i < redirects.size() + created.size(); ++i)
		{
			const bool isRedirect = i < redirects.size();
			const std::string &disc = isRedirect
									  ? redirects[i].disc
									  : created[i - redirects.size()].disc;
			const std::string &ext = isRedirect
									 ? redirects[i].external
									 : created[i - redirects.size()].external;
			u64 off = 0;
			u32 len = 0;
			if (!builder.FindAssigned(disc, &off, &len))
				continue;
			if (len == 0 || off < region)
				continue;
			PlacedFile f;
			f.offset = off;
			f.length = len;
			f.external = ext;
			byDisc[disc] = f;
		}

		out.reserve(byDisc.size());
		for (std::map<std::string, PlacedFile>::const_iterator it = byDisc.begin();
			 it != byDisc.end(); ++it)
			out.push_back(it->second);

		std::sort(out.begin(), out.end(), ByPlacedOffset);
	}

	static void ToExtents(const std::vector<PlacedFile> &in,
						  std::vector<ModExtent> &out)
	{
		out.clear();
		out.reserve(in.size());
		for (size_t i = 0; i < in.size(); ++i)
		{
			ModExtent e;
			e.offset = in[i].offset;
			e.length = in[i].length;
			out.push_back(e);
		}
	}

	//! The rebuilt table, waiting for the apploader to finish so it can be put
	//! into the game's memory. Held in MEM2 on purpose: the apploader fills MEM1
	//! with the game and would walk straight over anything parked there.
	static u8 *pendingFst = 0;
	static u32 pendingFstSize = 0;

	//! Register the extended fragment list, prove it reads back correctly, and
	//! only then touch the cIOS. Ordered so that every failure leaves the console
	//! in a state that still boots the game unmodified:
	//!
	//!   - a half-built fragment list is simply never registered;
	//!   - the extended list, if registered, is a superset of the game's own, so
	//!     every read below the mod region is byte-for-byte what it was;
	//!   - the four-byte patch alone changes nothing, because an unmodified file
	//!     table never sends the game above the 4 GiB line;
	//!   - the rebuilt table is stashed for installation ONLY once the patch is
	//!     in, so the game is never pointed at a region nothing serves.
	static void Activate(std::string &out, const FragPlan &plan,
						 const std::vector<PlacedFile> &placed,
						 const std::vector<u8> &newFst)
	{
		if (placed.empty())
		{
			out += "  Nothing was placed, so there is nothing to switch on.\n";
			return;
		}

		//! Looked up back in SetupDisc, before the SD unmount takes the
		//! partition object away.
		const u8 fsType = bootFsType;
		const u32 lba = bootFsLba;
		if (!bootFsKnown)
		{
			out += "  Could not tell which partition the game is on.\n";
			return;
		}

		FragBuildStats fs;
		if (!AppendModFragments(placed, bootSectorSize, fsType, lba, fs))
		{
			Addf(out, "  Could not build the fragments: %s\n", fs.firstFailure.c_str());
			out += "  The list was not registered, so nothing changed.\n";
			return;
		}

		Addf(out, "  located on the drive : %u file(s)", fs.files);
		if (fs.failed)
			Addf(out, ", %u could not be located", fs.failed);
		out += "\n";
		Addf(out, "  fragments            : %u -> %u of %u\n",
			 fs.fragsBefore, fs.fragsAfter, RIIVO_FRAG_MAX);

		const int reg = frag_list_register(Settings.SDMode);
		if (reg < 0)
		{
			Addf(out, "  The cIOS refused the extended list (%d).\n", reg);
			return;
		}

		//! Prove the whole chain before touching any code: read the first mod
		//! file back through the cIOS at the offset the game will ask for.
		std::string why;
		if (!VerifyModFragment(placed[0].offset, placed[0].external, why))
		{
			Addf(out, "  The read-back check failed: %s\n", why.c_str());
			out += "  Nothing was patched. The game boots unmodified.\n";
			return;
		}
		Addf(out, "  read-back check      : passed at 0x%010llx\n",
			 (unsigned long long) placed[0].offset);

		//! The patch itself went in back in SetupDisc; this only records that it
		//! is in place before the table that depends on it is installed.
		Addf(out, "  cIOS read patch      : already applied at %08x\n",
			 bootProbe.patchSites[0]);

		//! Last: hold on to the rebuilt table. Installing it is what actually
		//! points the game at the mod, and it can only happen once the apploader
		//! has run and said where the table lives.
		pendingFst = (u8 *) MEM2_alloc(newFst.size());
		if (!pendingFst)
		{
			out += "  Out of memory for the rebuilt table, so it will not be\n"
				   "  installed. The patch above is harmless on its own.\n";
			return;
		}
		memcpy(pendingFst, &newFst[0], newFst.size());
		pendingFstSize = (u32) newFst.size();

		Addf(out, "  rebuilt table        : %u bytes held, ready to install\n",
			 pendingFstSize);
		out += "\n  Riivolution is ON for this boot.\n";
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

		//! From here on the mod is one that needs its files. If they do not end
		//! up installed, the memory patches must not be applied either.
		fileWorkWanted = true;

		std::string out;
		out += "\n\nFile and folder replacement\n"
			   "---------------------------\n"
			   "Worked out against the real disc. If every check below passes it is\n"
			   "switched on at the end of this section; if any one fails, nothing is\n"
			   "applied and the game boots untouched.\n\n";

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

		//! The mod region has to clear two floors: the 4 GiB threshold the
		//! four-byte read patch tests (RiivoDiPatch.hpp), and the end of the
		//! backup's own virtual disc, or the game's fragments would shadow the
		//! mod's. PlanRegionStart takes the higher of the two.
		const FragList *gameFrags = frag_list_get();
		//! Use the size captured before the reservation, never gameFrags->size,
		//! which by now reads back as the whole virtual disc. Fall back to the
		//! live field only when no reservation was made.
		const u32 imageSectors = origImageSectors ? origImageSectors
												  : (gameFrags ? gameFrags->size : 0);
		const u64 imageBytes = gameFrags
							   ? (u64) imageSectors * bootSectorSize
							   : 0;
		const u64 extent = builder.OriginalExtent();
		const u64 region = PlanRegionStart(imageBytes, bootSectorSize);
		const bool extentFits = extent < region;
		//! Align to the drive's own sectors, never to less: a fragment cannot
		//! begin part-way through one. 2 KB is the floor because that is a Wii
		//! disc's own granularity, but a 4K-native drive needs 4 KB and would
		//! otherwise have every file rejected by the alignment check later.
		const u32 layoutAlign = bootSectorSize > 0x800 ? bootSectorSize : 0x800;
		builder.Layout(region, layoutAlign);

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

		// ------------------------------------------------------------------
		// Does the mod fit on the virtual disc the cIOS reads?
		// ------------------------------------------------------------------
		out += "\nRoom on the virtual disc\n";
		out += "------------------------\n";

		FragPlan plan;
		std::vector<ModExtent> extents;
		std::vector<PlacedFile> placed;

		if (fragListUntouched)
		{
			out += "  SKIPPED: the loader was not given hardware access (AHBPROT), so\n"
				   "  the cIOS cannot be patched and file replacement is impossible on\n"
				   "  this boot. The fragment list was therefore left exactly as it\n"
				   "  was, and the game is being read precisely as stock USB Loader GX\n"
				   "  reads it. Everything measured above is a dry run.\n\n"
				   "  Launch USB Loader GX from the Homebrew Channel directly - not\n"
				   "  from a forwarder channel, and not from anything that reloads IOS\n"
				   "  on the way in - and this will run for real.\n\n";
		}
		else if (!gameFrags)
		{
			out += "  The loader did not build a fragment list for this game, so there\n"
				   "  is no virtual disc to extend. That happens when the game is read\n"
				   "  straight off a real DVD, which this cannot work with.\n\n";
		}
		else
		{
			Addf(out, "  backup declares    : %u sectors of %u bytes = %llu bytes\n",
				 imageSectors, bootSectorSize, (unsigned long long) imageBytes);
			Addf(out, "  reserved up to     : %u sectors (for the dual-layer probe)\n",
				 gameFrags->size);
			Addf(out, "  its fragments      : %u of %u\n",
				 gameFrags->num, RIIVO_FRAG_MAX);

			//! Feed the laid-out files through the same checks that will gate
			//! the real thing: ordering, alignment, the read ceiling, the table.
			CollectPlaced(builder, region, redirects, created, placed);
			ToExtents(placed, extents);

			plan = PlanFragRegion(imageBytes, bootSectorSize, gameFrags->num, extents);

			if (!plan.ok)
			{
				Addf(out, "  REFUSED: %s\n", plan.why.c_str());
			}
			else
			{
				Addf(out, "  mod region         : 0x%010llx .. 0x%010llx\n",
					 (unsigned long long) plan.regionStart,
					 (unsigned long long) plan.regionEnd);
				Addf(out, "  files to place     : %u\n", plan.files);
				Addf(out, "  fragments needed   : %u at best, %u free in the table\n",
					 plan.minFragments, plan.fragsAvailable);
				Addf(out, "  payload            : %llu bytes\n",
					 (unsigned long long) plan.payloadBytes);
				Addf(out, "  spare below ceiling: %llu bytes\n",
					 (unsigned long long) plan.ceilingSpare);
				out += "  Everything fits.\n";
			}
			out += "\n";
		}

		//! The probe and the patch already happened, back in SetupDisc, because
		//! that is the last point where the access to do them is guaranteed.
		out += DescribeProbe(bootProbe);
		if (patchApplied)
			Addf(out, "\n  The cIOS read patch was applied early, at %08x.\n",
				 bootProbe.patchSites[0]);
		else if (!patchWhy.empty())
			Addf(out, "\n  The cIOS read patch was NOT applied: %s\n", patchWhy.c_str());

		// ------------------------------------------------------------------
		// Switch it on, but only if every single check above came back clean.
		// ------------------------------------------------------------------
		out += "\nSwitching it on\n";
		out += "---------------\n";

		if (!(extentFits && plan.ok && gameFrags && patchApplied))
		{
			out += "  Not attempted - one of the checks above did not pass. The game\n"
				   "  boots exactly as it would without Riivolution.\n";
		}
		else
		{
			Activate(out, plan, placed, newFst);
		}

		out += "\nHow this works\n";
		out += "--------------\n";
		out += "The fragment list the loader gives the cIOS serves the RAW backup, which\n"
			   "keeps the game partition encrypted exactly as it was pressed, and the IOS\n"
			   "disc code decrypts whatever comes back. A fragment aimed at a plaintext\n"
			   "file on the card would decrypt into noise - which is why this needs a\n"
			   "change inside IOS rather than only in the loader.\n\n"
			   "That change is four bytes: the read handler's test for 'is this image\n"
			   "already decrypted' becomes a test for 'is this read at or above 4 GiB'.\n"
			   "Mod files live above that line and are served raw from the fragment list,\n"
			   "with no decryption and no hash check. Everything below the line is real\n"
			   "game data and is read exactly as it always was.\n\n"
			   "The patch is made in memory only. Nothing is installed on your console\n"
			   "and nothing survives a reboot.\n";

		AppendLog(out);
		gprintf("Riivo: plan - %u redirect, %u new, %u entries, fst %u bytes\n",
				(unsigned) redirects.size(), (unsigned) created.size(),
				st.entryCount, st.fstSize);
	}

	// --------------------------------------------------------------------
	// 3. Where the rebuilt table would go in the game's memory
	// --------------------------------------------------------------------

	bool FileWorkIncomplete()
	{
		return fileWorkWanted && !fileWorkLive;
	}

	void PrepareFragList()
	{
		if (!bootSet)
			return;
		if (bootSet->files.empty() && bootSet->folders.empty())
			return;

		//! Everything below changes the fragment list the cIOS serves the game
		//! through, and it happens here - in SetupDisc - long before the checks
		//! in PrepareFileRedirects can say whether the mod is going to be
		//! applied at all. There is no way to take it back afterwards: by then
		//! the list has been handed over.
		//!
		//! So do not touch it unless file replacement can actually happen. The
		//! one thing that can be tested this early is hardware access: without
		//! AHBPROT the cIOS patch cannot be made, so the feature is impossible
		//! no matter what else lines up, and the game should be booted exactly
		//! as stock USB Loader GX would boot it.
		if (!AHBPROT_DISABLED)
		{
			fragListUntouched = true;
			gprintf("Riivo: no AHBPROT, leaving the fragment list alone\n");
			return;
		}

		//! Keep the list alive: set_frag_list frees it the moment it has handed
		//! it to the cIOS, and the mod's fragments cannot be worked out until
		//! later, when the partition is open.
		frag_list_retain(1);

		//! Record what the backup says about itself BEFORE the reservation
		//! below overwrites it. PrepareFileRedirects needs the real figure to
		//! work out where the mod region can start.
		const FragList *before = frag_list_get();
		origImageSectors = before ? before->size : 0;

		//! And which partition it is on, while the partition object still
		//! exists - SetupDisc unmounts SD a few lines below.
		bootFsKnown = (WBFS_GetFsInfo(bootGameId, &bootFsType, &bootFsLba) >= 0);
		gprintf("Riivo: partition lookup %s (fs %u, lba %u)\n",
				bootFsKnown ? "ok" : "FAILED", bootFsType, bootFsLba);

		//! Declare a virtual disc big enough to reach the dual-layer probe point.
		//! The cIOS decides the disc type by trying to read near the second
		//! layer; inside the declared size that read comes back as zeros and
		//! succeeds, so the disc is taken for dual-layer and the read ceiling
		//! goes from 4.7 GB to 8.5 GB - which is the only reason there is room
		//! above the backup for the mod at all. No fragments are added here, so
		//! this costs nothing on the drive.
		const u32 sector = bootSectorSize ? bootSectorSize : 512;
		const u32 sectors = (u32) (RIIVO_READ_CEILING / sector);
		const int ret = frag_list_reserve(sectors);

		gprintf("Riivo: reserved %u sectors of virtual disc (%d)\n", sectors, ret);

		//! Find the cIOS read handler and patch it now, while the access to do
		//! it still exists. The card is mounted at this point - SetupDisc
		//! unmounts it a few lines further on - so the dump can be written too.
		const std::string dumpPath = bootDevice + "/riivolution/usbloadergx_riivo_dip.bin";
		ProbeIosPlugin(dumpPath, bootProbe);

		if (bootProbe.patchSites.size() == 1)
		{
			patchApplied = ApplyDiPatch(bootProbe.patchSites[0], patchWhy);
			gprintf("Riivo: early cIOS patch at %08x: %s\n",
					bootProbe.patchSites[0],
					patchApplied ? "applied" : patchWhy.c_str());
		}
		else
		{
			patchWhy = bootProbe.patchSites.empty()
					   ? "the patch site was not found in the running cIOS"
					   : "the patch site was found more than once, which is not expected";
		}
	}

	void ReportFstPlacement()
	{
		if (!bootSet)
			return;

		//! The size that matters is the table actually held for installation;
		//! fall back to the planned size when nothing was switched on, so the
		//! report still says where it would have gone.
		const u32 want = pendingFstSize ? pendingFstSize : plannedFstSize;
		//! Nothing was planned - the section above gave up before it got that
		//! far. Still say what happens to the memory patches, because for a
		//! file-replacing mod they are now being held back and the log would
		//! otherwise stop without explaining why the game booted clean.
		if (want == 0 && FileWorkIncomplete())
		{
			AppendLog("\n\nMemory patches\n"
					  "--------------\n"
					  "  HELD BACK. This mod replaces files, the plan above did not\n"
					  "  complete, so the memory patches are skipped too. Applying\n"
					  "  them without the mod's files is what makes a game exit to\n"
					  "  the System Menu. The game boots completely unmodified.\n");
			return;
		}
		if (want == 0)
			return;

		const ArenaInfo arena = ReadArenaInfo();
		const FstPlacement place = PlaceFst(arena, want, 32);

		std::string out;
		out += "\n\nWhere the rebuilt table would go\n";
		out += "--------------------------------\n";
		out += "Read from the boot-info block the apploader has just filled in.\n\n";

		Addf(out, "  arena low    : %08x\n", arena.arenaLo);
		Addf(out, "  arena high   : %08x\n", arena.arenaHi);
		Addf(out, "  table now at : %08x, %u bytes reserved\n",
			 arena.fstAddr, arena.fstMaxSize);
		Addf(out, "  rebuilt size : %u bytes\n\n", want);

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

		//! This is the step that actually points the game at the mod. It only
		//! runs when the fragment list, the read-back check and the cIOS patch
		//! all succeeded earlier - otherwise pendingFst was never filled in, and
		//! the game boots with its own table exactly as it always did.
		if (pendingFst && pendingFstSize && place.ok)
		{
			if (InstallFst(place, pendingFst, pendingFstSize))
			{
				fileWorkLive = true;
				out += "\n  Installed. The game will read the mod's files.\n";
			}
			else
				out += "\n  The table could not be written, so the game boots with its\n"
					   "  own. The cIOS patch is harmless on its own.\n";
		}
		else if (pendingFst)
		{
			out += "\n  Held a rebuilt table but could not place it, so it was not\n"
				   "  installed. The game boots unmodified.\n";
		}

		//! Say plainly what this means for the rest of the boot. A mod whose
		//! files did not make it must not get its memory patches either.
		out += "\n\nMemory patches\n";
		out += "--------------\n";
		if (FileWorkIncomplete())
		{
			out += "  HELD BACK. This mod replaces files, and those files were not\n"
				   "  installed, so its memory patches are being skipped as well.\n"
				   "  They are written on the assumption that the mod's files are\n"
				   "  present - applying them on their own is what makes a game exit\n"
				   "  to the System Menu instead of booting. The game now boots\n"
				   "  completely unmodified, which is the safe outcome.\n"
				   "  Fix whatever the section above refused and they come back.\n";
		}
		else if (fileWorkWanted)
			out += "  Applied, alongside the mod's files.\n";
		else
			out += "  Applied. This mod does not replace any files.\n";

		AppendLog(out);
		gprintf("Riivo: placement %s (%08x, %u bytes), memory patches %s\n",
				place.ok ? "ok" : "refused", place.fstAddr, want,
				FileWorkIncomplete() ? "held back" : "applied");
	}
}
