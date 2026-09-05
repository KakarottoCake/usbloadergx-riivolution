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
 *   - IOS modules run at ARM virtual addresses (the DIP module sits at
 *     0x20200000) while the PPC only sees physical MEM2 at 0x90000000. The
 *     Starlet's MMU tables decide the mapping, the PPC cannot read its
 *     translation base register, and the layout shifts with the base IOS and
 *     the d2x revision. There is no constant to subtract.
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
		};
		std::vector<DiDump> dumps;

		IosProbe()
			: attempted(false), ahbprot(false), iosVersion(0), iosRevision(0), scanFrom(0), scanTo(0),
			  words(0), nonZero(0), selfAddr(0), selfLo(0), selfHi(0),
			  arena2Lo(0), arena2Hi(0) {}
	};

	//! Scan MEM2 for the plugin's fingerprints and, if one is found, write a
	//! window of the surrounding code to `dumpPath`. Appends a report to the
	//! boot log. Safe to call on any console: read-only with respect to IOS.
	void ProbeIosPlugin(const std::string &dumpPath, IosProbe &out);

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
	bool ApplyDiPatch(u32 site, u32 endWords, std::string &why);
}

#endif
