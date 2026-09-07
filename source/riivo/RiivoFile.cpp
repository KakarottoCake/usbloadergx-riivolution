/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include "RiivoFile.hpp"
#include "RiivoConfig.hpp"
#include "gecko.h"

namespace Riivo
{
	// --------------------------------------------------------------------
	// readdir-backed folder enumeration (real filesystem)
	// --------------------------------------------------------------------

	//! Filesystem metadata that is never mod content. A mod archive unpacked on a
	//! Mac carries a "._name" AppleDouble twin for every real file, which would
	//! otherwise double the enumeration and produce thousands of entries that
	//! match nothing on the disc.
	static bool IsMetadataFile(const char *name)
	{
		if (name[0] == '.' && name[1] == '_')
			return true;
		if (strcasecmp(name, ".DS_Store") == 0)
			return true;
		if (strcasecmp(name, "Thumbs.db") == 0)
			return true;
		return false;
	}

	static void ListRecurse(const std::string &base, const std::string &rel, bool recursive,
							std::vector<std::string> &out, int *skipped)
	{
		const std::string dirPath = rel.empty() ? base : (base + "/" + rel);
		DIR *dir = opendir(dirPath.c_str());
		if (!dir)
			return;
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL)
		{
			if (ent->d_name[0] == '.' &&
				(ent->d_name[1] == 0 || (ent->d_name[1] == '.' && ent->d_name[2] == 0)))
				continue; // skip . and ..
			if (IsMetadataFile(ent->d_name))
			{
				if (skipped)
					++(*skipped);
				continue;
			}
			const std::string childRel = rel.empty() ? std::string(ent->d_name)
													 : (rel + "/" + ent->d_name);

			//! readdir has already stat'ed this entry: devkitPro's dirent
			//! carries d_stat, filled from the directory entry the driver just
			//! read. Calling stat() on the full path again makes libfat walk
			//! the path from the root a second time - and libfat's sector
			//! cache is only a handful of entries, so on a mod with thousands
			//! of files across hundreds of directories that second walk
			//! evicts the directory we are in the middle of reading. It is
			//! thousands of redundant card reads on a screen that is already
			//! black, for an answer we were already handed.
			//! d_stat is a devkitPro extension, so the host build - where the
			//! cost this avoids does not exist - takes the plain path.
			struct stat st;
#ifdef _DIRENT_HAVE_D_STAT
			st = ent->d_stat;
			if (st.st_mode == 0)
#else
			memset(&st, 0, sizeof(st));
#endif
			{
				//! A driver that does not fill d_stat. Fall back rather than
				//! skip the file: correctness first, speed second.
				const std::string childFull = dirPath + "/" + ent->d_name;
				if (stat(childFull.c_str(), &st) != 0)
					continue;
			}
			if (S_ISDIR(st.st_mode))
			{
				if (recursive)
					ListRecurse(base, childRel, recursive, out, skipped);
			}
			else
				out.push_back(childRel);
		}
		closedir(dir);
	}

	//! Every <folder> rule is listed TWICE per boot - once by ListModFiles to
	//! decide placement, once by BuildRedirects to match against the disc - with
	//! the same arguments both times. Each pass is an opendir plus a stat per
	//! entry, so on a total conversion that is thousands of duplicated card
	//! reads on a screen that is already black. Same inputs, same answer, so
	//! remember it. Cleared per boot in SetBootContext, because the card can be
	//! swapped between one launch and the next.
	struct CachedListing
	{
		std::vector<std::string> files;
		int skipped;
		CachedListing() : skipped(0) {}
	};
	static std::map<std::string, CachedListing> g_dirCache;
	static u32 g_dirHits = 0;
	static u32 g_dirMisses = 0;

	void ClearDirListCache()
	{
		g_dirCache.clear();
		g_dirHits = g_dirMisses = 0;
	}

	void FsDirLister::List(const std::string &fullDir, bool recursive, std::vector<std::string> &out)
	{
		const std::string key = fullDir + (recursive ? "|r" : "|n");
		std::map<std::string, CachedListing>::const_iterator hit = g_dirCache.find(key);
		if (hit != g_dirCache.end())
		{
			++g_dirHits;
			out.insert(out.end(), hit->second.files.begin(), hit->second.files.end());
			skipped += hit->second.skipped;
			return;
		}

		++g_dirMisses;
		CachedListing entry;
		ListRecurse(fullDir, "", recursive, entry.files, &entry.skipped);
		skipped += entry.skipped;
		out.insert(out.end(), entry.files.begin(), entry.files.end());
		g_dirCache[key] = entry;
	}

	//! Sizes stated while enumerating, so the late phase reuses them instead
	//! of stat'ing every file twice more. See the header for why.
	static std::map<std::string, u32> g_sizeCache;
	static u32 g_sizeHits = 0;
	static u32 g_sizeMisses = 0;

	void ClearFileSizeCache()
	{
		g_sizeCache.clear();
		g_sizeHits = g_sizeMisses = 0;
	}

	void RememberFileSizes(const std::vector<ModCandidate> &candidates)
	{
		for (size_t i = 0; i < candidates.size(); ++i)
			g_sizeCache[candidates[i].external] = candidates[i].size;
	}

	bool KnownFileSize(const std::string &external, u32 *outSize)
	{
		std::map<std::string, u32>::const_iterator it = g_sizeCache.find(external);
		if (it == g_sizeCache.end())
		{
			++g_sizeMisses;
			return false;
		}
		++g_sizeHits;
		if (outSize)
			*outSize = it->second;
		return true;
	}

	void FileSizeCacheStats(u32 *hits, u32 *misses)
	{
		if (hits)
			*hits = g_sizeHits;
		if (misses)
			*misses = g_sizeMisses;
	}

	void DirCacheStats(u32 *hits, u32 *misses)
	{
		if (hits)
			*hits = g_dirHits;
		if (misses)
			*misses = g_dirMisses;
	}

	//! Normalise a Riivo disc= value to a full lower-cased path with leading '/'.
	//! (FindFile also accepts bare names, but folder joins need a real path.)
	static std::string DiscPath(const std::string &disc)
	{
		if (disc.empty())
			return "/";
		std::string p = disc;
		if (p[0] != '/')
			p = "/" + p;
		return p;
	}

	//! Join a disc folder path with a relative child path.
	static std::string JoinDisc(const std::string &dir, const std::string &rel)
	{
		std::string d = dir;
		if (!d.empty() && d[d.size() - 1] == '/')
			d.erase(d.size() - 1);
		std::string r = rel;
		if (!r.empty() && r[0] == '/')
			r = r.substr(1);
		return d + "/" + r;
	}

	static void BuildFile(const Fst &fst, const ResolvedFile &f, const std::string &device,
						  std::vector<RedirectSpec> &out, std::vector<CreatedFile> *outCreated)
	{
		const FstFile *entry = fst.FindFile(f.disc);
		const std::string external = JoinPath(device, f.root, f.external);

		if (!entry)
		{
			// No such disc file. Only meaningful if create=true (Phase 4 FST rebuild).
			if (f.create && outCreated)
			{
				CreatedFile cf;
				cf.disc = f.disc;
				cf.external = external;
				outCreated->push_back(cf);
			}
			else
				gprintf("Riivo file: disc file not found, skipped: %s\n", f.disc.c_str());
			return;
		}

		RedirectSpec spec;
		spec.discOffset = entry->offset + f.offset; // f.offset patches a sub-range
		spec.length = f.length;                     // 0 => whole external file (resolved at HW)
		spec.fileOffset = f.fileoffset;
		spec.discLength = entry->length;
		spec.disc = entry->path;
		spec.external = external;
		out.push_back(spec);
	}

	static void BuildFolder(const Fst &fst, const ResolvedFolder &f, const std::string &device,
							DirLister *lister, std::vector<RedirectSpec> &out,
							std::vector<CreatedFile> *outCreated)
	{
		if (!lister)
			return;

		const std::string discDir = DiscPath(f.disc);
		const std::string extDir = JoinPath(device, f.root, f.external);

		std::vector<std::string> extFiles;
		lister->List(extDir, f.recursive, extFiles);

		for (size_t i = 0; i < extFiles.size(); ++i)
		{
			const std::string &rel = extFiles[i]; // relative to extDir
			const std::string discFile = JoinDisc(discDir, rel);
			const FstFile *entry = fst.FindFile(discFile);
			const std::string external = JoinDisc(extDir, rel);

			if (!entry)
			{
				if (f.create && outCreated)
				{
					CreatedFile cf;
					cf.disc = discFile;
					cf.external = external;
					outCreated->push_back(cf);
				}
				// else: external file with no disc counterpart and no create => ignored
				continue;
			}

			RedirectSpec spec;
			spec.discOffset = entry->offset;
			spec.length = f.length; // 0 => whole external file
			spec.fileOffset = 0;
			spec.discLength = entry->length;
			spec.disc = entry->path;
			spec.external = external;
			out.push_back(spec);
		}
	}

	std::string NormaliseDiscPath(const std::string &path)
	{
		std::string out;
		size_t i = 0;
		while (i < path.size())
		{
			while (i < path.size() && path[i] == '/')
				++i;
			size_t j = i;
			while (j < path.size() && path[j] != '/')
				++j;
			if (j > i)
			{
				out += '/';
				for (size_t k = i; k < j; ++k)
					out += (char) tolower((unsigned char) path[k]);
			}
			i = j;
		}
		return out;
	}

	static bool ByCandidateDisc(const ModCandidate &a, const ModCandidate &b)
	{
		return a.disc < b.disc;
	}

	bool StatFileProbe::IsRegularFile(const std::string &path)
	{
		struct stat st;
		return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
	}

	void FindMissingExternals(const ResolvedPatchSet &set,
							  const std::string &device,
							  FileProbe &probe,
							  std::vector<MissingExternal> &out)
	{
		out.clear();
		for (size_t i = 0; i < set.files.size(); ++i)
		{
			const ResolvedFile &f = set.files[i];
			MissingExternal m;
			m.external = JoinPath(device, f.root, f.external);
			m.disc = NormaliseDiscPath(f.disc);
			//! An empty disc path is a malformed patch, not a missing file,
			//! and naming a card path for it would send the user looking in
			//! the wrong place.
			if (m.disc.empty())
				continue;
			if (!probe.IsRegularFile(m.external))
				out.push_back(m);
		}
	}

	//! Record a file the mod names that is not on the card. `missing` is
	//! optional so the enumeration path stays allocation-free for callers
	//! that do not want the list.
	static void NoteMissing(std::vector<MissingExternal> *missing,
							const std::string &disc, const std::string &external)
	{
		if (!missing)
			return;
		MissingExternal m;
		m.disc = disc;
		m.external = external;
		missing->push_back(m);
	}

	void ListModFiles(const ResolvedPatchSet &set, const std::string &device,
					  DirLister *lister, std::vector<ModCandidate> &out,
					  ListProgressFn progress, void *ctx,
					  std::vector<MissingExternal> *missing)
	{
		out.clear();
		if (missing)
			missing->clear();

		for (size_t i = 0; i < set.files.size(); ++i)
		{
			const ResolvedFile &f = set.files[i];
			ModCandidate c;
			c.external = JoinPath(device, f.root, f.external);
			c.disc = NormaliseDiscPath(f.disc);
			struct stat st;
			if (c.disc.empty())
				continue;
			if (stat(c.external.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			{
				NoteMissing(missing, c.disc, c.external);
				continue;
			}
			c.size = (u32) st.st_size;
			out.push_back(c);
		}

		if (lister)
		{
			for (size_t i = 0; i < set.folders.size(); ++i)
			{
				const ResolvedFolder &f = set.folders[i];
				const std::string discDir = DiscPath(f.disc);
				const std::string extDir = JoinPath(device, f.root, f.external);

				if (progress)
					progress(ctx, extDir, (u32) out.size());

				std::vector<std::string> rel;
				lister->List(extDir, f.recursive, rel);

				for (size_t j = 0; j < rel.size(); ++j)
				{
					ModCandidate c;
					c.external = JoinDisc(extDir, rel[j]);
					c.disc = NormaliseDiscPath(JoinDisc(discDir, rel[j]));
					struct stat st;
					if (c.disc.empty())
						continue;
					//! The lister just named this file, so a stat that fails
					//! here means it went away or is not a regular file -
					//! rare, and worth naming for exactly that reason.
					if (stat(c.external.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
					{
						NoteMissing(missing, c.disc, c.external);
						continue;
					}
					c.size = (u32) st.st_size;
					out.push_back(c);
				}
			}
		}

		//! Remember every stated size - including claims the dedup below
		//! drops - before sorting, so the late phase reuses them instead of
		//! stat'ing the same files again. Duplicate disc destinations keep
		//! the last claim in both phases alike.
		RememberFileSizes(out);

		//! Overlapping <folder> rules name the same disc file more than once.
		//! Keep the last, which is the one AddOrReplace keeps when the table is
		//! rebuilt, then sort so the placement is identical on every boot.
		std::stable_sort(out.begin(), out.end(), ByCandidateDisc);
		std::vector<ModCandidate> unique;
		unique.reserve(out.size());
		for (size_t i = 0; i < out.size(); ++i)
		{
			if (i + 1 < out.size() && out[i].disc == out[i + 1].disc)
				continue;
			unique.push_back(out[i]);
		}
		out.swap(unique);

		gprintf("Riivo file: %u mod file(s) enumerated\n", (unsigned) out.size());
	}

	void BuildRedirects(const Fst &fst, const ResolvedPatchSet &set, const std::string &device,
						DirLister *lister, std::vector<RedirectSpec> &out,
						std::vector<CreatedFile> *outCreated)
	{
		for (size_t i = 0; i < set.files.size(); ++i)
			BuildFile(fst, set.files[i], device, out, outCreated);
		for (size_t i = 0; i < set.folders.size(); ++i)
			BuildFolder(fst, set.folders[i], device, lister, out, outCreated);

		gprintf("Riivo file: built %u redirect(s)%s\n", (unsigned) out.size(),
				outCreated ? "" : " (create= files ignored)");
	}

	static bool ByManifestOffset(const ManifestExtent &a, const ManifestExtent &b)
	{
		return a.discOffset < b.discOffset;
	}

	//! Source id from an external path's device prefix ("sd:/..." -> SD,
	//! "usb1:/..." -> USB). RiiFS-backed paths arrive in WP8 with their own
	//! scheme and are refused here so they can never be misclassified as
	//! local sectors.
	static bool ClassifySource(const std::string &external, u16 &outSrc)
	{
		size_t colon = external.find(':');
		std::string dev = colon == std::string::npos ? "" : external.substr(0, colon);
		for (size_t i = 0; i < dev.size(); ++i)
			dev[i] = (char) tolower((unsigned char) dev[i]);
		if (dev == "sd" || dev.compare(0, 2, "sd") == 0)
		{
			outSrc = RIIVO_SRC_SD;
			return true;
		}
		if (dev.compare(0, 3, "usb") == 0)
		{
			outSrc = RIIVO_SRC_USB;
			return true;
		}
		return false;
	}

	//! Path within the FAT partition: strip the "sd:" device prefix, keeping
	//! the leading '/'. The runtime resolves it against partLba/discovery.
	static std::string StripDevice(const std::string &external)
	{
		size_t colon = external.find(':');
		if (colon == std::string::npos)
			return external;
		std::string p = external.substr(colon + 1);
		if (p.empty() || p[0] != '/')
			p = "/" + p;
		return p;
	}

	bool BuildManifestExtents(const std::vector<RedirectSpec> &specs,
							  const std::map<std::string, u32> &fileSizes,
							  const std::vector<bool> &resizeFlags,
							  std::vector<ManifestExtent> &out,
							  std::string &why)
	{
		why.clear();
		out.clear();
		if (specs.empty())
		{
			why = "manifest extents: no redirects to describe";
			return false;
		}
		if (!resizeFlags.empty() && resizeFlags.size() != specs.size())
		{
			why = "manifest extents: resize flags do not match specs";
			return false;
		}

		for (size_t i = 0; i < specs.size(); ++i)
		{
			const RedirectSpec &s = specs[i];
			std::map<std::string, u32>::const_iterator it = fileSizes.find(s.external);
			if (it == fileSizes.end())
			{
				why = "manifest extents: no size for " + s.external;
				return false;
			}
			u32 realSize = it->second;
			if (s.fileOffset > realSize)
			{
				why = "manifest extents: fileoffset past end of " + s.external;
				return false;
			}
			u64 avail = (u64) realSize - s.fileOffset;
			u64 len = s.length ? (u64) s.length : avail;
			if (s.length && (u64) s.fileOffset + s.length > realSize)
			{
				why = "manifest extents: range past end of " + s.external;
				return false;
			}
			bool resize = resizeFlags.empty() ? true : resizeFlags[i];
			if (!resize && len > s.discLength)
				len = s.discLength;
			if (len > 0xFFFFFFFFULL)
			{
				why = "manifest extents: file too large: " + s.external;
				return false;
			}
			if (len == 0)
				continue; // zero-length: placement is a no-op downstream

			u16 src = RIIVO_SRC_NONE;
			if (!ClassifySource(s.external, src))
			{
				why = "manifest extents: unknown device in " + s.external;
				return false;
			}
			ManifestExtent e;
			e.discOffset = s.discOffset;
			e.length = (u32) len;
			e.kind = RIIVO_EXT_EXTERNAL;
			e.source = src;
			e.srcOffset = s.fileOffset;
			e.path = StripDevice(s.external);
			e.genOff = 0;
			out.push_back(e);
		}

		if (out.empty())
		{
			why = "manifest extents: nothing left after zero-length pruning";
			return false;
		}
		std::stable_sort(out.begin(), out.end(), ByManifestOffset);
		return true;
	}
}
