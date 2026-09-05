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
#include "Controls/DeviceHandler.hpp"
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

	//! Why, when it is not the missing hardware access the report assumes by
	//! default. Empty means AHBPROT.
	static std::string fragRefusal;

	//! Where the GAME's own fragments ended, captured in SetupDisc before the
	//! mod's were appended to the same list.
	static u64 origMappedEnd = 0;

	//! The cIOS probe and the LOW_READ hook, both done in SetupDisc rather
	//! than later with everything else.
	//!
	//! Measured on a tester's console: AHBPROT is open when SetupDisc runs and
	//! CLOSED by the time BootPartition does, so a probe from the later window
	//! reads nothing and the patch can never be written. Whatever closes it
	//! between the two, the privileged work has to happen while the access is
	//! still there, so it is done here and the result carried forward.
	//!
	//! Applying the hook before the rest of the plan is known is safe, and is
	//! the same reasoning that already let it be applied before the table was
	//! installed: on its own it only changes how reads inside the synthetic
	//! window are served, and a game whose file table was never rebuilt does
	//! not make any.
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

	//! The placement decided in SetupDisc: disc path -> byte offset on the
	//! virtual disc, plus the region it occupies and how the fragments went.
	//! The rebuilt table is made to agree with this, not the other way round.
	static std::map<std::string, u64> modOffsets;
	static u64 modRegionStart = 0;
	static u64 modRegionEnd = 0;
	static bool fragsRegistered = false;
	static FragBuildStats fragStats;

	//! The game's id, needed to ask which partition it lives on.
	static u8 bootGameId[8] = { 0 };

	//! The USB port the backup is on, so the mod can be checked against it.
	static int bootUsbPort = 0;

	//! Where the mod's own files live, resolved in SetupDisc. The cIOS reads
	//! EVERY fragment in the list from a single drive - set_frag_list hands it
	//! one device number - so a mod on the other drive is not an error, it just
	//! reads the wrong sectors and comes back as noise.
	struct ModDevice
	{
		int drive;      //!< DeviceHandler's SD / USB1..USB8, -1 if unknown
		bool onSd;
		int fsType;     //!< PART_FS_*, -1 if unknown
		u32 lbaStart;
		int usbPort;
		ModDevice() : drive(-1), onSd(false), fsType(-1), lbaStart(0), usbPort(-1) {}
	};

	static ModDevice modDev;
	static bool listFromSd = false;

	//! Set when the tester drops a marker file next to the XML. The file half
	//! of a mod and its <memory> patches are normally all-or-nothing, because
	//! patches without files exit to the System Menu. This deliberately runs
	//! the other half alone - files installed, patches skipped - so a boot
	//! that fails with both can be told apart from one that fails with only
	//! the files. Diagnostic, and off unless the marker exists.
	static bool memPatchSuppressed = false;
	static std::string memPatchMarker;

	void SetBootContext(const ResolvedPatchSet *set, const std::string &device,
						const std::string &logPath, u32 sectorSize,
						const u8 *gameId, int usbPort)
	{
		bootSet = set;
		bootDevice = device;
		bootLogPath = logPath;
		bootSectorSize = sectorSize ? sectorSize : 512;
		bootUsbPort = usbPort;
		memset(bootGameId, 0, sizeof(bootGameId));
		if (gameId)
			memcpy(bootGameId, gameId, 6);

		//! Read the marker now, while the card is still mounted: by the time
		//! the patches would be applied, ShutDownDevices has taken it away.
		memPatchSuppressed = false;
		memPatchMarker.clear();
		if (!device.empty())
		{
			memPatchMarker = device + "/riivolution/nomempatch.txt";
			FILE *m = fopen(memPatchMarker.c_str(), "rb");
			if (m)
			{
				memPatchSuppressed = true;
				fclose(m);
			}
		}
	}

	bool MemoryPatchesSuppressed()
	{
		return memPatchSuppressed;
	}

	//! Put the fragment list back the way the loader handed it over, after the
	//! mod's entries have already been appended to it.
	//!
	//! `num` alone is not enough: frag_append merges a new entry into the
	//! previous one when both run on contiguously, so the last original
	//! fragment may have had its count extended. It is saved and restored
	//! whole. The size field is restored too, because frag_append rewrites it
	//! on every call.
	//!
	//! Only reached when something has already gone wrong, which is exactly
	//! when the list is most likely to be missing - hence the null check.
	static void RestoreFragList(u32 originalNum, const Fragment &originalLast)
	{
		FragList *fl = frag_list_mutable();
		if (!fl || !originalNum)
			return;
		fl->num = originalNum;
		fl->frag[originalNum - 1] = originalLast;
		fl->size = origImageSectors;
	}

	//! Ask DeviceHandler which drive a mount prefix ("sd:", "usb1:") names, and
	//! that drive's own filesystem and starting sector.
	//!
	//! The mod's OWN partition is what matters here, not the game's. NTFS and
	//! ext report sectors relative to the partition they are on, so adding the
	//! game's starting sector to a file on a different partition points the
	//! fragment somewhere else entirely - and the read succeeds, quietly, with
	//! the wrong bytes.
	static ModDevice ResolveModDevice(const std::string &device)
	{
		ModDevice m;
		if (device.empty())
			return m;
		m.drive = DeviceHandler::PathToDriveType(device.c_str());
		if (m.drive < 0)
			return m;
		m.onSd = (m.drive == SD);
		m.fsType = DeviceHandler::GetFilesystemType(m.drive);

		DeviceHandler *dh = DeviceHandler::Instance();
		if (!dh)
			return m;

		PartitionHandle *h = 0;
		int pos = -1;
		if (m.onSd)
		{
			h = dh->GetSDHandle();
			pos = DeviceHandler::GetSDPartition();
		}
		else
		{
			const int part = m.drive - USB1;
			h = dh->GetUSBHandleFromPartition(part);
			pos = DeviceHandler::PartitionToPortPartition(part);
			m.usbPort = DeviceHandler::PartitionToUSBPort(part);
		}
		if (h && pos >= 0)
			m.lbaStart = h->GetLBAStart(pos);
		return m;
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
	//!   - the hook alone changes nothing, because an unmodified file
	//!     table never sends the game into the synthetic window;
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

		//! The fragments went in back in SetupDisc, inside the list the loader
		//! handed over with set_frag_list. Nothing is registered here: d2x
		//! blocks IOCTL_DI_FRAG_SET once a title is running, and the game
		//! partition being open means one is - that refusal is what returned
		//! -128 when this used to re-register at this point.
		if (!fragsRegistered)
		{
			out += "  The mod's fragments were never registered, so there is\n"
				   "  nothing for the rebuilt table to point at.\n";
			return;
		}
		Addf(out, "  fragments            : %u -> %u of %u, registered in SetupDisc\n",
			 fragStats.fragsBefore, fragStats.fragsAfter, RIIVO_FRAG_MAX);

		std::string why;
		size_t verified = 0;

		//! Failures are COLLECTED, not thrown at the first one. Every round on
		//! hardware costs a day, and aborting on file 1 of 2088 spends that day
		//! learning one name when the same reads could have named all of them.
		//! Whether the boot is refused does not change - one failure still
		//! withholds the table - only how much the log knows when it happens.
		size_t failed = 0;
		std::string failures;
		static const size_t MAX_NAMED = 24;

		//! Every file, not a sample. The stride below was the right trade while
		//! the shared path - drive, LBA base, installed dispatch - was what was
		//! in doubt: one sample proves that for all of them. It has now been
		//! proved on hardware, and the largest remaining unknown is the two
		//! thousand files nobody has ever read back. On a total conversion one
		//! wrong file is a hang, and a hang tells us nothing about which file.
		//! Two disc reads each, on a screen that is already black.
		const size_t stride = 1;
		for (size_t i = 0; i < placed.size(); i += stride) {
			if (!VerifyModFragment(placed[i].offset, placed[i].length, placed[i].external, why)) {
				if (failed < MAX_NAMED)
					Addf(failures, "    %s\n", why.c_str());
				++failed;
				continue;
			}
			++verified;
		}
		if (stride > 1 && (placed.size() - 1) % stride != 0)
		{
			const PlacedFile &last = placed.back();
			if (!VerifyModFragment(last.offset, last.length, last.external, why)) {
				if (failed < MAX_NAMED)
					Addf(failures, "    %s\n", why.c_str());
				++failed;
			}
			else ++verified;
		}
		size_t extendedVerified = 0;
		//! Files rescued by tail-cluster recovery are verified
		//! unconditionally, exempt from the stride: the appended sector is a
		//! contiguity guess only the read-back can prove. Skips the ones the
		//! sample above already covered, so nothing is checked twice. A
		//! failure here counts exactly like any other read-back failure -
		//! the fallback is the behaviour without recovery, never a boot with
		//! wrong bytes. The count below is every rescued file, whichever of
		//! the two loops actually read it back.
		for (size_t k = 0; k < fragStats.extended.size(); ++k)
		{
			//! Matched by offset, walking the list rather than trusting the
			//! two vectors to have been built with the same membership. A
			//! recovered file that cannot be found here is a contradiction,
			//! and refusing is the only honest answer to it.
			size_t idx = placed.size();
			for (size_t j = 0; j < placed.size(); ++j)
				if (placed[j].offset == fragStats.extended[k]) { idx = j; break; }
			if (idx == placed.size())
			{
				out += "  A file rescued from an under-reported tail cluster is not\n"
					   "  in the placement list, so it cannot be read back.\n";
				out += "  Rebuilt FST and dependent memory patches are withheld.\n";
				return;
			}
			++extendedVerified;
			if (idx % stride == 0 || idx == placed.size() - 1)
				continue; // the sample above already read this one back
			if (!VerifyModFragment(placed[idx].offset, placed[idx].length, placed[idx].external, why)) {
				if (failed < MAX_NAMED)
					Addf(failures, "    %s\n", why.c_str());
				++failed;
			}
		}
		Addf(out, "  LOW_READ checks      : first/last bytes of %u of %u files passed\n",
			 (unsigned)verified, (unsigned)placed.size());
		if (extendedVerified)
			Addf(out, "  tail recovery        : %u extended file(s) verified unconditionally\n",
				 (unsigned)extendedVerified);
		if (failed)
		{
			Addf(out, "  Read-back FAILED for %u file(s):\n", (unsigned)failed);
			out += failures;
			if (failed > MAX_NAMED)
				Addf(out, "    ... and %u more\n", (unsigned)(failed - MAX_NAMED));
			out += "  Rebuilt FST and dependent memory patches are withheld.\n";
			return;
		}
		u8 check[32] ATTRIBUTE_ALIGN(32);
		// These must remain errors on a DVD5 image despite readable mod data.
		// LOW_READ, not UNENCREAD: the raw path serves any mapped fragment
		// past the declared size by design (that is how the mod itself is
		// read), so only the hooked dispatch can prove the layer checks
		// survived. A promoted disc would answer these instead of refusing.
		const u64 probes[] = { 0x460a0000ULL * 4, RIIVO_DVD9_PROBE_BYTES };
		for (u32 i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
			if (WDVD_Read(check, sizeof(check), probes[i]) == 0) {
				Addf(out, "  Unexpected LOW_READ success at 0x%010llx; FST withheld\n",
					 (unsigned long long)probes[i]);
				return;
			}
		}
		if (WDVD_Read(check, sizeof(check), modRegionEnd) == 0) {
			out += "  LOW_READ past mod end succeeded; FST withheld.\n";
			return;
		}
		out += "  layer/end-range checks : expected failures preserved\n";

		//! The patch itself went in back in SetupDisc; this only records that it
		//! is in place before the table that depends on it is installed.
		Addf(out, "  cIOS read hook       : already applied at %08x\n",
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

		//! The mod region has to clear two floors: the synthetic LOW_READ
		//! window the hook tests (RiivoDiPatch.hpp), and the end of the
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

		//! The floor the mod has to clear is where the game's own FRAGMENTS
		//! end, not where its declared disc ends. __Frag_Get hands back the
		//! first fragment covering an offset, so the only offsets that would
		//! shadow the mod are ones the game actually maps; the space between
		//! the last of those and the declared end is sparse and free to use.
		//!
		//! Using the declared size here forced the mod above the whole disc,
		//! which meant enlarging the disc, which is what tripped the
		//! anti-piracy check. A mod that fits in the sparse tail needs neither.
		//! Measured in SetupDisc, BEFORE the mod's fragments were appended. The
		//! list here contains both, so recomputing it now would measure the mod
		//! against itself: the floor would land above the mod and every file it
		//! placed would then be refused for sitting below the region start.
		u64 gameDataEnd = origMappedEnd;
		if (gameDataEnd == 0 && gameFrags)
		{
			for (u32 i = 0; i < gameFrags->num; ++i)
			{
				const u64 end = (u64) (gameFrags->frag[i].offset
									   + gameFrags->frag[i].count) * bootSectorSize;
				if (end > gameDataEnd)
					gameDataEnd = end;
			}
		}
		const u64 extent = builder.OriginalExtent();
		const u64 region = PlanRegionStart(gameDataEnd, bootSectorSize);

		//! The routing boundary is the synthetic window's start, not wherever
		//! the region happens to begin. Testing against `region` would pass a
		//! disc whose own data reaches into the window, and every one of those
		//! reads would be served raw from the fragment list and come back
		//! undecrypted.
		const bool extentFits = extent < RIIVO_REGION_BYTES;
		//! Align to the drive's own sectors, never to less: a fragment cannot
		//! begin part-way through one. 2 KB is the floor because that is a Wii
		//! disc's own granularity, but a 4K-native drive needs 4 KB and would
		//! otherwise have every file rejected by the alignment check later.
		const u32 layoutAlign = bootSectorSize > 0x800 ? bootSectorSize : 0x800;

		//! Apply the placement decided in SetupDisc rather than choosing a new
		//! one: the fragments are already registered against those offsets and
		//! cannot be changed now, because d2x refuses IOCTL_DI_FRAG_SET once
		//! the game partition is open. Layout() is only used when nothing was
		//! placed, so the report still shows what would have happened.
		u32 unplaced = 0;
		if (!modOffsets.empty())
			unplaced = builder.LayoutFrom(modOffsets);
		else
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
			 extentFits ? "below synthetic LOW_READ window"
						: "OVERLAPS SYNTHETIC WINDOW - REFUSED");
		Addf(out, "  mod relocated to   : 0x%010llx .. 0x%010llx\n",
			 (unsigned long long) region, (unsigned long long) st.highestOffset);
		Addf(out, "  headroom below     : %llu bytes spare before the line\n",
			 (unsigned long long) (extentFits ? region - extent : 0));
		Addf(out, "  mod payload        : %llu bytes\n", (unsigned long long) modBytes);

		Addf(out, "  raw DVD5 ceiling   : 0x%010llx (unchanged; does not limit mod LOW_READ)\n",
			 (unsigned long long) RIIVO_DVD5_CEILING);
		Addf(out, "  LOW_READ mod limit : 0x%010llx (2 GiB window)\n",
			 (unsigned long long) RIIVO_REGION_LIMIT);

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

		//! Which drive everything is on. The cIOS serves the whole list from
		//! one device, so a mismatch here reads the mod's sector numbers off
		//! the game's disk and hands the game noise - a successful read of
		//! the wrong bytes, which is the hardest kind of failure to see.
		//! Printed before the branch below so refusal logs name them too.
		if (modDev.drive >= 0)
		{
			Addf(out, "  game read from     : %s%s\n",
				 listFromSd ? "SD card" : "USB drive",
				 listFromSd ? "" : (bootUsbPort == 1 ? " (port 1)" : " (port 0)"));
			Addf(out, "  mod files on       : %s  (%s, starts at sector %u)\n",
				 bootDevice.c_str(),
				 modDev.fsType == PART_FS_FAT ? "FAT"
				 : modDev.fsType == PART_FS_NTFS ? "NTFS"
				 : modDev.fsType == PART_FS_EXT ? "ext"
				 : modDev.fsType == PART_FS_WBFS ? "raw WBFS" : "unknown",
				 modDev.lbaStart);
			Addf(out, "  game partition     : %s, starts at sector %u\n",
				 bootFsType == PART_FS_FAT ? "FAT"
				 : bootFsType == PART_FS_NTFS ? "NTFS"
				 : bootFsType == PART_FS_EXT ? "ext"
				 : bootFsType == PART_FS_WBFS ? "raw WBFS" : "unknown",
				 bootFsLba);
		}
		else
			out += "  drives               : not resolved on this boot\n";

		if (fragListUntouched && !fragRefusal.empty())
		{
			Addf(out, "  FRAGMENTS NOT REGISTERED: %s\n", fragRefusal.c_str());
			if (fragStats.failed)
			{
				Addf(out, "  files mapped         : %u of %u (%u failed)\n",
					 fragStats.files, fragStats.files + fragStats.failed,
					 fragStats.failed);
				out += DescribeFragFailList(fragStats);
			}
			out += "  The fragment list was left exactly as it was and\n"
				   "  the game is being read precisely as stock USB Loader GX reads\n"
				   "  it. Everything measured above is a dry run.\n\n";
		}
		else if (fragListUntouched)
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
			Addf(out, "  game data ends at  : 0x%010llx  (the floor the mod must clear)\n",
				 (unsigned long long) gameDataEnd);
			out += "  disc NOT enlarged  : the declared size is the backup's own. A mod\n"
				   "                       fragment is found by lookup, not by size, so an\n"
				   "                       unmapped read past the end of the disc still\n"
				   "                       fails - which is what the anti-piracy check\n"
				   "                       looks at.\n";
			Addf(out, "  single-layer limit : 0x%010llx  (%llu bytes of room above the\n"
					  "                       game's data for the mod to live in)\n",
				 (unsigned long long) RIIVO_DVD5_CEILING,
				 (unsigned long long) (RIIVO_DVD5_CEILING > gameDataEnd
									   ? RIIVO_DVD5_CEILING - gameDataEnd : 0));
			Addf(out, "  its fragments      : %u of %u\n",
				 fragStats.fragsBefore ? fragStats.fragsBefore : gameFrags->num,
				 RIIVO_FRAG_MAX);

			//! Report how the fragments actually went in, back in SetupDisc.
			if (fragsRegistered)
			{
				Addf(out, "  mod fragments      : %u file(s) located, %u fragment(s) total\n",
					 fragStats.files, fragStats.fragsAfter);
				if (!fragStats.extended.empty())
					Addf(out, "  tail recovery      : %u file(s) recovered from an under-reported tail cluster\n",
						 (unsigned) fragStats.extended.size());
				if (fragStats.failed)
					Addf(out, "  could not locate   : %u file(s)\n", fragStats.failed);
			}
			else
			{
				Addf(out, "  FRAGMENTS NOT REGISTERED: %s\n",
					 fragStats.firstFailure.empty() ? "the mod's files could not be located"
													: fragStats.firstFailure.c_str());
				if (fragStats.failed)
				{
					Addf(out, "  files mapped         : %u of %u (%u failed)\n",
						 fragStats.files, fragStats.files + fragStats.failed,
						 fragStats.failed);
					out += DescribeFragFailList(fragStats);
				}
			}

			//! Every modded entry must have been given an offset in SetupDisc.
			//! One that was not is pointed at whatever happens to be there, so
			//! the whole table has to be refused.
			if (unplaced)
				Addf(out, "  %u modded entr%s no placement, so the table is unusable\n",
					 unplaced, unplaced == 1 ? "y has" : "ies have");

			//! Feed the placed files through the same checks that would gate a
			//! fresh layout: ordering, alignment, the read ceiling, the table.
			CollectPlaced(builder, modOffsets.empty() ? region : modRegionStart,
						  redirects, created, placed);
			ToExtents(placed, extents);

			plan = PlanFragRegion(gameDataEnd, bootSectorSize,
								  fragStats.fragsBefore ? fragStats.fragsBefore
														: gameFrags->num, extents);

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
			Addf(out, "\n  The cIOS read hook was applied early, at %08x.\n",
				 bootProbe.patchSites[0]);
		else if (!patchWhy.empty())
			Addf(out, "\n  The cIOS read hook was NOT applied: %s\n", patchWhy.c_str());

		// ------------------------------------------------------------------
		// Switch it on, but only if every single check above came back clean.
		// ------------------------------------------------------------------
		out += "\nSwitching it on\n";
		out += "---------------\n";

		if (!(extentFits && plan.ok && gameFrags && patchApplied
			  && fragsRegistered && unplaced == 0))
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
		out += "Original reads retain the stock decrypt/hash path. Only LOW_READ in\n"
			   "the registered synthetic window reads plaintext mod fragments.\n"
			   "Raw-read limits and the declared image size are unchanged.\n"
			   "The hook is RAM-only and disappears on IOS reload or reboot.\n";

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
		//! AHBPROT the cIOS hook cannot be installed, so the feature is impossible
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
		if (!before || !before->num || before->num > RIIVO_FRAG_MAX) {
			fragRefusal = "no valid base-image fragment list";
			fragListUntouched = true;
			return;
		}
		origImageSectors = before->size;
		const u32 originalNum = before->num;
		const Fragment originalLast = before->frag[originalNum - 1];

		//! And which partition it is on, while the partition object still
		//! exists - SetupDisc unmounts SD a few lines below.
		bootFsKnown = (WBFS_GetFsInfo(bootGameId, &bootFsType, &bootFsLba) >= 0);
		gprintf("Riivo: partition lookup %s (fs %u, lba %u)\n",
				bootFsKnown ? "ok" : "FAILED", bootFsType, bootFsLba);

		//! The cIOS reads the WHOLE fragment list from one drive: set_frag_list
		//! passes Settings.SDMode as its device and every fragment, the game's
		//! and the mod's alike, is served from that one. A mod on the other
		//! drive does not fail - the sector numbers are simply read off the
		//! wrong disk and the game gets noise. Measured on a tester's console:
		//! the read-back returned 67f8b995 where the mod file starts "Yaz0".
		modDev = ResolveModDevice(bootDevice);
		listFromSd = (Settings.SDMode != 0);

		if (modDev.drive < 0)
		{
			fragRefusal = "the drive the mod is on could not be identified";
			fragListUntouched = true;
			return;
		}
		if (modDev.onSd != listFromSd)
		{
			fragRefusal = modDev.onSd
						  ? "the mod is on the SD card but the game is being read "
							"from USB - they have to be on the same drive"
						  : "the mod is on the USB drive but the game is being read "
							"from the SD card - they have to be on the same drive";
			gprintf("Riivo: mod on %s, list served from %s - refusing\n",
					modDev.onSd ? "SD" : "USB", listFromSd ? "SD" : "USB");
			fragListUntouched = true;
			return;
		}
		if (!modDev.onSd && modDev.usbPort >= 0 && modDev.usbPort != bootUsbPort)
		{
			fragRefusal = "the mod and the game are on two different USB drives";
			fragListUntouched = true;
			return;
		}
		if (modDev.fsType != PART_FS_FAT && modDev.fsType != PART_FS_NTFS
			&& modDev.fsType != PART_FS_EXT)
		{
			fragRefusal = "the mod is on a filesystem whose layout cannot be read";
			fragListUntouched = true;
			return;
		}
		gprintf("Riivo: mod on %s (fs %d, lba %u), list from %s\n",
				bootDevice.c_str(), modDev.fsType, modDev.lbaStart,
				listFromSd ? "SD" : "USB");

		//! Work the whole placement out HERE, from the files on the card.
		//!
		//! d2x blocks IOCTL_DI_FRAG_SET once a title is running
		//! (Stealth_CheckRunningTitle in its plugin), and opening the game
		//! partition is what starts one - so the extended list has to be handed
		//! over before that, in the same call the loader already makes. The file
		//! table cannot be read until afterwards, so the table is made to agree
		//! with this placement rather than the other way round.
		std::vector<ModCandidate> cand;
		{
			FsDirLister lister;
			ListModFiles(*bootSet, bootDevice, &lister, cand);
		}
		if (cand.empty())
		{
			fragListUntouched = true;
			fragRefusal = "none of the mod's files were found on the card";
			gprintf("Riivo: no mod files found on the card, list left alone\n");
			return;
		}

		//! The floor is where the game's own fragments end - see the report in
		//! PrepareFileRedirects for why that, and not the declared size.
		const u32 sector = bootSectorSize ? bootSectorSize : 512;
		u64 gameEnd = 0;
		for (u32 i = 0; i < before->num; ++i)
		{
			const u64 e = ((u64) before->frag[i].offset + before->frag[i].count) * sector;
			if (e > gameEnd)
				gameEnd = e;
		}

		//! Keep it: after the append below, frag_list_get() returns a list whose
		//! fragments are the game's AND the mod's, so recomputing this later
		//! would measure the mod against itself and refuse its own placement.
		origMappedEnd = gameEnd;

		const u32 align = sector > 0x800 ? sector : 0x800;
		const u64 regionStart = PlanRegionStart(gameEnd, align);

		std::vector<PlacedFile> placed;
		placed.reserve(cand.size());
		u64 cursor = regionStart;
		for (size_t i = 0; i < cand.size(); ++i)
		{
			if (cand[i].size == 0)
			{
				//! Empty files need no fragments, but they still need a
				//! placement: the rebuilt table holds an entry for them, and
				//! LayoutFrom refuses entries with none. They share the cursor
				//! without advancing it; a zero-length read never touches it.
				cursor = (cursor + align - 1) & ~((u64) align - 1);
				modOffsets[cand[i].disc] = cursor;
				continue;
			}
			cursor = (cursor + align - 1) & ~((u64) align - 1);
			PlacedFile pf;
			pf.offset = cursor;
			pf.length = cand[i].size;
			pf.external = cand[i].external;
			placed.push_back(pf);
			modOffsets[cand[i].disc] = cursor;
			cursor += cand[i].size;
		}
		modRegionStart = regionStart;
		modRegionEnd = cursor;

		// Validate before touching either the list or IOS. The declared RAW
		// size and mapped RAW extent both have to describe a DVD5 image.
		std::vector<ModExtent> extents;
		ToExtents(placed, extents);
		const u64 declaredBytes = (u64) origImageSectors * sector;
		const FragPlan earlyPlan = PlanFragRegion(
			std::max(gameEnd, declaredBytes), sector, originalNum, extents);
		if (!earlyPlan.ok) {
			fragRefusal = earlyPlan.why;
			modOffsets.clear();
			modRegionStart = modRegionEnd = 0;
			fragListUntouched = true;
			return;
		}
		modRegionEnd = earlyPlan.regionEnd;

		//! What the backup says its own virtual disc is, captured before any of
		//! this. frag_append rewrites the field on every call, so it has to be
		//! put back afterwards - see below.
		const u64 declared = (u64) origImageSectors * sector;

		//! Append the mod's fragments to the list the loader is about to hand
		//! over. set_frag_list registers the lot in one go, a few lines later in
		//! SetupDisc, which is the call d2x still allows.
		//! The MOD's filesystem and starting sector, not the game's. They are
		//! usually the same partition, but nothing guarantees it, and using the
		//! game's would silently point the fragments at the wrong place.
		if (!AppendModFragments(placed, sector, (u8) modDev.fsType,
								modDev.lbaStart, fragStats))
		{
			gprintf("Riivo: fragment build failed: %s\n", fragStats.firstFailure.c_str());
			modOffsets.clear();
			fragsRegistered = false;
		}
		else if (fragStats.failed)
		{
			//! A file that could not be mapped keeps its assigned offset, so the
			//! rebuilt table would point the game at unmapped space and it would
			//! read sparse zeros. Partial coverage is not a partial success.
			gprintf("Riivo: %u file(s) could not be mapped, refusing\n", fragStats.failed);
			modOffsets.clear();
			fragsRegistered = false;
		}
		else
		{
			fragsRegistered = true;
			gprintf("Riivo: %u mod fragment(s) appended, %u total\n",
					fragStats.files, fragStats.fragsAfter);
		}

		//! Put the declared size back, LAST. frag_append ends with
		//!     ff->size = offset + count;
		//! which is an assignment, not a maximum, and it runs on every call - so
		//! appending the mod silently redefines the virtual disc as ending at
		//! the last mod fragment, shrinking or stretching it to suit whatever
		//! went in last.
		//!
		//! The value it goes back to is the backup's own, unchanged. The disc is
		//! never enlarged for the mod's benefit: __Frag_Get looks the offset up
		//! in the fragment table FIRST and only consults the declared size when
		//! nothing matched, so a mod fragment above the declared end is read
		//! perfectly well while an unmapped offset past it still fails - which
		//! is exactly what a real disc does, and what the game's anti-piracy
		//! check is looking for.
		{
			FragList *fl = frag_list_mutable();
			if (fl && declared)
			{
				fl->size = (u32) ((declared + sector - 1) / sector);
				gprintf("Riivo: declared size restored to %u sectors (%llu bytes)\n",
						fl->size, (unsigned long long) declared);
			}
		}

		if (!fragsRegistered) {
			RestoreFragList(originalNum, originalLast);
			fragRefusal = fragStats.firstFailure;
			fragListUntouched = true;
			return;
		}

		//! Find the cIOS read handler and patch it now, while the access to do
		//! it still exists. The card is mounted at this point - SetupDisc
		//! unmounts it a few lines further on - so the dump can be written too.
		const std::string dumpPath = bootDevice + "/riivolution/usbloadergx_riivo_"
									 + (const char *) bootGameId + "_dip.bin";
		ProbeIosPlugin(dumpPath, bootProbe);

		if (bootProbe.patchSites.size() == 1)
		{
			patchApplied = ApplyDiPatch(bootProbe.patchSites[0], (u32)(modRegionEnd >> 2), patchWhy);
			gprintf("Riivo: early cIOS hook at %08x: %s\n",
					bootProbe.patchSites[0],
					patchApplied ? "applied" : patchWhy.c_str());
		}
		else
		{
			patchWhy = bootProbe.patchSites.empty()
					   ? "the patch site was not found in the running cIOS"
					   : "the patch site was found more than once, which is not expected";
		}
		if (!patchApplied) {
			RestoreFragList(originalNum, originalLast);
			fragsRegistered = false;
			modOffsets.clear();
			fragRefusal = patchWhy;
			fragListUntouched = true;
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
			if (place.heapLeft)
				Addf(out, "  heap the game still has    : %u MB\n",
					 place.heapLeft / (1024 * 1024));
			else
			{
				//! Arena low was never set, so there is no floor to measure
				//! the heap against. The cap actually enforced in that case
				//! is the blind-drop limit; print how much of it this takes.
				Addf(out, "  heap the game still has    : unknown (arena low not set)\n");
				Addf(out, "  blind-drop cap used        : %u of %u KB\n",
					 place.reserved / 1024, MAX_BLIND_DROP / 1024);
			}
		}

		//! This is the step that actually points the game at the mod. It only
		//! runs when the fragment list, the read-back check and the cIOS hook
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
					   "  own. The cIOS hook is harmless on its own.\n";
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
		else if (memPatchSuppressed)
		{
			Addf(out, "  SUPPRESSED by %s\n", memPatchMarker.c_str());
			out += "  The mod's files ARE installed; only its <memory> patches\n"
				   "  were skipped, deliberately. This is the halfway state the\n"
				   "  interlock normally forbids, and it exists to tell a fault in\n"
				   "  the files or the rebuilt table apart from one in the patches.\n"
				   "  Delete that file to boot the mod properly.\n";
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

	//! The last thing written while the card is still mounted. A black screen
	//! after this point says the game was handed control and did not come
	//! back, which is a different fault from anything above; without the
	//! entry point and the arena the log cannot tell those apart.
	void ReportLaunch(u32 entry)
	{
		std::string out;
		out += "\n\nHanding over to the game\n";
		out += "------------------------\n";
		Addf(out, "  entry point  : %08x\n", entry);
		Addf(out, "  arena low    : %08x\n", (u32) SYS_GetArenaLo());
		Addf(out, "  arena high   : %08x\n", (u32) SYS_GetArenaHi());
		if (!entry)
			out += "  No entry point: the apploader never produced one, so nothing\n"
				   "  below this line ever ran.\n";
		else
			out += "  Everything the loader does is finished. If the screen stays\n"
				   "  black from here, the fault is in the game's own execution -\n"
				   "  the mod's code, the rebuilt table it reads, or the files it\n"
				   "  asks for - not in the setup logged above.\n";
		AppendLog(out);
	}
}
