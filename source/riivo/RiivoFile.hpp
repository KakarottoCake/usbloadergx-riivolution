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
#include "RiivoManifest.hpp"
#include <map>

namespace Riivo
{
	//! One "read this disc range from that external file" mapping.
	struct RedirectSpec
	{
		u64 discOffset;       // byte offset on the disc where the redirect starts
		u32 length;           // bytes to redirect; 0 => "whole external file" (resolved at HW via stat)
		u32 fileOffset;       // start offset within the external file
		u32 discLength;       // size of the disc file being replaced, from the FST
		std::string disc;     // matched disc path, lower-cased, e.g. "/audiores/x.ast"
		std::string external; // full external path, e.g. "sd:/mymod/E/boot.arc"
	};

	//! An external file with no counterpart on the disc. These are the files a
	//! mod ADDS, and they need a new FST entry rather than a redirect.
	struct CreatedFile
	{
		std::string disc;     // where it should appear on the disc
		std::string external; // where it actually is on the card
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
						std::vector<CreatedFile> *outCreated = 0);

	//! One file the mod supplies, found WITHOUT consulting the disc.
	struct ModCandidate
	{
		std::string disc;     // where it should appear, lower-cased, leading '/'
		std::string external; // where it really is on the card
		u32 size;             // from stat
	};

	//! Enumerate everything the mod would put on the disc, before the disc can
	//! be read at all.
	//!
	//! The fragment list has to be handed to the cIOS while no title is running,
	//! which is before the game partition - and therefore the file table - can
	//! be opened. So placement is decided from this list, and the rebuilt table
	//! is made to agree with it afterwards. It shares BuildRedirects' path
	//! helpers precisely so the two cannot disagree about where a file goes.
	//!
	//! Sorted by disc path, so the placement is the same on every boot.
	//! Called once per <folder> rule, just before it is listed, with the
	//! number of files gathered so far. A total conversion can spend minutes
	//! in here, and a rule that resolves somewhere unintended - the drive
	//! root, say - looks identical from outside: both are a black screen.
	//! Reporting per rule names the one that stalled, and the running total
	//! makes a runaway obvious while it is still running.
	typedef void (*ListProgressFn)(void *ctx, const std::string &dir, u32 soFar);

	void ListModFiles(const ResolvedPatchSet &set, const std::string &device,
					  DirLister *lister, std::vector<ModCandidate> &out,
					  ListProgressFn progress = 0, void *ctx = 0);

	//! Lower-case a disc path and strip empty components, giving the exact key
	//! FstBuilder::LayoutFrom expects.
	std::string NormaliseDiscPath(const std::string &path);

	//! External-file sizes stated during early enumeration, reused late so
	//! each file is stat'ed once per boot instead of three times
	//! (ListModFiles, the size-accounting loop and the table-build loop each
	//! stat'ed every file: thousands of libfat root-to-leaf walks behind a
	//! black screen). A miss falls back to stat, so unknown paths behave
	//! exactly as before. Cleared per boot with ClearDirListCache.
	void RememberFileSizes(const std::vector<ModCandidate> &candidates);
	bool KnownFileSize(const std::string &external, u32 *outSize);
	void ClearFileSizeCache();
	void FileSizeCacheStats(u32 *hits, u32 *misses);

	//! Directory-listing cache hits and misses since the last clear, for the
	//! boot report. A miss walks the card; a hit replays the early pass.
	void DirCacheStats(u32 *hits, u32 *misses);

	//! Translate resolved redirect specs into v1 manifest External extents
	//! (WP2 planner -> manifest bridge, Gate C).
	//!
	//! Why this exists: RedirectSpec carries a fileOffset sub-range and a
	//! length of 0 meaning "whole external file", but the legacy 'RIIV' table
	//! (RedirectEntry) has no source-offset field, so partial replacements
	//! (<file offset= fileoffset= length=>) cannot be expressed there. The v1
	//! manifest does carry srcOffset, so this bridge preserves them.
	//!
	//! `fileSizes` maps spec.external to the external file's real size (from
	//! stat at enumeration time); keeping it a parameter - rather than
	//! stating here - is what keeps this host-testable with no FS calls.
	//! `sizes` must hold every spec.external; a missing entry is refused with
	//! a sentence naming the file, never silently zero-filled.
	//!
	//! resize=false (Riivolution: keep the original size) clamps the extent
	//! length to the disc file's size (spec.discLength). The FST rebuild is
	//! untouched by this; the game still reads the old length and gets the
	//! clamped prefix. resize=true keeps the full replacement length.
	//!
	//! Output is sorted ascending by discOffset for the manifest builder,
	//! which re-checks order/overlap at build time. Returns false + why on
	//! any refusal (missing size, fileOffset past EOF, empty result).
	//! `resizeFlags[i]` parallels `specs[i]` (true = allow growth).
	bool BuildManifestExtents(const std::vector<RedirectSpec> &specs,
							  const std::map<std::string, u32> &fileSizes,
							  const std::vector<bool> &resizeFlags,
							  std::vector<ManifestExtent> &out,
							  std::string &why);

	//! Concrete DirLister backed by opendir/readdir (real SD/USB filesystem).
	//! Returns file paths relative to the listed directory.
	//! Drop the memoised directory listings. Called once per boot: the card
	//! can be swapped between launches, and a stale listing would place
	//! files that are no longer there.
	void ClearDirListCache();

	struct FsDirLister : public DirLister
	{
		FsDirLister() : skipped(0) {}
		void List(const std::string &fullDir, bool recursive, std::vector<std::string> &out);

		//! Filesystem metadata files ignored so far (macOS "._" AppleDouble twins,
		//! .DS_Store, Thumbs.db). Mods unzipped on a Mac are full of these and they
		//! would otherwise flood the redirect table with entries matching nothing.
		int skipped;
	};
}

#endif
