/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Reconciling early registration against late placement (Newer SMBW fix).
 *
 * Two lists describe the mod's files, built in different boot phases:
 *   early - ListModFiles enumerates candidates from the card before the
 *           game partition (and therefore the file table) can be opened.
 *           Every candidate gets a synthetic offset and fragment mapping.
 *   late  - BuildRedirects matches those candidates against the real FST
 *           and CollectPlaced keeps only entries the rebuilt table actually
 *           references.
 * A candidate can be registered early but absent late: no disc counterpart
 * without create=true, a failed AddOrReplace (path conflicts with a disc
 * directory or file), a stat that fails between phases, or a zero-length
 * file. Nothing reads those offsets, so they are harmless - but the old
 * activation check demanded every tail-recovered offset appear in the late
 * list and withheld the whole mod anonymously when one did not.
 *
 * This header is the pure seam both phases share: registration records
 * (source path, synthetic offset, length), skip records naming why a
 * registered file never became active, and the match of recovered offsets
 * against both. No console dependency, so the two-phase interaction is
 * host-testable. Byte verification itself stays on the target, fed by the
 * matches computed here - recovered bytes used by the game are always
 * verified, never skipped.
 ***************************************************************************/
#ifndef RIIVO_RECONCILE_HPP_
#define RIIVO_RECONCILE_HPP_

#include <gctypes.h>
#include <map>
#include <string>
#include <vector>

namespace Riivo
{
	//! CRC-32 (IEEE) for post-install verification. Inline so both the
	//! loader and the host tests share one implementation.
	inline u32 Crc32(const u8 *data, u32 len)
	{
		u32 crc = 0xFFFFFFFFu;
		for (u32 i = 0; i < len; ++i)
		{
			crc ^= data[i];
			for (int k = 0; k < 8; ++k)
				crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
		}
		return crc ^ 0xFFFFFFFFu;
	}

	//! One early-registered file: what it is, where it was placed, how big
	//! the card said it was. Filled in PrepareFragList for every candidate,
	//! including zero-length files (they share the cursor without advancing
	//! it, so their offset may equal another file's - match on length first).
	struct RegRecord
	{
		std::string disc;     // normalised disc path, e.g. "/stage/1-1.arc"
		std::string external; // full path on the card, e.g. "usb1:/mod/1-1.arc"
		u64 offset;           // synthetic partition byte offset
		u32 length;           // stat size at enumeration time

		RegRecord() : offset(0), length(0) {}
	};

	//! Why a registered file never became a placed entry.
	enum SkipReason
	{
		SKIP_NO_REDIRECT,  // no redirect/created entry names this disc path
		SKIP_ADD_FAILED,   // the rebuilt table refused the entry (path
						   // conflicts with a disc directory or file)
		SKIP_STAT_FAILED,  // the external file failed to stat when the
						   // table was built, after stating fine early
		SKIP_ZERO_LENGTH,  // zero-length: placed, but needs no fragments
		SKIP_BELOW_REGION, // assigned below the mod region (internal error)
		SKIP_UNASSIGNED    // redirect exists and table build succeeded, yet
						   // no offset was assigned (internal error)
	};

	struct SkipRecord
	{
		std::string disc;
		SkipReason reason;

		SkipRecord() : reason(SKIP_NO_REDIRECT) {}
		SkipRecord(const std::string &d, SkipReason r) : disc(d), reason(r) {}
	};

	inline const char *SkipReasonText(SkipReason r)
	{
		switch (r)
		{
			case SKIP_NO_REDIRECT: return "no redirect names this file (no disc counterpart and create=false)";
			case SKIP_ADD_FAILED: return "the rebuilt table refused the entry (path conflicts with a disc directory or file)";
			case SKIP_STAT_FAILED: return "the external file failed to stat when the table was built";
			case SKIP_ZERO_LENGTH: return "zero-length file (needs no fragments)";
			case SKIP_BELOW_REGION: return "assigned below the mod region";
			default: return "redirect exists but no offset was assigned";
		}
	}

