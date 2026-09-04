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

	//! Serialise the current selections as "optIdx=choiceIdx,..." for GameCFG,
	//! and load them back. Indices are section-flattened option order.
	std::string SerializeSelection(const Disc &disc);
	void ApplySelection(Disc &disc, const std::string &serialized);

	//! gprintf a human-readable dump of the parsed disc and the resolved set.
	void DumpDisc(const Disc &disc);
	void DumpResolved(const ResolvedPatchSet &set);

	//! Write a plain-text summary of the Riivolution stage of boot to `path`.
	//! The loader unmounts SD/USB long before the game starts and shows nothing
	//! on screen at that point, so without a USB Gecko this file is the only way
	//! to see whether the XML parsed, which options were active, and whether
	//! every valuefile was found. `disc`/`set` may be NULL when parsing failed;
	//! `parseError` is NULL when it succeeded.
	void WriteLog(const std::string &path, const char *gameId, const std::string &xmlPath,
				  const char *parseError, const Disc *disc, const ResolvedPatchSet *set,
				  int valuefileFailures);
}

#endif
