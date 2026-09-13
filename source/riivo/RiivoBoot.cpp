/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <gccore.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "sys.h"
#include "RiivoBoot.hpp"
#include "RiivoNet.hpp"
#include "RiivoConfig.hpp"
#include "RiivoReconcile.hpp"
#include "RiivoValidate.hpp"
#include "RiivoFst.hpp"
#include "RiivoFile.hpp"
#include "RiivoFstBuild.hpp"
#include "RiivoFstWalk.hpp"
#include "RiivoReadVerify.hpp"
#include "RiivoFstInstall.hpp"
#include "RiivoLaunchState.hpp"
#include "RiivoCheckpoints.hpp"
#include "RiivoPersist.hpp"
#include "RiivoPatchPlan.hpp"
#include "RiivoBootView.hpp"
#include "RiivoManifest.hpp"
#include "RiivoPatchGuard.h"
#include "RiivoIosProbe.hpp"
#include "RiivoOnDemand.hpp"
#include "RiivoRedirectTable.hpp"
#include "RiivoDiPatch.hpp"
#include "RiivoFragPlan.hpp"
#include "RiivoFragBuild.hpp"
#include "usbloader/frag.h"
#include "usbloader/wbfs.h"
#include "patches/gamepatches.h"
#include "settings/CSettings.h"
#include "Controls/DeviceHandler.hpp"
#include "memory/mem2.h"
#include "prompts/ProgressWindow.h"
#include "language/gettext.h"
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

	//! Single owner of this boot's staging, booking, and install verdicts.
	//! The named references below are aliases into this object, so existing
	//! code keeps working unchanged while storage, reset, and the install
	//! guard live in one place (see RiivoLaunchState.hpp). Fresh storage
	//! per boot is what makes a second launch - or an aborted one - unable
	//! to inherit the previous boot's staged table.
	static LaunchState g_launch;

	//! Set when the boot took the on-demand path: the mod's files were NOT
	//! mapped to sectors, so the redirect table and the module are what make
	//! them readable, and the fragment-based hook must not be used instead.
	static bool onDemandPlanned = false;
	static OnDemandLayout onDemandLayout;

	//! Opt-in, by a file on the card, in the same shape as dumpios.txt. The
	//! fragment path still works and is still the default; this one replaces
	//! how every mod file is reached, so it earns its way in rather than being
	//! switched on under people who did not ask for it.
	static bool OnDemandRequested()
	{
		if (bootDevice.empty())
			return false;
		FILE *f = fopen((bootDevice + "/riivolution/ondemand.txt").c_str(), "rb");
		if (!f)
			return false;
		fclose(f);
		return true;
	}

	//! "usb1:/Spectral/x.arc" -> "/Spectral/x.arc". The module mounts the FAT
	//! partition itself and knows nothing about the loader's device names.
	static std::string PartitionPath(const std::string &external)
	{
		const size_t colon = external.find(':');
		if (colon == std::string::npos)
			return external;
		std::string rest = external.substr(colon + 1);
		if (rest.empty() || rest[0] != '/')
			rest = "/" + rest;
		return rest;
	}
	static std::string bootLogPath;

	//! Size of the table PrepareFileRedirects worked out, carried across to
	//! ReportFstPlacement - which runs later, after the apploader, and needs to
	//! know how much room the rebuilt table wants.
	static u32 &plannedFstSize = g_launch.plannedFstSize;

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
	static bool &fileWorkWanted = g_launch.fileWorkWanted;
	static bool &fileWorkLive = g_launch.fileWorkLive;

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
	static u32 patchStorage = 0;
	static std::string patchWhy;
	//! IOS lifecycle generations (separate contract from PPC LaunchState).
	//! Clearing patchApplied/fragsRegistered does NOT restore a successfully
	//! installed Starlet hook or fragment registration. hookGeneration /
	//! fragGeneration record the launch generation that installed them;
	//! a nonzero value differing from g_launch.generation means a previous
	//! boot's IOS state persists (no reload since). SetupDisc refuses new
	//! file work on stale state instead of layering a new hook atop the old
	//! (BuildDiHook expects the original signature) or serving new fragments
	//! through an old hook. NoteIosReload() (BootGame after LoadGameCios)
	//! clears both to stock. Same-process retry after a successful hook is
	//! therefore unsupported by design: relaunch GX (fresh IOS) and retry.
	static u32 hookGeneration = 0;
	static u32 fragGeneration = 0;

	//! Patched boot view (production instance). Armed in Activate() once the
	//! plan is composed and the rebuilt table staged; consulted by WDVD_Read
	//! while the partition is open and devices are mounted (the apploader
	//! window); disarmed right after Apploader_Run and at every BeginLaunch.
	//! DOL coverage serves the composed executable positionally (apploader
	//! responses follow the stock layout; the served image preserves stock
	//! size exactly). FST coverage serves the staged rebuilt bytes at the
	//! disc FST range. Everything else falls through to stock.
	static BootView bootView;
	//! True once a grown virtual header has been served to apploader reads
	//! this boot. From then on there is no stock boot to fall back to: the
	//! apploader's reservation, loaded table and published pointers may all
	//! differ from stock, so every later grown deviation returns to the
	//! loader instead of booting. Cleared per boot with everything else.
	static bool grownHeaderServed = false;
	//! A grown evaluation refused after the header above was served: the
	//! apploader already consumed non-stock metadata, so booting would run
	//! a table the loader did not approve. BootPartition turns this into a
	//! null entry (blink 6 + loader, like every other failed load).
	static bool grownBlocked = false;
	//! Composed executable + image base from the plan block. Persisted (small:
	//! segments and paths only, never payloads) so Activate() can arm the
	//! view after staging; cleared per boot with everything else.
	static PlannedFile stagedDol;
	static bool haveStagedDol = false;
	static u64 stagedDolBase = 0;
	//! Authoritative file plan for the late phases (FST sizes, offset remap,
	//! manifest emission). Persisted from the plan block; cleared per boot.
	//! Bounded by the early fragment budget (refused beyond it pre-plan).
	static PatchPlan activePlan;
	static bool haveActivePlan = false;
	//! True once DOL coverage is armed AND verified through the real read
	//! path. Gates the alt-DOL stacking refusal in BootPartition: an SD
	//! alternate DOL would overwrite the served image in MEM after the fact.
	static bool dolServing = false;
	//! Consult counters, reported once at disarm (bounded single line).
	static u32 ovReads = 0, ovServedDol = 0, ovServedMeta = 0, ovFatOpens = 0,
			   ovFailed = 0;

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

	//! One registration record per early candidate: source path, synthetic
	//! offset and stat size. The late list (CollectPlaced) only holds files
	//! the rebuilt table references, so a recovered offset missing there is
	//! looked up here instead of refused anonymously - the record names the
	//! file and its retained bytes are verified in its place. Filled in
	//! PrepareFragList, cleared wherever modOffsets is cleared.
	static std::vector<RegRecord> modRecords;

	//! The game's DOL section table, read off the disc in PrepareFileRedirects
	//! (same window as the FST: partition open, devices mounted). The loaded
	//! range list carries RAM addresses only; this is what traces a range
	//! back to its DOL section and disc offset - and tells BSS (zero-filled,
	//! no disc bytes) apart from disc-read data. Reset per boot in
	//! SetBootContext; empty when the header could not be read.
	//! Slots keep their DOL section numbers even when empty, so a section's
	//! reported number is always its real one, never a compacted position.
	struct DolSection
	{
		u32 fileOff; // offset within the DOL image, not the partition
		u32 addr;
		u32 size;
		u32 index;   // 0-6 text, 7-17 data
		bool text;
	};
	static DolSection dolSections[18];
	static u32 dolSectionCount = 0; // non-empty sections (for the log line)
	static u32 dolBssAddr = 0, dolBssSize = 0;
	//! Partition byte offset of the DOL image itself: section file offsets
	//! are relative to it, so the absolute disc offset is base + fileOff.
	static u32 dolImageBase = 0;

	//! One record per apploader yield: where the bytes went, how many, and
	//! at which disc offset they were read from. RegisterDOL keeps only the
	//! first two, which is enough to steer placement but leaves a range's
	//! source untraceable - notably for chunks no DOL section describes
	//! (scratch rereads). Last write wins: a repeated destination means the
	//! earlier bytes are gone. Reset per boot in SetBootContext.
	struct DolRangeNote
	{
		u32 dst;
		u32 len;
		u32 disc;
	};
	static DolRangeNote dolNotes[128];
	static u32 dolNoteCount = 0, dolNotesDropped = 0;

	//! Record side of the RiivoNoteDOLRange bridge (see RiivoLight.h).
	//! Runs inside the apploader's read loop, beside RegisterDOL itself.
	void NoteDOLRange(u32 dst, u32 len, u32 discOffset)
	{
		for (u32 i = 0; i < dolNoteCount; ++i)
		{
			if (dolNotes[i].dst == dst && dolNotes[i].len == len)
			{
				dolNotes[i].disc = discOffset;
				return;
			}
		}
		if (dolNoteCount < sizeof(dolNotes) / sizeof(dolNotes[0]))
		{
			dolNotes[dolNoteCount].dst = dst;
			dolNotes[dolNoteCount].len = len;
			dolNotes[dolNoteCount].disc = discOffset;
			++dolNoteCount;
		}
		else
			++dolNotesDropped;
	}

	//! Production byte sources for the boot view (PPC apploader window only:
	//! partition open, devices mounted). Stock reads bypass the overlay
	//! (WDVD_ReadStock) so serving can never recurse into itself. FAT reads
	//! stream straight into the caller's buffer; short transfers are failures.
	//! Bounded: one open per run served, no retained handles, no staging.
	static bool BootStockReader(u64 absOff, u8 *dst, u32 len, void *ctx)
	{
		(void)ctx;
		return WDVD_ReadStock(dst, len, absOff) == 0;
	}

	static bool BootFatReader(const std::string &path, u64 srcOff, u8 *dst,
							  u32 len, void *ctx)
	{
		(void)ctx;
		if (len == 0)
			return true;
		if (srcOff > (u64)LONG_MAX)
			return false;
		++ovFatOpens;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		bool ok = false;
		if (fseek(f, (long)srcOff, SEEK_SET) == 0)
		{
			size_t got = 0;
			while (got < len)
			{
				size_t n = fread(dst + got, 1, len - got, f);
				if (n == 0)
					break;
				got += n;
			}
			ok = (got == len);
		}
		fclose(f);
		return ok;
	}

	//! Serve one production read through the armed boot view.
	//! Tri-state: 1 served (bytes in buffer), 0 fall through to stock,
	//! -1 fail loudly (a patched range that cannot be served: the caller
	//! must boot nothing, never mix stock bytes into a patched image).
	//! Routing (DOL touch -> all-or-nothing; header/FST containment ->
	//! serve; else stock) lives in BootView::Route, shared with host
	//! tests; this function only binds readers and counts.
	static int ServeBootRead(u64 offset, u8 *buffer, u32 length)
	{
		if (!buffer || length == 0)
			return 0;
		// Consulted-read counter keeps its meaning (every read while any
		// coverage is armed, including stock fallthroughs).
		if (bootView.Active() || bootView.HasDol())
			++ovReads;
		switch (bootView.Route(offset, length))
		{
			case BootView::ROUTE_STOCK:
				return 0;
			case BootView::ROUTE_FAIL:
				++ovFailed;
				gprintf("Riivo: boot-view read straddles the served image at 0x%llx len %u\n",
						(unsigned long long)offset, length);
				return -1;
			case BootView::ROUTE_DOL:
			{
				BootReaders r;
				r.stock = BootStockReader;
				r.fat = BootFatReader;
				r.ctx = 0;
				u32 done = 0;
				if (bootView.ServeDol(offset, buffer, length, r, &done))
				{
					++ovServedDol;
					return 1;
				}
				++ovFailed;
				gprintf("Riivo: boot-view DOL read failed at 0x%llx len %u (done %u)\n",
						(unsigned long long)offset, length, done);
				return -1;
			}
			case BootView::ROUTE_META:
			{
				if (bootView.Serve(offset, buffer, length))
				{
					++ovServedMeta;
					return 1;
				}
				// Classified servable but not served (empty coverage after
				// a state change mid-window): fail closed, never stock-mix.
				++ovFailed;
				return -1;
			}
		}
		return 0;
	}

	//! C bridge for wdvd.c (int, not bool: 1 served, 0 stock, -1 fail).
	extern "C" int RiivoBootViewServe(u64 offset, u8 *buffer, u32 length)
	{
		return ServeBootRead(offset, buffer, length);
	}

	//! Disarm the boot view at the end of the apploader window (and every
	//! fresh boot). Logs one bounded summary line while the card is alive.
	void DeactivateBootView()
	{
		if (bootView.Active() || bootView.HasDol() || ovReads > 0)
		{
			gprintf("Riivo: boot view disarmed (reads %u, dol %u, meta %u, fat opens %u, failed %u)\n",
					ovReads, ovServedDol, ovServedMeta, ovFatOpens, ovFailed);
			char line[160];
			snprintf(line, sizeof(line),
				"boot view: %u read(s) consulted, %u dol + %u header/table served, %u fat open(s), %u failed\n",
				ovReads, ovServedDol, ovServedMeta, ovFatOpens, ovFailed);
			AppendLog(line);
		}
		bootView.Deactivate();
		dolServing = false;
		ovReads = ovServedDol = ovServedMeta = ovFatOpens = ovFailed = 0;
	}

	bool DolWillServe()
	{
		return dolServing;
	}

	bool GrownHeaderServed()
	{
		return grownHeaderServed;
	}

	bool GrownBlocked()
	{
		return grownBlocked;
	}

	bool GrownTablePending()
	{
		return g_launch.fileWorkLive && g_launch.HaveStaged()
			   && g_launch.placeOk && !g_launch.place.inPlace;
	}

	//! Per-disc table-build failures from PrepareFileRedirects: the redirect
	//! existed but the entry never made it into the rebuilt table (the table
	//! refused it, or the external file failed to stat between phases).
	//! Lets the reconcile step name the exclusion reason for each file.
	static std::map<std::string, SkipReason> modAddFails;

	//! Registered files that never became placed entries, with reasons.
	//! Built after CollectPlaced; consumed by Activate and the report.
	static std::vector<SkipRecord> modSkips;

	//! Files the mod names that are not on the card, from the enumeration
	//! stat. Purely diagnostic, so it is NOT cleared alongside modOffsets
	//! and modRecords on a refusal: the refusal is exactly when the tester
	//! needs these names. Cleared once per boot in SetBootContext.
	static std::vector<MissingExternal> modMissing;

	//! "The console is still working." Everything else this feature can say
	//! needs something that is gone by the time it matters: the card log
	//! stops when the card is unmounted, the screen stops when the GUI does,
	//! gprintf needs hardware the tester does not own. The drive light is a
	//! single register write - no devices, no threads, no allocation - so it
	//! is the one channel that survives the whole boot, including the window
	//! where a black screen is the only other thing on offer.
	//!
	//! Toggled rather than driven from a timer: this is called from the
	//! points that already mark progress, so the light changing IS progress,
	//! and a light that stops changing means the step it stopped on is the
	//! one that hung. No thread to schedule, nothing to tear down, and it
	//! cannot itself be the thing that breaks a boot.
	static bool pulseOn = false;
	static bool pulseArmed = false;

	void PulseLight()
	{
		if (!pulseArmed)
			return;
		pulseOn = !pulseOn;
		wiilight_diag(pulseOn ? 1 : 0);
	}

	//! Set the drive light to an explicit state for checkpoint boundaries.
	//! Unlike PulseLight (toggle), an explicit state cannot be cancelled by
	//! a neighboring toggle: ON stays ON no matter the prior parity, for as
	//! long as nothing else touches the light. Same gate and register as
	//! every other light write.
	static void LightSet(bool on)
	{
		if (!pulseArmed)
			return;
		pulseOn = on;
		wiilight_diag(on ? 1 : 0);
	}

	//! Off, once and for all. Called immediately before the jump: from then
	//! on a dark light means the loader is done and the game has it, which
	//! is what makes "still pulsing" and "went out" mean different things.
	void EndLightPulse()
	{
		pulseArmed = false;
		pulseOn = false;
		wiilight_diag(0);
	}

	//! Machine-parseable outcome of the file work, for the previous-boot
	//! check in the game settings UI. FST_STAGED once the table is staged
	//! for install (installation itself is verified after device shutdown
	//! and cannot extend the card log); WITHHELD carries the stage that
	//! stopped it.
	static std::string withholdStage;

	//! Which install check refused last, for the drive-light blink code in
	//! the caller: 0 none/success, 1 live game with nothing staged,
	//! 2 staged buffer failed its pre-copy checksum, 3 InstallFst bounds
	//! refusal, 4 installed bytes/CRC mismatch, 5 low-memory pointer/arena
	//! mismatch. The refusal text itself only reaches gprintf - the card is
	//! gone - so this number is the part the tester can see.
	static u32 &installFailCode = g_launch.installFailCode;

	//! Outcome counters for the pre-jump screen, captured where they are
	//! known.
	static u32 sumPlaced = 0;
	static u32 sumFailed = 0;

	static bool skipFstInstall = false;

	//! Diagnostic split (riivolution/relocorig.txt): relocate the VERBATIM
	//! original table instead of the rebuilt one. Same placement, same
	//! install, same hook and fragments - the only difference from a normal
	//! boot is the installed bytes. If this boots and the created-file
	//! table does not, the fault is the new table content; if this dies
	//! the same way, it is the relocation itself. Raw bytes are retained
	//! at FST parse time (below); the builder path re-serializes and would
	//! hide exactly the difference under test.
	//! Do not combine with nofstinstall.txt (which wins: nothing installs).
	static bool relocOrig = false;
	static std::vector<u8> relocOrigRaw;

	//! Retired (general pipeline, 2026-09-12): mem2fst.txt / smg2reserve.txt
	//! per-title MEM2 placements removed. PlaceFstMem2 arithmetic remains
	//! host-tested in RiivoFstInstall.cpp but is no longer consulted by the
	//! production placement policy, which supports only in-place tables until
	//! the patched boot view lands. See docs/archive/smg2-reserve/README.md.
	static u8 bootDiscRevision = 0xff;

	//! The game's id, needed to ask which partition it lives on.
	static u8 bootGameId[8] = { 0 };

	//! Disc FST range (partition bytes) captured with the FST read: lets
	//! late phases tell apploader-loaded FST bytes (dead post-install)
	//! apart from live game chunks without guessing by address. Cleared
	//! per boot with everything else.
	static u64 trackFstOff = 0;
	static u32 trackFstSize = 0;

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
	//! When the first boot step ran, and how long the whole file half is
	//! allowed to take. Nothing measured time before this, so a phase that
	//! was merely slow and one that was wedged produced the same black
	//! screen, and the only way to tell them apart was to sit there.
	//!
	//! The budget is a safety net, not a performance policy: a mod big
	//! enough to need longer than this is better off booting unmodified
	//! than leaving someone staring at nothing with no way to know.
	static const u32 RIIVO_TIME_BUDGET_MS = 300000;   // five minutes
	static u64 bootClockStart = 0;
	static bool stepHeaderWritten = false;
	static bool deadlinePassed = false;

	//! Step names and times for the phase-duration table, kept in a small
	//! ring. Declared up here so per-boot state resets in SetBootContext;
	//! recorded in LogStep, printed in ReportLaunch.
	struct StepMark
	{
		char name[56];
		u32 ms;
	};
	static StepMark stepMarks[48];
	static u32 stepMarkCount = 0;

	//! Milliseconds since the first step. Zero until the clock starts.
	static u32 BootElapsedMs()
	{
		if (!bootClockStart)
			return 0;
		return (u32) ticks_to_millisecs(diff_ticks(bootClockStart, gettime()));
	}

	//! Checked at every phase boundary. Once it trips it stays tripped, so
	//! the refusal is reported once and every later phase declines to start.
	bool RiivoDeadlinePassed()
	{
		if (!deadlinePassed && bootClockStart
			&& BootElapsedMs() > RIIVO_TIME_BUDGET_MS)
			deadlinePassed = true;
		return deadlinePassed;
	}

	//! Set by riivolution/verify.txt. The large-read pass reads the WHOLE
	//! mod back through the cIOS and compares it against the card - 128 MB
	//! on Starshine, so 256 MB of traffic and minutes of black screen. It
	//! proved what it was written to prove; it is diagnosis, not a gate, and
	//! the per-file first/last check that stays on already establishes that
	//! every fragment maps where the table says it does.
	static bool deepVerify = false;


	//! Start one boot: the single reset point for every piece of per-boot
	//! Riivo state. BootGame calls this once per launch, before anything
	//! else - including launches with no mod selected, which never reach
	//! SetBootContext. Without it, an aborted launch or a second launch in
	//! one loader session inherits the previous boot's staged table,
	//! placement verdict, and fragment bookkeeping; InstallPendingFst would
	//! then install the WRONG mod's table into the new game (it verifies
	//! the install against the same stale buffer). The previous staging
	//! buffer is freed here (it leaked before); the generation stamp in
	//! LaunchState additionally refuses a table staged under any other boot.
	//! Defined after the fragment bookkeeping it resets.
	void BeginLaunch();

	void SetBootContext(const ResolvedPatchSet *set, const std::string &device,
						const std::string &logPath, u32 sectorSize,
						const u8 *gameId, int usbPort, u8 discRevision)
	{
		bootSet = set;
		bootDiscRevision = discRevision;
		bootDevice = device;
		bootLogPath = logPath;
		bootSectorSize = sectorSize ? sectorSize : 512;
		bootUsbPort = usbPort;
		memset(bootGameId, 0, sizeof(bootGameId));
		if (gameId)
			memcpy(bootGameId, gameId, 6);

		//! Read the marker now, while the card is still mounted: by the time
		//! the patches would be applied, ShutDownDevices has taken it away.
		//! The progress window and the per-folder log lines are the two
		//! things that run INSIDE the file-listing loop, and they are the
		//! only things in that phase that did not exist in v2.9 - the last
		//! build known to list this mod and boot it. Two consoles have since
		//! stopped mid-listing, so both are off unless asked for. On, they
		//! give a moving bar and a line per <folder> rule; off, the phase is
		//! as quiet as it used to be and only its start and end are recorded.
		if (!device.empty())
		{
			FILE *w = fopen((device + "/riivolution/verify.txt").c_str(), "rb");
			if (w)
			{
				deepVerify = true;
				fclose(w);
			}
		}
		//! Pre-jump summary (riivolution/showlog.txt): OFF unless the marker
		//! is present. It used to be unconditional, and that made it the only
		//! thing this loader does after ShutDownDevices that stock does not:
		//! it starts the progress GUI, sleeps, and blocks in ProgressStop,
		//! immediately before the jump. A Test=Disabled boot - no memory
		//! patch, no file work, no table install, nothing else on the path -
		//! still black-screened with the drive light stuck on, which is what
		//! a hang before EndLightPulse looks like. It is also why the summary
		//! and the loading bar were reported as never appearing: the GUI
		//! cannot present this late, and the attempt is not free.
		//! Read here because the card is gone by the time the screen shows,
		//! and reset per boot like every marker.
		//! Diagnostic split (riivolution/nofstinstall.txt): stage everything -
		//! hook, fragments, rebuilt table - then DO NOT install the table.
		//! The game then reads its own original file table and never sees the
		//! mod's files, while the cIOS hook and the registered fragments stay
		//! exactly as a real mod boot leaves them. So a boot that fails with
		//! the table installed and succeeds without it puts the fault in the
		//! install or the table; one that fails both ways puts it in the hook
		//! or the fragments. Nothing else distinguishes those two halves.
		//! (Resets live in BeginLaunch; only the marker reads stay here.)
		if (!device.empty())
		{
			FILE *n = fopen((device + "/riivolution/nofstinstall.txt").c_str(), "rb");
			if (n)
			{
				skipFstInstall = true;
				fclose(n);
			}
		}
		if (!device.empty())
		{
			FILE *r = fopen((device + "/riivolution/relocorig.txt").c_str(), "rb");
			if (r)
			{
				relocOrig = true;
				fclose(r);
			}
		}
		//! Retired markers (general pipeline, 2026-09-12): smg2reserve.txt
		//! (per-game MEM2 reservation) and mem2fst.txt (per-title surveyed
		//! MEM2 window) are no longer consulted. If present they are reported
		//! as retired and ignored; grown placements refuse via the general
		//! policy below, never via a game-ID gate. See
		//! docs/archive/smg2-reserve/README.md.
		if (!device.empty())
		{
			FILE *m = fopen((device + "/riivolution/smg2reserve.txt").c_str(), "rb");
			if (m)
			{
				fclose(m);
				gprintf("Riivo: smg2reserve.txt is retired (per-game reservation removed); ignored, general placement policy applies\n");
			}
		}
		if (!device.empty())
		{
			FILE *m = fopen((device + "/riivolution/mem2fst.txt").c_str(), "rb");
			if (m)
			{
				fclose(m);
				gprintf("Riivo: mem2fst.txt is retired (surveyed MEM2 window removed); ignored, general placement policy applies\n");
			}
		}
		//! Optional: "addr:port" of a listener on the LAN. Absent for
		//! everyone who is not debugging, and then no socket is opened.
		if (!device.empty())
		{
			FILE *c = fopen((device + "/riivolution/collector.txt").c_str(), "rb");
			if (c)
			{
				char spec[64] = {0};
				const size_t rd = fread(spec, 1, sizeof(spec) - 1, c);
				fclose(c);
				spec[rd] = 0;
				OpenCollector(spec);
			}
		}
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
	//! The game's own fragment list, as it was before any of the mod's were
	//! appended. Held at namespace scope so the revert below can reach it.
	static u32 savedOrigNum = 0;
	static Fragment savedOrigLast;

	static void RestoreFragList(u32 originalNum, const Fragment &originalLast)
	{
		FragList *fl = frag_list_mutable();
		if (!fl || !originalNum)
			return;
		fl->num = originalNum;
		fl->frag[originalNum - 1] = originalLast;
		fl->size = origImageSectors;
	}

	void BeginLaunch()
	{
		u8 *oldStaging = g_launch.Begin();
		if (oldStaging)
			MEM2_free(oldStaging);
		//! PPC flags clear every boot; IOS generations persist for stale
		//! detection (see statics). NoteIosReload() clears them on stock.
		//! Fragment bookkeeping: re-derived every boot from the live list.
		fragsRegistered = false;
		fragListUntouched = false;
		fragRefusal.clear();
		origMappedEnd = 0;
		origImageSectors = 0;
		savedOrigNum = 0;
		savedOrigLast = Fragment();
		modOffsets.clear();
		modRegionStart = 0;
		modRegionEnd = 0;
		fragStats = FragBuildStats();
		modDev = ModDevice();
		listFromSd = false;
		//! Markers: re-read from the card by SetBootContext when a mod is
		//! selected; cleared here so a mod-less boot cannot inherit them.
		memPatchSuppressed = false;
		memPatchMarker.clear();
		deepVerify = false;
		onDemandPlanned = false;
		onDemandLayout = OnDemandLayout();
		skipFstInstall = false;
		relocOrig = false;
		relocOrigRaw.clear();
		//! Diagnostics and verdicts: never inherited across boots.
		bootClockStart = 0;
		deadlinePassed = false;
		stepHeaderWritten = false;
		stepMarkCount = 0;
		sumPlaced = 0;
		sumFailed = 0;
		withholdStage.clear();
		//! Every refusal in this boot assigns a short literal here; hold
		//! capacity once, while the heap is fresh, so a late out-of-memory
		//! withhold cannot throw inside its own assignment (libstdc++
		//! strings always heap-allocate, even for short literals).
		withholdStage.reserve(32);
		modRecords.clear();
		modSkips.clear();
		modMissing.clear();
		modAddFails.clear();
		activePlan = PatchPlan();
		haveActivePlan = false;
		stagedDol = PlannedFile();
		haveStagedDol = false;
		stagedDolBase = 0;
		dolSectionCount = 0;
		dolBssAddr = dolBssSize = 0;
		dolImageBase = 0;
		dolNoteCount = 0;
		dolNotesDropped = 0;
		pulseArmed = true;
		pulseOn = false;
		patchApplied = false;
		patchStorage = 0;
		patchWhy.clear();
		bootFsKnown = false;
		bootFsType = 0;
		bootFsLba = 0;
		bootSet = 0;
		bootDevice.clear();
		bootLogPath.clear();
		bootSectorSize = 512;
		bootUsbPort = 0;
		bootDiscRevision = 0xff;
		trackFstOff = 0;
		trackFstSize = 0;
		memset(bootGameId, 0, sizeof(bootGameId));
		ClearDirListCache();
		ClearFileSizeCache();
		grownHeaderServed = false;
		grownBlocked = false;
		// Last: disarm any leftover boot-view state. Runs after the log path
		// is cleared so a stale summary can never land in a new boot's log;
		// normally a silent no-op (BootPartition disarms on every exit path).
		DeactivateBootView();
	}

	//! Stock IOS restored (BootGame after LoadGameCios slot reload). Clears
	//! the installed-hook/fragment generations so the next SetupDisc starts
	//! from stock, not stale. Call only after a verified reload, never to
	//! "unpatch" blindly against an unidentified IOS instance.
	void NoteIosReload()
	{
		hookGeneration = 0;
		fragGeneration = 0;
		patchApplied = false;
		patchStorage = 0;
		patchWhy.clear();
		fragsRegistered = false;
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

	//! Checked persistence both AppendLog and the paths that need the
	//! verdict use. True when there was nothing to do or every byte is
	//! confirmed on the card; false after a gprintf naming the failure.
	//! The gprintf is the whole failure record: reporting it through the
	//! log would recurse, and the card is exactly what just failed.
	static bool PersistReport(const std::string &text)
	{
		if (bootLogPath.empty() || text.empty())
			return true;
		//! Same text to the listener, if one was configured. Sent first so
		//! it survives even when the card is unmounted: the SetupDisc
		//! register/remount window unmounts SD, and a file-only copy would
		//! go silent exactly when it is needed most. The file stays the
		//! record; this is the copy that survives a boot that never
		//! finishes or a card that never comes back.
		SendCollector(text);
		//! Checked write: an unreported short write or failed close leaves
		//! a log that ends mid-boot with no explanation - indistinguishable
		//! from a hang at that point. gprintf is the only channel that does
		//! not need the card, so a persist failure is reported there and
		//! nowhere else (never back into the log: that would recurse).
		if (!AppendFileBytes(bootLogPath.c_str(), text.data(), text.size()))
		{
			gprintf("Riivo: log append failed (%s)\n", bootLogPath.c_str());
			return false;
		}
		return true;
	}

	void AppendLog(const std::string &text)
	{
		PersistReport(text);
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

	//! One line per completed step, written the moment it completes.
	//! Everything between SetupDisc and the first report in
	//! PrepareFileRedirects used to be silent, so a mod big enough to spend
	//! minutes listing files - or one that ran the heap out - left a log that
	//! stopped dead after the settings block and a console showing nothing.
	//! Those two cannot be told apart after the fact, which cost a whole test
	//! round. A step costs one line and names the phase that did not finish;
	//! the free-heap figure beside it catches exhaustion directly.
	//!
	//! Step names and times are also kept in the ring above, so the report
	//! can print a phase-duration table at the end: per-step lines say when,
	//! the table says how long each phase took.
	static void LogStepImpl(const char *fmt, va_list args, bool pulse)
	{
		std::string out;
		if (!stepHeaderWritten)
		{
			out += "\n\nBoot progress\n-------------\n"
				   "Written as each step completes. If the log stops inside this\n"
				   "section, the step after the last line is the one that did not\n"
				   "finish - it was still running when the console stopped.\n\n";
			stepHeaderWritten = true;
		}
		char buf[256];
		vsnprintf(buf, sizeof(buf), fmt, args);
		if (!bootClockStart)
			bootClockStart = gettime();
		//! Every ordinary logged step flips the light, so the blink rate IS
		//! the work rate and a light that stops tells the tester which step
		//! hung. Checkpoint lines (below) skip this: they own the light
		//! explicitly and a toggle here would cancel the state reported.
		if (pulse)
			PulseLight();
		const u32 now = BootElapsedMs();
		try
		{
			Addf(out, "  %-52s %6u ms  MEM2 free %u KB\n", buf,
				 (unsigned) now,
				 (unsigned) (MEM2_freesize() / 1024));
			AppendLog(out);
		}
		catch (...)
		{
			//! The diagnostic channel must not become the fault: the light
			//! above already flipped, and a string-growth failure here gets
			//! one gecko line instead of a silent death. (Unwinding is
			//! already relied upon: ValidateTable reports OOM this way.)
			gprintf("Riivo: log step failed (%s)\n", buf);
		}
		if (stepMarkCount < sizeof(stepMarks) / sizeof(stepMarks[0]))
		{
			StepMark &m = stepMarks[stepMarkCount++];
			strncpy(m.name, buf, sizeof(m.name) - 1);
			m.name[sizeof(m.name) - 1] = 0;
			m.ms = now;
		}
	}

	static void LogStep(const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		LogStepImpl(fmt, args, true);
		va_end(args);
	}

	//! Checkpoint line that leaves the light exactly as it found it, for
	//! the validation boundary whose states LightSet owns. Same text,
	//! timing, persist checks, and phase-table row as an ordinary step -
	//! only the toggle is skipped, so a state set just before it survives.
	static void LogCheckpoint(const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		LogStepImpl(fmt, args, false);
		va_end(args);
	}

	//! Phase durations from the ring above, oldest first. Step-to-step
	//! deltas: how long each phase took, not just when it finished. Covers
	//! pre-shutdown steps only - this prints from ReportLaunch, which runs
	//! before device shutdown, so shutdown itself and the final jump are
	//! not timed here.
	static void AppendStepTimings(std::string &out)
	{
		if (stepMarkCount < 2)
			return;
		out += "\nPhase durations, pre-shutdown steps only (step-to-step deltas)\n";
		for (u32 i = 1; i < stepMarkCount; ++i)
		{
			Addf(out, "  %-44s +%6u ms\n", stepMarks[i].name,
				 stepMarks[i].ms - stepMarks[i - 1].ms);
		}
	}

	//! The loader's own progress window, driven across the two phases that
	//! can take minutes on a large mod. The GUI threads are still running
	//! at this point - ExitGUIThreads() fires only on Wii U - so this is
	//! the same machinery the rest of the loader already uses here.
	//!

	// --------------------------------------------------------------------
	// 1. cIOS survey
	// --------------------------------------------------------------------

	void ReportCios(bool filesWanted)
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
		const s32 effSlot = IOS_GetVersion();
		bool effHasInfo = false;
		u32 effVersion = 0;
		char effName[0x11] = {0}, effVers[0x11] = {0};
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
				 slot == effSlot ? "   <== in use" : "");
			if (slot == effSlot)
			{
				effHasInfo = true;
				effVersion = info->version;
				memcpy(effName, name, sizeof(effName));
				memcpy(effVers, vers, sizeof(effVers));
			}
			free(info);
			++found;
		}

		ISFS_Deinitialize();

		if (found == 0)
			out += "  (none - no slot in 200-254 carries a cIOS info block)\n";

		out += "\nA slot with no line above either holds no title at all, or holds a cIOS\n"
			   "that does not publish the d2x info block - a Hermes cIOS or a custom\n"
			   "build, say. Those are the ones the read hook may not recognise.\n";

		//! The verdict the Play-click check cannot give: on AUTO the boot
		//! resolver picks the running slot from the disc's requested base, so
		//! only here - after the reload into it - is the effective slot known.
		//! Same predicate as GameWindow's RiivoInfoIsBeta3 (name, numeric v11,
		//! beta3 string); keep the two in agreement.
		if (filesWanted)
		{
			const bool beta3 = effHasInfo && effVersion == 11
				&& strncasecmp(effName, "d2x", 3) == 0
				&& strncasecmp(effVers, "beta3", 5) == 0;
			out += "\nEffective slot verdict (the cIOS above marked in use runs the game):\n";
			if (beta3)
				Addf(out, "  slot %d IS d2x v11 beta3 - file replacement supported.\n",
					 (int) effSlot);
			else
			{
				Addf(out, "  slot %d is NOT d2x v11 beta3", (int) effSlot);
				if (effHasInfo)
					Addf(out, " (found %s v%u %s)", effName,
						 (unsigned) effVersion, effVers);
				else
					out += " (no info block in that slot)";
				out += " - the mod's files cannot be served; the boot continues\n"
					   "  without them.\n";
			}
		}

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
	//! Also returns the DOL's disc offset, so the section table can be read
	//! next: naming which DOL section an apploader-loaded range came from is
	//! how a range like "the 8 KB below the FST" gets traced to its purpose.
	//! And the reservation size from boot.bin+0x42C (same sector, no extra
	//! read): the apploader reserves exactly that many bytes for the table,
	//! so a rebuild that fits it can stay in place instead of relocating
	//! below into memory game startup may clear (general policy: in-place
	//! only until the patched boot view lands).
	static bool ReadDiscFst(u8 **outData, u32 *outSize, u32 *outOffset,
							 u32 *outDolOffset, u32 *outMaxSize, std::string &err)
	{
		*outData = 0;
		*outSize = 0;
		*outOffset = 0;
		*outDolOffset = 0;
		if (outMaxSize)
			*outMaxSize = 0;

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
		//! Reservation the apploader will set aside (0x42C, same >>2 units).
		//! Zero or insane means unknown, never zero room: the caller then
		//! behaves exactly as before this value existed.
		const u32 fstMax = be32(hdr + 0x0C) << 2;

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
		*outDolOffset = be32(hdr) << 2;
		//! A reservation smaller than the table it reserves is nonsense;
		//! cap it at unknown rather than refuse a boot that worked before.
		if (outMaxSize)
			*outMaxSize = (fstMax >= fstSize && fstMax <= 0x00800000)
						  ? fstMax : 0;
		return true;
	}

	//! Read the DOL header (18 section records + BSS + entry, 0xE4 bytes off
	//! the disc) so loaded ranges can be traced to their section and disc
	//! offset later. One small aligned read in a phase already doing disc
	//! IO; a failure just leaves the table empty and the evidence block
	//! says so.
	static void ReadDolSections(u32 dolOffset, std::string &out)
	{
		dolSectionCount = 0;
		dolBssAddr = dolBssSize = 0;
		dolImageBase = 0;
		if (dolOffset == 0)
		{
			out += "disc DOL : no offset in boot.bin, section table unavailable\n";
			return;
		}
		static u8 dolHdr[0x100] ATTRIBUTE_ALIGN(32);
		if (WDVD_Read(dolHdr, sizeof(dolHdr), dolOffset) < 0)
		{
			out += "disc DOL : header unreadable, section table unavailable\n";
			return;
		}
		for (u32 i = 0; i < 18; ++i)
		{
			dolSections[i].fileOff = be32(dolHdr + 4 * i);
			dolSections[i].addr = be32(dolHdr + 0x48 + 4 * i);
			dolSections[i].size = be32(dolHdr + 0x90 + 4 * i);
			dolSections[i].index = i;
			dolSections[i].text = (i < 7);
			if (dolSections[i].size > 0)
				++dolSectionCount;
		}
		dolBssAddr = be32(dolHdr + 0xD8);
		dolBssSize = be32(dolHdr + 0xDC);
		dolImageBase = dolOffset;
		Addf(out, "disc DOL : offset 0x%08x, %u section(s), BSS [%08x, %08x)\n",
			 dolOffset, dolSectionCount, dolBssAddr, dolBssAddr + dolBssSize);
	}

	//! BSS occupancy for placement: absent (size zero, normal), valid, or
	//! invalid header values. An invalid non-empty BSS is a malformed entry
	//! like any other - placement must not claim clearance it cannot check.
	enum BssState { BSS_ABSENT, BSS_VALID, BSS_INVALID };
	static BssState CheckBss(u32 &lo, u32 &hi)
	{
		lo = hi = 0;
		if (dolBssSize == 0)
			return BSS_ABSENT;
		if (dolBssAddr < MEM1_BASE || dolBssSize > MEM1_END - dolBssAddr)
			return BSS_INVALID;
		lo = dolBssAddr;
		hi = dolBssAddr + dolBssSize;
		return BSS_VALID;
	}

	//! Which DOL section fully contains [lo,hi), or -1. Sets isBss when the
	//! range sits inside BSS instead - zero-filled by the apploader, so it
	//! has an address but no disc offset. Anything else (apploader scratch,
	//! a range no section claims) is neither.
	static int FindDolSection(u32 lo, u32 hi, bool &isBss)
	{
		isBss = false;
		for (u32 i = 0; i < sizeof(dolSections) / sizeof(dolSections[0]); ++i)
		{
			if (dolSections[i].size == 0)
				continue;
			const u32 sLo = dolSections[i].addr;
			const u32 sHi = sLo + dolSections[i].size;
			if (sHi < sLo)
				continue;
			if (lo >= sLo && hi <= sHi)
				return (int) dolSections[i].index;
		}
		if (dolBssSize > 0 && lo >= dolBssAddr
			&& hi <= dolBssAddr + dolBssSize
			&& dolBssAddr + dolBssSize >= dolBssAddr)
			isBss = true;
		return -1;
	}

	//! Apploader-retained words around 0x81201b80, where the reference scan
	//! found the original FST address at +0x10. 0x81200000 is where this
	//! loader puts the apploader image (apploader.c). Writers to that
	//! window: the image load itself, the apploader while running, a later
	//! disc read landing on the same destination (every yield is recorded,
	//! so this is checkable, not assumed), and - if the heap ever reaches
	//! that high - our own loader heap over it (see the break line below).
	//! Our tree reads nothing back from it. Two compares, in priority
	//! order: first the chunk's OWN recorded source (a later read to this
	//! address supersedes the image bytes as the explanation), then the
	//! image bytes as context. Per word in both: the +0x10 verdict stands
	//! on its own, independent of the neighbors. A difference from either
	//! source proves post-source change, not a live reference; an unchanged
	//! word may still be read by live code. Either way this function only
	//! reads: whether relocation must also update +0x10 depends on a
	//! consumer no static audit can name (the apploader itself is dead
	//! post-run; game startup is unobservable from here), so that decision
	//! waits on this log - not on a global word replace, which is
	//! explicitly not done.
	static void AppendApploaderStructEvidence(std::string &out)
	{
		//! RAM addresses only - no disc reads in this window. The struct
		//! address comes from the T0 reference scan; ramBase backs off
		//! 0x40 so the struct sits mid-window.
		static const u32 ramBase = 0x81201b40;   // struct - 0x40

		out += "\nApploader struct around 81201b80 (read-only - nothing updated)\n";
		out += "--------------------------------------------------------------\n";

		//! The 8 words at the struct, as the CPU reads them, each compared
		//! against its disc twin on its own. Neighbors name the shape: boot
		//! words beside +0x10 would read as the apploader's working copy of
		//! what it wrote to low memory; code bytes around it would read as
		//! an embedded constant instead. The +0x10 marker is re-checked
		//! live against low memory rather than assumed from T0.
		const u32 fstNow = *(vu32 *) 0x80000038;
		for (u32 i = 0; i < 8; ++i)
		{
			const u32 w = *(const volatile u32 *) (uintptr_t) (ramBase + 0x40 + 4 * i);
			Addf(out, "  +0x%02x : %08x%s\n", 4 * i, w,
				 (4 * i == 0x10 && w == fstNow)
				 ? "  <-- equals the FST address right now" : "");
		}
		out += "  (disc compares removed - see note above; the yield list still names each read's source)\n";

		//! Third writer candidate: if the newlib break has passed the
		//! struct, loader-heap objects may overlay the image - undecided,
		//! like everything else here, but excludable per boot.
		const u32 brk = (u32) (uintptr_t) sbrk(0);
		if (brk > ramBase + 0x40)
			Addf(out, "  newlib break %08x reaches the struct: loader-heap overlay possible (candidate, undecided)\n",
				 brk);
		else
			Addf(out, "  newlib break %08x is below the struct: loader heap cannot have stored there\n",
				 brk);
	}


	//! The rebuilt table, waiting for the apploader to finish so it can be put
	//! into the game's memory. Held in MEM2 on purpose: the apploader fills MEM1
	//! with the game and would walk straight over anything parked there.
	static u8 *&pendingFst = g_launch.stageBytes;
	static u32 &pendingFstSize = g_launch.stageSize;
	//! CRC of the staged table, captured when it is staged - not recomputed
	//! from the staging buffer at install time, which would bless a buffer
	//! that rotted in MEM2 in between.
	static u32 &pendingFstCrc = g_launch.stageCrc;

	//! Where it is going, worked out once the apploader has filled the
	//! boot-info block in. The copy into MEM1 does NOT happen there: the
	//! game's table sits at the top of MEM1, which is also inside the
	//! loader's own heap (measured: table 817c8100..817fffe9, loader arena
	//! 81581000..817feff0), and ShutDownDevices, gamepatches and the memory
	//! patches all still run after this point and all allocate. Anything
	//! written here can be handed straight back out by malloc and
	//! overwritten before the game ever sees it - which verifies perfectly
	//! at install time and then black-screens. So the placement is kept and
	//! the write is done last, by InstallPendingFst, just before the jump.
	static FstPlacement &pendingPlace = g_launch.place;
	static bool &pendingPlaceOk = g_launch.placeOk;

	struct ReadVerifyContext {
		FILE *file;
		std::string path;
		ReadVerifyContext() : file(0) {}
		~ReadVerifyContext() { if (file) fclose(file); }
	};
	static s32 ReadLargeDisc(void *, void *buffer, u32 length, u64 offset) {
		return WDVD_Read(buffer, length, offset);
	}
	static int CompareLargeFile(void *opaque, const char *path, u64 offset,
								const void *data, u32 length) {
		ReadVerifyContext &ctx = *(ReadVerifyContext *)opaque;
		if (ctx.path != path || !ctx.file) {
			if (ctx.file) fclose(ctx.file);
			ctx.path = path;
			ctx.file = fopen(path, "rb");
		}
		if (!ctx.file || offset > 0x7fffffffULL || fseek(ctx.file, (long)offset, SEEK_SET))
			return -1;
		u8 expected[4096];
		for (u32 at = 0; at < length; ) {
			const u32 count = std::min((u32)sizeof(expected), length - at);
			if (fread(expected, 1, count, ctx.file) != count) return -1;
			if (memcmp(expected, (const u8 *)data + at, count)) return 1;
			at += count;
		}
		return 0;
	}
	static void LogPendingRead(void *, const char *path, u64 fileOffset,
							   u64 discOffset, u32 length, ReadVerifyKind kind) {
		std::string line;
		Addf(line, "  pending %s LOW_READ disc=%010llx file=%llu bytes=%u: ",
			kind == READ_VERIFY_FRAGMENT_BOUNDARY ? "boundary" : "large",
			(unsigned long long)discOffset, (unsigned long long)fileOffset, length);
		line += path;
		line += "\n";
		// AppendLog flushes and closes before IOS is called. A hang leaves
		// the exact request here, instead of losing the entire test report.
		AppendLog(line);
	}

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
						 const std::vector<u8> &newFst,
						 u64 fstDiscOffset, u32 fstDiscSize);
	static bool ArmBootView(std::string &out, u64 fstDiscOffset, u32 fstDiscSize);
	static bool EmitManifest(std::string &out,
							 const std::vector<PlacedFile> &placed);

	static void Activate(std::string &out, const FragPlan &plan,
						 const std::vector<PlacedFile> &placed,
						 const std::vector<u8> &newFst,
						 u64 fstDiscOffset, u32 fstDiscSize)
	{
		if (placed.empty())
		{
			out += "  Nothing was placed, so there is nothing to switch on.\n";
			withholdStage = "NOTHING_PLACED";
			return;
		}

		//! Reads every placed file back through the hook. On a large mod
		//! that is minutes of card traffic with nothing else on screen.

		//! The fragments went in back in SetupDisc, inside the list the loader
		//! handed over with set_frag_list. Nothing is registered here: a late
		//! re-register historically returned -128, and registration stays
		//! pre-partition (the working order) until a hardware capture names
		//! the actual refusal - the sourced d2x gate (stealth_mode, on by
		//! default, AND running_title; wiidev/d2x-cios@33ad1ee dip-plugin
		//! FRAG_SET via Stealth_CheckRunningTitle, set only by ES launch or
		//! PPC mload-32, cleared only by IOS reload) is never armed by this
		//! loader's flow, and partition-open sets nothing per that source.
		if (!fragsRegistered)
		{
			out += "  The mod's fragments were never registered, so there is\n"
				   "  nothing for the rebuilt table to point at.\n";
			withholdStage = "NOT_REGISTERED";
			return;
		}
		Addf(out, "  fragments            : %u -> %u of %u, registered in SetupDisc\n",
			 fragStats.fragsBefore, fragStats.fragsAfter, RIIVO_FRAG_MAX);
		sumPlaced = (u32) placed.size();
		sumFailed = 0;

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

		//! Sampled by default. Every file was the right trade while a hang told
		//! us nothing about which file - but 2789 files is 5578 disc reads on a
		//! screen that is already black, and that cost is why a tester never got
		//! far enough to report anything at all. What this check exists to catch
		//! is systematic: the wrong drive, the wrong LBA base, a dispatch that
		//! did not take. All of those miss every file, so any sample finds them.
		//! The first and last file are always read, tail-recovered files are read
		//! unconditionally below, and riivolution/verify.txt restores every file
		//! for when one specific file is in doubt.
		static const size_t MAX_SAMPLED = 128;
		const size_t stride = (deepVerify || placed.size() <= MAX_SAMPLED)
							  ? 1 : (placed.size() + MAX_SAMPLED - 1) / MAX_SAMPLED;
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
	//! the loops actually read it back.
	//!
	//! Matched against the registration records, not just the placed list:
	//! a recovered file can be registered early but absent late (no disc
	//! counterpart without create=true, a refused table entry, a stat that
	//! failed between phases). Nothing reads those offsets, so they are
	//! harmless - but the retained record still names the file, and its
	//! bytes still get proven rather than assumed.
	std::vector<u64> placedOffsets;
	placedOffsets.reserve(placed.size());
	for (size_t j = 0; j < placed.size(); ++j)
		placedOffsets.push_back(placed[j].offset);
	std::vector<RecMatch> recMatches;
	std::vector<u64> unregistered;
	ReconcileRecovered(placedOffsets, modRecords, modSkips,
					   fragStats.extended, recMatches, unregistered);
	if (!unregistered.empty())
	{
		out += "  The rebuilt table references file offset(s) with no registration\n"
			   "  record, so activation cannot prove what serves them.\n";
		for (size_t i = 0; i < unregistered.size() && i < 8; ++i)
			Addf(out, "    0x%010llx\n", (unsigned long long) unregistered[i]);
		if (unregistered.size() > 8)
			Addf(out, "    ... and %u more\n",
				 (unsigned) (unregistered.size() - 8));
		out += "  Rebuilt FST and dependent memory patches are withheld.\n";
		withholdStage = "UNREGISTERED";
		return;
	}
	for (size_t k = 0; k < recMatches.size(); ++k)
	{
		const RecMatch &m = recMatches[k];
		if (m.state == REC_UNKNOWN)
		{
			Addf(out, "  A file rescued from an under-reported tail cluster at 0x%010llx\n"
					  "  matches no registered file, so it cannot be read back.\n",
				 (unsigned long long) fragStats.extended[k]);
			out += "  Rebuilt FST and dependent memory patches are withheld.\n";
			withholdStage = "UNREGISTERED";
			return;
		}
		if (m.state == REC_INACTIVE)
		{
			const RegRecord &rec = modRecords[m.index];
			Addf(out, "    recovered but unreferenced: %s at 0x%010llx (%u bytes): %s\n",
				 rec.external.c_str(), (unsigned long long) rec.offset,
				 rec.length, SkipReasonText(m.reason));
			++extendedVerified;
			if (!VerifyModFragment(rec.offset, rec.length, rec.external, why)) {
				if (failed < MAX_NAMED)
					Addf(failures, "    %s\n", why.c_str());
				++failed;
			}
			continue;
		}
		const size_t idx = m.index;
		++extendedVerified;
		if (idx % stride == 0 || idx == placed.size() - 1)
			continue; // the sample above already read this one back
		if (!VerifyModFragment(placed[idx].offset, placed[idx].length, placed[idx].external, why)) {
			if (failed < MAX_NAMED)
				Addf(failures, "    %s\n", why.c_str());
			++failed;
		}
	}
		Addf(out, "  LOW_READ checks      : first/last-byte sample of %u of %u files passed (interior bytes are not covered by this check)\n",
			 (unsigned)verified, (unsigned)placed.size());
		if (extendedVerified)
			Addf(out, "  tail recovery        : %u extended file(s) verified unconditionally\n",
				 (unsigned)extendedVerified);
		if (failed)
		{
			sumFailed = (u32) failed;
			Addf(out, "  Read-back FAILED for %u file(s):\n", (unsigned)failed);
			out += failures;
			if (failed > MAX_NAMED)
				Addf(out, "    ... and %u more\n", (unsigned)(failed - MAX_NAMED));
			out += "  Rebuilt FST and dependent memory patches are withheld.\n";
			withholdStage = "READBACK";
			return;
		}
		if (!deepVerify)
		{
			//! The generic "minutes" warning is wrong for small mods - a
			//! 590 KB payload verifies in seconds. Scale the cost by what is
			//! actually staged so the advice fits the mod in question.
			u64 verifyBytes = 0;
			for (size_t i = 0; i < placed.size(); ++i)
				verifyBytes += placed[i].length;
			const char *cost = verifyBytes < 2ULL * 1024 * 1024 ? "seconds of a"
							 : verifyBytes < 20ULL * 1024 * 1024 ? "well under a minute of"
							 : "minutes of a";
			out += "\nLarge-read verification: SKIPPED.\n"
				   "  It reads the whole mod back through the cIOS and compares it\n";
			Addf(out, "  against the card - on a mod this size that is %s\n"
					  "  black screen. The per-file check above already proves every\n"
					  "  file's head and tail map. Create riivolution/verify.txt to run it.\n",
				 cost);
		}
		else
		{
			const FragList *retained = frag_list_get();
			void *scratch = MEM2_alloc(READ_VERIFY_CHUNK);
			if (!retained || !scratch) {
				if (scratch) MEM2_free(scratch);
				out += "  Large-read verification unavailable; FST withheld.\n";
				withholdStage = "LARGE_VERIFY";
				return;
			}
			out += "\nLarge-read verification (128 KiB maximum single request)\n";
			AppendLog(out);
			out.clear();
			ReadVerifyCallbacks callbacks;
			ReadVerifyContext context;
			callbacks.context = &context;
			callbacks.readDisc = ReadLargeDisc;
			callbacks.compareFile = CompareLargeFile;
			callbacks.pendingRead = LogPendingRead;
			ReadVerifyStats readStats;
			const bool largeOK = VerifyLargeReads(placed, *retained, bootSectorSize,
				scratch, READ_VERIFY_CHUNK, callbacks, readStats);
			MEM2_free(scratch);
			Addf(out, "  full files selected=%u internal-multifragment files=%u\n",
				 readStats.fullFiles, readStats.multiFragmentFiles);
			Addf(out, "  full calls=%u boundary calls=%u bytes compared=%llu largest successful call=%u\n",
				 readStats.fullReads, readStats.boundaryReads,
				 (unsigned long long)readStats.totalBytes, readStats.largestRead);
			Addf(out, "  failed files=%u failed calls=%u\n", readStats.failedFiles, readStats.failedReads);
			for (size_t i = 0; i < readStats.failureDetails.size(); ++i) {
				const ReadVerifyStats::FailureDetail &failure = readStats.failureDetails[i];
				Addf(out, "    disc=%010llx file=%llu request=%u read=%d ",
					 (unsigned long long)failure.discOffset,
					 (unsigned long long)failure.fileOffset, failure.requestLength,
					 (int)failure.readResult);
				if (failure.compareResult == READ_VERIFY_COMPARE_NOT_RUN)
					out += "compare=not-run: ";
				else
					Addf(out, "compare=%d (-1=file I/O, 1=mismatch): ", failure.compareResult);
				out += failure.path + "\n";
			}
			if (readStats.failedFiles > readStats.failureFiles.size())
				Addf(out, "    ... and %u more\n", readStats.failedFiles - (u32)readStats.failureFiles.size());
			if (!largeOK) {
				if (!readStats.fatal.empty()) out += "  " + readStats.fatal + "\n";
				out += "  Large-read verification failed; FST withheld.\n";
				withholdStage = "LARGE_VERIFY";
				return;
			}
			out += "  Large-read verification passed. Larger single calls remain untested.\n";
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
				withholdStage = "LAYER_PROBE";
				return;
			}
		}
		if (WDVD_Read(check, sizeof(check), modRegionEnd) == 0) {
			out += "  LOW_READ past mod end succeeded; FST withheld.\n";
			withholdStage = "PAST_END";
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
		//! DIAGNOSTIC (riivolution/relocorig.txt): stage the verbatim
		//! original table plus zero padding instead of the rebuilt one.
		//! Same placement, same install, same hook and fragments - the only
		//! difference from a normal boot is the installed bytes. If this
		//! boots and the created-file table does not, the fault is the new
		//! table content; if this dies the same way, it is the relocation
		//! itself. The pad mirrors T0's measured relocation (153934-153792
		//! = 142, 160 with 32-alignment) so the geometry matches exactly.
		static const u32 RELOC_ORIG_PAD = 160;
		const bool stageRelocOrig = (relocOrig && !relocOrigRaw.empty());
		const u8 *stageSrc = stageRelocOrig ? &relocOrigRaw[0] : &newFst[0];
		const u32 stageSize = stageRelocOrig ? (u32) relocOrigRaw.size() + RELOC_ORIG_PAD
											 : (u32) newFst.size();
		u8 *staged = (u8 *) MEM2_alloc(stageSize);
		if (!staged)
		{
			out += "  Out of memory for the rebuilt table, so it will not be\n"
				   "  installed. The patch above is harmless on its own.\n";
			withholdStage = "NO_MEMORY";
			return;
		}
		if (stageRelocOrig)
		{
			memcpy(staged, stageSrc, relocOrigRaw.size());
			memset(staged + relocOrigRaw.size(), 0, RELOC_ORIG_PAD);
		}
		else
			memcpy(staged, stageSrc, stageSize);
		//! Captured now, from the bytes just staged - the install step
		//! compares the installed region against this, so a staging buffer
		//! that rotted in MEM2 in between still fails instead of blessing
		//! itself. Staged through the launch owner: a second staging in one
		//! boot withholds instead of swapping tables under a booking.
		if (!g_launch.Stage(staged, stageSize, Crc32(staged, stageSize)))
		{
			MEM2_free(staged);
			withholdStage = "FST_WITHHELD";
			return;
		}

		Addf(out, "  rebuilt table        : %u bytes held, ready to install\n",
			 pendingFstSize);
		if (stageRelocOrig)
			out += "\n  DIAGNOSTIC: riivolution/relocorig.txt is present: staged\n"
				   "  the VERBATIM original table plus 160 zero bytes, NOT the\n"
				   "  rebuilt one. Placement, install, hook and fragments are\n"
				   "  exactly as usual. A normal boot from here means the fault\n"
				   "  is the new table content; the same failure means it is\n"
				   "  the relocation itself.\n";
		else if (relocOrig)
			out += "\n  DIAGNOSTIC: riivolution/relocorig.txt is present but no\n"
				   "  original bytes were retained; the normal rebuilt table\n"
				   "  was staged instead. This run does not test relocation.\n";
		if (skipFstInstall)
			out += "\n  DIAGNOSTIC: riivolution/nofstinstall.txt is present, so the\n"
				   "  rebuilt table will NOT be installed. The hook and the mod's\n"
				   "  fragments are live exactly as usual, but the game keeps its\n"
				   "  own file table and will not see the mod's files. A normal\n"
				   "  boot from here means the fault is the install or the table;\n"
				   "  the same failure means it is the hook or the fragments.\n";
		else
			out += "\n  Riivolution is prepared for this boot; the table installs\n"
				   "  last, just before the jump, and only a verified install runs.\n";

		//! Arm the patched boot view and prove it through the real read path.
		//! FST coverage serves the just-staged bytes at the disc FST range;
		//! DOL coverage serves the composed executable positionally (unless
		//! the nofstinstall diagnostic holds it to stock). Verification reads
		//! go through WDVD_Read - the same function the apploader calls - so
		//! a passing check proves overlay bytes, offsets and lengths agree
		//! on real PPC, not just in a host harness. Any failure withholds
		//! with code 8 (card log authoritative; stock boot, no blink).
		if (!ArmBootView(out, fstDiscOffset, fstDiscSize))
			return;
		//! Emit the RIV1 manifest: the staged segment contract for a future
		//! runtime, cross-checked against the served placements now. A
		//! mismatch means the plan and the placements drifted - withhold
		//! rather than serve either.
		if (!EmitManifest(out, placed))
			return;
	}

	//! Bounded verification buffers (one IOS-sized chunk each). Static: no
	//! per-boot allocation failure mode, freed never (loader lifetime).
	static u8 verifyBounce[32768] ATTRIBUTE_ALIGN(32);
	static u8 verifyExpect[32768] ATTRIBUTE_ALIGN(32);

	//! Withhold a staged table with an explicit stage: free the staging
	//! buffer, release + refuse through the launch owner (sticky: no later
	//! Book can install it), disarm the boot view, and log the reason.
	//! The boot then proceeds stock with memory held back; post-shutdown
	//! InstallPendingFst finds nothing to do and blinks nothing (code 8 is
	//! log-only by design - the card is still alive here).
	static void WithholdStaged(std::string &out, const char *stage,
							   const char *detail)
	{
		if (pendingFst)
			MEM2_free(pendingFst);
		g_launch.ReleaseStaging();
		g_launch.Refuse(8);
		DeactivateBootView();
		withholdStage = stage;
		out += detail;
	}

	//! Verify one DOL sample: read [dolBase+rel, +len) through the production
	//! path and compare against FAT bytes at srcOff. Bounded by the caller.
	static bool VerifyDolSample(u64 dolBase, u64 rel, const std::string &path,
								u64 srcOff, u32 len, std::string &why)
	{
		if (len == 0 || len > sizeof(verifyBounce))
		{
			why = "sample length out of bounds";
			return false;
		}
		if (WDVD_Read(verifyBounce, len, dolBase + rel) != 0)
		{
			why = "production read failed";
			return false;
		}
		if (!BootFatReader(path, srcOff, verifyExpect, len, 0)
			|| memcmp(verifyBounce, verifyExpect, len) != 0)
		{
			why = "served bytes differ from the mod file";
			return false;
		}
		return true;
	}

	//! Arm the patched boot view and prove it through the real read path.
	//! FST coverage serves the just-staged bytes at the disc FST range; DOL
	//! coverage serves the composed executable positionally (held to stock
	//! under the nofstinstall diagnostic). FST read-back is full but chunked
	//! (bounded 32 KB peak); DOL sampling covers head+tail of the first
	//! runs plus one original-identity and one zero sample. Any failure
	//! withholds with code 8 (stock boot, memory held back, no blink).
	static bool ArmBootView(std::string &out, u64 fstDiscOffset,
							u32 fstDiscSize)
	{
		static u8 ovHeader[BOOTVIEW_HEADER_BYTES] ATTRIBUTE_ALIGN(32);
		if (WDVD_ReadStock(ovHeader, sizeof(ovHeader), 0) != 0)
		{
			WithholdStaged(out, "BOOTVIEW",
				"  boot view: stock header unreadable, cannot arm; FST withheld.\n");
			return false;
		}
		// Virtual header: the apploader reads these words through its own
		// first triple (partition 0x420, 32 bytes - traced) and establishes
		// the FST destination and size from them (traced publication; a
		// coherent grown view boots to idle in the reference backend with
		// the base below the stock reservation). 0x420/0x424 stay disc
		// truth (DOL offset, FST disc offset - the disc image is immutable);
		// 0x428/0x42C both carry the staged size, matching the proven
		// coherent header shape, so size-driven and max-driven apploader
		// logic agree. In-place, same-size and compacted stagings keep the
		// stock-identical header: zero behavior change there. Held to stock
		// under nofstinstall like everything installable: the diagnostic
		// splits hook-vs-table with a stock boot, and a grown header would
		// corrupt that split.
		std::vector<u8> patchedHeader(ovHeader, ovHeader + sizeof(ovHeader));
		bool grownHeader = false;
		if (pendingFstSize > fstDiscSize && !skipFstInstall)
		{
			std::string hwhy;
			if (!PatchBootHeader(ovHeader, sizeof(ovHeader), fstDiscOffset,
								 pendingFstSize, pendingFstSize,
								 patchedHeader, hwhy))
			{
				char line[224];
				snprintf(line, sizeof(line),
					"  boot view: grown header refused (%s); FST withheld.\n",
					hwhy.c_str());
				WithholdStaged(out, "BOOTVIEW", line);
				return false;
			}
			grownHeader = true;
			Addf(out, "  boot view : virtual header serves grown size/max %u (disc %u)\n",
				 pendingFstSize, fstDiscSize);
		}
		grownHeaderServed = grownHeader;
		std::vector<u8> stagedView(pendingFst, pendingFst + pendingFstSize);
		std::string why;
		if (!bootView.Activate(ovHeader, sizeof(ovHeader), patchedHeader, why))
		{
			char line[192];
			snprintf(line, sizeof(line),
				"  boot view: activation refused (%s); FST withheld.\n", why.c_str());
			WithholdStaged(out, "BOOTVIEW", line);
			return false;
		}
		// FST coverage serves the staged table only when it is exactly what
		// the advertised header names: the apploader asking for the whole
		// table must get a whole table - never a truncated prefix (shorter
		// staging) nor bytes no disc reader asked for. Under a stock header
		// the advertised size is the disc size, so grown/compacted staging
		// falls back to stock bytes (the coherent uninstalled boot); under
		// a grown virtual header the advertised size is the staged size,
		// so the apploader receives the entire grown table it was told
		// about. Either way a withhold later cannot strand a partial image.
		bool fstServed = false;
		const u32 advertisedSize = grownHeader ? pendingFstSize : fstDiscSize;
		if (bootView.ArmFst(fstDiscOffset, stagedView, advertisedSize, why))
			fstServed = true;
		else
			Addf(out, "  boot view : FST served stock (%s; staged %u, advertised %u)\n",
				 why.c_str(), pendingFstSize, advertisedSize);
		if (haveStagedDol && !skipFstInstall)
		{
			if (stagedDolBase == 0 || stagedDol.discLengthOrig == 0)
			{
				WithholdStaged(out, "BOOTVIEW",
					"  boot view: DOL image base unknown; FST withheld.\n");
				return false;
			}
			if (!bootView.SetDol(stagedDolBase, stagedDol.discLengthOrig,
								 stagedDol, why))
			{
				char line[224];
				snprintf(line, sizeof(line),
					"  boot view: DOL coverage refused (%s); FST withheld.\n",
					why.c_str());
				WithholdStaged(out, "BOOTVIEW", line);
				return false;
			}
		}
		else if (haveStagedDol)
		{
			out += "  DIAGNOSTIC: DOL served stock under nofstinstall (see above).\n";
		}
		Addf(out, "  boot view : armed (FST %s%u bytes at 0x%08x%s)\n",
			 fstServed ? "" : "stock, staged ",
			 fstServed ? pendingFstSize : fstDiscSize, (unsigned)fstDiscOffset,
			 (haveStagedDol && !skipFstInstall) ? ", DOL image served" : "");
		// FST read-back through WDVD_Read (the apploader's function): proves
		// overlay bytes, offsets and lengths agree on real PPC - but only
		// when coverage is armed. The disc still holds stock bytes there -
		// a mismatch names the overlay, and only the overlay, as the fault.
		if (fstServed)
		{
			LogStep("verifying the boot view through WDVD_Read");
			u32 remaining = pendingFstSize;
			u64 off = fstDiscOffset;
			bool ok = true;
			while (remaining > 0 && ok)
			{
				u32 take = remaining > sizeof(verifyBounce)
						   ? sizeof(verifyBounce) : remaining;
				if (WDVD_Read(verifyBounce, take, off) != 0)
					ok = false;
				else if (memcmp(verifyBounce, pendingFst + (off - fstDiscOffset),
								take) != 0)
					ok = false;
				off += take;
				remaining -= take;
			}
			if (!ok)
			{
				WithholdStaged(out, "BOOTVIEW",
					"  boot view: FST read-back mismatch; FST withheld.\n");
				return false;
			}
			Addf(out, "  boot view : FST read-back matches staged bytes (%u)\n",
				 pendingFstSize);
		}
		// DOL samples: head+tail of the edge external runs (bounded eight),
		// one original-identity sample (overlay vs stock, same bytes), one
		// zero sample. Any failure withholds: no partial patched images.
		if (haveStagedDol && !skipFstInstall && bootView.HasDol())
		{
			u32 extRuns = 0;
			for (size_t i = 0; i < stagedDol.segs.size(); ++i)
				if (stagedDol.segs[i].kind == PlanSegment::SEG_EXTERNAL)
					++extRuns;
			u32 extSeen = 0, samples = 0;
			bool ok = true;
			std::string detail;
			u32 lastExt = 0;
			for (size_t i = 0; i < stagedDol.segs.size(); ++i)
				if (stagedDol.segs[i].kind == PlanSegment::SEG_EXTERNAL)
					lastExt = (u32)i;
			for (size_t i = 0; ok && i < stagedDol.segs.size(); ++i)
			{
				const PlanSegment &s = stagedDol.segs[i];
				if (s.kind != PlanSegment::SEG_EXTERNAL)
					continue;
				bool edge = (extSeen < 7) || (i == lastExt);
				++extSeen;
				if (!edge)
					continue;
				u32 head = s.length < 32768 ? s.length : 32768;
				if (!VerifyDolSample(stagedDolBase, s.fileOffset, s.external,
									 s.srcOffset, head, detail))
				{
					ok = false;
					break;
				}
				++samples;
				if (s.length > head)
				{
					u32 tailOff = s.length - (s.length - head < 32768
											  ? s.length - head : 32768);
					u32 tailLen = s.length - tailOff;
					if (!VerifyDolSample(stagedDolBase, s.fileOffset + tailOff,
										 s.external, s.srcOffset + tailOff,
										 tailLen, detail))
					{
						ok = false;
						break;
					}
					++samples;
				}
			}
			// Original-identity: first sufficiently long ORIGINAL run reads
			// identically through the overlay and the stock path.
			if (ok)
			{
				for (size_t i = 0; i < stagedDol.segs.size(); ++i)
				{
					const PlanSegment &s = stagedDol.segs[i];
					if (s.kind != PlanSegment::SEG_ORIGINAL || s.length < 4096)
						continue;
					if (WDVD_Read(verifyBounce, 4096,
								  stagedDolBase + s.fileOffset) != 0
						|| WDVD_ReadStock(verifyExpect, 4096,
										  stagedDolBase + s.fileOffset) != 0
						|| memcmp(verifyBounce, verifyExpect, 4096) != 0)
					{
						ok = false;
						detail = "original-identity sample differs";
					}
					else
						++samples;
					break;
				}
			}
			// Zero sample: first ZERO run reads back zeros.
			if (ok)
			{
				for (size_t i = 0; i < stagedDol.segs.size(); ++i)
				{
					const PlanSegment &s = stagedDol.segs[i];
					if (s.kind != PlanSegment::SEG_ZERO || s.length == 0)
						continue;
					u32 take = s.length < 4096 ? s.length : 4096;
					if (WDVD_Read(verifyBounce, take,
								  stagedDolBase + s.fileOffset) != 0)
					{
						ok = false;
						detail = "zero sample unreadable";
						break;
					}
					for (u32 k = 0; k < take; ++k)
					{
						if (verifyBounce[k] != 0)
						{
							ok = false;
							detail = "zero sample not zero";
							break;
						}
					}
					if (ok)
						++samples;
					break;
				}
			}
			if (!ok)
			{
				char line[256];
				snprintf(line, sizeof(line),
					"  boot view: DOL verification failed (%s); FST withheld.\n",
					detail.c_str());
				WithholdStaged(out, "BOOTVIEW", line);
				return false;
			}
			dolServing = true;
			Addf(out, "  boot view : DOL verified (%u external run(s), %u sample read(s))\n",
				 extRuns, samples);
		}
		return true;
	}

	//! Emit the RIV1 manifest: the staged segment contract for a future
	//! runtime, cross-checked against the served placements now. Shared
	//! implementation (BuildPlanManifest) with the host parity suite: a
	//! mismatch means the plan and the placements drifted - withhold
	//! rather than serve either. The digest (entries, bytes, blob size,
	//! crc) is the artifact to compare.
	static bool EmitManifest(std::string &out,
							 const std::vector<PlacedFile> &placed)
	{
		if (!haveActivePlan)
			return true; // no plan drove this boot; nothing to contract
		u32 discId = ((u32)bootGameId[0] << 24) | ((u32)bootGameId[1] << 16) |
					 ((u32)bootGameId[2] << 8) | (u32)bootGameId[3];
		// Partition index is untracked (0 = unspecified); readers must not
		// depend on it. The discovery LBA likewise defers to the module.
		std::vector<u8> blob;
		std::string why;
		if (!BuildPlanManifest(activePlan, placed, discId, blob, why))
		{
			char line[300];
			snprintf(line, sizeof(line),
				"  manifest: %s; withheld.\n", why.c_str());
			WithholdStaged(out, "MANIFEST", line);
			return false;
		}
		u64 totalBytes = 0;
		for (size_t i = 0; i < placed.size(); ++i)
			totalBytes += placed[i].length;
		u32 crc = (u32)blob[16] | ((u32)blob[17] << 8)
				  | ((u32)blob[18] << 16) | ((u32)blob[19] << 24);
		Addf(out, "  manifest RIV1 : %u extent(s), %llu byte(s), %u blob byte(s), crc %08x (validated)\n",
			 (unsigned)placed.size(), (unsigned long long)totalBytes,
			 (unsigned)blob.size(), crc);
		return true;
	}

	static bool ExternalFileSize(const std::string &path, u32 *outSize)
	{
		//! Stated during early enumeration: same boot, same card, so the
		//! late phase reuses it instead of walking libfat from the root
		//! again for every file. A miss stats as before.
		if (KnownFileSize(path, outSize))
			return true;
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
		modAddFails.clear();
		modSkips.clear();
		relocOrigRaw.clear();
		withholdStage = "FST_WITHHELD";

		std::string out;
		out += "\n\nFile and folder replacement\n"
			   "---------------------------\n"
			   "Worked out against the real disc. If every check below passes it is\n"
			   "switched on at the end of this section; if any one fails, nothing is\n"
			   "applied and the game boots untouched.\n\n";

		u8 *fstData = 0;
		u32 fstSize = 0, fstOffset = 0, dolOffset = 0, fstReserve = 0;
		std::string err;
		LogStep("reading the game's file table");
		if (!ReadDiscFst(&fstData, &fstSize, &fstOffset, &dolOffset, &fstReserve, err))
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

		//! Section table for tracing loaded ranges (see the statics). After
		//! the parse so a garbage FST never spends a disc read on sections
		//! nothing will use.
		ReadDolSections(dolOffset, out);
		// Disc FST range for late phases (dead-span identification).
		trackFstOff = fstOffset;
		trackFstSize = fstSize;

		Addf(out, "patches  : %u <file>, %u <folder>\n\n",
			 (unsigned) bootSet->files.size(), (unsigned) bootSet->folders.size());

		FsDirLister lister;
		std::vector<RedirectSpec> redirects;
		std::vector<CreatedFile> created;
		//! Walks every <folder> rule over the card a SECOND time. On a mod
		//! this size that is thousands more directory reads, and it used to
		//! happen with nothing on screen and nothing in the log.
		LogStep("matching the mod against the disc (reads the card again)");
		BuildRedirects(fst, *bootSet, bootDevice, &lister, redirects, &created);
		LogStep("matched: %u replacement(s), %u addition(s)",
				(unsigned) redirects.size(), (unsigned) created.size());

		//! Coherent plan (general pipeline): compose the same inputs into
		//! PlannedFiles with Dolphin-compatible segment semantics. FST sizes
		//! below derive from the plan's finalSize, not whole external sizes,
		//! so offset/fileoffset/length/resize reach the live rebuild path.
		//! Partial-segment non-DOL files refuse explicitly here (named
		//! limitation: the fragment runtime serves whole files only); the
		//! executable composes against the DOL image for boot-view serving
		//! (size-preserving by rule, verified below). Never silently apply
		//! whole-file bytes for a sub-range patch.
		{
			struct BootSizes : public FileSizeProvider
			{
				virtual bool GetSize(const std::string &external, u32 *outSize)
				{
					return ExternalFileSize(external, outSize);
				}
			} bootSizes;
			// DOL image span from the section table read above (DOL-relative
			// file offsets; the served image preserves this size exactly).
			// Zero when the header was unreadable: DOL entries then refuse
			// explicitly instead of composing against a guessed extent.
			u64 dolSize = 0;
			if (dolSectionCount > 0)
			{
				u32 offs[18], lens[18];
				for (u32 i = 0; i < 18; ++i)
				{
					offs[i] = dolSections[i].fileOff;
					lens[i] = dolSections[i].size;
				}
				dolSize = DolImageSize(offs, lens, 18);
			}
			PatchPlan plan;
			PlannedFile dolPlan;
			std::string planWhy;
			if (!BuildPatchPlan(fst, *bootSet, bootDevice, &lister,
								&bootSizes, plan, planWhy,
								dolSize, &dolPlan))
			{
				Addf(out, "plan FAILED: %s\n", planWhy.c_str());
				Addf(out, "\nNothing is applied for an unplannable mod. The game boots unmodified.\n");
				AppendLog(out);
				return;
			}
			if (!plan.errors.empty())
			{
				Addf(out, "unsupported enabled file operation(s) - refusing file work:\n");
				for (size_t i = 0; i < plan.errors.size() && i < 8; ++i)
					Addf(out, "  %s\n", plan.errors[i].c_str());
				if (plan.errors.size() > 8)
					Addf(out, "  ... and %u more\n",
						 (unsigned)(plan.errors.size() - 8));
				Addf(out, "\nNever silently apply a different operation. The game boots unmodified;\n"
						  "memory patches are held back via the file-work gate.\n");
				AppendLog(out);
				return;
			}
			if (plan.hasPartial)
			{
				Addf(out, "partial file replacement (offset/fileoffset/length/resize sub-ranges)\n"
						  "detected: the fragment runtime serves whole files only.\n");
				Addf(out, "Composed sizes would be:");
				for (size_t i = 0; i < plan.files.size() && i < 8; ++i)
				{
					if (!plan.files[i].wholeFile)
						Addf(out, " %s=%u", plan.files[i].disc.c_str(),
							 plan.files[i].finalSize);
				}
				Addf(out, "\nRefusing file work until the segment runtime lands;\n"
						  "the game boots unmodified (named limitation, not silent whole-file).\n");
				AppendLog(out);
				return;
			}
			// Persist for the late phases (single authoritative plan: FST
			// sizes, offset remap, manifest emission, boot-view arming).
			// Small: paths, sizes and file segments only, never payloads;
			// cleared per boot in BeginLaunch.
			activePlan = plan;
			haveActivePlan = true;
			// Stat/compose agreement: every whole-file final must equal the
			// size the early enumeration stated for the same pre-FST key
			// (same card, same boot, shared size cache). A mismatch means
			// the card changed between phases or the composer drifted from
			// the enumerator - refuse explicitly, never size either side.
			{
				std::map<std::string, u32> earlySize;
				for (size_t i = 0; i < modRecords.size(); ++i)
					earlySize[modRecords[i].disc] = modRecords[i].length;
				std::string drift;
				u32 drifted = 0;
				for (size_t i = 0; i < plan.files.size(); ++i)
				{
					const PlannedFile &f = plan.files[i];
					if (!f.wholeFile || f.finalSize == 0)
						continue;
					std::map<std::string, u32>::const_iterator it =
						earlySize.find(f.earlyKey);
					if (it == earlySize.end() || it->second != f.finalSize)
					{
						++drifted;
						if (drifted <= 8)
						{
							char b[192];
							snprintf(b, sizeof(b), " %s(plan %u, early %s)",
								f.disc.c_str(), f.finalSize,
								it == earlySize.end() ? "absent"
								: "different");
							drift += b;
						}
					}
				}
				if (drifted > 0)
				{
					Addf(out, "stat/compose disagreement on %u file(s):%s\n",
						 drifted, drift.c_str());
					if (drifted > 8)
						Addf(out, "  ... and %u more\n", drifted - 8);
					out += "  The early and late phases disagree about file sizes;\n"
						   "  building the table from either would mis-size reads.\n"
						   "  Withheld.\n";
					withholdStage = "SIZE_DRIFT";
					AppendLog(out);
					return;
				}
			}
			// Externals missing at composition time, filed for the skip audit
			// under the plan's (pre-FST) disc keys - the same identity the
			// early registration records carry.
			for (size_t i = 0; i < plan.missingExternals.size(); ++i)
			{
				const std::string &key = plan.missingExternals[i].disc;
				if (!key.empty()
					&& modAddFails.find(key) == modAddFails.end())
					modAddFails[key] = SKIP_STAT_FAILED;
			}
			if (dolPlan.bootFile)
			{
				dolPlan.dolBase = dolImageBase;
				stagedDol = dolPlan;
				stagedDolBase = dolImageBase;
				haveStagedDol = true;
				Addf(out, "executable : %u composed byte(s), image size %llu, %u segment(s)\n",
					 dolPlan.finalSize, (unsigned long long)dolSize,
					 (unsigned)dolPlan.segs.size());
			}
			// Whole-file plan agrees with redirects; FST sizes below use the
			// plan's finalSize (equal to external sizes here) as the single
			// authoritative source going forward.
		}

		//! Size accounting. A replacement bigger than the file it stands in for
		//! cannot be served by redirection alone: the file table still advertises
		//! the old length, so the game never asks for the extra bytes.
		//! Payload bytes come from the plan loop below (composed finals,
		//! deduplicated); this loop only classifies fits/grows.
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
		Addf(out, "  metadata ignored  : %d  (macOS ._ twins, .DS_Store, Thumbs.db)\n",
			 lister.skipped);
		//! The files the mod names that the card does not have. Without these
		//! names an unconfigured mod and a broken loader read identically -
		//! "0 found" and nothing else - and telling those apart cost a round
		//! of hardware tests. Each path here was built exactly as enumeration
		//! built it, so it is the path that was actually tried.
		if (!modMissing.empty())
		{
			Addf(out, "  NOT ON THE CARD   : %u file(s) the mod names but the card does not have:\n",
				 (unsigned) modMissing.size());
			for (size_t i = 0; i < modMissing.size() && i < 16; ++i)
				Addf(out, "    %s  (for disc path %s)\n",
					 modMissing[i].external.c_str(), modMissing[i].disc.c_str());
			if (modMissing.size() > 16)
				Addf(out, "    ... and %u more\n",
					 (unsigned) (modMissing.size() - 16));
			out += "  Nothing is applied for a file that is not there. Check the mod\n"
				   "  is fully unpacked and that its XML names these paths correctly.\n";
		}
		out += "\n";

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
		if (relocOrig && fstSize > 0)
		{
			//! Retain the verbatim disc bytes for the relocorig diagnostic:
			//! the builder below re-serializes and would hide exactly the
			//! content difference under test. Kept until Activate stages.
			relocOrigRaw.assign(fstData, fstData + fstSize);
		}
		free(fstData);
		fstData = 0;

		u32 planned = 0, rejected = 0;
		std::map<std::string, u32> expectedModSizes;
		//! Created files whose externals are not on the card. BuildRedirects
		//! counts them as additions without stating them, so without this
		//! list a missing folder looks like one phantom addition that plans
		//! nothing - exactly the confusion to avoid.
		std::vector<std::string> missingCreated;
		//! Router-divergence tripwire: the plan and the redirect builder must
		//! name the same file set (the plan skips DOL-routed entries, which
		//! never reach the builder - that exclusion is by design, not drift).
		//! Any other difference means the two routers disagree about what the
		//! XML selected, and the table must not be built from either.
		{
			std::map<std::string, char> routed;
			for (size_t i = 0; i < redirects.size(); ++i)
				routed[NormaliseDiscPath(redirects[i].disc)] = 1;
			for (size_t i = 0; i < created.size(); ++i)
				routed[NormaliseDiscPath(created[i].disc)] = 1;
			std::string drift;
			u32 drifted = 0;
			std::map<std::string, char> planned;
			for (size_t i = 0; i < activePlan.files.size(); ++i)
			{
				planned[activePlan.files[i].disc] = 1;
				if (routed.find(activePlan.files[i].disc) == routed.end())
				{
					++drifted;
					if (drifted <= 8)
						drift += " plan-without-redirect:" + activePlan.files[i].disc;
				}
			}
			for (std::map<std::string, char>::const_iterator it = routed.begin();
				 it != routed.end(); ++it)
			{
				if (planned.find(it->first) == planned.end())
				{
					++drifted;
					if (drifted <= 8)
						drift += " redirect-without-plan:" + it->first;
				}
			}
			if (drifted > 0)
			{
				Addf(out, "router divergence on %u file(s) (plan vs redirects):%s%s\n",
					 drifted, drift.c_str(),
					 drifted > 8 ? " ..." : "");
				out += "  The two file routers disagree; building the table from\n"
					   "  either would silently apply a different mod. Withheld.\n";
				withholdStage = "ROUTER_DRIFT";
				AppendLog(out);
				return;
			}
		}
		bool isNew = false;
		// Single authoritative source: the plan's composed finals. Whole-file
		// entries carry the winning external; the stat below reuses the early
		// enumeration cache (same card, same boot). A mismatch with the
		// composed final cannot happen through the cache - it would mean the
		// wholeFile invariant broke - so it refuses explicitly instead of
		// sizing the table from either value.
		// Failures are filed under both the late disc key and the pre-FST
		// early key when they differ, so the skip audit (keyed by early
		// registration records) names the true reason for basename-routed
		// files instead of a generic unassigned verdict.
		for (size_t i = 0; i < activePlan.files.size(); ++i)
		{
			const PlannedFile &pf = activePlan.files[i];
			const std::string &key = pf.disc;
			u32 extSize = 0;
			if (pf.finalSize == 0 && pf.external.empty())
			{
				// Zero-length file: valid entry, no fragments, no stat.
				if (builder.AddOrReplace(pf.disc, 0, &isNew))
				{
					++planned;
					expectedModSizes[key] = 0;
				}
				else
				{
					modAddFails[key] = SKIP_ADD_FAILED;
					if (pf.earlyKey != key)
						modAddFails[pf.earlyKey] = SKIP_ADD_FAILED;
					++rejected;
				}
				continue;
			}
			if (!ExternalFileSize(pf.external, &extSize)
				|| extSize != pf.finalSize)
			{
				if (modAddFails.find(key) == modAddFails.end())
					modAddFails[key] = SKIP_STAT_FAILED;
				if (pf.earlyKey != key
					&& modAddFails.find(pf.earlyKey) == modAddFails.end())
					modAddFails[pf.earlyKey] = SKIP_STAT_FAILED;
				if (!ExternalFileSize(pf.external, &extSize))
					missingCreated.push_back(pf.external);
				continue;
			}
			modBytes += pf.finalSize;
			if (builder.AddOrReplace(pf.disc, pf.finalSize, &isNew))
			{
				++planned;
				expectedModSizes[key] = pf.finalSize;
			}
			else
			{
				modAddFails[key] = SKIP_ADD_FAILED;
				if (pf.earlyKey != key)
					modAddFails[pf.earlyKey] = SKIP_ADD_FAILED;
				++rejected;
			}
		}
		// Diagnostics for created files missing from the card keep their
		// historical shape (the plan records them as missing externals).
		for (size_t i = 0; i < created.size(); ++i)
		{
			u32 extSize = 0;
			if (!ExternalFileSize(created[i].external, &extSize))
			{
				const std::string key = NormaliseDiscPath(created[i].disc);
				if (modAddFails.find(key) == modAddFails.end())
					modAddFails[key] = SKIP_STAT_FAILED;
				missingCreated.push_back(created[i].external);
			}
		}
		LogStep("table entries planned: %u (%u rejected)", planned, rejected);
		if (!missingCreated.empty())
		{
			Addf(out, "  missing created file(s): %u listed as additions above but NOT on the card:\n",
				 (unsigned) missingCreated.size());
			for (size_t i = 0; i < missingCreated.size() && i < 8; ++i)
				Addf(out, "    %s\n", missingCreated[i].c_str());
			if (missingCreated.size() > 8)
				Addf(out, "    ... and %u more\n",
					 (unsigned) (missingCreated.size() - 8));
		}
		{
			//! How much card traffic the caches saved. Sizes were stated
			//! once during early enumeration; listings replay the early
			//! pass. Misses walk the card again.
			u32 sizeHits = 0, sizeMisses = 0, dirHits = 0, dirMisses = 0;
			FileSizeCacheStats(&sizeHits, &sizeMisses);
			DirCacheStats(&dirHits, &dirMisses);
			Addf(out, "  file sizes         : %u reused, %u re-statted\n",
				 sizeHits, sizeMisses);
			Addf(out, "  directory listings : %u cached, %u walked\n",
				 dirHits, dirMisses);
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
		//! begin part-way through one. A 4K-native drive needs 4 KB and would
		//! otherwise have every file rejected by the alignment check later.
		//! Deliberately sector-granular, not the old 2 KB floor: fragments
		//! cover ceil(len/sector) sectors, so packing at sector granularity
		//! leaves no unmapped padding sliver between files. The host
		//! boundary test (test_fragtail section 11) proves the old shape
		//! errors past the declared size where the rawksd reference serves
		//! original bytes; ordinary sector-aligned game reads can land in
		//! those slivers independently of any partial <file> XML usage.
		const u32 layoutAlign = bootSectorSize ? bootSectorSize : 512;

		//! Apply the placement decided in SetupDisc rather than choosing a new
		//! one: the fragments are already registered against those offsets,
		//! and late re-registering historically returned -128, so the
		//! working pre-partition order is kept until a hardware capture
		//! names the actual refusal (see the sourced d2x gate note at
		//! Activate: stealth_mode AND running_title, never armed here).
		//! The late keys (post-FST routing) meet
		//! the early offsets through each file's pre-FST earlyKey, so
		//! basename-routed files land on registered fragments instead of
		//! withholding the whole table. LayoutFrom then counts builder
		//! entries the resolved map cannot serve - the single gate below.
		//! Layout() is only used when nothing was placed, so the report
		//! still shows what would have happened.
		u32 unplaced = 0;
		u32 remapped = 0;
		std::map<std::string, u64> lateOffsets;
		if (!modOffsets.empty())
		{
			u32 resolveUnplaced = 0;
			ResolveLateOffsets(activePlan, modOffsets, lateOffsets,
							   resolveUnplaced, remapped);
			unplaced = builder.LayoutFrom(lateOffsets);
			// Both counters must agree: the resolver sees plan files, the
			// builder sees table entries. A file the resolver cannot serve
			// but the builder does not contain is an internal inconsistency,
			// refused the same way.
			if (resolveUnplaced > unplaced)
				unplaced = resolveUnplaced;
		}
		else
			builder.Layout(region, layoutAlign);
		LogStep("early placement applied: %u without, %u remapped by name",
				unplaced, remapped);

		std::vector<u8> newFst;
		builder.Serialize(newFst, true);
		LogStep("table serialised: %u bytes", (unsigned) newFst.size());
		// The validation window now lives in RiivoValidate.cpp (pure TU,
		// also linked by host tests and the Dolphin harness): same order,
		// same checks, same outcomes. Device-derived inputs cross as
		// parameters; the report text below is unchanged and still reads
		// the same names, now bound to the result.
		Riivo::ValidateRequest vreq;
		vreq.builder = &builder;
		vreq.fst = &fst;
		vreq.plainFst = &newFst;
		// Late keys (post-FST routing), matching expectedModSizes and the
		// redirect/created lists below. Passing the early map here would
		// fail basename-routed files against their own placements.
		vreq.modOffsets = &lateOffsets;
		vreq.expectedModSizes = &expectedModSizes;
		vreq.fstReserve = fstReserve;
		vreq.region = region;
		vreq.modRegionStart = modRegionStart;
		vreq.redirects = &redirects;
		vreq.created = &created;
		vreq.modRecords = &modRecords;
		vreq.modAddFails = &modAddFails;
		vreq.imageBytes = gameDataEnd;
		vreq.sectorSize = bootSectorSize;
		vreq.usedFrags = fragStats.fragsBefore ? fragStats.fragsBefore
					   : gameFrags->num;
		Riivo::ValidateResult vres;
		//! Entry checkpoint owns the light explicitly: ON from here until
		//! validation returns. Set BEFORE persisting, so even a lost entry
		//! line leaves the state behind (ON with no entry line means death
		//! persisting the entry itself - or earlier - and validation may
		//! never have run). The checkpoint line itself never pulses, so
		//! nothing here can cancel the state. What ON-stable with an entry
		//! line but no return line covers, honestly: death anywhere in the
		//! interval from entry persist through ValidateTable - inside
		//! validation (hang or fault; caught exceptions already travel via
		//! vres.oom, so silence is neither refusal nor OOM) or in the
		//! microsecond gap before function entry. That interval is one
		//! straight-line span below; read it, it has nowhere to hide.
		//! NULL trace: production records outcomes in the log, not op codes.
		//! The entry half runs through ValidationBoundary (template over
		//! light/log sinks): host tests pin its exact order with mocks,
		//! production passes the register/card sinks here.
		struct BoundaryLight { void Set(bool on) { LightSet(on); } };
		struct BoundaryLog { void Line(const char *text) { LogCheckpoint("%s", text); } };
		BoundaryLight boundaryLight;
		BoundaryLog boundaryLog;
		ValidationBoundary(boundaryLight, boundaryLog,
						   [&]{ Riivo::ValidateTable(vreq, vres, 0); },
						   "validating the rebuilt table");
		//! Return checkpoint: validation is back, light OFF, stably. OFF
		//! with an entry line but no return line means the return line
		//! itself failed to build or persist - the call returned. A return
		//! line means validation returned with the verdicts it names;
		//! anything dying later is in report building, staging, or the
		//! card. None of this holds for a run nobody watched: an
		//! unobserved signal stays inconclusive, and a flickering or
		//! uncertain reading settles nothing - report what was seen.
		LightSet(false);
		LogCheckpoint("validation returned: walk=%d paths=%u compact=%d compactBytes=%u oom=%d plan=%d",
				vres.fstWalkOK ? 1 : 0, (unsigned) vres.walkPaths,
				vres.useCompact ? 1 : 0, (unsigned) vres.compactBytes,
				vres.oom ? 1 : 0, vres.plan.ok ? 1 : 0);
		const bool expectedComplete = vres.expectedComplete;
		const bool fstWalkOK = vres.fstWalkOK;
		const u32 walkPaths = vres.walkPaths;
		const std::string &walkError = vres.walkError;
		const bool useCompact = vres.useCompact;
		const bool compactOK = vres.compactOK;
		const u32 compactBytes = vres.compactBytes;
		const std::string &cwErr = vres.compactWhy;
		const FstBuildStats &st = vres.stats;
		const FragPlan &plan = vres.plan;
		std::vector<PlacedFile> placed;
		placed.swap(vres.placed);
		modSkips.swap(vres.modSkips);
		const bool compactAttempted = fstReserve > 0 && fstWalkOK &&
									newFst.size() > fstReserve;
		if (vres.useCompact)
			newFst.swap(vres.staged);
		//! Word-align the staged table: boot-info words carry sizes
		//! word-shifted, so a table whose size is not a multiple of 4
		//! cannot round-trip its own length through the apploader
		//! (traced: 153934 staged bytes published as 153932, which the
		//! exact size match must then refuse). At most 3 zero bytes;
		//! count-driven FST parsing ignores trailing pad, and the
		//! install path verifies these exact bytes.
		{
			const size_t before = newFst.size();
			FstBuilder::PadTableWords(newFst);
			if (newFst.size() != before)
				Addf(out, "  word pad           : %u -> %u bytes\n",
					 (unsigned)before, (unsigned)newFst.size());
		}
		const bool validationOom = vres.oom;
		//! Return line: every outcome the validator can produce is named
		//! here. Logged without pulsing so the OFF state set above survives
		//! intact; a log reaching this line proves validation returned with
		//! these verdicts.
		LogCheckpoint("validation returned: walk=%d paths=%u compact=%d compactBytes=%u oom=%d plan=%d",
				vres.fstWalkOK ? 1 : 0, (unsigned) vres.walkPaths,
				vres.useCompact ? 1 : 0, (unsigned) vres.compactBytes,
				vres.oom ? 1 : 0, vres.plan.ok ? 1 : 0);
		if (validationOom)
		{
			//! Allocation failure inside validation (refusal recorded in
			//! vres by ValidateTable): refuse with a reason instead of
			//! stopping silent. newFst is untouched on the compacted path
			//! unless staging was chosen, so the refusal below is clean.
			//! The append is capacity-checked and the record goes out
			//! through a C-only persist independent of `out` - see below.
			char oomLine[192];
			snprintf(oomLine, sizeof(oomLine),
					 "  independent FST walk: REFUSED: out of memory during validation (%u paths, MEM2 free %u KB)\n",
					 (unsigned) walkPaths,
					 (unsigned) (MEM2_freesize() / 1024));
			const size_t oomLen = strlen(oomLine);
			if (out.capacity() - out.size() > oomLen)
				out += oomLine;
			if (!bootLogPath.empty())
			{
				if (!AppendFileBytes(bootLogPath.c_str(), oomLine, oomLen))
					gprintf("Riivo: OOM refusal (log write failed)\n");
			}
		}
		if (fstWalkOK)
			Addf(out, "  independent FST walk: %u paths passed (includes unchanged and empty files)\n",
				 (unsigned)walkPaths);
		else
			Addf(out, "  independent FST walk: REFUSED: %s\n",
				 expectedComplete ? walkError.c_str() : "a mod path has no registered placement");
		Addf(out, "  validation workspace : %u paths, MEM2 free %u KB\n",
			 (unsigned) walkPaths,
			 (unsigned) (MEM2_freesize() / 1024));
		//! Suffix-compacted variant of the same tree (same entries, paths,
		//! offsets and sizes; shared string tails stored once). If the plain
		//! table outgrows the apploader's reservation but the compacted one
		//! fits, stage the compacted bytes instead: they install in place,
		//! which the general policy supports. Grown tables that still overflow
		//! refuse explicitly; relocation is unsupported until the patched boot
		//! view lands. Anything else keeps today's
		//! bytes exactly - unknown reservation, fitting plain table, failed
		//! build, failed walk, or still-overflowing compaction.
		if (!validationOom && compactAttempted)
			Addf(out, "  compacted table    : %u bytes (%s), reservation %u: %s\n",
				 compactBytes,
				 compactOK ? "walk passed" : ("REFUSED: " + cwErr).c_str(),
				 fstReserve,
				 useCompact ? "STAGED instead of the plain table" : "kept plain table");
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

		//! plan/placed/modSkips arrive filled in vres (see above).

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
				Addf(out, "  mode               : %s\n",
					 onDemandPlanned ? "on-demand (files resolved by path at read time)"
									 : "fraglist (every file mapped up front)");
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
		if (!modSkips.empty())
		{
			Addf(out, "  registered but unreferenced: %u file(s) got fragments, but the rebuilt table points nowhere at them (nothing reads those offsets)\n",
				 (unsigned) modSkips.size());
			for (size_t i = 0; i < modSkips.size() && i < 16; ++i)
				Addf(out, "    %s: %s\n",
					 modSkips[i].disc.c_str(), SkipReasonText(modSkips[i].reason));
			if (modSkips.size() > 16)
				Addf(out, "    ... and %u more\n",
					 (unsigned) (modSkips.size() - 16));
		}
		out += "\n";
		}

		//! The probe and the patch already happened, back in SetupDisc, because
		//! that is the last point where the access to do them is guaranteed.
		out += DescribeProbe(bootProbe);
		if (patchApplied)
			Addf(out, "\n  Hook site %08x, storage %08x; every written byte matched uncached read-back.\n",
				 bootProbe.patchSites[0], patchStorage);
		else if (!patchWhy.empty())
			Addf(out, "\n  The cIOS read hook was NOT applied: %s\n", patchWhy.c_str());

		// ------------------------------------------------------------------
		// Switch it on, but only if every single check above came back clean.
		// ------------------------------------------------------------------
		out += "\nSwitching it on\n";
		out += "---------------\n";

		if (!(fstWalkOK && extentFits && plan.ok && gameFrags && patchApplied
			  && fragsRegistered && unplaced == 0 && !RiivoDeadlinePassed()))
		{
			out += "  Not attempted - one of the checks above did not pass. The game\n"
				   "  boots exactly as it would without Riivolution.\n";
			withholdStage = "NOT_SWITCHED";
		}
		else
		{
			LogStep("checking the mod's files through the hook");
			Activate(out, plan, placed, newFst, fstOffset, fstSize);
			LogStep("file work finished");
			//! The staged copy (or the relocorig original) already lives in
			//! its own MEM2 buffer; holding the disc bytes too would just
			//! sit on 150 KB for the rest of the boot.
			relocOrigRaw.clear();
			relocOrigRaw.shrink_to_fit();
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
		return g_launch.FileWorkIncomplete();
	}

	bool FileWorkLive()
	{
		return fileWorkLive;
	}


	//! One line per <folder> rule. The path is trimmed to the tail because
	//! a full external path is mostly the device and mod root repeated.
	//! Always on: it costs one log line, and LogStep flips the drive light,
	//! which is the only progress signal this boot has left.
	static void LogListProgress(void *, const std::string &dir, u32 soFar)
	{
		const size_t cut = dir.size() > 44 ? dir.size() - 44 : 0;
		LogStep("  listing %s%s (%u so far)", cut ? "..." : "",
				dir.c_str() + cut, soFar);
	}

	//! The last thing the loader does before handing the console to the
	//! game. Everything that allocates has already run, so this copy is the
	//! one the game actually reads. Nothing is logged from here - the card
	//! is gone by now - which is why the report above says what it booked.
	//! Install the staged table and prove it landed. Past device shutdown
	//! the card is gone, so this verification is the last thing that can
	//! establish what the game will read: the installed bytes are compared
	//! against what was staged, the low-memory words the game uses to find
	//! the table are re-read, and the region is checksummed. True (the
	//! common case, including nothing staged) lets the boot continue; false
	//! refuses the jump below, so a corrupted install returns to the loader
	//! - a visible outcome naming the install - instead of a black screen
	//! that could be anything past this point.
	//! Current stack pointer: a snapshot of one instant. A plain read of r1
	//! at the call site; it covers frames live at that instant and nothing
	//! between samples - calls made after one sample can use deeper frames
	//! and return before the next.
	static u32 ReadStackPointer()
	{
		u32 sp = 0;
		__asm__ volatile ("mr %0, 1" : "=r" (sp));
		return sp;
	}

	//! Main-thread stack bounds, from the same symbols libogc itself uses to
	//! start that thread: __lwp_thread_init(_thr_main, __stack_end,
	//! __stack_addr - __stack_end, ...) in lwp.c, validated against the
	//! v2.11.0 sources (May 2025 - the vintage the CI image carries). The
	//! thread-control struct itself is private to libogc, so the symbols are
	//! read directly instead: they must resolve at link for ANY libogc build
	//! to link, which proves their presence, and weak linkage degrades a
	//! future toolchain without them to "unknown" rather than a build break.
	//! Values are validated at runtime where they are used (both inside
	//! MEM1, low < high, SP inside) before any reading is drawn from them.
	extern "C"
	{
		extern u8 __stack_addr[] __attribute__((weak));
		extern u8 __stack_end[] __attribute__((weak));
	}

	//! Validated main-thread stack acquisition, shared by the relocation
	//! evidence (observations) and the reported-base gate (verdicts) so the
	//! two cannot disagree about what "known" means. Bounds must resolve
	//! inside MEM1 with low < high and SP inside; anything else (absent
	//! symbols, garbage values, a foreign thread's SP) reads unknown.
	//! Target-only (stack register).
	static void ReadLoaderStack(u32 &sp, u32 &stackLo, u32 &stackHi,
								bool &stackKnown)
	{
		sp = ReadStackPointer();
		stackLo = 0;
		stackHi = 0;
		stackKnown = false;
		const u32 top = __stack_addr ? (u32) __stack_addr : 0;
		const u32 bot = __stack_end ? (u32) __stack_end : 0;
		if (!(top > bot && bot >= MEM1_BASE && top <= MEM1_END
			  && sp >= bot && sp < top))
			return;
		stackLo = bot;
		stackHi = top;
		stackKnown = true;
	}

	bool HaveStagedFst()
	{
		return g_launch.HaveStaged();
	}

	bool StagedFstStillIntact()
	{
		if (!HaveStagedFst())
			return true; // nothing staged; not this check's business
		return Crc32(pendingFst, pendingFstSize) == pendingFstCrc;
	}

	bool InstallPendingFst()
	{
		//! Deliberately not installed; see the nofstinstall.txt marker. The
		//! staged table is simply dropped and the game keeps its own, so the
		//! jump goes ahead exactly as it would for a mod that staged nothing.
		//! Gated on a live staging: with nothing staged this falls through
		//! to the guard below like any unstaged boot (same outcome), and a
		//! spent or refused booking can never be re-labelled bypassed.
		if (skipFstInstall && g_launch.HaveStaged())
		{
			installFailCode = 0;
			g_launch.Skip();
			gprintf("Riivo: FST install SKIPPED by riivolution/nofstinstall.txt\n");
			return true;
		}
		if (!g_launch.CanInstall())
		{
			//! No staged state means no install is expected - except when
			//! the game was already told its files are live. A live game
			//! pointed at an uninstalled table reads unmapped space, so
			//! that combination refuses instead of jumping (code 1).
			//! CanInstall is the same check the old guard spelled out,
			//! plus the generation stamp: a table staged by an earlier
			//! boot can never satisfy it.
			if (!fileWorkLive)
			{
				installFailCode = 0;
				return true;
			}
			g_launch.Refuse(1);
			return false;
		}
		const u32 addr = pendingPlace.fstAddr;
		const u32 size = pendingFstSize;
		//! A relocated table overwrites bytes below the apploader's
		//! reservation (and possibly past its old top). Whether anything
		//! live sits there cannot be established from byte contents: free
		//! heap routinely holds stale nonzero bytes (freed vectors from the
		//! table build itself, HBC leftovers), and zeroed bytes can still
		//! belong to something. So this scan is evidence only, never a
		//! verdict: it records what the span held and where the loader heap
		//! break sits, for post-hoc analysis, and the install always
		//! proceeds to the verification below. In-place installs touch
		//! nothing outside the reservation and skip this entirely.
		if (!pendingPlace.inPlace && addr < MEM1_END)
		{
			const u32 curPtr = *(vu32 *) 0x80000038;
			const u32 curMax = *(vu32 *) 0x8000003C;
			//! A zero reservation says nothing about what is reserved, so
			//! there is nothing to scan against - PlaceFst already refused
			//! the cases that matter.
			if (curMax > 0 && curPtr >= MEM1_BASE && curPtr < MEM1_END
				&& curMax <= MEM1_END - curPtr)
			{
				const u32 origAddr = curPtr;
				const u32 origTop = curPtr + curMax;
				char dirt[3 * 32 + 1];
				size_t dirtLen = 0;
				u32 dirtyAt = 0;
				int dirtyCount = 0;
				for (u32 p = addr; p < origAddr && dirtyCount < 32; ++p)
				{
					const u8 b = *(const volatile u8 *) p;
					if (b && dirtyCount == 0)
						dirtyAt = p;
					if (b || dirtyCount > 0)
					{
						dirtLen += (size_t) snprintf(dirt + dirtLen,
													 sizeof(dirt) - dirtLen,
													 "%02x", b);
						++dirtyCount;
					}
				}
				if (dirtyCount == 0 && addr + size > origTop)
				{
					for (u32 p = origTop; p < addr + size && dirtyCount < 32; ++p)
					{
						const u8 b = *(const volatile u8 *) p;
						if (b && dirtyCount == 0)
							dirtyAt = p;
						if (b || dirtyCount > 0)
						{
							dirtLen += (size_t) snprintf(dirt + dirtLen,
														 sizeof(dirt) - dirtLen,
														 "%02x", b);
							++dirtyCount;
						}
					}
				}
				if (dirtyCount > 0)
					gprintf("Riivo: relocation span below 0x%08x held nonzero bytes (first at 0x%08x: %s); sbrk break 0x%08x\n",
							origAddr, dirtyAt, dirt, (u32) (uintptr_t) sbrk(0));
				else
					gprintf("Riivo: relocation span below 0x%08x held all zeros; sbrk break 0x%08x\n",
							origAddr, (u32) (uintptr_t) sbrk(0));
			}
		}
		//! Install-time SP/break snapshots, first of two. Post-shutdown, so this
		//! only reaches USB Gecko - the placement-time block in the card log
		//! is the persistent record and this is its closer-to-the-copy twin.
		//! Each sample covers only its own instant: calls between the two can
		//! use deeper frames and return unseen. Past the last sample only the
		//! return path, light-out and jump sequence execute - shallow, but
		//! likewise unsampled.
		gprintf("Riivo: pre-copy SP %08x (thread %08x), break %08x\n",
				(unsigned) ReadStackPointer(), (unsigned) LWP_GetSelf(),
				(unsigned) (uintptr_t) sbrk(0));
		//! Prove the staging buffer before copying it: the check below
		//! compares installed bytes against this same buffer, so rot between
		//! staging and here would otherwise bless itself.
		if (Crc32(pendingFst, size) != pendingFstCrc)
		{
			g_launch.Refuse(2);
			gprintf("Riivo: late FST install REFUSED - staged table failed its checksum before copying\n");
			return false;
		}
		const bool ok = InstallFst(pendingPlace, pendingFst, pendingFstSize);
		if (!ok)
			g_launch.Refuse(3);
		bool verified = false;
		u32 ptr = 0, max = 0, arena = 0;
		if (!ok)
			installFailCode = 3;
		else
		{
			ptr = *(vu32 *) 0x80000038;
			max = *(vu32 *) 0x8000003C;
			arena = *(vu32 *) 0x80000034;
			const bool bytesOk = (memcmp((const void *) addr, pendingFst, size) == 0)
								 && Crc32((const u8 *) addr, size) == pendingFstCrc;
			const bool ptrsOk = ptr == addr && max == size
								&& arena == pendingPlace.newArenaHi;
			verified = bytesOk && ptrsOk;
			//! Bytes first: a failed write and a moved pointer are different
			//! faults, and the blink code is the only channel that survives.
			installFailCode = verified ? 0 : (bytesOk ? 5 : 4);
			//! Installed means verified: a failed verification refuses with
			//! its code instead of claiming a table the game must not trust.
			if (verified)
				g_launch.Consume();
			else
				g_launch.Refuse(installFailCode);
		}
		if (verified)
		{
			//! Staging served its purpose: the installed bytes verified
			//! against it, so hand the MEM2 back before the jump instead of
			//! leaking it every boot. Later stages see nulls, not stale
			//! bytes; a repeated install refuses at the guard above.
			MEM2_free(pendingFst);
			g_launch.ReleaseStaging();
		}
		gprintf("Riivo: late FST install %s at %08x, %u bytes, crc %08x (ptr %08x max %u arena %08x)\n",
				verified ? "verified" : (ok ? "UNVERIFIED" : "REFUSED"),
				addr, (unsigned) size, pendingFstCrc, ptr, max, arena);
		gprintf("Riivo: post-verify SP %08x, break %08x\n",
				(unsigned) ReadStackPointer(), (unsigned) (uintptr_t) sbrk(0));
		return verified;
	}

	u32 InstallFailCode()
	{
		return installFailCode;
	}

	//! Put the game's own fragment list back and stand everything else down.
	//! Called when the cIOS refuses the enlarged list: registering it is the
	//! one step whose failure used to end the boot outright, because
	//! SetupDisc returns the error and BootGame answers it with
	//! Sys_BackToLoader - the loader disappears to the Homebrew Channel.
	//! Every other failure in this feature boots the game unmodified, and
	//! this one should too.
	bool RevertFragList()
	{
		if (!savedOrigNum || fragListUntouched)
			return false;
		RestoreFragList(savedOrigNum, savedOrigLast);
		//! The table must not be installed against fragments that were never
		//! registered - that points the game at unmapped space.
		fragsRegistered = false;
		fragGeneration = 0;
		modOffsets.clear();
		modRecords.clear();
		fragListUntouched = true;
		fragRefusal = "the cIOS refused the enlarged fragment list";
		return true;
	}

	void LogBootStep(const char *what)
	{
		if (what)
			LogStep("%s", what);
	}

	void PrepareFragList()
	{
		if (!bootSet)
			return;
		if (bootSet->files.empty() && bootSet->folders.empty())
			return;

		//! Stale IOS state from a previous boot in this loader session (hook
		//! installed, no reload since). Layering a new hook atop the old
		//! fails its original-signature check at best and serves mixed
		//! generations at worst. Refuse new file work explicitly; relaunch GX
		//! (fresh IOS) to retry. Clearing PPC flags never restores stock IOS.
		if ((hookGeneration != 0 && hookGeneration != g_launch.generation)
			|| (fragGeneration != 0 && fragGeneration != g_launch.generation))
		{
			fragListUntouched = true;
			fragRefusal = "stale IOS hook/fragments from a previous boot (relaunch GX to retry)";
			gprintf("Riivo: %s (hookGen %u fragGen %u curGen %u)\n",
					fragRefusal.c_str(), hookGeneration, fragGeneration,
					g_launch.generation);
			return;
		}

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
		LogStep("fragment list retained");

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
		//! Kept so SetupDisc can put the game's own list back if the cIOS
		//! will not take the enlarged one - see RevertFragList.
		savedOrigNum = originalNum;
		savedOrigLast = originalLast;

		//! And which partition it is on, while the partition object still
		//! exists - SetupDisc unmounts SD a few lines below.
		bootFsKnown = (WBFS_GetFsInfo(bootGameId, &bootFsType, &bootFsLba) >= 0);
		gprintf("Riivo: partition lookup %s (fs %u, lba %u)\n",
				bootFsKnown ? "ok" : "FAILED", bootFsType, bootFsLba);
		LogStep("game partition identified (fs %u, lba %u)",
				(unsigned) bootFsType, (unsigned) bootFsLba);

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
		//! d2x gates IOCTL_DI_FRAG_SET (0xF9) on stealth_mode (on by
		//! default) AND running_title (wiidev/d2x-cios@33ad1ee: dip-plugin
		//! FRAG_SET via Stealth_CheckRunningTitle; set only by ES launch
		//! or PPC mload-32; cleared only by IOS reload) - and nothing in
		//! this loader's flow arms that gate, with partition-open setting
		//! nothing per that source. A late re-register historically
		//! returned -128 all the same, from an unsourced cause, so the
		//! extended list is handed over before partition-open, in the same
		//! call the loader already makes. The file table cannot be read
		//! until afterwards, so the table is made to agree with this
		//! placement rather than the other way round.
		std::vector<ModCandidate> cand;
		//! Reads every directory the mod's rules name. On a total conversion
		//! that is thousands of entries off FAT, and it is the slowest thing
		//! in the whole boot - so say so before starting, not after.
		LogStep("listing the mod's files (reads the card)");
		u32 dolRoutedEarly = 0;
		{
			FsDirLister lister;
			ListModFiles(*bootSet, bootDevice, &lister, cand,
						 LogListProgress, 0, &modMissing, &dolRoutedEarly);
		}
		LogStep("mod files listed: %u found", (unsigned) cand.size());
		if (dolRoutedEarly > 0)
			LogStep("executable file(s) serve via boot view, no fragments: %u",
					dolRoutedEarly);
		//! Named in the progress section as well as the report below: a boot
		//! that dies later still shows the count here, and "0 found" with a
		//! nonzero missing count is a mod pointing at files that are not
		//! there - not a loader that cannot see them.
		if (!modMissing.empty())
			LogStep("mod files NOT on the card: %u (named in the report below)",
					(unsigned) modMissing.size());
		//! Checked here because nothing has been changed yet: the fragment
		//! list is still the game's own, so bailing out costs no cleanup.
		if (RiivoDeadlinePassed())
		{
			LogStep("over the time budget - booting the game unmodified");
			fragListUntouched = true;
			fragRefusal = "reading the mod took longer than the time budget";
			return;
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

		const u32 align = sector ? sector : 512;
		const u64 regionStart = PlanRegionStart(gameEnd, align);

		std::vector<PlacedFile> placed;
		placed.reserve(cand.size());
		modRecords.clear();
		modRecords.reserve(cand.size());
		//! One cursor walk for both phases (see AssignModOffsets): the late
		//! table build meets these same offsets through ResolveLateOffsets
		//! (keyed by each file's pre-FST earlyKey), so basename-routed files
		//! still land on registered fragments. test_planparity pins the
		//! early/late identity on shared fixtures.
		modOffsets.clear();
		u64 cursor = AssignModOffsets(cand, regionStart, align, modOffsets,
									  placed, modRecords);
		modRegionStart = regionStart;
		modRegionEnd = cursor;
		LogStep("placement computed: %u file(s), %llu bytes",
				(unsigned) placed.size(),
				(unsigned long long) (cursor - regionStart));

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
			modRecords.clear();
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
		//! Opens every placed file and walks its cluster chain. The other
		//! long phase, and the other one worth naming before it starts.
		LogStep("mapping fragments for %u file(s)", (unsigned) placed.size());
		if (OnDemandRequested())
		{
			//! The whole point of the on-demand path. Mapping opens every
			//! placed file and walks its cluster chain - thousands of them, on
			//! a screen that is already black, and the cost grows with the mod
			//! rather than with what the game actually reads. On-demand resolves
			//! a file by path when the game asks for it, so none of that
			//! happens here.
			//!
			//! The game's own fragments are already in the list and are left
			//! exactly as they were, so a refusal further on still boots the
			//! game unmodified.
			fragsRegistered = true;
			fragGeneration = g_launch.generation;
			onDemandPlanned = true;
			LogStep("on-demand: skipped mapping %u file(s)",
					(unsigned) placed.size());
		}
		else if (!AppendModFragments(placed, sector, (u8) modDev.fsType,
								modDev.lbaStart, fragStats,
								0, 0))
		{
			gprintf("Riivo: fragment build failed: %s\n", fragStats.firstFailure.c_str());
			modOffsets.clear();
			modRecords.clear();
			fragsRegistered = false;
		}
		else if (fragStats.failed)
		{
			//! A file that could not be mapped keeps its assigned offset, so the
			//! rebuilt table would point the game at unmapped space and it would
			//! read sparse zeros. Partial coverage is not a partial success.
			gprintf("Riivo: %u file(s) could not be mapped, refusing\n", fragStats.failed);
			modOffsets.clear();
			modRecords.clear();
			fragsRegistered = false;
		}
		else
		{
			fragsRegistered = true;
			fragGeneration = g_launch.generation;
			gprintf("Riivo: %u mod fragment(s) appended, %u total\n",
					fragStats.files, fragStats.fragsAfter);
		}
		LogStep(fragsRegistered
				? "fragments mapped: %u file(s), %u fragment(s)"
				: "fragment mapping FAILED after %u file(s), %u fragment(s)",
				fragStats.files, fragStats.fragsAfter);

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
		FILE *dumpMarker = fopen((bootDevice + "/riivolution/dumpios.txt").c_str(), "rb");
		const bool writeDumps = dumpMarker != 0;
		if (dumpMarker) fclose(dumpMarker);
		ProbeIosPlugin(dumpPath, bootProbe, writeDumps);

		if (bootProbe.patchSites.size() == 1 && onDemandPlanned)
		{
			//! Paths go over as the module will look them up: from the root of
			//! the FAT partition, without the loader's device prefix.
			std::vector<RedirectEntry> entries;
			entries.reserve(placed.size());
			for (size_t i = 0; i < placed.size(); ++i)
				entries.push_back(RedirectEntry(placed[i].offset, placed[i].length,
												PartitionPath(placed[i].external)));

			std::vector<u8> table;
			if (!BuildRedirectTable(entries, RIIVO_PART_DISCOVER, table, patchWhy))
			{
				gprintf("Riivo: redirect table refused: %s\n", patchWhy.c_str());
			}
			else
			{
				LogStep("on-demand: table built, %u file(s), %u bytes",
						(unsigned) entries.size(), (unsigned) table.size());
			patchApplied = InstallOnDemand(bootProbe.patchSites[0], table,
										   RIIVO_PART_DISCOVER, onDemandLayout,
										   patchWhy);
			if (patchApplied)
			{
				patchStorage = onDemandLayout.moduleAddr;
				hookGeneration = g_launch.generation;
			}
			}
			gprintf("Riivo: on-demand hook at %08x: %s\n",
					bootProbe.patchSites[0],
					patchApplied ? "applied" : patchWhy.c_str());
		}
		else if (bootProbe.patchSites.size() == 1)
		{
			patchApplied = ApplyDiPatch(bootProbe.patchSites[0], (u32)(modRegionEnd >> 2), patchWhy, &patchStorage);
			if (patchApplied)
				hookGeneration = g_launch.generation;
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
			//! A refusal is the one outcome worth bytes. The probe already
			//! chose its windows and the card is still mounted, so write them
			//! now rather than spend another round on a marker file.
			WriteProbeDumps(bootProbe, dumpPath);
			RestoreFragList(originalNum, originalLast);
			fragsRegistered = false;
			fragGeneration = 0;
			modOffsets.clear();
			modRecords.clear();
			fragRefusal = patchWhy;
			fragListUntouched = true;
		}
	}

	//! Relocation evidence, observations only. Runs inside
	//! ReportFstPlacement, while the card log is still writable, the
	//! apploader is done, and the DOL list is still alive. All install and
	//! launch decisions below run exactly as without this block: no refusal,
	//! no new blink code, no pointer writes. The block itself is NOT free -
	//! it allocates, writes the card log, and shifts timing, stack use and
	//! the binary layout, which is stated plainly because it matters to
	//! exactly this investigation. It names, for the planned destination
	//! interval, the three resident classes that could collide with it: the
	//! executing thread's live stack, the apploader-loaded game ranges, and
	//! the newlib heap extent. Closer-to-the-copy SP/break snapshots are
	//! logged again at InstallPendingFst, but that runs after device
	//! shutdown and only reaches USB Gecko - this card-log block is the
	//! persistent record.
	static void AppendRelocationEvidence(std::string &out, const FstPlacement &place,
										 u32 want, BssState bss, u32 bssLo, u32 bssHi)
	{
		out += "\nRelocation evidence (observations only - installation decisions unchanged)\n";
		out += "--------------------------------------------------------------------\n";

		const u32 destLo = place.ok ? place.fstAddr : 0;
		const u32 destHi = place.ok ? place.fstAddr + want : 0;
		if (place.ok)
			Addf(out, "  planned destination : [%08x, %08x) %u bytes, %s\n",
				 destLo, destHi, want, place.inPlace ? "in place" : "relocated");
		else
			Addf(out, "  planned destination : none booked (%s)\n", place.why.c_str());

		//! Placement-time SP is context, not proof: a snapshot of one instant on
		//! a deeper chain than the install will use - expected below the
		//! install-time SP, not guaranteed. Frames that run deeper in between
		//! but return before the copy (gamepatches, the memory engine) are
		//! dead by then and safe to have overwritten; only frames live across
		//! the copy matter, and no single snapshot covers them all.
		//! Acquisition shared with the reported-base gate below.
		u32 sp = 0, stackBot = 0, stackTop = 0;
		bool stackKnown = false;
		ReadLoaderStack(sp, stackBot, stackTop, stackKnown);
		const u32 self = (u32) LWP_GetSelf();
		Addf(out, "  executing thread    : id %08x, SP %08x at placement\n", self, sp);

		if (__stack_addr && __stack_end)
			Addf(out, "  main-thread stack   : [%08x, %08x)%s\n", stackBot, stackTop,
				 stackKnown ? " (SP inside: bounds describe this stack)"
							: " (SP outside: NOT this thread's stack, bounds unused)");
		else
			out += "  main-thread stack   : symbols absent at link, top unknown\n";

		//! What sbrk(0) is: the current newlib/MEM1 heap break. With this
		//! loader's MALLOC_MEM2 = 0 (mem2.cpp), libogc's _sbrk_r stays in its
		//! MEM1-only branch (sbrk.c, identical at v2.11.0 and master): the
		//! heap is [startup SYS_GetArenaLo, current SYS_GetArenaLo). It does
		//! NOT cover: the MEM2 pool [0x90002000, 0x933e0000) (separate
		//! allocator), anything the apploader reserved (untracked), or the
		//! main-thread stack (HBC-provided). Non-main LWP stacks come from
		//! the LWP workspace (lwp_stack.c), which is itself carved out of the
		//! sbrk region at init (lwp_wkspace.c), so the break still bounds
		//! them from above. The break alone does not establish the full heap
		//! interval: the floor is the startup Lo (BSS end 0x8106c260 on the
		//! tested binary IF symbol-inited - that startup path was not
		//! re-verified, so the floor is cited, not relied on).
		const u32 brk = (u32) (uintptr_t) sbrk(0);
		const bool brkOk = brk != 0xFFFFFFFFu && brk >= MEM1_BASE && brk <= MEM1_END;
		if (brkOk)
			Addf(out, "  newlib break (sbrk): %08x\n", brk);
		else
			Addf(out, "  newlib break (sbrk): %08x INVALID - heap extent UNKNOWN\n", brk);

		//! The ACTUAL loaded ranges, one line each, straight off the list the
		//! apploader filled in. Entries are apploader chunks (a section may
		//! arrive as more than one), which is finer than sections and exactly
		//! what an overwrite would hit.
		const int dolN = RiivoGetDOLCount();
		Addf(out, "  loaded game ranges  : %d apploader chunk(s)\n", dolN);
		u32 dolMin = 0, dolMax = 0;
		int dolValid = 0, dolInvalid = 0, dolLines = 0;
		bool dolHit = false;
		for (int i = 0; i < dolN; ++i)
		{
			u8 *d = RiivoGetDOLDst(i);
			int l = RiivoGetDOLLen(i);
			//! Every entry must be usable before the verdict below may claim
			//! an exclusion: a null/empty entry, a wrapped-around end, or an
			//! address outside MEM1 makes the whole reading UNKNOWN rather
			//! than silently narrower.
			const u32 lo = d ? (u32) d : 0;
			const u32 hi = d && l > 0 ? lo + (u32) l : 0;
			const bool entryOk = d && l > 0 && hi > lo
				&& lo >= MEM1_BASE && hi <= MEM1_END;
			if (!entryOk)
			{
				++dolInvalid;
				if (dolLines < 40)
				{
					Addf(out, "    entry %d INVALID (dst %08x, len %d) - excluded from the reading\n",
						 i, lo, l);
					++dolLines;
				}
				continue;
			}
			const bool hit = place.ok && RangesOverlap(lo, hi, destLo, destHi);
			//! Where the chunk came from: its DOL section and disc offset,
			//! BSS (an address with no disc bytes), or neither. Decided by
			//! containment - a chunk straddling sections matches none and
			//! says so rather than naming the wrong one.
			char src[96];
			if (dolSectionCount == 0 && dolBssAddr == 0)
				snprintf(src, sizeof(src), " (section table unavailable)");
			else
			{
				bool isBss = false;
				const int si = FindDolSection(lo, hi, isBss);
				if (si >= 0)
					snprintf(src, sizeof(src), " disc 0x%010llx (%s %d)",
							 (unsigned long long) dolImageBase
							 + dolSections[si].fileOff + (lo - dolSections[si].addr),
							 dolSections[si].text ? "text" : "data",
							 dolSections[si].text ? si : si - 7);
				else if (isBss)
					snprintf(src, sizeof(src), " (BSS, zero-filled - no disc bytes)");
				else
					snprintf(src, sizeof(src), " (no section match)");
			}
			//! The yield that put these bytes here, when recorded: the only
			//! source offset for chunks no DOL section describes. Last
			//! write wins at record time, so a repeated destination names
			//! its current bytes. Channel and alternate-DOL chunks never
			//! report and read as not recorded.
			char req[32];
			bool reqKnown = false;
			for (u32 n = 0; n < dolNoteCount; ++n)
			{
				if (dolNotes[n].dst == lo && dolNotes[n].len == (u32) l)
				{
					snprintf(req, sizeof(req), " req 0x%08x", dolNotes[n].disc);
					reqKnown = true;
					break;
				}
			}
			if (!reqKnown)
				snprintf(req, sizeof(req), " req ?");
			if (dolLines < 40)
			{
				Addf(out, "    [%08x, %08x) %d bytes%s%s%s\n", lo, hi, l,
					 hit ? "  <-- OVERLAPS planned destination" : "", src, req);
				++dolLines;
			}
			if (dolValid == 0) { dolMin = lo; dolMax = hi; }
			else
			{
				if (lo < dolMin) dolMin = lo;
				if (hi > dolMax) dolMax = hi;
			}
			++dolValid;
			if (hit)
				dolHit = true;
		}
		if (dolValid + dolInvalid > dolLines)
			Addf(out, "    ... and %d more chunk(s)\n",
				 dolValid + dolInvalid - dolLines);
		if (dolValid == 0 && dolInvalid == 0)
			out += "    (list empty at placement: an alternate-DOL path clears it; "
				   "a main-DOL boot always registers)\n";
		else
		{
			if (dolValid > 0)
				Addf(out, "    overall game image: [%08x, %08x)\n", dolMin, dolMax);
			if (dolInvalid > 0)
				Addf(out, "    %d chunk(s) failed validation (INVALID above)\n", dolInvalid);
			if (dolNotesDropped > 0)
				Addf(out, "    %u range note(s) dropped (cap)\n", dolNotesDropped);
		}

		out += "  overlap reading (at placement) :\n";
		if (!place.ok)
			out += "    no destination booked; ranges above are context only\n";
		else
		{
			//! An SP word alone decides two of three cases at its own instant:
			//! inside the span is an observed risk, above it excludes the
			//! stack with no top required (frames live upward of SP). Below
			//! it with no top is UNDECIDED, not clear.
			if (sp >= destLo && sp < destHi)
				out += "    SP is INSIDE the destination at placement: live-stack overlap risk OBSERVED\n";
			else if (stackKnown)
				Addf(out, "    live stack [%08x, %08x) vs destination at placement: %s\n",
					 sp, stackTop,
					 RangesOverlap(sp, stackTop, destLo, destHi)
					 ? "OVERLAP OBSERVED" : "no overlap observed");
			else if (sp >= destHi)
				out += "    SP is above the destination; the top is unknown, so frames live above SP are unbounded - context only\n";
			else
				out += "    SP is below the destination and the top is unknown: UNDECIDED from SP alone\n";
			//! Missing or invalid ranges can never support an exclusion.
			if (dolHit)
				out += "    game ranges vs destination: OVERLAP OBSERVED\n";
			else if (dolValid > 0 && dolInvalid == 0)
				out += "    game ranges vs destination: no overlap observed\n";
			else if (dolValid == 0 && dolInvalid == 0)
				out += "    game ranges vs destination: UNKNOWN (list empty at placement)\n";
			else
				Addf(out, "    game ranges vs destination: UNKNOWN (%d invalid chunk(s))\n",
					 dolInvalid);
			if (bss == BSS_VALID)
				Addf(out, "    BSS [%08x, %08x): held as a placement obstacle above\n",
					 bssLo, bssHi);
			else if (bss == BSS_INVALID)
				out += "    BSS: INVALID header values (counts as malformed input)\n";
			else
				out += "    BSS: absent (size zero)\n";
			if (!brkOk)
				out += "    heap extent: UNKNOWN (break failed validation)\n";
			else if (brk > destLo)
				out += "    newlib break reaches past the destination bottom: heap EXTENT overlaps (evidence only - the loader heap is live through launch and dead after the jump; the floor is cited, not verified)\n";
			else
				out += "    newlib break is below the destination: heap extent clear (floor cited, not verified)\n";
		}
	}

	//! Retired (general pipeline, 2026-09-12): per-game reservation
	//! conflict checks and MEM2 evaluation removed. General placement
	//! policy in ReportFstPlacement refuses grown tables explicitly with
	//! required vs reserved capacity. See docs/archive/smg2-reserve/.

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
			if (bootSet && !bootSet->memories.empty())
			{
				AppendLog("\n\nMemory patches\n"
						  "--------------\n"
						  "  HELD BACK. This mod replaces files, the plan above did not\n"
						  "  complete, so the memory patches are skipped too. Applying\n"
						  "  them without the mod's files is what makes a game exit to\n"
						  "  the System Menu. The game boots completely unmodified.\n");
			}
			else
			{
				AppendLog("\n\nMemory patches\n"
						  "--------------\n"
						  "  No <memory> patches requested by this mod's options;\n"
						  "  nothing scheduled.\n");
			}
			withholdStage = "NO_PLACEMENT";
			AppendLog("OUTCOME: WITHHELD NO_PLACEMENT\n");
			return;
		}
		if (want == 0)
		{
			//! No file work was ever wanted (a memory-only mod, say): record
			//! the outcome so a WITHHELD line from an earlier file-mod boot
			//! does not linger and mislead the next pre-launch check.
			AppendLog("OUTCOME: NO_FILE_WORK\n");
			return;
		}

		const ArenaInfo arena = ReadArenaInfo();
		//! The apploader's loaded ranges, as obstacles for a grown table.
		//! The COMPLETE list, unfiltered: every dolList entry goes in raw
		//! (even malformed ones - PlaceFst counts those and refuses grown
		//! placement on them rather than steering around guesses), plus the
		//! BSS range when the header names one. Capped at nothing: the list
		//! is short and silently dropping entries is exactly the defect
		//! being removed. Validated the same way as the evidence block
		//! below validates them.
		std::vector<OccupiedRange> occ;
		const int dolN = RiivoGetDOLCount();
		occ.reserve(dolN > 0 ? (size_t) dolN + 1 : 1);
		for (int i = 0; i < dolN; ++i)
		{
			u8 *d = RiivoGetDOLDst(i);
			int l = RiivoGetDOLLen(i);
			OccupiedRange r;
			if (d && l > 0)
			{
				r.lo = (u32) d;
				r.hi = r.lo + (u32) l;
			}
			occ.push_back(r);
		}
		u32 bssLo = 0, bssHi = 0;
		const BssState bss = CheckBss(bssLo, bssHi);
		if (bss == BSS_VALID)
			occ.push_back(OccupiedRange(bssLo, bssHi));
		else if (bss == BSS_INVALID)
			occ.push_back(OccupiedRange()); // malformed marker: forces refusal
		const FstPlacement place = PlaceFst(arena, want, 32,
											occ.empty() ? 0 : &occ[0],
											(u32) occ.size());

		std::string out;
		out += "\n\nWhere the rebuilt table would go\n";
		out += "--------------------------------\n";
		out += "Read from the boot-info block the apploader has just filled in.\n\n";

		Addf(out, "  arena low    : %08x\n", arena.arenaLo);
		Addf(out, "  arena high   : %08x\n", arena.arenaHi);
		Addf(out, "  table now at : %08x, %u bytes reserved\n",
			 arena.fstAddr, arena.fstMaxSize);
		Addf(out, "  rebuilt size : %u bytes\n\n", want);

		//! General placement policy (no game-ID gates): in-place tables install
		//! where the apploader put them. A grown table installs at the
		//! APPLOADER-REPORTED base (the 0x80000038/0x8000003C words the
		//! apploader itself published after reading the served boot header
		//! through its own first triple - traced end to end) - or nothing.
		//! The reported size must equal the staged size exactly; the span
		//! must clear MEM1 bounds, loaded game ranges (apploader-loaded FST
		//! bytes excepted - they die with the install by construction), the
		//! validated loader stack, and the heap accounting (same floor
		//! rules as PlaceFst). Startup preservation is NOT established
		//! here: a verified install that never boots isolates the fault to
		//! startup, which is the observation that decides it.
		//! Everything below books and reports the EFFECTIVE placement.
		FstPlacement effPlace = place;
		// Under a served grown header the apploader's own words decide, so
		// every placement - including one PlaceFst calls in-place (its
		// reservation arithmetic cannot see that the words moved) - goes
		// through reported-base evaluation below. Stock-header boots keep
		// the cascade/in-place split exactly as before.
		if (effPlace.ok && (!effPlace.inPlace || grownHeaderServed))
		{
			// Live loaded ranges: drop apploader-loaded FST bytes (matched
			// to their yields by destination+length, identified by disc
			// source range). Everything else the install would clobber.
			std::vector<OccupiedRange> liveOcc;
			liveOcc.reserve(occ.size());
			u32 fstLoadSkipped = 0;
			for (size_t i = 0; i < occ.size(); ++i)
			{
				bool isFstLoad = false;
				for (u32 n = 0; n < dolNoteCount; ++n)
				{
					// Full-span match only: a crossing yield must not exempt
					// unrelated loaded code/data from the veto below.
					if (dolNotes[n].dst == occ[i].lo
						&& dolNotes[n].len == occ[i].hi - occ[i].lo
						&& occ[i].hi > occ[i].lo
						&& NoteInFstRange(dolNotes[n].disc, dolNotes[n].len,
										  trackFstOff, trackFstSize))
					{
						isFstLoad = true;
						break;
					}
				}
				if (isFstLoad)
					++fstLoadSkipped;
				else
					liveOcc.push_back(occ[i]);
			}
			u32 rsp = 0, rlo = 0, rhi = 0;
			bool rknown = false;
			ReadLoaderStack(rsp, rlo, rhi, rknown);
			FstPlacement rep;
			const char *repWhy = 0;
			Addf(out, "\n  apploader reported : base %08x, size %u (words it published)\n",
				 arena.fstAddr, arena.fstMaxSize);
			Addf(out, "  staged for install : %u bytes%s\n", want,
				 fstLoadSkipped ? "" : " (no FST-load yields filtered)");
			if (fstLoadSkipped > 0)
				Addf(out, "  superseded FST loads filtered from obstacles: %u\n",
					 fstLoadSkipped);
			if (dolNotesDropped > 0)
				Addf(out, "  yield-note cap dropped %u range(s): filtering is incomplete, vetting stays strict\n",
					 dolNotesDropped);
			if (EvaluateReportedBase(arena, arena.fstAddr, arena.fstMaxSize,
									 want,
									 liveOcc.empty() ? 0 : &liveOcc[0],
									 (u32)liveOcc.size(),
									 rsp, rlo, rhi, rknown, rep, repWhy))
			{
				effPlace = rep;
				Addf(out, "  installs at the reported base %08x\n", rep.fstAddr);
				out += "  startup preservation observed, not proven: a verified\n"
					   "  install that never boots isolates the fault to startup.\n";
			}
			else
			{
				effPlace = FstPlacement();
				effPlace.why = repWhy ? repWhy : "reported placement refused";
				Addf(out, "\n  reported placement refused: %s\n", effPlace.why.c_str());
			}
		}

		if (!effPlace.ok)
		{
			Addf(out, "  REFUSED: %s\n", effPlace.why.c_str());
			out += "\n  Nothing would be written. Refusing is the right outcome here -\n"
				   "  a wrong address writes over the running game and shows up as a\n"
				   "  hang with nothing on screen.\n";
			// After a served grown header there is no stock boot to fall
			// back to - whichever gate refused (reported evaluation above
			// or placement arithmetic), the apploader's reservation,
			// loaded table and published pointers may already differ from
			// stock, so a withheld table plus a boot would run whatever
			// the apploader loaded, not a stock game. Block the boot
			// instead (BootPartition returns no entry, like every failed
			// load). Under the nofstinstall diagnostic the header stayed
			// stock, so the plain withhold path (stock boot) remains safe
			// there.
			if (grownHeaderServed)
			{
				grownBlocked = true;
				out += "  No stock fallback exists past a served grown header:\n"
					   "  this boot returns to the loader instead.\n";
			}
		}
		else if (effPlace.inPlace)
		{
			Addf(out, "  fits in the room the apploader already set aside (%u bytes spare)\n",
				 arena.fstMaxSize - plannedFstSize);
			out += "  Nothing would move and the game's heap would be untouched.\n";
		}
		else
		{
			Addf(out, "  installs at the apploader-reported base %08x\n", effPlace.fstAddr);
			Addf(out, "  arena high   : %08x -> %08x\n", arena.arenaHi, effPlace.newArenaHi);
			Addf(out, "  taken from the game's heap : %u KB\n", effPlace.reserved / 1024);
			if (effPlace.heapLeft)
				Addf(out, "  heap the game still has    : %u MB\n",
					 effPlace.heapLeft / (1024 * 1024));
			else
			{
				Addf(out, "  heap the game still has    : unknown (arena low not set)\n");
				Addf(out, "  blind-drop cap used        : %u of %u KB\n",
					 effPlace.reserved / 1024, MAX_BLIND_DROP / 1024);
			}
		}
		if (effPlace.ok)
			Addf(out, "  loaded ranges kept clear : %u considered, %u ignored "
					  "(stale-table space), %u malformed; BSS %s\n",
				 (unsigned) occ.size(), effPlace.ignoredRanges, effPlace.malformedRanges,
				 bss == BSS_VALID ? "held as a placement obstacle"
				 : bss == BSS_INVALID ? "INVALID header values" : "absent");
		//! Persisted here, not only at the end: everything above is pure
		//! computation over words already read, while everything below
		//! walks loaded ranges and reads the disc again. A log ending here
		//! died in the evidence/struct computation, not the apploader.
		AppendLog(out);
		out.clear();
		if (relocOrig)
		{
			//! Self-check for the diagnostic: without a real relocation this
			//! run tests nothing. An in-place original is exactly a vanilla
			//! boot, still worth running, but say so plainly.
			out += effPlace.inPlace ?
				   "\n  relocorig.txt active but the table fit in place: relocation\n"
				   "  NOT exercised, this run is a vanilla boot, not a test.\n" :
				   "\n  relocorig.txt active: verbatim original staged for a real\n"
				   "  relocation. Compare against the created-file run.\n";
		}

		//! Evidence first, verdicts never: the block runs for in-place
		//! installs too, where the same geometry is a control. It costs log
		//! lines plus the timing/stack/layout perturbation stated above.
		AppendRelocationEvidence(out, effPlace, want, bss, bssLo, bssHi);

		//! Apploader-struct evidence rides with the relocation block: same
		//! window (card alive, apploader done), same read-only terms.
		AppendApploaderStructEvidence(out);

		//! Second persist point: the evidence above is computed. What
		//! follows is booking and outcome text only - no reads, no writes
		//! to game memory - so a log ending here died assembling text,
		//! not touching the game.
		AppendLog(out);
		out.clear();

		//! This is the step that actually points the game at the mod. It only
		//! runs when the fragment list, the read-back check and the cIOS hook
		//! all succeeded earlier - otherwise pendingFst was never filled in, and
		//! the game boots with its own table exactly as it always did.
		//! Booked, not written - see pendingPlace. The only thing that could
		//! still refuse it is the bounds re-check inside InstallFst, and that
		//! is decided by this placement, which is already known good.
		//! Booked through the launch owner: placement, verdict, and the
		//! live flag move together and can never desynchronise. Book itself
		//! refuses invalid placements and any re-booking, so reaching the
		//! Ready text below means exactly one live booking exists.
		if (pendingFst && pendingFstSize && effPlace.ok && g_launch.Book(effPlace))
		{
			out += "\n  Ready. The table goes in last, immediately before the\n"
				   "  game starts, so nothing the loader still has to do can land\n"
				   "  on top of it. The game will read the mod\'s files.\n";
		}
		else if (pendingFst)
		{
			out += "\n  Held a rebuilt table but could not place it, so it was not\n"
				   "  installed. The game boots unmodified.\n";
			withholdStage = "FST_PLACE";
		}

		//! Machine-parseable outcome for the previous-boot check in the game
		//! settings UI. The prose above carries the details; this line is the
		//! part the UI can read without parsing prose.
		if (!fileWorkWanted)
			out += "OUTCOME: NO_FILE_WORK\n";
		else if (fileWorkLive)
			out += "OUTCOME: FST_STAGED\n";
		else
			Addf(out, "OUTCOME: WITHHELD %s\n", withholdStage.c_str());

		//! Say plainly what this means for the rest of the boot. A mod whose
		//! files did not make it must not get its memory patches either.
		out += "\n\nMemory patches\n";
		out += "--------------\n";
		if (!bootSet || bootSet->memories.empty())
		{
			out += "  No <memory> patches requested by this mod's options;\n"
				   "  nothing scheduled.\n";
		}
		else if (FileWorkIncomplete())
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
			out += "  Scheduled after device shutdown, alongside the mod's installed files.\n";
		else
			out += "  Scheduled after device shutdown. This mod does not replace any files.\n";

		AppendLog(out);
		gprintf("Riivo: placement %s (%08x, %u bytes), memory patches %s\n",
				place.ok ? "ok" : "refused", place.fstAddr, want,
				FileWorkIncomplete() ? "held back" : "applied");
	}

	//! The last thing written while the card is still mounted. Everything
	//! after this runs without a card log: shutdown, patching, install,
	//! jump. A black screen past here is unresolved among a hang in that
	//! window, a refusal whose return died silently, and a game dead
	//! before its own video init. The entry point and arena below bound
	//! the window but do not say which of the three happened.
	void ReportLaunch(u32 entry)
	{
		std::string out;
		out += "\n\nApploader completed; final patching and jump pending\n";
		out += "------------------------\n";
		Addf(out, "  entry point  : %08x\n", entry);
		Addf(out, "  arena low    : %08x\n", (u32) SYS_GetArenaLo());
		Addf(out, "  arena high   : %08x\n", (u32) SYS_GetArenaHi());
		if (!entry)
			out += "  No entry point: the apploader never produced one, so nothing\n"
				   "  below this line ever ran.\n";
		else
			out += "  This entry point was returned by the apploader. Device shutdown,\n"
				   "  loader patches, mod memory patches and the actual jump follow.\n"
				   "  This log cannot prove those later operations completed.\n";
		//! Grand total, wall-clock from the first Riivolution step (fragment
		//! list retained in SetupDisc) to here. The per-step lines above say
		//! where the time went; this line says how much loading cost in all.
		Addf(out, "  total elapsed      : %u ms (%u s) since the first Riivolution step\n",
			 (unsigned) BootElapsedMs(), (unsigned) (BootElapsedMs() / 1000));
		AppendStepTimings(out);
		AppendLog(out);
	}
}

//! C bridge for apploader.c, which cannot see the namespace. Defined here
//! next to the state it toggles rather than in a shim of its own.
extern "C" void RiivoPulseLight(void)
{
	Riivo::PulseLight();
}

//! C bridge for the per-yield disc offsets (see RiivoLight.h).
extern "C" void RiivoNoteDOLRange(unsigned int dst, unsigned int len,
								  unsigned int discOffset)
{
	Riivo::NoteDOLRange((u32) dst, (u32) len, (u32) discOffset);
}

//! C bridge for a failed apploader chunk read (see RiivoLight.h). Formats
//! and persists the record immediately: the caller returns failure, after
//! which BootPartition is over and no placement report can follow, so a
//! later write would never happen. Card log only; the blink-6 return path
//! below carries the outcome to the tester.
extern "C" void RiivoLogChunkFailure(unsigned int dst, unsigned int len,
									 unsigned int discOffset, int code)
{
	char line[192];
	snprintf(line, sizeof(line),
			 "\nApploader chunk read FAILED: dst %08x, %d bytes, disc 0x%08x, code %d\n"
			 "  The game image is incomplete; refusing the boot below.\n",
			 dst, (int) len, discOffset, code);
	Riivo::AppendLog(line);
}
