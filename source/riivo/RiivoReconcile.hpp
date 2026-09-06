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
}

#endif
