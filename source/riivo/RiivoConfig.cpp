/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RiivoConfig.hpp"
#include "gecko.h"

namespace Riivo
{
	// --------------------------------------------------------------------
	// Disc helpers declared in RiivoTypes.hpp
	// --------------------------------------------------------------------

	bool Disc::IsValidForGame(const char *gameId, int discNumber, int revision) const
	{
		if (!gameId || strlen(gameId) < 6)
			return false;

		const std::string id(gameId, 6);
		const char region = gameId[3];
		const std::string developer = id.substr(4, 2);

		if (filter.hasGame && id.compare(0, filter.game.size(), filter.game) != 0)
			return false;
		if (filter.hasDeveloper && developer != filter.developer)
			return false;
		if (filter.hasDisc && discNumber != filter.disc)
			return false;
		if (filter.hasVersion && revision != filter.version)
			return false;
		if (!filter.regions.empty())
		{
			bool ok = false;
			for (size_t i = 0; i < filter.regions.size(); ++i)
				if (filter.regions[i].size() == 1 && filter.regions[i][0] == region)
				{ ok = true; break; }
			if (!ok)
				return false;
		}
		return true;
	}

	const Patch *Disc::FindPatch(const std::string &id) const
	{
		for (size_t i = 0; i < patches.size(); ++i)
			if (patches[i].id == id)
				return &patches[i];
		return 0;
	}

	// --------------------------------------------------------------------
	// Substitution (mirrors substitute() with ${name} and {$name})
	// --------------------------------------------------------------------

	std::string Substitute(const std::string &text, const ParamMap &params)
	{
		if (text.empty() || text.find('{') == std::string::npos)
			return text;

		std::string out;
		out.reserve(text.size());
		size_t i = 0;
		const size_t n = text.size();
		while (i < n)
		{
			// ${name}
			if (text[i] == '$' && i + 1 < n && text[i + 1] == '{')
			{
				size_t end = text.find('}', i + 2);
				if (end != std::string::npos)
				{
					std::string name = text.substr(i + 2, end - (i + 2));
					ParamMap::const_iterator it = params.find(name);
					if (it != params.end())
						out += it->second;
					else
						out += text.substr(i, end - i + 1); // leave verbatim
					i = end + 1;
					continue;
				}
			}
			// {$name}
			if (text[i] == '{' && i + 1 < n && text[i + 1] == '$')
			{
				size_t end = text.find('}', i + 2);
				if (end != std::string::npos)
				{
					std::string name = text.substr(i + 2, end - (i + 2));
					ParamMap::const_iterator it = params.find(name);
					if (it != params.end())
						out += it->second;
					else
						out += text.substr(i, end - i + 1);
					i = end + 1;
					continue;
				}
			}
			out += text[i++];
		}
		return out;
	}

	std::string JoinPath(const std::string &device, const std::string &root, const std::string &rel)
	{
		std::string p = device;
		const std::string parts[2] = { root, rel };
		for (int k = 0; k < 2; ++k)
		{
			const std::string &part = parts[k];
			if (part.empty())
				continue;
			const bool pslash = !p.empty() && p[p.size() - 1] == '/';
			const bool pcolon = !p.empty() && p[p.size() - 1] == ':';
			const bool cslash = part[0] == '/';
			if (pslash && cslash)
				p += part.substr(1);        // avoid "//"
			else if (!pslash && !cslash && !pcolon)
				p += "/" + part;            // insert missing "/"
			else
				p += part;                  // "sd:" + "/x", or one slash present
		}
		return p;
	}

	ParamMap BuiltinParams(const char *gameId)
	{
		ParamMap p;
		if (gameId && strlen(gameId) >= 6)
		{
			p["__gameid"] = std::string(gameId, 3);
			p["__region"] = std::string(gameId + 3, 1);
			p["__maker"] = std::string(gameId + 4, 2);
		}
		return p;
	}

	// --------------------------------------------------------------------
	// Resolution
	// --------------------------------------------------------------------

	//! Effective root for a patch: the per-patch root overrides the disc root.
	static std::string EffectiveRoot(const Disc &disc, const Patch &patch, const ParamMap &params)
	{
		const std::string &base = patch.root.empty() ? disc.root : patch.root;
		return Substitute(base, params);
	}

