/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Find the d2x DIP plugin inside the running cIOS, and photograph it.
 *
 * The last piece of runtime file replacement is a very small change to the
 * plugin's disc-read handler: reads below a threshold keep going through the
 * normal decrypting path, reads above it - where the mod's files have been
 * relocated to - go through the plugin's own unencrypted fragment reader,
 * which does no AES and no hash check. That is a couple of ARM instructions.
 *
 * Writing those instructions needs the plugin's actual compiled code, and the
 * code is not derivable from here:
 *   - The d2x plugin (DIPP) runs at MEM2 physical addresses: its literal pool
 *     holds 0x13802340 where the PPC sees 0x93802340, so the Starlet-to-PPC
 *     offset for this module is the constant 0x80000000. An older comment here
 *     claimed ARM virtual addresses near 0x20200000 with no constant to
 *     subtract; the dump disproved that for DIPP. Other IOS modules may still
 *     map differently, so every new address range gets verified, not assumed.
 *   - d2x does not ship the plugin as a separate binary; it exists as DIPP.app
 *     inside the installer, assembled at install time.
 * So the universal technique - the one libruntimeiospatch, Priiloader and every
 * USB loader uses - is to scan MEM2 for a distinctive byte pattern. This module
 * does that scan and dumps what it finds, so the patch can be written from real
 * bytes instead of guessed at.
 *
 * It only ever READS from IOS and writes a file to the card. Nothing here
 * modifies the console.
 ***************************************************************************/
#ifndef RIIVO_IOS_PROBE_HPP_
#define RIIVO_IOS_PROBE_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include "RiivoProbeClassify.hpp"

namespace Riivo
{
	//! One thing worth finding in IOS memory, and where it turned up.
	struct IosPattern
	{
		const char *name;
		const char *what; // why this value identifies the plugin
		u32 value;
		std::vector<u32> hits; // PPC addresses, cached view
	};

	struct IosProbe
	{
		bool attempted;
		bool ahbprot;             // could we see IOS memory at all
		u32 iosVersion;
		u32 iosRevision;
		u32 scanFrom, scanTo;
		u32 words;                // words examined
		u32 nonZero;              // of those, how many were not zero - a sanity
		                          // check that we are really seeing IOS and not
		                          // a blank window because reads were refused
		std::vector<IosPattern> patterns;

		//! Where the read dispatch that has to be patched was found. Exactly one
		//! hit is the healthy answer: none means this cIOS is not the d2x build
		//! the patch was derived from, and more than one means the pattern is
		//! not as distinctive as it looked and must not be applied blind.
		//! The dispatch is only searched for inside module windows below, so
		//! our own image can no longer contribute a hit.
		std::vector<u32> patchSites;

		//! Our own pattern copy as seen at runtime, and the loader window
		//! around it that matches were excluded from. Printed unconditionally
		//! so the next log can never again mistake our rodata for IOS.
		u32 selfAddr;
		u32 selfLo, selfHi;

		//! MEM2 arena bounds at probe time, for placing every address above.
		u32 arena2Lo, arena2Hi;

		//! Every ceiling-pair cluster found, with its classification.
		std::vector<DiModule> modules;

		//! Fragment-code and stock-DI anchors found, with classification.
		//! Fragment anchors are logged only, never dumped: MAX_FRAG proximity
		//! fires on non-fragment modules too, and the read path lives in DIPP.
		//! Ours entries are kept for the log either way.
		std::vector<DiAnchor> fragAnchors;
		std::vector<DiAnchor> stockAnchors;

		//! One dumped module window per surviving candidate, in ascending
		//! address order. Empty when no module window was identified.
		struct DiDump
		{
			u32 base;
			u32 size;
			std::string path;
			//! True when no candidate survived classification and this window
			//! was taken anyway, so the round still comes home with bytes. A
			//! fallback is never searched for a dispatch.
			bool fallback;
			//! The anchor the window was taken around, and what found it.
			u32 anchor;
			DiAnchorKind kind;
		};
		std::vector<DiDump> dumps;

		//! Windows cut by the dump cap. Printed so a silent drop is impossible.
		u32 dumpsSkipped;
		bool dumpsEnabled;

		IosProbe()
			: attempted(false), ahbprot(false), iosVersion(0), iosRevision(0), scanFrom(0), scanTo(0),
			  words(0), nonZero(0), selfAddr(0), selfLo(0), selfHi(0),
			  arena2Lo(0), arena2Hi(0), dumpsSkipped(0), dumpsEnabled(false) {}
	};

	//! Scan MEM2 for the plugin's fingerprints. Only when writeDumps is true,
	//! write diagnostic windows to `dumpPath`. Appends a report to the
	//! boot log. Safe to call on any console: read-only with respect to IOS.
	void ProbeIosPlugin(const std::string &dumpPath, IosProbe &out, bool writeDumps = false);

	//! Human-readable rendering of a probe result, for the boot log.
	std::string DescribeProbe(const IosProbe &p);

	//! Install the LOW_READ hook at `site`, which must be a patch site the
	//! probe found. Verifies the dispatch, its calling convention and the
	//! executable helper before writing, and rolls both writes back if either
	//! fails to stick, so a half-applied hook is reported rather than left in
	//! place. Returns false and fills `why` on any mismatch.
	//!
	//! Applying this to a game whose file table has NOT been rebuilt is
	//! harmless: it only changes what happens to reads inside the synthetic
	//! window, and an unmodified game never makes one.
	bool ApplyDiPatch(u32 site, u32 endWords, std::string &why, u32 *storage = 0);
}

#endif
