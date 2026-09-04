/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>
#include <algorithm>
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
			const std::string childFull = dirPath + "/" + ent->d_name;
			struct stat st;
			if (stat(childFull.c_str(), &st) != 0)
				continue;
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

	void FsDirLister::List(const std::string &fullDir, bool recursive, std::vector<std::string> &out)
	{
		ListRecurse(fullDir, "", recursive, out, &skipped);
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

	void ListModFiles(const ResolvedPatchSet &set, const std::string &device,
					  DirLister *lister, std::vector<ModCandidate> &out)
	{
		out.clear();

		for (size_t i = 0; i < set.files.size(); ++i)
		{
			const ResolvedFile &f = set.files[i];
			ModCandidate c;
			c.external = JoinPath(device, f.root, f.external);
			c.disc = NormaliseDiscPath(f.disc);
			struct stat st;
			if (c.disc.empty() || stat(c.external.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
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

				std::vector<std::string> rel;
				lister->List(extDir, f.recursive, rel);

				for (size_t j = 0; j < rel.size(); ++j)
				{
					ModCandidate c;
					c.external = JoinDisc(extDir, rel[j]);
					c.disc = NormaliseDiscPath(JoinDisc(discDir, rel[j]));
					struct stat st;
					if (c.disc.empty() || stat(c.external.c_str(), &st) != 0
						|| !S_ISREG(st.st_mode))
						continue;
					c.size = (u32) st.st_size;
					out.push_back(c);
				}
			}
		}

		//! Overlapping <folder> rules name the same disc file more than once.
		//! Keep the last, which is the one AddOrReplace keeps when the table is
		//! rebuilt, then sort so the placement is identical on every boot.
		std::sort(out.begin(), out.end(), ByCandidateDisc);
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
}