	static void ResolvePatch(const Disc &disc, const Patch &patch, const ParamMap &params,
							 ResolvedPatchSet &out)
	{
		const std::string root = EffectiveRoot(disc, patch, params);

		for (size_t i = 0; i < patch.files.size(); ++i)
		{
			const File &f = patch.files[i];
			ResolvedFile r;
			r.root = root;
			r.disc = Substitute(f.disc, params);
			r.external = Substitute(f.external, params);
			r.resize = f.resize;
			r.create = f.create;
			r.offset = f.offset;
			r.fileoffset = f.fileoffset;
			r.length = f.length;
			out.files.push_back(r);
		}
		for (size_t i = 0; i < patch.folders.size(); ++i)
		{
			const Folder &f = patch.folders[i];
			ResolvedFolder r;
			r.root = root;
			r.disc = Substitute(f.disc, params);
			r.external = Substitute(f.external, params);
			r.resize = f.resize;
			r.create = f.create;
			r.recursive = f.recursive;
			r.length = f.length;
			out.folders.push_back(r);
		}
		for (size_t i = 0; i < patch.memories.size(); ++i)
		{
			const Memory &m = patch.memories[i];
			ResolvedMemory r;
			r.root = root;
			r.offset = m.offset;
			r.value = m.value;
			r.valuefile = Substitute(m.valuefile, params);
			r.original = m.original;
			r.ocarina = m.ocarina;
			r.search = m.search;
			r.align = m.align;
			out.memories.push_back(r);
		}
		for (size_t i = 0; i < patch.savegames.size(); ++i)
		{
			const Savegame &s = patch.savegames[i];
			ResolvedSavegame r;
			r.root = root;
			r.external = Substitute(s.external, params);
			r.clone = s.clone;
			out.savegames.push_back(r);
		}
	}

	void Resolve(const Disc &disc, const char *gameId, ResolvedPatchSet &out)
	{
		const ParamMap builtins = BuiltinParams(gameId);

		for (size_t s = 0; s < disc.sections.size(); ++s)
		{
			const Section &section = disc.sections[s];
			for (size_t o = 0; o < section.options.size(); ++o)
			{
				const Option &option = section.options[o];
				// selectedChoice is 1-based; 0 means disabled.
				if (option.selectedChoice <= 0 || option.selectedChoice > (int)option.choices.size())
					continue;
				const Choice &choice = option.choices[option.selectedChoice - 1];
				for (size_t r = 0; r < choice.patchRefs.size(); ++r)
				{
					const PatchRef &ref = choice.patchRefs[r];
					const Patch *patch = disc.FindPatch(ref.id);
					if (!patch)
						continue;
					// Merge built-ins under the reference's params (built-ins are
					// __-prefixed so a mod can't accidentally shadow them).
					ParamMap params = ref.params;
					for (ParamMap::const_iterator it = builtins.begin(); it != builtins.end(); ++it)
						params[it->first] = it->second;
					ResolvePatch(disc, *patch, params, out);
				}
			}
		}
	}

	// --------------------------------------------------------------------
	// Selection persistence (flattened option order across all sections)
	// --------------------------------------------------------------------

	std::string SerializeSelection(const Disc &disc)
	{
		std::string out;
		int idx = 0;
		char buf[24];
		for (size_t s = 0; s < disc.sections.size(); ++s)
			for (size_t o = 0; o < disc.sections[s].options.size(); ++o, ++idx)
			{
				if (!out.empty())
					out += ",";
				snprintf(buf, sizeof(buf), "%d=%d", idx, disc.sections[s].options[o].selectedChoice);
				out += buf;
			}
		return out;
	}

