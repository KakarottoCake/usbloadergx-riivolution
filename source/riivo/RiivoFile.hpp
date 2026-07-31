/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Phase 3 (loader-side): turn resolved <file>/<folder> patches + the game FST
 * into a list of RedirectSpecs. Each spec says "reads of this disc byte range
 * should come from this external file instead". The hardware step then stats
 * each external file, builds its fragment list, and registers the table with
 * the in-RAM cIOS DI read-hook.
 *
 * This module is pure loader-side logic (no disc/IOS calls) so it is
 * host-testable; folder enumeration is injected via DirLister.
 ***************************************************************************/
#ifndef RIIVO_FILE_HPP_
#define RIIVO_FILE_HPP_

#include "RiivoTypes.hpp"
#include "RiivoFst.hpp"

namespace Riivo
{
	//! One "read this disc range from that external file" mapping.
	struct RedirectSpec
	{
		u32 discOffset;       // byte offset on the disc where the redirect starts
		u32 length;           // bytes to redirect; 0 => "whole external file" (resolved at HW via stat)
		u32 fileOffset;       // start offset within the external file
		std::string external; // full external path, e.g. "sd:/mymod/E/boot.arc"
	};

	//! Enumerates files under an external folder. Backed by readdir on hardware;
	//! injected in tests. Returned paths are RELATIVE to `fullDir` (e.g. "a/b.arc").
	struct DirLister
	{
		virtual ~DirLister() {}
		virtual void List(const std::string &fullDir, bool recursive,
						   std::vector<std::string> &out) = 0;
	};

	//! Build redirect specs from the resolved <file>/<folder> patches against `fst`.
	//! `device` is the SD/USB prefix ("sd:"). `lister` enumerates external folders
	//! for <folder> patches (may be NULL to skip them). `create=true` external files
	//! that have no matching FST entry are collected into `outCreated` (Phase 4 FST
	//! rebuild) rather than redirected.
	void BuildRedirects(const Fst &fst, const ResolvedPatchSet &set, const std::string &device,
						DirLister *lister, std::vector<RedirectSpec> &out,
						std::vector<std::string> *outCreated = 0);

	//! Concrete DirLister backed by opendir/readdir (real SD/USB filesystem).
	//! Returns file paths relative to the listed directory.
	struct FsDirLister : public DirLister
	{
		void List(const std::string &fullDir, bool recursive, std::vector<std::string> &out);
	};
}

#endif
