/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <dirent.h>
#include <sys/stat.h>
#include "RiivoFile.hpp"
#include "RiivoConfig.hpp"
#include "gecko.h"

namespace Riivo
{
	// --------------------------------------------------------------------
	// readdir-backed folder enumeration (real filesystem)
	// --------------------------------------------------------------------

	static void ListRecurse(const std::string &base, const std::string &rel, bool recursive,
							std::vector<std::string> &out)
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
			const std::string childRel = rel.empty() ? std::string(ent->d_name)
													 : (rel + "/" + ent->d_name);
			const std::string childFull = dirPath + "/" + ent->d_name;
			struct stat st;
			if (stat(childFull.c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
			{
				if (recursive)
					ListRecurse(base, childRel, recursive, out);
			}
			else
				out.push_back(childRel);
		}
		closedir(dir);
	}

	void FsDirLister::List(const std::string &fullDir, bool recursive, std::vector<std::string> &out)
	{
		ListRecurse(fullDir, "", recursive, out);
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
						  std::vector<RedirectSpec> &out, std::vector<std::string> *outCreated)
	{
		const FstFile *entry = fst.FindFile(f.disc);
		const std::string external = JoinPath(device, f.root, f.external);

		if (!entry)
		{
			// No such disc file. Only meaningful if create=true (Phase 4 FST rebuild).
			if (f.create && outCreated)
				outCreated->push_back(external);
			else
				gprintf("Riivo file: disc file not found, skipped: %s\n", f.disc.c_str());
			return;
		}

		RedirectSpec spec;
		spec.discOffset = entry->offset + f.offset; // f.offset patches a sub-range
		spec.length = f.length;                     // 0 => whole external file (resolved at HW)
		spec.fileOffset = f.fileoffset;
		spec.external = external;
		out.push_back(spec);
	}

	static void BuildFolder(const Fst &fst, const ResolvedFolder &f, const std::string &device,
							DirLister *lister, std::vector<RedirectSpec> &out,
							std::vector<std::string> *outCreated)
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
					outCreated->push_back(external);
				// else: external file with no disc counterpart and no create => ignored
				continue;
			}

			RedirectSpec spec;
			spec.discOffset = entry->offset;
			spec.length = f.length; // 0 => whole external file
			spec.fileOffset = 0;
			spec.external = external;
			out.push_back(spec);
		}
	}

	void BuildRedirects(const Fst &fst, const ResolvedPatchSet &set, const std::string &device,
						DirLister *lister, std::vector<RedirectSpec> &out,
						std::vector<std::string> *outCreated)
	{
		for (size_t i = 0; i < set.files.size(); ++i)
			BuildFile(fst, set.files[i], device, out, outCreated);
		for (size_t i = 0; i < set.folders.size(); ++i)
			BuildFolder(fst, set.folders[i], device, lister, out, outCreated);

		gprintf("Riivo file: built %u redirect(s)%s\n", (unsigned) out.size(),
				outCreated ? "" : " (create= files ignored)");
	}
}