	void ApplySelection(Disc &disc, const std::string &serialized)
	{
		// Build a flat pointer list in the same order SerializeSelection used.
		std::vector<Option *> flat;
		for (size_t s = 0; s < disc.sections.size(); ++s)
			for (size_t o = 0; o < disc.sections[s].options.size(); ++o)
				flat.push_back(&disc.sections[s].options[o]);

		size_t i = 0;
		const size_t n = serialized.size();
		while (i < n)
		{
			size_t comma = serialized.find(',', i);
			if (comma == std::string::npos)
				comma = n;
			std::string token = serialized.substr(i, comma - i);
			size_t eq = token.find('=');
			if (eq != std::string::npos)
			{
				int idx = atoi(token.substr(0, eq).c_str());
				int choice = atoi(token.substr(eq + 1).c_str());
				if (idx >= 0 && idx < (int)flat.size())
				{
					if (choice < 0)
						choice = 0;
					if (choice > (int)flat[idx]->choices.size())
						choice = 0;
					flat[idx]->selectedChoice = choice;
				}
			}
			i = comma + 1;
		}
	}

	// --------------------------------------------------------------------
	// Debug dumps
	// --------------------------------------------------------------------

	static void DumpBytes(const char *label, const std::vector<u8> &b)
	{
		if (b.empty())
			return;
		gprintf("      %s (%u):", label, (unsigned)b.size());
		for (size_t i = 0; i < b.size() && i < 16; ++i)
			gprintf(" %02x", b[i]);
		gprintf("%s\n", b.size() > 16 ? " ..." : "");
	}

	void DumpDisc(const Disc &disc)
	{
		gprintf("Riivo: parsed %s\n", disc.xmlPath.c_str());
		gprintf("  version=%d root=%s\n", disc.version, disc.root.c_str());
		gprintf("  filter: game=%s dev=%s disc=%s ver=%s regions=%u\n",
				disc.filter.hasGame ? disc.filter.game.c_str() : "(any)",
				disc.filter.hasDeveloper ? disc.filter.developer.c_str() : "(any)",
				disc.filter.hasDisc ? "set" : "(any)",
				disc.filter.hasVersion ? "set" : "(any)",
				(unsigned)disc.filter.regions.size());
		for (size_t s = 0; s < disc.sections.size(); ++s)
		{
			gprintf("  section '%s'\n", disc.sections[s].name.c_str());
			for (size_t o = 0; o < disc.sections[s].options.size(); ++o)
			{
				const Option &opt = disc.sections[s].options[o];
				gprintf("    option '%s' (default/sel=%d, %u choices)\n",
						opt.name.c_str(), opt.selectedChoice, (unsigned)opt.choices.size());
				for (size_t c = 0; c < opt.choices.size(); ++c)
					gprintf("      [%u] '%s' -> %u patch ref(s)\n",
							(unsigned)(c + 1), opt.choices[c].name.c_str(),
							(unsigned)opt.choices[c].patchRefs.size());
			}
		}
		gprintf("  %u <patch> definition(s)\n", (unsigned)disc.patches.size());
	}

	void DumpResolved(const ResolvedPatchSet &set)
	{
		gprintf("Riivo: resolved set - %u file, %u folder, %u memory, %u savegame\n",
				(unsigned)set.files.size(), (unsigned)set.folders.size(),
				(unsigned)set.memories.size(), (unsigned)set.savegames.size());
		for (size_t i = 0; i < set.files.size(); ++i)
			gprintf("  file: disc='%s' external='%s' (root='%s') off=%u len=%u resize=%d create=%d\n",
					set.files[i].disc.c_str(), set.files[i].external.c_str(), set.files[i].root.c_str(),
					set.files[i].offset, set.files[i].length, set.files[i].resize, set.files[i].create);
		for (size_t i = 0; i < set.folders.size(); ++i)
			gprintf("  folder: disc='%s' external='%s' (root='%s') recursive=%d create=%d\n",
					set.folders[i].disc.c_str(), set.folders[i].external.c_str(), set.folders[i].root.c_str(),
					set.folders[i].recursive, set.folders[i].create);
		for (size_t i = 0; i < set.memories.size(); ++i)
		{
			const ResolvedMemory &m = set.memories[i];
			gprintf("  memory: offset=0x%08x %s align=%u valuefile='%s'\n",
					m.offset | 0x80000000,
					m.ocarina ? "ocarina" : (m.search ? "search" : "direct"),
					m.align, m.valuefile.c_str());
			DumpBytes("value", m.value);
			DumpBytes("original", m.original);
		}
		for (size_t i = 0; i < set.savegames.size(); ++i)
			gprintf("  savegame: external='%s' (root='%s') clone=%d\n",
					set.savegames[i].external.c_str(), set.savegames[i].root.c_str(), set.savegames[i].clone);
	}

