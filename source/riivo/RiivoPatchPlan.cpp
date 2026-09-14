/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Patch-plan builder: compose ResolvedFile/Folder patches against the FST
 * into PlannedFiles with Dolphin-compatible segment semantics.
 *
 * Composition mirrors Dolphin ApplyPatchToFile (split/discard/insert/pad,
 * resize retain/truncate, offset & ~3, length 0 => remainder, successive
 * application for multiple mods to one file). File routing mirrors
 * ApplyFilePatchToFST ('/' => specific path; bare "main.dol" => boot-file;
 * otherwise first basename match). Folders recurse; empty folder disc =>
 * bare filenames => basename matching (dataless, e.g. Newer).
 *
 * No console calls; filesystem via injected DirLister + FileSizeProvider.
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>
#include <ctype.h>

#include "RiivoPatchPlan.hpp"
#include "RiivoConfig.hpp" // JoinPath

namespace Riivo
{

bool StatFileSizeProvider::GetSize(const std::string &external, u32 *outSize)
{
	struct stat st;
	if (stat(external.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	if (outSize)
		*outSize = (u32)st.st_size;
	return true;
}

//! Lower-case, leading '/', collapse empty components (same key as
//! FstBuilder::LayoutFrom expects via NormaliseDiscPath).
static std::string PlanKey(const std::string &disc)
{
	return NormaliseDiscPath(disc);
}

static std::string ToLowerStr(const std::string &s)
{
	std::string o = s;
	for (size_t i = 0; i < o.size(); ++i)
		o[i] = (char)tolower((unsigned char)o[i]);
	return o;
}

//! Working composition state per disc path, in application order.
struct ComposeState
{
	std::string disc; // plan key (post-FST routing)
	std::string earlyKey; // pre-FST enumeration key (rule-derived),
	                      // retained as metadata alongside the plan key
	u64 origOffset;
	u32 origLength;
	bool existed;
	bool create;
	bool resize;
	std::vector<PlanSegment> segs; // covering [0,curSize)
	u32 curSize;
	std::string lastExternal;
	bool touched;

	ComposeState()
		: origOffset(0), origLength(0), existed(false),
		  create(false), resize(true), curSize(0), touched(false) {}
};

static void InitFromDisc(ComposeState &st, const FstFile *entry)
{
	if (entry)
	{
		st.existed = true;
		st.origOffset = entry->offset;
		st.origLength = entry->length;
		st.curSize = entry->length;
		st.segs.clear();
		if (entry->length)
		{
			PlanSegment s;
			s.kind = PlanSegment::SEG_ORIGINAL;
			s.fileOffset = 0;
			s.length = entry->length;
			st.segs.push_back(s);
		}
	}
	else
	{
		st.existed = false;
		st.origOffset = 0;
		st.origLength = 0;
		st.curSize = 0;
		st.segs.clear();
	}
}

//! Split segs at file offset `at` (0 < at < curSize assumed by caller check).
//! External srcOffsets advance; Original/Zero just shorten.
static void SplitAt(std::vector<PlanSegment> &segs, u64 at)
{
	for (size_t i = 0; i < segs.size(); ++i)
	{
		u64 s = segs[i].fileOffset;
		u64 e = s + segs[i].length;
		if (at > s && at < e)
		{
			PlanSegment after = segs[i];
			u64 firstLen = at - s;
			segs[i].length = (u32)firstLen;
			after.fileOffset = at;
			after.length = (u32)(e - at);
			if (after.kind == PlanSegment::SEG_EXTERNAL)
				after.srcOffset += firstLen;
			segs.insert(segs.begin() + i + 1, after);
			return;
		}
	}
}

//! Apply one (offset,fileoffset,length,resize,external,size) patch to state.
//! Mirrors Dolphin ApplyPatchToFile. Returns false + why on arithmetic
//! refusal (overflow); missing-external skips are handled by the caller.
static bool ApplyOne(ComposeState &st,
					 u32 rawOffset, u32 rawFileOffset, u32 rawLength,
					 bool resize, const std::string &external, u32 extSize,
					 std::string &why)
{
	// Actual Riivolution ignores the low 2 bits (Dolphin).
	u64 patchStart = (u64)(rawOffset & ~3u);
	u64 extOff = rawFileOffset;
	if (extOff > extSize)
		extOff = extSize;
	u64 extAvail = (u64)extSize - extOff;
	u64 patchSize = rawLength == 0 ? extAvail : (u64)rawLength;
	// Checked: patchEnd must fit u64 (always) and final u32 (checked below).
	u64 patchEnd = patchStart + patchSize;
	if (patchEnd < patchStart)
	{
		why = "patch range overflow";
		return false;
	}
	u64 target;
	if (resize)
		target = patchEnd;
	else
		target = st.curSize > patchEnd ? st.curSize : patchEnd;
	if (target > 0xFFFFFFFFULL)
	{
		why = "composed file exceeds 4 GiB";
		return false;
	}

	if (patchStart >= st.curSize)
	{
		// At/past end: extend. Pad gap with zeros when start > size.
		if (patchStart > st.curSize)
		{
			u64 pad = patchStart - st.curSize;
			if (pad > 0xFFFFFFFFULL || st.curSize + pad > 0xFFFFFFFFULL)
			{
				why = "extension padding exceeds 4 GiB";
				return false;
			}
			PlanSegment z;
			z.kind = PlanSegment::SEG_ZERO;
			z.fileOffset = st.curSize;
			z.length = (u32)pad;
			st.segs.push_back(z);
			st.curSize = (u32)patchStart;
		}
	}
	else
	{
		// Split at boundaries, discard overlapping.
		if (patchStart > 0)
			SplitAt(st.segs, patchStart);
		if (patchEnd > patchStart && patchEnd < st.curSize)
			SplitAt(st.segs, patchEnd);
		// Erase [patchStart, patchEnd).
		size_t at = 0;
		bool found = false;
		for (size_t i = 0; i < st.segs.size(); ++i)
		{
			if (st.segs[i].fileOffset == patchStart)
			{
				at = i;
				found = true;
				break;
			}
		}
		if (found)
		{
			size_t e = at;
			while (e < st.segs.size()
				&& st.segs[e].fileOffset + st.segs[e].length <= patchEnd)
				++e;
			// Partial tail overlapping patchEnd was split above, so any
			// segment starting before patchEnd but ending after it would
			// have been split; remaining overlap is exact.
			st.segs.erase(st.segs.begin() + at, st.segs.begin() + e);
		}
		else if (patchSize > 0)
		{
			// No exact boundary (e.g. empty file): insert position is where
			// patchStart would sort.
			at = 0;
			while (at < st.segs.size() && st.segs[at].fileOffset < patchStart)
				++at;
		}
		// Insert external + zero pad at `at` below.
		if (patchSize > 0)
		{
			size_t ins = at;
			// Re-find insert position after erase (erase may shift).
			if (found)
			{
				ins = at;
			}
			else
			{
				ins = 0;
				while (ins < st.segs.size() && st.segs[ins].fileOffset < patchStart)
					++ins;
			}
			u64 useExt = patchSize < extAvail ? patchSize : extAvail;
			if (useExt > 0)
			{
				PlanSegment s;
				s.kind = PlanSegment::SEG_EXTERNAL;
				s.fileOffset = patchStart;
				s.length = (u32)useExt;
				s.srcOffset = extOff;
				s.external = external;
				st.segs.insert(st.segs.begin() + ins, s);
				++ins;
			}
			if (extAvail < patchSize)
			{
				PlanSegment z;
				z.kind = PlanSegment::SEG_ZERO;
				z.fileOffset = patchStart + useExt;
				z.length = (u32)(patchSize - useExt);
				st.segs.insert(st.segs.begin() + ins, z);
			}
			st.curSize = (u32)target;
			// Renumber fileOffsets after insert (splits/erases/inserts keep
			// order but offsets after insertion point shift only when sizes
			// change; recompute densely to stay exact).
			u64 off = 0;
			// Segments before `patchStart` keep offsets; recompute all for
			// simplicity (they were contiguous by invariant).
			for (size_t i = 0; i < st.segs.size(); ++i)
			{
				st.segs[i].fileOffset = off;
				off += st.segs[i].length;
			}
			// Drop trailing sources past the new end (truncation).
			while (!st.segs.empty()
				&& st.segs.back().fileOffset >= target)
				st.segs.pop_back();
			if (!st.segs.empty())
			{
				PlanSegment &last = st.segs.back();
				u64 lend = last.fileOffset + last.length;
				if (lend > target)
					last.length = (u32)(target - last.fileOffset);
			}
			st.curSize = (u32)target;
			st.resize = resize;
			st.lastExternal = external;
			st.touched = true;
			return true;
		}
		st.curSize = (u32)target;
		st.resize = resize;
		st.touched = true;
		return true;
	}

	// Past-end extension: append external + zero pad.
	{
		u64 useExt = patchSize < extAvail ? patchSize : extAvail;
		if (useExt > 0)
		{
			PlanSegment s;
			s.kind = PlanSegment::SEG_EXTERNAL;
			s.fileOffset = patchStart;
			s.length = (u32)useExt;
			s.srcOffset = extOff;
			s.external = external;
			st.segs.push_back(s);
		}
		if (extAvail < patchSize)
		{
			PlanSegment z;
			z.kind = PlanSegment::SEG_ZERO;
			z.fileOffset = patchStart + useExt;
			z.length = (u32)(patchSize - useExt);
			st.segs.push_back(z);
		}
		st.curSize = (u32)target;
		// Truncate trailing beyond target (only when resize shrinks, which
		// cannot happen here since target >= patchEnd >= curSize old).
		st.resize = resize;
		st.lastExternal = external;
		st.touched = true;
		return true;
	}
}

//! DOL image span from a parsed section table: max(fileOff+size), at
//! least 0x100 (the header every apploader read implies). Pure.
u64 DolImageSize(const u32 *fileOffs, const u32 *sizes, u32 count)
{
	u64 hi = 0x100;
	if (!fileOffs || !sizes)
		return hi;
	for (u32 i = 0; i < count; ++i)
	{
		u64 end = (u64)fileOffs[i] + sizes[i];
		if (end > hi)
			hi = end;
	}
	return hi;
}

//! Compose one executable patch entry into the DOL state (base ORIGINAL
//! [0,dolSize) backed by the DOL image on disc). The external size must
//! already be statted. First use initializes the base; dolSize == 0 is a
//! refusal (image extent unknown). Resize is FORCED size-preserving:
//! apploader responses follow the stock image layout, so a grown or shrunken
//! served image would desynchronize destinations; growth past the image end
//! fails the size rule at materialization. Records errors, never throws.
static bool ComposeDolEntry(ComposeState &dolSt, bool &dolHave, u64 dolSize,
							u32 rawOffset, u32 rawFileOffset, u32 rawLength,
							bool resize, const std::string &external, u32 extSize,
							const char *what, std::vector<std::string> &errors)
{
	(void)resize; // entry flag ignored: executables preserve image size
	if (!dolHave)
	{
		if (dolSize == 0 || dolSize > 0xFFFFFFFFULL)
		{
			errors.push_back("executable patch refused: DOL image extent unknown");
			return false;
		}
		dolSt.disc = "/main.dol";
		dolSt.existed = true;
		dolSt.origOffset = 0;
		dolSt.origLength = (u32)dolSize;
		dolSt.curSize = (u32)dolSize;
		dolSt.create = false;
		dolSt.resize = true;
		dolSt.segs.clear();
		PlanSegment base;
		base.kind = PlanSegment::SEG_ORIGINAL;
		base.fileOffset = 0;
		base.length = (u32)dolSize;
		dolSt.segs.push_back(base);
		dolHave = true;
	}
	std::string awhy;
	if (!ApplyOne(dolSt, rawOffset, rawFileOffset, rawLength,
				  false, external, extSize, awhy))
	{
		char b[300];
		snprintf(b, sizeof(b), "executable patch '%s' refused: %s",
			what ? what : "main.dol", awhy.c_str());
		errors.push_back(b);
		return false;
	}
	return true;
}

bool BuildPatchPlan(const Fst &fst,
					const ResolvedPatchSet &set,
					const std::string &device,
					DirLister *lister,
					FileSizeProvider *sizes,
					PatchPlan &out,
					std::string &why,
					u64 dolSize,
					PlannedFile *outDol)
{
	why.clear();
	out = PatchPlan();
	if (!sizes)
	{
		why = "no file-size provider";
		return false;
	}

	// Index compose states by plan key, preserving first-seen order for
	// deterministic application (files in order, then folders).
	std::map<std::string, size_t> idx;
	std::vector<ComposeState> states;
	std::vector<std::string> order;

	std::vector<MissingExternal> missing;
	std::vector<std::string> warnings;
	std::vector<std::string> errors;
	bool hasBoot = false;
	bool hasPartial = false;

	// Executable composition state (main.dol). Base is the ORIGINAL DOL
	// image [0,dolSize) backed by disc; apploader requests stay at stock
	// offsets and are served positionally, so the served image must preserve
	// the stock image size exactly (checked at materialization).
	ComposeState dolSt;
	bool dolHave = false;

	// Helper: ensure state for disc key (create vs existing resolved here).
	// Returns 0 on skip (warning recorded), 1 on ready, -1 on boot-file.
	int ensureState = 0;
	(void)ensureState;

	// Process <file> patches in order.
	for (size_t i = 0; i < set.files.size(); ++i)
	{
		const ResolvedFile &f = set.files[i];
		if (f.disc.empty())
		{
			warnings.push_back("empty <file> disc path skipped");
			continue;
		}
		if (IsBootFileDisc(f.disc))
		{
			hasBoot = true;
			if (!outDol)
			{
				char b[256];
				snprintf(b, sizeof(b),
					"boot-file patch '%s' requires the patched boot view (unsupported)",
					f.disc.c_str());
				errors.push_back(b);
				continue;
			}
			warnings.push_back("executable patch routed to the boot view: " + f.disc);
			const std::string external = JoinPath(device, f.root, f.external);
			if (external.find('\\') != std::string::npos)
			{
				warnings.push_back("executable external with backslash skipped: " + external);
				continue;
			}
			u32 extSize = 0;
			if (!sizes->GetSize(external, &extSize))
			{
				MissingExternal m;
				m.disc = "/main.dol";
				m.external = external;
				missing.push_back(m);
				continue; // leave executable original (file-missing rule)
			}
			ComposeDolEntry(dolSt, dolHave, dolSize,
							f.offset, f.fileoffset, f.length,
							f.resize, external, extSize,
							f.disc.c_str(), errors);
			continue;
		}
		const std::string external = JoinPath(device, f.root, f.external);
		// Backslash is a filename char for Riivolution; Windows hosts
		// cannot represent it — refuse the file, never mis-resolve.
		if (external.find('\\') != std::string::npos)
		{
			warnings.push_back("external path with backslash skipped: " + external);
			continue;
		}
		u32 extSize = 0;
		if (!sizes->GetSize(external, &extSize))
		{
			MissingExternal m;
			m.disc = NormaliseDiscPath(f.disc);
			m.external = external;
			missing.push_back(m);
			continue; // leave original (Dolphin: no patch applied)
		}
		// Route disc: '/' => specific; otherwise basename first-match.
		const FstFile *entry = 0;
		std::string key;
		if (!f.disc.empty() && f.disc[0] == '/')
		{
			key = PlanKey(f.disc);
			entry = fst.FindFile(f.disc);
		}
		else
		{
			entry = fst.FindFile(f.disc); // bare => basename search
			if (entry)
				key = PlanKey(entry->path);
			else
				key = PlanKey(std::string("/") + f.disc);
		}
		if (!entry && !f.create)
			continue; // no disc entry, no create => no patch (Dolphin)
		// Pre-FST enumeration key for this rule (what ListModFiles filed it
		// under before any FST existed): the late layout meets early offsets
		// through this identity, so basename routing still finds placement.
		const std::string earlyKey = PlanKey(f.disc);
		std::map<std::string, size_t>::iterator it = idx.find(key);
		size_t si;
		if (it == idx.end())
		{
			ComposeState st;
			st.disc = key;
			st.earlyKey = earlyKey;
			st.create = f.create;
			st.resize = f.resize;
			InitFromDisc(st, entry);
			si = states.size();
			idx[key] = si;
			states.push_back(st);
			order.push_back(key);
		}
		else
			si = it->second;
		ComposeState &st = states[si];
		st.create = st.create || f.create;
		std::string awhy;
		if (!ApplyOne(st, f.offset, f.fileoffset, f.length,
					  f.resize, external, extSize, awhy))
		{
			char b[300];
			snprintf(b, sizeof(b), "file '%s' refused: %s",
				key.c_str(), awhy.c_str());
			errors.push_back(b);
			continue;
		}
		// Partial when not a single whole-file external (zero-length files
		// compose to no segments and take no fragments: whole by vacuity).
		if (!((st.segs.size() == 1
			  && st.segs[0].kind == PlanSegment::SEG_EXTERNAL
			  && st.segs[0].fileOffset == 0
			  && st.segs[0].srcOffset == 0
			  && st.segs[0].length == st.curSize
			  && st.curSize == extSize
			  && (f.offset & ~3u) == 0 && f.fileoffset == 0
			  && (f.length == 0 || f.length == extSize))
			 || (st.curSize == 0 && st.segs.empty())))
			hasPartial = true;
	}

	// Process <folder> patches: enumerate externals, each child becomes a
	// file patch (resize/create/length from the folder; fileoffset 0).
	if (lister)
	{
		for (size_t i = 0; i < set.folders.size(); ++i)
		{
			const ResolvedFolder &fl = set.folders[i];
			const std::string extDir = JoinPath(device, fl.root, fl.external);
			if (extDir.find('\\') != std::string::npos)
			{
				warnings.push_back("folder external with backslash skipped: " + extDir);
				continue;
			}
			std::vector<std::string> rel;
			lister->List(extDir, fl.recursive, rel);
			const bool dataless = fl.disc.empty();
			// Shared helper with the early enumeration (ListModFiles): the
			// pre-FST key below is byte-identical to what the early phase
			// filed, by construction rather than by parallel formulas.
			const std::string discDir = DiscDirPath(fl.disc);
			for (size_t j = 0; j < rel.size(); ++j)
			{
				// Host/shim listings use '/' separators; normalize.
				std::string r = rel[j];
				// Nested dataless children match nothing (reference parity
				// with the early phase: a slashed name is never a bare
				// filename). Top-level executable names route to the DOL.
				if (dataless && r.find('/') != std::string::npos)
					continue;
				const std::string childExternal = JoinDiscPath(extDir, r);
				if (childExternal.find('\\') != std::string::npos)
					continue;
				u32 extSize = 0;
				if (!sizes->GetSize(childExternal, &extSize))
				{
					MissingExternal m;
					m.external = childExternal;
					m.disc = dataless ? ToLowerStr(std::string("/") + BaseFileName(r))
						: PlanKey(JoinDiscPath(discDir, r));
					missing.push_back(m);
					continue;
				}
				// Dataless folders name bare files; a child naming the
				// executable routes to the DOL plan (Dolphin parity: folder
				// children become file patches routed by disc name).
				if (dataless && outDol && IsBootFileDisc(BaseFileName(r)))
				{
					hasBoot = true;
					warnings.push_back("executable patch routed to the boot view: " + r);
					ComposeDolEntry(dolSt, dolHave, dolSize,
									0, 0, fl.length, fl.resize,
									childExternal, extSize,
									r.c_str(), errors);
					continue;
				}
				const FstFile *entry = 0;
				std::string key;
				if (dataless)
				{
					// Basename match (Newer): first disc file with this name.
					entry = fst.FindFile(BaseFileName(r));
					if (entry)
						key = PlanKey(entry->path);
					else
						key = PlanKey(std::string("/") + r);
				}
				else
				{
					std::string discFile = JoinDiscPath(discDir, r);
					key = PlanKey(discFile);
					entry = fst.FindFile(discFile);
				}
				if (!entry && !fl.create)
					continue;
			// Pre-FST enumeration key (rule-derived disc identity, kept as
			// composer metadata alongside the plan disc key).
			const std::string earlyKey = PlanKey(JoinDiscPath(discDir, r));
				std::map<std::string, size_t>::iterator it = idx.find(key);
				size_t si;
				if (it == idx.end())
				{
					ComposeState st;
					st.disc = key;
					st.earlyKey = earlyKey;
					st.create = fl.create;
					st.resize = fl.resize;
					InitFromDisc(st, entry);
					si = states.size();
					idx[key] = si;
					states.push_back(st);
					order.push_back(key);
				}
				else
					si = it->second;
				ComposeState &st = states[si];
				st.create = st.create || fl.create;
				std::string awhy;
				// Folder children: whole-file replace w/ folder flags.
				// offset 0, fileoffset 0, length = folder.length (0 => all).
				if (!ApplyOne(st, 0, 0, fl.length, fl.resize,
							  childExternal, extSize, awhy))
				{
					char b[300];
					snprintf(b, sizeof(b), "folder child '%s' refused: %s",
						key.c_str(), awhy.c_str());
					errors.push_back(b);
					continue;
				}
			}
		}
	}

	// Materialize plan in stable disc order.
	std::sort(order.begin(), order.end());
	out.files.reserve(order.size());
	u64 total = 0;
	u32 whole = 0;
	for (size_t oi = 0; oi < order.size(); ++oi)
	{
		std::map<std::string, size_t>::iterator it = idx.find(order[oi]);
		if (it == idx.end())
			continue;
		ComposeState &st = states[it->second];
		if (!st.touched)
			continue; // enumerated but never patched (all externals missing)
		PlannedFile pf;
		pf.disc = st.disc;
		pf.earlyKey = st.earlyKey;
		pf.discOffsetOrig = st.origOffset;
		pf.discLengthOrig = st.origLength;
		pf.finalSize = st.curSize;
		pf.isNew = !st.existed;
		pf.resize = st.resize;
		pf.create = st.create;
		pf.segs = st.segs;
		pf.bootFile = false;
		// Whole-file when single external covering all from 0.
		if (st.segs.size() == 1
			&& st.segs[0].kind == PlanSegment::SEG_EXTERNAL
			&& st.segs[0].fileOffset == 0
			&& st.segs[0].srcOffset == 0
			&& st.segs[0].length == st.curSize)
		{
			pf.wholeFile = true;
			pf.external = st.segs[0].external;
			++whole;
		}
		else if (st.curSize == 0 && st.segs.empty())
		{
			// Zero-length file: valid, no fragments.
			pf.wholeFile = true;
			pf.external.clear();
		}
		else
		{
			pf.wholeFile = false;
		}
		if (total + pf.finalSize < total)
		{
			why = "total payload overflow";
			return false;
		}
		total += pf.finalSize;
		out.files.push_back(pf);
	}
	out.missingExternals = missing;
	out.warnings = warnings;
	out.errors = errors;
	out.hasBootFile = hasBoot;
	// hasPartial when any NON-DOL file is not whole-file: the fragment
	// runtime serves whole files only. DOL partials are served positionally
	// by the boot view and never set this.
	out.hasPartial = hasPartial;
	for (size_t i = 0; i < out.files.size(); ++i)
		if (!out.files[i].wholeFile)
			out.hasPartial = true;
	out.totalFinalBytes = total;
	out.wholeFileCount = whole;

	// Materialize the executable plan. The served image must preserve the
	// stock image size exactly: apploader responses follow the stock layout,
	// so a grown or shrunken image would desynchronize destinations.
	if (dolHave && outDol)
	{
		if (dolSt.curSize != (u32)dolSize)
		{
			char b[256];
			snprintf(b, sizeof(b),
				"executable patch changes image size (%u, need %llu); "
				"the served image must preserve the stock layout",
				dolSt.curSize, (unsigned long long)dolSize);
			errors.push_back(b);
			out.errors = errors;
			return true; // errors refuse file work; not a planning failure
		}
		PlannedFile pf;
		pf.disc = "/main.dol";
		pf.discOffsetOrig = 0;
		pf.discLengthOrig = (u32)dolSize;
		pf.finalSize = (u32)dolSize;
		pf.isNew = false;
		pf.resize = dolSt.resize;
		pf.create = false;
		pf.segs = dolSt.segs;
		pf.bootFile = true;
		pf.dolBase = 0; // production assigns the partition DOL offset
		if (dolSt.segs.size() == 1
			&& dolSt.segs[0].kind == PlanSegment::SEG_EXTERNAL
			&& dolSt.segs[0].fileOffset == 0
			&& dolSt.segs[0].srcOffset == 0
			&& dolSt.segs[0].length == dolSt.curSize)
		{
			pf.wholeFile = true;
			pf.external = dolSt.segs[0].external;
		}
		else
		{
			pf.wholeFile = false;
		}
		*outDol = pf;
	}
	return true;
}

bool PlanToManifestRuns(const PlannedFile &file,
						std::vector<ManifestExtent> &out,
						std::string &why)
{
	why.clear();
	out.clear();
	for (size_t i = 0; i < file.segs.size(); ++i)
	{
		const PlanSegment &s = file.segs[i];
		if (s.kind == PlanSegment::SEG_ORIGINAL)
			continue; // gaps delegated, not listed
		ManifestExtent e;
		e.discOffset = 0; // caller adds the file's assigned base
		e.length = s.length;
		if (s.kind == PlanSegment::SEG_EXTERNAL)
		{
			e.kind = RIIVO_EXT_EXTERNAL;
			e.source = RIIVO_SRC_NONE; // caller classifies device
			e.srcOffset = s.srcOffset;
			e.path = s.external;
		}
		else
		{
			e.kind = RIIVO_EXT_ZERO;
			e.source = RIIVO_SRC_NONE;
			e.srcOffset = 0;
		}
		e.genOff = 0;
		out.push_back(e);
	}
	return true;
}

bool BuildPlanManifest(const PatchPlan &plan,
					   const std::vector<PlacedFile> &placed,
					   u32 discId,
					   std::vector<u8> &blob,
					   std::string &why)
{
	why.clear();
	blob.clear();
	// Index plan entries by external path once (bounded n log n).
	std::map<std::string, std::vector<size_t> > byExternal;
	for (size_t j = 0; j < plan.files.size(); ++j)
	{
		const PlannedFile &f = plan.files[j];
		if (!f.bootFile && f.wholeFile && !f.external.empty())
			byExternal[f.external].push_back(j);
	}
	std::vector<ManifestExtent> exts;
	exts.reserve(placed.size());
	for (size_t i = 0; i < placed.size(); ++i)
	{
		const PlacedFile &p = placed[i];
		const PlannedFile *match = 0;
		std::map<std::string, std::vector<size_t> >::const_iterator it =
			byExternal.find(p.external);
		if (it != byExternal.end())
		{
			for (size_t k = 0; k < it->second.size(); ++k)
			{
				const PlannedFile &f = plan.files[it->second[k]];
				if (f.finalSize == p.length)
				{
					match = &f;
					break;
				}
			}
		}
		if (!match)
		{
			char b[256];
			snprintf(b, sizeof(b),
				"placed file with no plan entry: %s (%u bytes)",
				p.external.c_str(), p.length);
			why = b;
			return false;
		}
		ManifestExtent e;
		e.discOffset = p.offset;
		e.length = p.length;
		e.kind = RIIVO_EXT_EXTERNAL;
		if (!ManifestSourceFor(p.external, e.source))
		{
			char b[256];
			snprintf(b, sizeof(b), "unknown device in %s",
				p.external.c_str());
			why = b;
			return false;
		}
		e.srcOffset = 0;
		e.path = ManifestPathFor(p.external);
		e.genOff = 0;
		exts.push_back(e);
	}
	if (!BuildManifestV1(exts, RIIVO_MANIFEST_DISCOVER, discId, 0,
						 RIIVO_CAP_SPLIT_READ, RIIVO_PROV_BOUNDED,
						 blob, why))
		return false;
	if (blob.size() < RIIVO_MANIFEST_HEADER
		|| !ValidateManifestV1(&blob[0], (u32)blob.size(), why))
	{
		if (why.empty())
			why = "self-validation refused";
		return false;
	}
	return true;
}

//! Visit order for gap detection: ascending disc offset. Equal offsets
//! keep plan order (stable_sort) so genuine overlaps still refuse
//! downstream exactly as before.
static bool ManifestExtentLess(const ManifestExtent &a,
							   const ManifestExtent &b)
{
	return a.discOffset < b.discOffset;
}

bool BuildSegmentManifest(const PatchPlan &plan,
						  const std::map<std::string, u64> &bases,
						  FileSizeProvider *sizes,
						  u32 discId, u32 partIdx,
						  std::vector<u8> &blob, GenLayout &gen,
						  std::string &why,
						  const std::string &scratchExternal)
{
	why.clear();
	blob.clear();
	gen = GenLayout();
	if (!sizes)
	{
		why = "segment manifest: no size provider (externals unverifiable)";
		return false;
	}
	//! Scratch mode resolves once: every ORIGINAL run in this manifest
	//! names the same file, so one mapping proves the whole mode.
	u16 scratchSource = 0;
	std::string scratchPath;
	if (!scratchExternal.empty())
	{
		if (!ManifestSourceFor(scratchExternal, scratchSource)
			|| (scratchPath = ManifestPathFor(scratchExternal),
				scratchPath.empty() || scratchPath[0] != '/'))
		{
			why = "segment manifest: scratch file unresolvable: "
				+ scratchExternal;
			return false;
		}
	}
	std::vector<ManifestExtent> exts;
	u32 caps = RIIVO_CAP_SPLIT_READ;
	u64 genTotal = 0;
	for (size_t i = 0; i < plan.files.size(); ++i)
	{
		const PlannedFile &f = plan.files[i];
		if (f.bootFile || f.finalSize == 0)
			continue; // executable serves via boot view; empty files need no bytes
		std::map<std::string, u64>::const_iterator bi = bases.find(f.disc);
		if (bi == bases.end())
		{
			char b[256];
			snprintf(b, sizeof(b), "segment manifest: no slot for %s",
				f.disc.c_str());
			why = b;
			return false;
		}
		const u64 base = bi->second;
		if (f.wholeFile)
		{
			u32 extSize = 0;
			if (!sizes->GetSize(f.external, &extSize) || extSize != f.finalSize)
			{
				char b[300];
				snprintf(b, sizeof(b), "segment manifest: %s changed size "
					"(plan %u)", f.disc.c_str(), f.finalSize);
				why = b;
				return false;
			}
			ManifestExtent e;
			e.discOffset = base;
			e.length = f.finalSize;
			e.kind = RIIVO_EXT_EXTERNAL;
			if (!ManifestSourceFor(f.external, e.source))
			{
				char b[256];
				snprintf(b, sizeof(b), "unknown device in %s",
					f.external.c_str());
				why = b;
				return false;
			}
			e.srcOffset = 0;
			e.path = ManifestPathFor(f.external);
			e.genOff = 0;
			exts.push_back(e);
			continue;
		}
		// Partial file: per-segment runs at base + fileOffset. ORIGINAL
		// runs name staged slices (bytes filled late from the disc).
		for (size_t s = 0; s < f.segs.size(); ++s)
		{
			const PlanSegment &seg = f.segs[s];
			if (seg.length == 0)
				continue;
			const u64 at = base + seg.fileOffset;
			if (at < base || at + seg.length < at)
			{
				why = "segment manifest: file range overflows";
				return false;
			}
			if (seg.kind == PlanSegment::SEG_ORIGINAL)
			{
				if (f.isNew)
				{
					char b[256];
					snprintf(b, sizeof(b), "segment manifest: %s keeps "
						"original bytes it never had", f.disc.c_str());
					why = b;
					return false;
				}
				const u64 origAbs = f.discOffsetOrig + seg.fileOffset;
				if (origAbs < f.discOffsetOrig
					|| origAbs + seg.length < origAbs)
				{
					why = "segment manifest: original range overflows";
					return false;
				}
				if (genTotal + seg.length < genTotal
					|| genTotal + seg.length > RIV1_GEN_MAX)
				{
					char b[256];
					snprintf(b, sizeof(b), "segment manifest: staged slices "
						"exceed %u bytes", (unsigned) RIV1_GEN_MAX);
					why = b;
					return false;
				}
				GenSlice slice;
				slice.disc = f.disc;
				slice.fileOffset = seg.fileOffset;
				slice.length = seg.length;
				slice.origAbs = origAbs;
				slice.genOff = (u32) genTotal;
				gen.slices.push_back(slice);
				genTotal += seg.length;
				ManifestExtent e;
				e.discOffset = at;
				e.length = seg.length;
				if (!scratchExternal.empty())
				{
					//! Filed run: the slice bytes land in the scratch
					//! file at the same genOff the layout records, so
					//! the runtime serves original content through the
					//! ordinary file path - no reserved store involved.
					e.kind = RIIVO_EXT_EXTERNAL;
					e.source = scratchSource;
					e.srcOffset = slice.genOff;
					e.path = scratchPath;
					e.genOff = 0;
					exts.push_back(e);
					continue;
				}
				e.kind = RIIVO_EXT_GENERATED;
				e.source = RIIVO_SRC_NONE;
				e.srcOffset = 0;
				e.genOff = slice.genOff;
				exts.push_back(e);
				caps |= RIIVO_CAP_GENERATED;
				continue;
			}
			if (seg.kind == PlanSegment::SEG_ZERO)
			{
				ManifestExtent e;
				e.discOffset = at;
				e.length = seg.length;
				e.kind = RIIVO_EXT_ZERO;
				e.source = RIIVO_SRC_NONE;
				e.srcOffset = 0;
				e.genOff = 0;
				exts.push_back(e);
				caps |= RIIVO_CAP_ZERO_FILL;
				continue;
			}
			u32 extSize = 0;
			if (!sizes->GetSize(seg.external, &extSize)
				|| (u64) seg.srcOffset + seg.length > extSize)
			{
				char b[300];
				snprintf(b, sizeof(b), "segment manifest: %s of %s "
					"unavailable", seg.external.c_str(), f.disc.c_str());
				why = b;
				return false;
			}
			ManifestExtent e;
			e.discOffset = at;
			e.length = seg.length;
			e.kind = RIIVO_EXT_EXTERNAL;
			if (!ManifestSourceFor(seg.external, e.source))
			{
				char b[256];
				snprintf(b, sizeof(b), "unknown device in %s",
					seg.external.c_str());
				why = b;
				return false;
			}
			e.srcOffset = seg.srcOffset;
			e.path = ManifestPathFor(seg.external);
			e.genOff = 0;
			exts.push_back(e);
		}
	}
	//! Interior gaps are alignment slop between packed slots (AssignModOffsets
	//! walks one cursor: every file advances past its size, so consecutive
	//! files abut except for alignment). No original bytes exist for gap
	//! addresses anywhere - true ORIGINAL runs already became GENERATED
	//! slices above - so gaps are named explicitly as ZERO runs. Zeros cost
	//! no storage and no fill time, which is what makes this unconditional:
	//! the emitted span then has no unlisted bytes, and a request spanning
	//! replacement tail -> gap -> replacement head composes exactly instead
	//! of delegating away its replacements. (Sorted first, stable: the
	//! builder refuses misordered extents, and gap detection needs visit
	//! order. A no-op when emission is already ordered; previously-refused
	//! misordered plans now encode, which is the safe direction.)
	std::stable_sort(exts.begin(), exts.end(), ManifestExtentLess);
	{
		std::vector<ManifestExtent> filled;
		filled.reserve(exts.size());
		u64 prevEnd = 0;
		bool havePrev = false;
		for (size_t i = 0; i < exts.size(); ++i)
		{
			const ManifestExtent &e = exts[i];
			u64 eEnd = e.discOffset + e.length;
			if (havePrev && e.discOffset > prevEnd)
			{
				u64 gapLen = e.discOffset - prevEnd;
				if (gapLen > 0xFFFFFFFFULL)
				{
					char b[300];
					snprintf(b, sizeof(b), "segment manifest: unfillable "
						"gap of %llu bytes at %llu", gapLen, prevEnd);
					why = b;
					return false;
				}
				ManifestExtent z;
				z.discOffset = prevEnd;
				z.length = (u32) gapLen;
				z.kind = RIIVO_EXT_ZERO;
				z.source = RIIVO_SRC_NONE;
				z.srcOffset = 0;
				z.genOff = 0;
				filled.push_back(z);
				caps |= RIIVO_CAP_ZERO_FILL;
			}
			if (!havePrev || eEnd > prevEnd)
			{
				prevEnd = eEnd;
				havePrev = true;
			}
			filled.push_back(e);
		}
		exts.swap(filled);
	}
	if (genTotal > 0xFFFFFFFFULL)
	{
		why = "segment manifest: staged slices exceed address range";
		return false;
	}
	gen.total = (u32) genTotal;
	if (!BuildManifestV1(exts, RIIVO_MANIFEST_DISCOVER, discId, partIdx,
						 caps, RIIVO_PROV_BOUNDED, blob, why))
		return false;
	if (blob.size() < RIIVO_MANIFEST_HEADER
		|| !ValidateManifestV1(&blob[0], (u32)blob.size(), why))
	{
		if (why.empty())
			why = "self-validation refused";
		return false;
	}
	return true;
}

} // namespace Riivo
