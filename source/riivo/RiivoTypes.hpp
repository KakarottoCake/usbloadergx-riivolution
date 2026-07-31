/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Data model for a parsed Riivolution XML. Mirrors Dolphin's
 * DiscIO/RiivolutionParser.h field-for-field (and the Python port in
 * riivultimatum/riivo/parser.py) so behaviour can be compared directly against
 * the reference implementations.
 *
 * Reference: https://aerialx.github.io/rvlution.net/wiki/Patch_Format/
 ***************************************************************************/
#ifndef RIIVO_TYPES_HPP_
#define RIIVO_TYPES_HPP_

#include <gctypes.h>
#include <string>
#include <vector>
#include <map>

namespace Riivo
{
	//! A key/value bag used for ${param} substitution. Kept ordered-insensitive;
	//! std::map is fine, lookups are rare and lists are tiny.
	typedef std::map<std::string, std::string> ParamMap;

	//! <file> - replace (part of) a disc file with an external file.
	struct File
	{
		std::string disc;      // disc path or bare filename to match
		std::string external;  // replacement file (SD/USB), relative to root chain
		bool resize = true;
		bool create = false;
		u32 offset = 0;        // start position within the disc file
		u32 fileoffset = 0;    // start position within the external file
		u32 length = 0;        // 0 => external file size
	};

	//! <folder> - overlay a directory of external files onto the disc.
	struct Folder
	{
		std::string disc;      // target disc folder (may be empty => name search)
		std::string external;  // source folder (SD/USB)
		bool resize = true;
		bool create = false;
		bool recursive = true;
		u32 length = 0;
	};

	//! <memory> - direct / search / ocarina RAM patch applied before the entry point.
	struct Memory
	{
		u32 offset = 0;               // effective address is (offset | 0x80000000)
		std::vector<u8> value;        // bytes to write / pattern to find
		std::string valuefile;        // alternative to value: read bytes from file
		std::vector<u8> original;     // verification pattern (direct) / search pattern
		bool ocarina = false;
		bool search = false;
		u32 align = 1;                // search stride

		bool IsDirect() const { return !ocarina && !search; }
	};

	//! <savegame> - redirect the game's save to an external folder.
	struct Savegame
	{
		std::string external;
		bool clone = true;
	};

	//! <patch id="..."> - a named bundle of items enabled by a choice.
	struct Patch
	{
		std::string id;
		std::string root;  // per-patch root override
		std::vector<File> files;
		std::vector<Folder> folders;
		std::vector<Savegame> savegames;
		std::vector<Memory> memories;

		bool IsEmpty() const
		{
			return files.empty() && folders.empty() && savegames.empty() && memories.empty();
		}
	};

	//! A <patch id> reference from inside a <choice>, carrying the params that
	//! were in scope at the reference site (option + choice + ref params merged).
	struct PatchRef
	{
		std::string id;
		ParamMap params;
	};

	//! <choice> - one selectable variant of an option.
	struct Choice
	{
		std::string name;
		std::vector<PatchRef> patchRefs;
	};

	//! <option> - a single setting; at most one active choice.
	struct Option
	{
		std::string name;
		std::string id;
		std::vector<Choice> choices;
		//! 1-based index into choices; 0 means "disabled" (Dolphin's default).
		int selectedChoice = 0;
	};

	//! <section> - a GUI page grouping options.
	struct Section
	{
		std::string name;
		std::vector<Option> options;
	};

	//! <id> - restricts the XML to matching games. Absent fields => no constraint.
	struct GameFilter
	{
		bool hasGame = false;      std::string game;       // up to 3 chars
		bool hasDeveloper = false; std::string developer;  // 2 chars (maker)
		bool hasDisc = false;      int disc = 0;
		bool hasVersion = false;   int version = 0;
		std::vector<std::string> regions;                  // region <type> values
	};

	//! <wiidisc> - the whole parsed document.
	struct Disc
	{
		int version = 0;
		GameFilter filter;
		std::vector<Section> sections;
		std::vector<Patch> patches;
		std::string root = "/riivolution";  // <wiidisc root="...">
		std::string xmlPath;                // where it was loaded from

		//! Mirror of Dolphin's Disc::IsValidForGame. gameId must be >= 6 chars.
		bool IsValidForGame(const char *gameId, int discNumber, int revision) const;

		//! Find a <patch> by id, or NULL.
		const Patch *FindPatch(const std::string &id) const;
	};

	// ------------------------------------------------------------------
	// Resolved (post-selection, post-substitution) view handed to the
	// patch engines. String fields have ${param} substitution applied.
	// `root` is the effective root for this item (substituted); the Phase 3
	// file loader joins it with `external`/`valuefile` against the SD mount.
	// Memory offsets/values are never substituted.
	// ------------------------------------------------------------------

	struct ResolvedFile
	{
		std::string root;
		std::string disc;
		std::string external;
		bool resize;
		bool create;
		u32 offset;
		u32 fileoffset;
		u32 length;
	};

	struct ResolvedFolder
	{
		std::string root;
		std::string disc;
		std::string external;
		bool resize;
		bool create;
		bool recursive;
		u32 length;
	};

	struct ResolvedMemory
	{
		std::string root;
		u32 offset;
		std::vector<u8> value;
		std::string valuefile;  // substituted (empty if inline value used)
		std::vector<u8> original;
		bool ocarina;
		bool search;
		u32 align;
	};

	struct ResolvedSavegame
	{
		std::string root;
		std::string external;
		bool clone;
	};

	struct ResolvedPatchSet
	{
		std::vector<ResolvedFile> files;
		std::vector<ResolvedFolder> folders;
		std::vector<ResolvedMemory> memories;
		std::vector<ResolvedSavegame> savegames;

		bool IsEmpty() const
		{
			return files.empty() && folders.empty() && memories.empty() && savegames.empty();
		}
	};
}

#endif
