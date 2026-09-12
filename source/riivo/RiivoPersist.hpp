/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Checked log persistence: every byte of a record is verified onto the
 * card, because an unchecked write fails silently and the resulting log -
 * ending mid-boot with no explanation - is indistinguishable from a hang
 * at that point. A record must never be claimed to survive a storage
 * failure: on any short write or close failure the caller is told, and
 * reports it over gprintf (the only channel that does not need the card).
 *
 * Pure C file I/O: no console calls, so host tests exercise this exact
 * function, including the failure paths (unwritable path, short write).
 ***************************************************************************/
#ifndef RIIVO_PERSIST_HPP_
#define RIIVO_PERSIST_HPP_

#include <stdio.h>

namespace Riivo
{

//! Append [data, data+len) to the file at path (opened in append mode,
//! matching the boot log's history of one record per line). Returns true
//! only when every byte is confirmed written AND the stream closes cleanly:
//! a short fwrite or a failing fclose (buffered bytes lost on flush) both
//! report false. Empty inputs succeed trivially - nothing was asked for.
inline bool AppendFileBytes(const char *path, const char *data, size_t len)
{
	if (!path || !*path || !data || !len)
		return true;
	FILE *f = fopen(path, "a");
	if (!f)
		return false;
	const size_t wrote = fwrite(data, 1, len, f);
	const int closed = fclose(f);
	return wrote == len && closed == 0;
}

} // namespace Riivo

#endif
