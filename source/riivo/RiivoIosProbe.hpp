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
		std::vector<u32> patchSites;

		u32 dumpBase;             // what got written to the card, 0 if nothing
		u32 dumpSize;
		std::string dumpPath;

		IosProbe()
			: ahbprot(false), iosVersion(0), iosRevision(0), scanFrom(0), scanTo(0),
			  words(0), nonZero(0), dumpBase(0), dumpSize(0) {}
	};

	//! Scan MEM2 for the plugin's fingerprints and, if one is found, write a
	//! window of the surrounding code to `dumpPath`. Appends a report to the
	//! boot log. Safe to call on any console: read-only with respect to IOS.
	void ProbeIosPlugin(const std::string &dumpPath, IosProbe &out);

	//! Human-readable rendering of a probe result, for the boot log.
	std::string DescribeProbe(const IosProbe &p);
}

#endif
