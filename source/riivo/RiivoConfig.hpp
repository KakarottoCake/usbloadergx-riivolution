/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Turn a parsed Riivo::Disc plus the current option selections into a flat
 * ResolvedPatchSet, applying ${param} substitution and built-in params.
 * Port of the resolver half of riivultimatum/riivo/parser.py + Dolphin's
 * GeneratePatches.
 ***************************************************************************/
#ifndef RIIVO_CONFIG_HPP_
#define RIIVO_CONFIG_HPP_

#include "RiivoTypes.hpp"

namespace Riivo
{
	//! ${name} / {$name} substitution. Unknown vars are left verbatim.
	std::string Substitute(const std::string &text, const ParamMap &params);

	//! Join an SD/USB path: device + root + rel, normalising slashes.
	//! e.g. ("sd:", "/mymod/E", "blob.bin") -> "sd:/mymod/E/blob.bin".
	std::string JoinPath(const std::string &device, const std::string &root, const std::string &rel);

	//! Built-in params derived from the 6-char game id: __gameid, __region, __maker.
	ParamMap BuiltinParams(const char *gameId);

	//! Collect every patch enabled by the current selections in `disc` into `out`,
	//! substituting params (built-ins + per-reference params). `gameId` is the
	//! 6-char disc id used for the built-in params.
	void Resolve(const Disc &disc, const char *gameId, ResolvedPatchSet &out);

	//! Resolve statistics: references that named a <patch> id with no
	//! definition are skipped (Riivolution leniency). The count exists so the
	//! boot log can name a misspelled id instead of applying nothing silently.
	struct ResolveStats
	{
		u32 skippedPatchRefs;
		ResolveStats() : skippedPatchRefs(0) {}
	};

	//! Resolve with statistics. Identical to Resolve; `stats` reports skips.
	void ResolveWithStats(const Disc &disc, const char *gameId,
						  ResolvedPatchSet &out, ResolveStats &stats);

	//! Merge two parsed XMLs for multi-XML mods (WP1). Sections concatenate
	//! (a's first, then b's); <patch> definitions concatenate with later-wins
	//! on id collision (b replaces a's entry in place, keeping a's position).
	//! The merged filter and root come from `a`; callers must pre-validate
	//! each XML with IsValidForGame before merging. Deterministic: merging is
	//! associative in the patch list, and Resolve over the merged disc applies
	//! patch refs in selection order with duplicate-disc file claims keeping
	//! the last claim downstream (FsDirLister/BuildRedirects stable order).
	void MergeDiscs(const Disc &a, const Disc &b, Disc &out);

	//! Serialise the current selections as "optIdx=choiceIdx,..." for GameCFG,
	//! and load them back. Indices are section-flattened option order.
	std::string SerializeSelection(const Disc &disc);
	void ApplySelection(Disc &disc, const std::string &serialized);

	//! Read the machine-parseable outcome of a previous boot log. The running
	//! boot appends one `OUTCOME:` line: `FST_STAGED`, `NO_FILE_WORK`, or
	//! `WITHHELD <STAGE>` (`FILES_LIVE` from older builds reads as staged).
	//! The last such line wins; returns false when the
	//! text holds none (a log from before outcome lines existed). `liveOut`
	//! is true only for a staged table; `codeOut` is the token after the prefix
	//! (`FST_STAGED`, `NO_FILE_WORK`, or the WITHHELD stage). The game
	//! settings UI uses this to report the previous boot's file work before
	//! launching again.
	bool ParseBootOutcome(const std::string &logText, bool &liveOut,
						  std::string &codeOut);

	//! gprintf a human-readable dump of the parsed disc and the resolved set.
	void DumpDisc(const Disc &disc);
	void DumpResolved(const ResolvedPatchSet &set);

	//! Write a plain-text summary of the Riivolution stage of boot to `path`.
	//! The loader unmounts SD/USB long before the game starts and shows nothing
	//! on screen at that point, so without a USB Gecko this file is the only way
	//! to see whether the XML parsed, which options were active, and whether
	//! every valuefile was found. `disc`/`set` may be NULL when parsing failed;
	//! `parseError` is NULL when it succeeded. `discNumber`/`revision` feed the
	//! "matches this game" line; pass RIIVO_DISC_UNKNOWN / RIIVO_REVISION_UNKNOWN
	//! (the defaults) when the boot path does not know them - a negative value
	//! skips that axis instead of comparing against disc 0 / revision 0, which
	//! would falsely report version-filtered XMLs as meant for another game.
	//! `skippedPatchRefs` (from ResolveWithStats) names misspelled choice->patch
	//! ids in the log; without it a typo applies nothing silently.
	void WriteLog(const std::string &path, const char *gameId, const std::string &xmlPath,
				  const char *parseError, const Disc *disc, const ResolvedPatchSet *set,
				  int valuefileFailures,
				  int discNumber = RIIVO_DISC_UNKNOWN,
				  int revision = RIIVO_REVISION_UNKNOWN,
				  unsigned skippedPatchRefs = 0);
}

#endif
