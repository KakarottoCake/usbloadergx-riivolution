/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <gccore.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "sys.h"
#include "RiivoBoot.hpp"
#include "RiivoNet.hpp"
#include "RiivoConfig.hpp"
#include "RiivoReconcile.hpp"
#include "RiivoFst.hpp"
#include "RiivoFile.hpp"
#include "RiivoFstBuild.hpp"
#include "RiivoFstWalk.hpp"
#include "RiivoReadVerify.hpp"
#include "RiivoFstInstall.hpp"
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
	static u32 patchStorage = 0;
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
	static u32 installFailCode = 0;

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
		//! The progress window and the per-folder log lines are the two
		//! things that run INSIDE the file-listing loop, and they are the
		//! only things in that phase that did not exist in v2.9 - the last
		//! build known to list this mod and boot it. Two consoles have since
		//! stopped mid-listing, so both are off unless asked for. On, they
		//! give a moving bar and a line per <folder> rule; off, the phase is
		//! as quiet as it used to be and only its start and end are recorded.
		deepVerify = false;
		//! Per boot, like the directory cache: the card can be swapped between
		//! one launch and the next, and a stale flag here would take the
		//! on-demand path with no module installed.
		onDemandPlanned = false;
		onDemandLayout = OnDemandLayout();
		ClearDirListCache();
		ClearFileSizeCache();
		bootClockStart = 0;
		deadlinePassed = false;
		stepHeaderWritten = false;
		//! Per-boot diagnostics: a single boot runs per process, but reset
		//! anyway so an aborted launch or a changed mod can never inherit
		//! another boot's verdicts, records, or timings.
		stepMarkCount = 0;
		sumPlaced = 0;
		sumFailed = 0;
		withholdStage.clear();
		modRecords.clear();
		modSkips.clear();
		modMissing.clear();
		modAddFails.clear();
		dolSectionCount = 0;
		dolBssAddr = dolBssSize = 0;
		dolImageBase = 0;
		dolNoteCount = 0;
		dolNotesDropped = 0;
		installFailCode = 0;
		//! No loading bar, and no marker to turn one on. Two separate GUI
		//! draws on this path were confirmed on hardware to stop the boot -
		//! the pre-jump summary and this one - and each hid the next problem
		//! behind it. An opt-in would only be a trap for whoever sets it.
		//!
		//! The drive light is the progress signal now: LogStep flips it, so
		//! every logged step is a blink, and it needs no GUI, no thread and
		//! no allocation - one register write. See PulseLight.
		//! Arm the drive-light pulse for this boot. Disarmed at the jump.
		pulseArmed = true;
		pulseOn = false;
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
		skipFstInstall = false;
		if (!device.empty())
		{
			FILE *n = fopen((device + "/riivolution/nofstinstall.txt").c_str(), "rb");
			if (n)
			{
				skipFstInstall = true;
				fclose(n);
			}
		}
		relocOrig = false;
		if (!device.empty())
		{
			FILE *r = fopen((device + "/riivolution/relocorig.txt").c_str(), "rb");
			if (r)
			{
				relocOrig = true;
				fclose(r);
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
		//! Same text to the listener, if one was configured. Sent first so
		//! it survives even when the card is unmounted: the SetupDisc
		//! register/remount window unmounts SD, and a file-only copy would
		//! go silent exactly when it is needed most. The file stays the
		//! record; this is the copy that survives a boot that never
		//! finishes or a card that never comes back.
		SendCollector(text);
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
	static void LogStep(const char *fmt, ...)
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
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (!bootClockStart)
			bootClockStart = gettime();
		//! Every logged step flips the light, so the blink rate IS the work
		//! rate and a light that stops tells the tester which step hung.
		PulseLight();
		const u32 now = BootElapsedMs();
		Addf(out, "  %-52s %6u ms  MEM2 free %u KB\n", buf,
			 (unsigned) now,
			 (unsigned) (MEM2_freesize() / 1024));
		AppendLog(out);
		if (stepMarkCount < sizeof(stepMarks) / sizeof(stepMarks[0]))
		{
			StepMark &m = stepMarks[stepMarkCount++];
			strncpy(m.name, buf, sizeof(m.name) - 1);
			m.name[sizeof(m.name) - 1] = 0;
			m.ms = now;
		}
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
	static bool ReadDiscFst(u8 **outData, u32 *outSize, u32 *outOffset,
							 u32 *outDolOffset, std::string &err)
	{
		*outData = 0;
		*outSize = 0;
		*outOffset = 0;
		*outDolOffset = 0;

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
		*outDolOffset = be32(hdr) << 2;
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
		//! Measured offsets, not protocol: the struct address comes from
		//! the T0 reference scan. Disc base = APPLDR_OFFSET (0x2440, same
		//! constant apploader.c reads the header from) + 0x20 header +
		//! 0x1b40 struct offset within the image. Length mirrors
		//! apploader.c exactly: buffer[5] + buffer[6] at header +0x14/+0x18.
		static const u32 ramBase = 0x81201b40;   // struct - 0x40
		static const u32 discBase = 0x2440 + 0x20 + 0x1b40;
		static u8 disc[0x100] ATTRIBUTE_ALIGN(32);
		static u8 appHdr[0x20] ATTRIBUTE_ALIGN(32);

		out += "\nApploader struct around 81201b80 (read-only - nothing updated)\n";
		out += "--------------------------------------------------------------\n";

		u32 appLen = 0;
		if (WDVD_Read(appHdr, sizeof(appHdr), 0x2440) >= 0)
			appLen = be32(appHdr + 0x14) + be32(appHdr + 0x18);
		const bool inImage = appLen > 0x1bc0;
		Addf(out, "  apploader image length: %u bytes (%s +0x1b80)\n", appLen,
			 appLen == 0 ? "header unreadable - coverage unknown"
			 : inImage ? "covers" : "DOES NOT COVER - not apploader image");

		//! The 8 words at the struct, as the CPU reads them, each compared
		//! against its disc twin on its own. Neighbors name the shape: boot
		//! words beside +0x10 would read as the apploader's working copy of
		//! what it wrote to low memory; code bytes around it would read as
		//! an embedded constant instead. The +0x10 marker is re-checked
		//! live against low memory rather than assumed from T0.
		const u32 fstNow = *(vu32 *) 0x80000038;
		const bool discOk = inImage
			&& WDVD_Read(disc, sizeof(disc), discBase) >= 0;
		if (!discOk && inImage)
			out += "  (disc bytes unreadable - per-word compare skipped)\n";
		u32 ndiff = 0;
		bool w10diff = false;
		for (u32 i = 0; i < 8; ++i)
		{
			const u32 ram = *(const volatile u32 *) (uintptr_t) (ramBase + 0x40 + 4 * i);
			const bool diff = discOk && ram != be32(disc + 0x40 + 4 * i);
			if (diff)
			{
				++ndiff;
				if (i == 4)
					w10diff = true;
			}
			Addf(out, "  +0x%02x : %08x%s%s\n", 4 * i, ram,
				 (i == 4 && ram == fstNow)
				 ? "  <-- equals the FST address right now" : "",
				 diff ? "  (differs from disc)" : "");
		}
		if (!discOk)
			out += "  disc verdict: unavailable\n";
		else if (ndiff == 0)
			out += "  image verdict: all 8 identical to image bytes (consistent with an untouched image region)\n";
		else
		{
			Addf(out, "  image verdict: %u of 8 differ from image bytes (see source verdict below: a later read supersedes the image as the explanation)\n",
				 ndiff);
			out += w10diff ? "  +0x10 itself differs from the image\n"
						   : "  +0x10 matches the image; change is in neighbors only\n";
		}

		//! Primary verdict: the chunk's own recorded source. Latest yield
		//! covering the struct wins - an earlier read's bytes are gone where
		//! a later one landed. Comparing those source bytes against the
		//! final RAM contents is what distinguishes bytes the read supplied
		//! from later changes. Neither outcome names a store or a consumer.
		u32 srcDst = 0, srcLen = 0, srcOff = 0;
		for (u32 n = dolNoteCount; n > 0; --n)
		{
			const DolRangeNote &r = dolNotes[n - 1];
			if (r.len == 0 || r.len > 0x01000000 || r.dst < MEM1_BASE
				|| r.dst + r.len <= r.dst || r.dst > 0x81201b80
				|| r.dst + r.len <= 0x81201b80
				|| r.disc + 0x20 < r.disc)
				continue;
			srcDst = r.dst;
			srcLen = r.len;
			srcOff = r.disc;
			break;
		}
		if (srcLen == 0)
			out += "  source verdict: no recorded yield covers 0x81201b80\n";
		else
		{
			u32 lo = srcDst < 0x81201b80 ? 0x81201b80 : srcDst;
			u32 hi = srcDst + srcLen > 0x81201ba0 ? 0x81201ba0 : srcDst + srcLen;
			Addf(out, "  source: latest yield [%08x, %08x) from disc 0x%08x covers struct [%08x, %08x)\n",
				 srcDst, srcDst + srcLen, srcOff, lo, hi);
			static u8 src[0x40] ATTRIBUTE_ALIGN(32);
			const u32 s0 = srcOff + (lo - 0x81201b80);
			const u32 rBase = s0 & ~31u;
			const u32 rEnd = (s0 + (hi - lo) + 31) & ~31u;
			if (rEnd - rBase > sizeof(src))
				out += "  source bytes unreadable (window error)\n";
			else if (WDVD_Read(src, rEnd - rBase, rBase) < 0)
				out += "  source bytes unreadable (disc read failed)\n";
			else
			{
				u32 sdiff = 0;
				bool s10diff = false, s10covered = false;
				for (u32 a = lo; a + 4 <= hi; a += 4)
				{
					u32 ram = 0;
					memcpy(&ram, (const void *) (uintptr_t) a, 4);
					const u32 img = be32(src + (a - rBase));
					if (ram != img)
					{
						++sdiff;
						Addf(out, "  struct +0x%02x: RAM %08x != source %08x\n",
							 a - 0x81201b80, ram, img);
						if (a == 0x81201b90)
							s10diff = true;
					}
					if (a == 0x81201b90)
						s10covered = true;
				}
				if (sdiff == 0)
					out += "  source verdict: covered bytes match the read\n";
				else
					Addf(out, "  source verdict: %u covered word(s) differ - changed after the read\n",
						 sdiff);
				if (s10covered)
					Addf(out, "  +0x10 vs source: %s\n",
						 s10diff ? "DIFFERS (stored after the read)"
						 : "matches (as read)");
				else
					out += "  +0x10 vs source: uncovered by the recorded yield\n";
			}
		}

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
	//! CRC of the staged table, captured when it is staged - not recomputed
	//! from the staging buffer at install time, which would bless a buffer
	//! that rotted in MEM2 in between.
	static u32 pendingFstCrc = 0;

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
	static FstPlacement pendingPlace;
	static bool pendingPlaceOk = false;

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
						 const std::vector<u8> &newFst)
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
		//! handed over with set_frag_list. Nothing is registered here: d2x
		//! blocks IOCTL_DI_FRAG_SET once a title is running, and the game
		//! partition being open means one is - that refusal is what returned
		//! -128 when this used to re-register at this point.
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
		pendingFst = (u8 *) MEM2_alloc(stageSize);
		if (!pendingFst)
		{
			out += "  Out of memory for the rebuilt table, so it will not be\n"
				   "  installed. The patch above is harmless on its own.\n";
			withholdStage = "NO_MEMORY";
			return;
		}
		if (stageRelocOrig)
		{
			memcpy(pendingFst, stageSrc, relocOrigRaw.size());
			memset(pendingFst + relocOrigRaw.size(), 0, RELOC_ORIG_PAD);
		}
		else
			memcpy(pendingFst, stageSrc, stageSize);
		pendingFstSize = stageSize;
		//! Captured now, from the bytes just staged - the install step
		//! compares the installed region against this, so a staging buffer
		//! that rotted in MEM2 in between still fails instead of blessing
		//! itself.
		pendingFstCrc = Crc32(pendingFst, pendingFstSize);

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
		u32 fstSize = 0, fstOffset = 0, dolOffset = 0;
		std::string err;
		LogStep("reading the game's file table");
		if (!ReadDiscFst(&fstData, &fstSize, &fstOffset, &dolOffset, err))
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
		bool isNew = false;
		for (size_t i = 0; i < redirects.size(); ++i)
		{
			u32 extSize = 0;
			const std::string key = NormaliseDiscPath(redirects[i].disc);
			if (!ExternalFileSize(redirects[i].external, &extSize))
			{
				modAddFails[key] = SKIP_STAT_FAILED;
				continue;
			}
			if (builder.AddOrReplace(redirects[i].disc, extSize, &isNew))
			{
				++planned;
				expectedModSizes[key] = extSize;
			}
			else
			{
				modAddFails[key] = SKIP_ADD_FAILED;
				++rejected;
			}
		}
		for (size_t i = 0; i < created.size(); ++i)
		{
			u32 extSize = 0;
			const std::string key = NormaliseDiscPath(created[i].disc);
			if (!ExternalFileSize(created[i].external, &extSize))
			{
				modAddFails[key] = SKIP_STAT_FAILED;
				missingCreated.push_back(created[i].external);
				continue;
			}
			modBytes += extSize;
			if (builder.AddOrReplace(created[i].disc, extSize, &isNew))
			{
				++planned;
				expectedModSizes[key] = extSize;
			}
			else
			{
				modAddFails[key] = SKIP_ADD_FAILED;
				++rejected;
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
		//! one: the fragments are already registered against those offsets and
		//! cannot be changed now, because d2x refuses IOCTL_DI_FRAG_SET once
		//! the game partition is open. Layout() is only used when nothing was
		//! placed, so the report still shows what would have happened.
		u32 unplaced = 0;
		if (!modOffsets.empty())
			unplaced = builder.LayoutFrom(modOffsets);
		else
			builder.Layout(region, layoutAlign);
		LogStep("early placement applied: %u without", unplaced);

		std::vector<u8> newFst;
		builder.Serialize(newFst, true);
		LogStep("table serialised: %u bytes", (unsigned) newFst.size());
		// Expectations come from the original disc and external-file sizes,
		// not from reparsing the builder's result with the builder itself.
		std::vector<FstWalkExpectation> expectedFst;
		for (size_t i = 0; i < fst.FileCount(); ++i) {
			const FstFile &original = fst.FileAt(i);
			if (expectedModSizes.find(original.path) == expectedModSizes.end())
				expectedFst.push_back(FstWalkExpectation(original.path, original.offset, original.length));
		}
		bool expectedComplete = true;
		for (std::map<std::string, u32>::const_iterator it = expectedModSizes.begin();
			 it != expectedModSizes.end(); ++it) {
			std::map<std::string, u64>::const_iterator offset = modOffsets.find(it->first);
			if (offset == modOffsets.end()) { expectedComplete = false; continue; }
			expectedFst.push_back(FstWalkExpectation(it->first, offset->second, it->second));
		}
		FstWalk gameWalk;
		std::string walkError;
		const bool fstWalkOK = expectedComplete && !newFst.empty() &&
			gameWalk.Open(&newFst[0], newFst.size(), true, &walkError) &&
			gameWalk.Check(expectedFst, &walkError);
		if (fstWalkOK)
			Addf(out, "  independent FST walk: %u paths passed (includes unchanged and empty files)\n",
				 (unsigned)expectedFst.size());
		else
			Addf(out, "  independent FST walk: REFUSED: %s\n",
				 expectedComplete ? walkError.c_str() : "a mod path has no registered placement");
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

		//! Feed the placed files through the same checks that would gate a
		//! fresh layout: ordering, alignment, the read ceiling, the table.
		CollectPlaced(builder, modOffsets.empty() ? region : modRegionStart,
					  redirects, created, placed);
		ToExtents(placed, extents);

		//! Registration records that never became placed entries, with
		//! reasons. A tail-recovered offset landing on one of these is
		//! verified through its retained record instead of refused
		//! anonymously - nothing reads those offsets, so they are harmless,
		//! but the bytes still get proven (see Activate).
		{
			std::vector<u64> lateOffsets;
			lateOffsets.reserve(placed.size());
			for (size_t i = 0; i < placed.size(); ++i)
				lateOffsets.push_back(placed[i].offset);
			std::map<std::string, char> hasRedirect;
			for (size_t i = 0; i < redirects.size(); ++i)
				hasRedirect[NormaliseDiscPath(redirects[i].disc)] = 1;
			for (size_t i = 0; i < created.size(); ++i)
				hasRedirect[NormaliseDiscPath(created[i].disc)] = 1;
			FindSkips(modRecords, lateOffsets,
					  modOffsets.empty() ? region : modRegionStart,
					  hasRedirect, modAddFails, modSkips);
		}

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
			Activate(out, plan, placed, newFst);
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
		return fileWorkWanted && !fileWorkLive;
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

	bool HaveStagedFst()
	{
		return pendingPlaceOk && pendingFst && pendingFstSize;
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
		if (skipFstInstall)
		{
			pendingPlaceOk = false;
			installFailCode = 0;
			gprintf("Riivo: FST install SKIPPED by riivolution/nofstinstall.txt\n");
			return true;
		}
		if (!pendingPlaceOk || !pendingFst || !pendingFstSize)
		{
			//! No staged state means no install is expected - except when
			//! the game was already told its files are live. A live game
			//! pointed at an uninstalled table reads unmapped space, so
			//! that combination refuses instead of jumping (code 1).
			installFailCode = fileWorkLive ? 1 : 0;
			return !fileWorkLive;
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
		if (!pendingPlace.inPlace)
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
			pendingPlaceOk = false;
			installFailCode = 2;
			gprintf("Riivo: late FST install REFUSED - staged table failed its checksum before copying\n");
			return false;
		}
		const bool ok = InstallFst(pendingPlace, pendingFst, pendingFstSize);
		pendingPlaceOk = false;
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
		//! d2x blocks IOCTL_DI_FRAG_SET once a title is running
		//! (Stealth_CheckRunningTitle in its plugin), and opening the game
		//! partition is what starts one - so the extended list has to be handed
		//! over before that, in the same call the loader already makes. The file
		//! table cannot be read until afterwards, so the table is made to agree
		//! with this placement rather than the other way round.
		std::vector<ModCandidate> cand;
		//! Reads every directory the mod's rules name. On a total conversion
		//! that is thousands of entries off FAT, and it is the slowest thing
		//! in the whole boot - so say so before starting, not after.
		LogStep("listing the mod's files (reads the card)");
		{
			FsDirLister lister;
			ListModFiles(*bootSet, bootDevice, &lister, cand,
						 LogListProgress, 0, &modMissing);
		}
		LogStep("mod files listed: %u found", (unsigned) cand.size());
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
				RegRecord rec;
				rec.disc = cand[i].disc;
				rec.external = cand[i].external;
				rec.offset = cursor;
				rec.length = 0;
				modRecords.push_back(rec);
				continue;
			}
			cursor = (cursor + align - 1) & ~((u64) align - 1);
			PlacedFile pf;
			pf.offset = cursor;
			pf.length = cand[i].size;
			pf.external = cand[i].external;
			placed.push_back(pf);
			modOffsets[cand[i].disc] = cursor;
			RegRecord rec;
			rec.disc = cand[i].disc;
			rec.external = cand[i].external;
			rec.offset = cursor;
			rec.length = cand[i].size;
			modRecords.push_back(rec);
			cursor += cand[i].size;
		}
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
					patchStorage = onDemandLayout.moduleAddr;
			}
			gprintf("Riivo: on-demand hook at %08x: %s\n",
					bootProbe.patchSites[0],
					patchApplied ? "applied" : patchWhy.c_str());
		}
		else if (bootProbe.patchSites.size() == 1)
		{
			patchApplied = ApplyDiPatch(bootProbe.patchSites[0], (u32)(modRegionEnd >> 2), patchWhy, &patchStorage);
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
	//! Report every boot-word hit inside one resident range, capped. listed
	//! and total accumulate across ranges so the caller can close with an
	//! "N more unlisted" line instead of implying absence.
	static void ReportWordHits(std::string &out, const char *label, u32 lo, u32 hi,
							   const u32 *vals, const char *const *names, u32 nvals,
							   u32 &listed, u32 &total)
	{
		if (hi <= lo)
			return;
		u32 ho[16];
		const u32 found = FindWordRefs((const u8 *) (uintptr_t) lo, hi - lo,
									   vals, nvals, ho, sizeof(ho) / sizeof(ho[0]));
		total += found;
		for (u32 k = 0; k < found && k < 16 && listed < 16; ++k, ++listed)
		{
			u32 w = 0;
			memcpy(&w, (const void *) (uintptr_t) (lo + ho[k]), 4);
			const char *role = "?";
			for (u32 v = 0; v < nvals; ++v)
			{
				if (vals[v] == w)
				{
					role = names[v];
					break;
				}
			}
			Addf(out, "    word %08x (= %s) at %s +0x%x\n", w, role, label, ho[k]);
		}
	}

	static void AppendRelocationEvidence(std::string &out, const ArenaInfo &arena,
										 const FstPlacement &place,
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
		const u32 sp = ReadStackPointer();
		const u32 self = (u32) LWP_GetSelf();
		Addf(out, "  executing thread    : id %08x, SP %08x at placement\n", self, sp);

		const u32 stackTop = __stack_addr ? (u32) __stack_addr : 0;
		const u32 stackBot = __stack_end ? (u32) __stack_end : 0;
		const bool stackKnown = stackTop > stackBot
			&& stackBot >= MEM1_BASE && stackTop <= MEM1_END
			&& sp >= stackBot && sp < stackTop;
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

		//! Table references: words anywhere resident that already equal the
		//! boot words. A moved table breaks such references even where no
		//! byte is overwritten, so avoidance alone cannot exclude them.
		//! Snapshots at placement time, like everything else here.
		out += "  table references (words already equal to the boot words) :\n";
		u32 refVals[5];
		const char *refNames[5];
		u32 refN = 0;
		if (arena.fstAddr) { refVals[refN] = arena.fstAddr; refNames[refN++] = "FST address"; }
		if (arena.fstMaxSize) { refVals[refN] = arena.fstMaxSize; refNames[refN++] = "FST max size"; }
		if (arena.arenaHi) { refVals[refN] = arena.arenaHi; refNames[refN++] = "arena high"; }
		if (arena.arenaLo) { refVals[refN] = arena.arenaLo; refNames[refN++] = "arena low"; }
		if (arena.fstAddr && arena.fstMaxSize
			&& arena.fstAddr + arena.fstMaxSize > arena.fstAddr)
		{
			refVals[refN] = arena.fstAddr + arena.fstMaxSize;
			refNames[refN++] = "reservation top";
		}
		u32 listed = 0, total = 0, scanned = 0;
		if (refN == 0)
			out += "    (no nonzero boot words to match)\n";
		else
		{
			char lbl[32];
			for (int i = 0; i < dolN; ++i)
			{
				u8 *d = RiivoGetDOLDst(i);
				int l = RiivoGetDOLLen(i);
				if (!d || l <= 0)
					continue;
				const u32 lo = (u32) d, hi = lo + (u32) l;
				if (hi <= lo || lo < MEM1_BASE || hi > MEM1_END)
					continue;
				snprintf(lbl, sizeof(lbl), "chunk %d", i);
				ReportWordHits(out, lbl, lo, hi, refVals, refNames, refN,
							   listed, total);
				++scanned;
			}
			if (bss == BSS_VALID)
			{
				ReportWordHits(out, "BSS", bssLo, bssHi, refVals, refNames, refN,
							   listed, total);
				++scanned;
			}
			if (place.ok && !place.inPlace && destLo < arena.fstAddr)
			{
				ReportWordHits(out, "sliver below reservation", destLo, arena.fstAddr,
							   refVals, refNames, refN, listed, total);
				++scanned;
			}
			if (total == 0)
				Addf(out, "    none observed in %u scanned range(s)\n", scanned);
			else if (total > listed)
				Addf(out, "    ... and %u more hit(s) unlisted\n", total - listed);
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
		if (place.ok)
			Addf(out, "  loaded ranges kept clear : %u considered, %u ignored "
					  "(stale-table space), %u malformed; BSS %s\n",
				 (unsigned) occ.size(), place.ignoredRanges, place.malformedRanges,
				 bss == BSS_VALID ? "held as an obstacle"
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
			out += place.inPlace ?
				   "\n  relocorig.txt active but the table fit in place: relocation\n"
				   "  NOT exercised, this run is a vanilla boot, not a test.\n" :
				   "\n  relocorig.txt active: verbatim original staged for a real\n"
				   "  relocation. Compare against the created-file run.\n";
		}

		//! Evidence first, verdicts never: the block runs for in-place
		//! installs too, where the same geometry is a control. It costs log
		//! lines plus the timing/stack/layout perturbation stated above.
		AppendRelocationEvidence(out, arena, place, want, bss, bssLo, bssHi);

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
		if (pendingFst && pendingFstSize && place.ok)
		{
			//! Booked, not written - see pendingPlace. The only thing that could
			//! still refuse it is the bounds re-check inside InstallFst, and that
			//! is decided by this placement, which is already known good.
			pendingPlace = place;
			pendingPlaceOk = true;
			fileWorkLive = true;
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