	// --------------------------------------------------------------------
	// Boot log (see the header for why this exists)
	// --------------------------------------------------------------------

	void WriteLog(const std::string &path, const char *gameId, const std::string &xmlPath,
				  const char *parseError, const Disc *disc, const ResolvedPatchSet *set,
				  int valuefileFailures)
	{
		FILE *f = fopen(path.c_str(), "w");
		if (!f)
		{
			gprintf("Riivo: could not write log to %s\n", path.c_str());
			return;
		}

		fprintf(f, "USB Loader GX - Riivolution boot log\n");
		fprintf(f, "game id : %s\n", gameId ? gameId : "(unknown)");
		fprintf(f, "xml     : %s\n", xmlPath.c_str());

		if (parseError)
		{
			fprintf(f, "\nRESULT  : XML FAILED TO PARSE\n");
			fprintf(f, "reason  : %s\n", parseError);
			fprintf(f, "\nNothing was applied to the game.\n");
			fclose(f);
			return;
		}

		if (disc)
		{
			fprintf(f, "root    : %s\n", disc->root.c_str());
			fprintf(f, "matches this game: %s\n", disc->IsValidForGame(gameId, 0, 0)
					? "yes" : "NO - this XML is meant for a different game");

			fprintf(f, "\nOptions\n-------\n");
			int shown = 0;
			for (size_t sec = 0; sec < disc->sections.size(); ++sec)
			{
				fprintf(f, "[%s]\n", disc->sections[sec].name.c_str());
				for (size_t o = 0; o < disc->sections[sec].options.size(); ++o, ++shown)
				{
					const Option &opt = disc->sections[sec].options[o];
					const int sel = opt.selectedChoice;
					const char *chosen = "Disabled";
					if (sel > 0 && sel <= (int) opt.choices.size())
						chosen = opt.choices[sel - 1].name.c_str();
					fprintf(f, "  %-40s = %s\n", opt.name.c_str(), chosen);
				}
			}
			if (shown == 0)
				fprintf(f, "  (this XML defines no options)\n");
		}

		if (set)
		{
			fprintf(f, "\nPatches enabled by that selection\n");
			fprintf(f, "---------------------------------\n");
			fprintf(f, "memory   : %u\n", (unsigned) set->memories.size());
			fprintf(f, "savegame : %u\n", (unsigned) set->savegames.size());
			fprintf(f, "file     : %u\n", (unsigned) set->files.size());
			fprintf(f, "folder   : %u\n", (unsigned) set->folders.size());
			if (!set->files.empty() || !set->folders.empty())
				fprintf(f, "  (whether these were applied is decided later - see the\n"
						   "   file and folder replacement section further down)\n");

			for (size_t i = 0; i < set->memories.size(); ++i)
			{
				const ResolvedMemory &m = set->memories[i];
				fprintf(f, "  memory 0x%08x %-7s %u byte(s)%s\n",
						m.offset | 0x80000000,
						m.ocarina ? "ocarina" : (m.search ? "search" : "direct"),
						(unsigned) m.value.size(),
						m.valuefile.empty() ? "" : "  <- VALUEFILE MISSING");
			}
			for (size_t i = 0; i < set->savegames.size(); ++i)
				fprintf(f, "  savegame -> %s (clone=%s)\n",
						set->savegames[i].external.c_str(),
						set->savegames[i].clone ? "yes" : "no");

			if (valuefileFailures > 0)
				fprintf(f, "\nWARNING: %d valuefile(s) could not be read; those patches will be\n"
						   "skipped. Check that the mod's files sit on the same device as the XML.\n",
						valuefileFailures);
			else if (set->memories.empty() && set->savegames.empty())
				fprintf(f, "\nNothing to apply. If you expected a mod here, check that an option\n"
						   "above is set to something other than Disabled.\n");
			else
				fprintf(f, "\nThe patches above were handed to the boot sequence.\n");
		}

		fclose(f);
		gprintf("Riivo: wrote boot log to %s\n", path.c_str());
	}
}