	//! Where one recovered offset resolved.
	enum RecState
	{
		REC_ACTIVE,   // referenced by the rebuilt table: verify via placed
		REC_INACTIVE, // registered early but unreferenced: verify via record
				REC_UNKNOWN   // matches no registration record: refuse, naming it
	};
	struct RecMatch
	{
		RecState state;
		size_t index;      // placed index (ACTIVE) or records index (INACTIVE)
		SkipReason reason; // valid only for INACTIVE

		RecMatch() : state(REC_UNKNOWN), index(0), reason(SKIP_NO_REDIRECT) {}
	};

	//! Build skip records for registered files that never became placed
	//! entries. `placedOffsets` holds every late offset, `hasRedirect` every
	//! disc path with a redirect/created entry, `addFails` the per-disc
	//! table-build failures (ADD_FAILED/STAT_FAILED). Zero-length records
	//! are reported before anything else: they share the cursor, so their
	//! offset can equal an active file's without being active themselves.
	inline void FindSkips(const std::vector<RegRecord> &records,
						  const std::vector<u64> &placedOffsets,
						  u64 region,
						  const std::map<std::string, char> &hasRedirect,
						  const std::map<std::string, SkipReason> &addFails,
						  std::vector<SkipRecord> &out)
	{
		out.clear();
		for (size_t i = 0; i < records.size(); ++i)
		{
			const RegRecord &r = records[i];
			if (r.length == 0)
			{
				out.push_back(SkipRecord(r.disc, SKIP_ZERO_LENGTH));
				continue;
			}
			bool active = false;
			for (size_t j = 0; j < placedOffsets.size(); ++j)
			{
				if (placedOffsets[j] == r.offset)
				{
					active = true;
					break;
				}
			}
			if (active)
				continue;
			if (r.offset < region)
			{
				out.push_back(SkipRecord(r.disc, SKIP_BELOW_REGION));
				continue;
			}
			if (hasRedirect.find(r.disc) == hasRedirect.end())
			{
				out.push_back(SkipRecord(r.disc, SKIP_NO_REDIRECT));
				continue;
			}
			std::map<std::string, SkipReason>::const_iterator it =
				addFails.find(r.disc);
			out.push_back(SkipRecord(r.disc, it == addFails.end()
											 ? SKIP_UNASSIGNED : it->second));
		}
	}

	//! Match every recovered offset against placed entries and registration
	//! records. `matches` parallels `recovered`; `unregistered` collects
	//! placed offsets with no registration record, which activation must
	//! refuse - the table would reference bytes nothing registered.
	inline void ReconcileRecovered(const std::vector<u64> &placedOffsets,
								   const std::vector<RegRecord> &records,
								   const std::vector<SkipRecord> &skips,
								   const std::vector<u64> &recovered,
								   std::vector<RecMatch> &matches,
								   std::vector<u64> &unregistered)
	{
		matches.clear();
		matches.reserve(recovered.size());
		unregistered.clear();

		for (size_t j = 0; j < placedOffsets.size(); ++j)
		{
			bool known = false;
			for (size_t i = 0; i < records.size(); ++i)
			{
				if (records[i].offset == placedOffsets[j])
				{
					known = true;
					break;
				}
			}
			if (!known)
				unregistered.push_back(placedOffsets[j]);
		}

		for (size_t k = 0; k < recovered.size(); ++k)
		{
			RecMatch m;
			bool done = false;
			for (size_t j = 0; j < placedOffsets.size(); ++j)
			{
				if (placedOffsets[j] == recovered[k])
				{
					m.state = REC_ACTIVE;
					m.index = j;
					done = true;
					break;
				}
			}
			if (!done)
			{
				for (size_t i = 0; i < records.size(); ++i)
				{
					if (records[i].offset == recovered[k])
					{
						m.state = REC_INACTIVE;
						m.index = i;
						m.reason = SKIP_NO_REDIRECT;
						for (size_t s = 0; s < skips.size(); ++s)
						{
							if (skips[s].disc == records[i].disc)
							{
								m.reason = skips[s].reason;
								break;
							}
						}
						done = true;
						break;
					}
				}
			}
			matches.push_back(m);
		}
	}

