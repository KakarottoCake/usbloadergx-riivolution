#ifndef _WBFS_FAT_H
#define _WBFS_FAT_H

#include <string>
#include <set>
#include <ogcsys.h>

#include "usbloader/splits.h"
#include "usbloader/wbfs.h"
#include "wbfs_base.h"

#define MAX_FAT_PATH 260

class Wbfs_Fat: public Wbfs
{
	public:
		Wbfs_Fat(u32 lba, u32 size, u32 part, u32 port);

		virtual s32 Open();
		virtual void Close();
		wbfs_disc_t* OpenDisc(u8 *);
		void CloseDisc(wbfs_disc_t *);

		s32 GetCount(u32 *);
		s32 GetHeaders(struct discHdr *, u32, u32);

		s32 AddGame();
		s32 RemoveGame(u8 *);

		s32 DiskSpace(f32 *, f32 *);

		s32 RenameGame(u8 *, const void *);
		s32 ReIDGame(u8 *, const void *);

		u64 EstimateGameSize();

		void AddHeader(const struct discHdr &discHeader);

		virtual s32 GetFragList(u8 *);
		virtual u8 GetFSType(void) { return PART_FS_FAT; }
		static void CleanTitleCharacters(char *title);
	protected:

		split_info_t split;

		std::vector<struct discHdr> fat_hdr_vector;
		char wbfs_fs_drive[16];

		wbfs_t* OpenPart(char *fname);
		void ClosePart(wbfs_t* part);
		wbfs_t* CreatePart(u8 *id, char *path);
		std::string FindFilename(u8 *id);
		std::string GetDir(struct discHdr *header);
		bool IsDirectFile(const std::string &path);
		bool ValidExtension(const char *filename);
		bool TryAddGameFile(const std::string &fpath, const char *expected_id, const char *folder_title, std::set<std::string> &added_ids, bool use_folder_title);
		void Filename(u8 *id, char *fname, int len, char *path);
		s32 GetHeadersCount();

		void mk_gameid_title(struct discHdr *header, char *name, int re_space, int layout);

		static s32 nop_rw_sector(void *_fp, u32 lba, u32 count, void* buf) { return 0; }
};

#endif //_WBFS_FAT_H
