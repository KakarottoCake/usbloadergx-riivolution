#ifndef _FRAG_H_
#define _FRAG_H_
// worst case wbfs fragmentation scenario:
// 9GB (dual layer) / 2mb (wbfs sector size) = 4608
#define MAX_FRAG 20000
// max that ehcmodule_frag will allow at the moment is about:
// 40000/4/3-1 = 21844

#ifdef __cplusplus
extern "C" {
#endif

#include "gctypes.h"

typedef struct
{
	u32 offset; // file offset, in sectors unit
	u32 sector;
	u32 count;
} Fragment;

typedef struct
{
	u32 size; // num sectors
	u32 num;  // num fragments
	u32 maxnum;
	Fragment frag[MAX_FRAG];
} FragList;

void frag_init(FragList *ff, int maxnum);
void frag_dump(FragList *ff);
int  frag_append(void *ff, u32 offset, u32 sector, u32 count);
int  frag_concat(FragList *ff, FragList *src);

// in case a sparse block is requested,
// the returned poffset might not be equal to requested offset
// the difference should be filled with 0
int frag_get(FragList *ff, u32 offset, u32 count, u32 *poffset, u32 *psector, u32 *pcount);

int frag_remap(FragList *ff, FragList *log, FragList *phy);

int get_frag_list_for_file(char *fname, u8 *id, const u8 wbfs_part_fs, const u32 lba_offset, const u32 sector_size);
int get_frag_list(u8 *id);

// The master list registered with the cIOS, or NULL if none was built.
// Riivolution reads its size to work out where a mod region can start.
const FragList *frag_list_get(void);

// ... and the same list, writable, so Riivolution can append the mod's files.
FragList *frag_list_mutable(void);

// Raise the declared virtual-disc size to at least `sectors`, without adding
// any fragments. Offsets inside the declared size but not covered by a fragment
// read back as zeros, so this costs nothing on disk - but it is what makes the
// cIOS decide the disc is dual-layer when it probes for a second layer, and
// that raises its read ceiling from 4.7 GB to 8.5 GB. Must be called before
// set_frag_list, because the size travels with the list.
int frag_list_reserve(u32 sectors);

// Ask set_frag_list to keep the list instead of freeing it, so Riivolution can
// extend it later. Must be set before set_frag_list runs.
void frag_list_retain(int on);

// Hand the (extended) list to the cIOS again, replacing the one it holds.
int frag_list_register(bool sd_only);
int set_frag_list(u8 *id, bool sd_only);

#ifdef __cplusplus
}
#endif

#endif