	// ------------------------------------------------------------------
	// Runtime read contract, after rawksd-2013 dip.cpp read dispatch.
	//
	// Provenance note: STATUS.md records t5 (our launcher + official
	// filemodule + our dipmodule) working, then a FileProvider
	// ES_DiVerify fix for v3's Newer run - not "both official modules".
	// The historical v3 zip is not in this checkout, so its exact binary
	// composition is unresolved here; treat dip.cpp as a reference design,
	// not a hardware-proven artifact of a verified build.
	//
	// A game read [pos, pos+len) against replaced extents resolves to an
	// ordered segment list covering the whole request: file bytes where an
	// extent covers, original-disc bytes everywhere else. The reference
	// forwards the read to the disc first unless file extents cover it
	// exactly, then overlays each clipped file range (advancing the file
	// offset when the read starts mid-extent, clamping when it overruns).
	// GX's fraglist runtime today serves neither the original side of a
	// spanning read nor a nonzero source offset. This fixture is
	// specification groundwork, not a compatibility fix: it pins the
	// contract a future runtime must satisfy. No test mod currently issues
	// partial <file> patches, and no runtime change is made here.
	//
	// Boot-order note: the reference installs its rebuilt FST into MEM
	// before memory patches and unmounting (MenuLaunch: RVL_Patch, then
	// apploader, then CommitRVL(false), then PatchMemory, then Unmount);
	// GX stages pre-shutdown but installs after memory patches,
	// post-shutdown, just before the jump. Registering asset redirects
	// does not by itself establish that GX's apploader sees rebuilt
	// metadata or executable replacements - its apploader runs before the
	// install, so main.dol loads from the real disc.
	// ------------------------------------------------------------------

	//! One replaced range: partition bytes [off, off+len) come from a file
	//! starting at fileOff. Sorted ascending, non-overlapping.
	struct SpanExtent
	{
		u64 off;
		u32 len;
		u64 fileOff;

		SpanExtent() : off(0), len(0), fileOff(0) {}
	};

	//! One output segment: [outOff, outOff+len) of the read buffer comes
	//! from the file at fileOff (fromFile) or from the original disc.
	struct SpanSeg
	{
		u64 outOff;
		u32 len;
		bool fromFile;
		u64 fileOff;

		SpanSeg() : outOff(0), len(0), fromFile(false), fileOff(0) {}
	};

	//! Split [pos, pos+len) into ascending segments covering exactly len
	//! bytes. `fullyCovered` mirrors the reference's filecover: every byte
	//! comes from a file extent, so no original-side forward is needed.
	inline void ClipReadSpans(u64 pos, u32 len,
							  const std::vector<SpanExtent> &ext,
							  std::vector<SpanSeg> &out, bool &fullyCovered)
	{
		out.clear();
		fullyCovered = true;
		if (len == 0)
			return;
		u64 covered = 0;
		for (size_t i = 0; i < ext.size(); ++i)
		{
			const u64 eEnd = ext[i].off + ext[i].len;
			if (eEnd <= pos || ext[i].off >= pos + len)
				continue;
			const u64 start = ext[i].off > pos ? ext[i].off : pos;
			const u64 end = eEnd < pos + len ? eEnd : pos + len;
			if (start > pos + covered)
			{
				SpanSeg gap;
				gap.outOff = covered;
				gap.len = (u32) (start - (pos + covered));
				gap.fromFile = false;
				out.push_back(gap);
				covered = start - pos;
				fullyCovered = false;
			}
			SpanSeg seg;
			seg.outOff = start - pos;
			seg.len = (u32) (end - start);
			seg.fromFile = true;
			seg.fileOff = ext[i].fileOff + (start - ext[i].off);
			out.push_back(seg);
			covered = end - pos;
		}
		if (covered < len)
		{
			SpanSeg gap;
			gap.outOff = covered;
			gap.len = (u32) ((u64) len - covered);
			gap.fromFile = false;
			out.push_back(gap);
			fullyCovered = false;
		}
	}
}

#endif
