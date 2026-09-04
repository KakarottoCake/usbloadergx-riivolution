/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 3, loader side. Two jobs, both run from inside the boot sequence:
 *
 *   1. Identify the cIOS we are actually running under. The in-RAM DI read
 *      hook has to recognise the DIP code of whatever cIOS the game was told
 *      to use, so we need to know what is in every slot, not just the default.
 *
 *   2. Read the game's FST straight off the open partition and work out which
 *      disc byte ranges the selected <file>/<folder> patches would redirect.
 *
 * Timing is the whole difficulty here, so it is spelled out:
 *   - The FST must come from the DISC, not from the in-RAM copy at *0x80000038.
 *     The RAM copy only exists after the apploader has run, and by then the
 *     devices are gone.
 *   - Redirect fragment lists need SD/USB MOUNTED, which stops being true the
 *     moment ShutDownDevices() runs, immediately after BootPartition returns.
 *   - So the only window that satisfies both is inside BootPartition, after
 *     WDVD_OpenPartition and before Apploader_Run. That is where Prepare()
 *     is called from.
 *
 * This module deliberately stops short of registering anything with IOS. It
 * reads, resolves and reports. Nothing it does changes how the game boots, so
 * it is safe to ship while the IOS-side hook is still being written.
 ***************************************************************************/
#ifndef RIIVO_BOOT_HPP_
#define RIIVO_BOOT_HPP_

#include <gctypes.h>
#include <string>
#include "RiivoTypes.hpp"

namespace Riivo
{
	//! Hand the boot-time context to this module. Called from BootGame once the
	//! selection has been resolved, while the devices are still mounted.
	//! `set` must outlive the boot; `logPath` may be empty to disable reporting.
	//! `sectorSize` is the geometry of the drive the backup lives on; the mod
	//! region has to be aligned to it because a fragment cannot start mid-sector.
	//! `usbPort` is the USB port the backup is on, ignored in SD mode. The cIOS
	//! serves the WHOLE fragment list from one drive, so the mod has to be on
	//! that same drive and this is how that is checked.
	void SetBootContext(const ResolvedPatchSet *set, const std::string &device,
						const std::string &logPath, u32 sectorSize,
						const u8 *gameId, int usbPort);

	//! Append a block of text to the boot log set up by SetBootContext.
	//! No-op when there is no log path or the device has already gone away.
	void AppendLog(const std::string &text);

	//! Survey the cIOS slots and describe what is installed, including which
	//! one we are currently running under. Appended to the boot log.
	void ReportCios();

	//! Read the FST from the currently open partition, match the selected
	//! <file>/<folder> patches against it and report every redirect that would
	//! be applied. Call ONLY from the window described above.
	//! Does not modify the boot in any way.
	void PrepareFileRedirects();

	//! Report where the rebuilt file table would be installed in the running
	//! game's memory. This needs the boot-info block the apploader fills in, so
	//! unlike everything above it must be called AFTER Apploader_Run - but still
	//! inside BootPartition, because the log lives on a card that
	//! ShutDownDevices() unmounts the moment BootPartition returns.
	//! Reports only; installs nothing.
	void ReportFstPlacement();

	//! Retain and enlarge the game's fragment list before it is handed to
	//! the cIOS, so there is room above the backup for the mod and so the
	//! disc is recognised as dual-layer (which raises the read ceiling from
	//! 4.7 GB to 8.5 GB). Must be called between get_frag_list and
	//! set_frag_list; does nothing unless file/folder patches are active.
	void PrepareFragList();

	//! True when the selected options need the mod's files but those files did
	//! not get installed. The memory patches must then be skipped: a mod that
	//! replaces files writes them expecting the files to be present, and a
	//! total conversion patched without its assets exits to the System Menu
	//! rather than booting. Only meaningful after ReportFstPlacement has run.
	bool FileWorkIncomplete();
}

#endif
