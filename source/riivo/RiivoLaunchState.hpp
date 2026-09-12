/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Per-boot launch state: the single owner of everything one boot stages,
 * books, and verifies.
 *
 * Previously these lived as namespace-scope statics in RiivoBoot.cpp that
 * SetBootContext reset only partially. An aborted launch, or a second
 * launch in the same loader session (back to menu, boot again), inherited
 * the previous boot's staged table, placement verdict, and file-work
 * flags - and InstallPendingFst would then install the WRONG mod's table
 * into the new game: ReportFstPlacement reuses a stale pendingFstSize as
 * `want`, re-books it, and the install verifies against itself. Every
 * field here is cleared by Begin(), which runs once per BootGame before
 * anything else; the only value that crosses a boot boundary is
 * `generation`, bumped by Begin(), and it exists precisely so a table
 * staged under one generation can never install under another.
 *
 * Pure storage and arithmetic: no console calls, so host tests exercise
 * this exact type. The staging buffer itself stays a raw pointer because
 * only the console side may allocate/free MEM2; Begin() hands the old
 * pointer back so the caller frees it exactly once.
 ***************************************************************************/
#ifndef RIIVO_LAUNCH_STATE_HPP_
#define RIIVO_LAUNCH_STATE_HPP_

#include <gctypes.h>
#include "RiivoFstInstall.hpp" // FstPlacement (words + std::string: host-safe)

namespace Riivo
{

//! Stages one launch passes through, in order. Forward-only within a boot;
//! Begin() restarts the sequence for the next boot.
enum class LaunchStage
{
	None,      // fresh object / new boot, nothing staged
	Staged,    // rebuilt table held (bytes + CRC captured)
	Booked,    // placement chosen and recorded; install may consume once
	Installed, // table written and verified; staging is spent
	Refused    // a stage refused; nothing further may install
};

//! Everything one boot stages, books, and verifies. Owned by a single
//! instance per boot (see Begin); never namespace statics.
struct LaunchState
{
	u8 *stageBytes;      // MEM2 staging buffer (owned by the caller side)
	u32 stageSize;       // bytes held
	u32 stageCrc;        // CRC captured at stage time, never recomputed
	u32 stageGeneration; // generation that staged the bytes above
	FstPlacement place;  // booked placement (meaningful only when booked)
	bool placeOk;        // a live booking exists (false once consumed)
	bool fileWorkWanted; // the selection needs file replacement
	bool fileWorkLive;   // ...and the files were actually installed
	bool smg2Armed;      // SB4E01 reservation patch armed for install
	u32 plannedFstSize;  // room the rebuilt table wants (for placement)
	u32 installFailCode; // last install verdict, for the blink code
	u32 generation;      // boot generation; bumped by Begin()
	LaunchStage stage;   // current stage, forward-only within a boot

	LaunchState()
		: stageBytes(0), stageSize(0), stageCrc(0), stageGeneration(0),
		  placeOk(false), fileWorkWanted(false), fileWorkLive(false),
		  smg2Armed(false), plannedFstSize(0), installFailCode(0),
		  generation(0), stage(LaunchStage::None) {}

	//! Start a boot: hand back the previous staging buffer (the caller
	//! frees it - this header cannot touch the console allocator) and
	//! clear every verdict, so nothing inherits a previous boot. Only
	//! `generation` survives, bumped, so a stale stage is detectable.
	u8 *Begin()
	{
		u8 *old = stageBytes;
		u32 next = generation + 1;
		*this = LaunchState();
		generation = next;
		return old;
	}

	//! Record a freshly staged table. Call once, right after the bytes
	//! and their CRC are captured. Refuses anything but the first staging
	//! of a boot: a second staging, or one after a refusal, would mean two
	//! candidate tables with one install slot.
	bool Stage(u8 *bytes, u32 size, u32 crc)
	{
		if (stage != LaunchStage::None || !bytes || !size)
			return false;
		stageBytes = bytes;
		stageSize = size;
		stageCrc = crc;
		stageGeneration = generation;
		stage = LaunchStage::Staged;
		return true;
	}

	//! Record a placement booking. The game is live from here: fileWorkLive
	//! is what later stages consult, never a leftover flag. Refuses invalid
	//! placements and any re-booking: one boot gets exactly one booking,
	//! and a refused launch stays refused.
	bool Book(const FstPlacement &p)
	{
		if (stage != LaunchStage::Staged || !p.ok)
			return false;
		place = p;
		placeOk = true;
		fileWorkLive = true;
		stage = LaunchStage::Booked;
		return true;
	}

	//! The memory-holdback predicate: wanted files that never went live.
	//! Pure logic over owned fields, so host tests exercise the real rule.
	bool FileWorkIncomplete() const { return fileWorkWanted && !fileWorkLive; }

	//! True when a booked install from THIS boot may be consumed: a live
	//! booking, staged bytes of this generation. A table staged by an
	//! aborted earlier boot can never satisfy this after Begin() ran.
	bool CanInstall() const
	{
		return stage == LaunchStage::Booked && placeOk && stageBytes &&
			   stageSize && stageGeneration == generation;
	}

	//! True when a staged table is booked for install (same-boot view).
	bool HaveStaged() const
	{
		return placeOk && stageBytes && stageSize;
	}

	//! Consume the booking after InstallFst runs. Success and refusal both
	//! end here: either way this table must never install twice.
	void Consume()
	{
		placeOk = false;
		stage = LaunchStage::Installed;
	}

	//! Release the staging buffer after a verified install. The caller
	//! frees the memory (this header cannot touch the console allocator);
	//! this clears the fields so later stages see nulls, not stale bytes.
	//! A repeated install refuses at the guard above.
	void ReleaseStaging()
	{
		stageBytes = 0;
		stageSize = 0;
	}

	//! Record a refusal. A refused launch stays refused.
	void Refuse(u32 code)
	{
		installFailCode = code;
		placeOk = false;
		stage = LaunchStage::Refused;
	}
};

} // namespace Riivo

#endif
