// WBFS FAT by oggzee
// Updated by blackb0x

#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <ctype.h>
#include <vector>
#include <string>
#include <algorithm>

#include "Controls/DeviceHandler.hpp"
#include "FileOperations/fileops.h"
#include "settings/CSettings.h"
#include "settings/GameTitles.h"
#include "usbloader/disc.h"
#include "usbloader/usbstorage2.h"
#include "language/gettext.h"
#include "libs/libfat/fat.h"
#include "libs/libfat/fatfile_frag.h"
#include "utils/ShowError.h"
#include "wbfs_fat.h"
#include "prompts/ProgressWindow.h"
#include "usbloader/wbfs.h"
#include "usbloader/GameList.h"
#include "utils/tools.h"
#include "wbfs_rw.h"

#define TITLE_LEN 130

using namespace std;

static const char wbfs_fat_dir[] = "/wbfs";
static const char invalid_path[] = "/\\:|<>?*\"'";
extern u32 hdd_sector_size[2];
extern int install_abort_signal;

inline bool isGameID(const char *id)
{
	for (int i = 0; i < 6; i++)
		if (!isalnum((int)id[i]))
			return false;

	return true;
}

Wbfs_Fat::Wbfs_Fat(u32 lba, u32 size, u32 part, u32 port) : Wbfs(lba, size, part, port)
{
	memset(wbfs_fs_drive, 0, sizeof(wbfs_fs_drive));
}

s32 Wbfs_Fat::Open()
{
	Close();

	if (Settings.SDMode)
	{
		PartitionHandle *sdHandle = DeviceHandler::Instance()->GetSDHandle();
		if (lba == sdHandle->GetLBAStart(partition))
		{
			snprintf(wbfs_fs_drive, sizeof(wbfs_fs_drive), "sd:");
			return 0;
		}
		return -1;
	}

	if (partition < (u32)DeviceHandler::GetUSBPartitionCount())
	{
		PartitionHandle *usbHandle = DeviceHandler::Instance()->GetUSBHandleFromPartition(partition);
		int portPart = DeviceHandler::PartitionToPortPartition(partition);
		if (lba == usbHandle->GetLBAStart(portPart))
		{
			snprintf(wbfs_fs_drive, sizeof(wbfs_fs_drive), "%s:", usbHandle->MountName(portPart));
			return 0;
		}
	}

	return -1;
}

void Wbfs_Fat::Close()
{
	if (hdd)
	{
		wbfs_close(hdd);
		hdd = NULL;
	}

	memset(wbfs_fs_drive, 0, sizeof(wbfs_fs_drive));
}

wbfs_disc_t *Wbfs_Fat::OpenDisc(u8 *discid)
{
	std::string fname = FindFilename(discid);
	if (fname.empty())
		return NULL;

	if (strcasecmp(strrchr(fname.c_str(), '.'), ".iso") == 0)
	{
		// .iso file
		// Create a fake wbfs_disc
		int fd = open(fname.c_str(), O_RDONLY);
		if (fd == -1)
			return NULL;
		wbfs_disc_t *iso_file = (wbfs_disc_t *)calloc(1, sizeof(wbfs_disc_t));
		if (iso_file == NULL)
			return NULL;
		// Mark with a special wbfs_part
		wbfs_iso_file.wbfs_sec_sz = hdd_sector_size[usbport];
		iso_file->p = &wbfs_iso_file;
		iso_file->header = (wbfs_disc_info_t *)malloc(sizeof(wbfs_disc_info_t));
		if (!iso_file->header)
		{
			free(iso_file);
			close(fd);
			return NULL;
		}
		read(fd, iso_file->header, sizeof(wbfs_disc_info_t));
		iso_file->i = fd;
		return iso_file;
	}

	wbfs_t *part = OpenPart((char *)fname.c_str());
	if (!part)
		return NULL;

	wbfs_disc_t *disc = wbfs_open_disc(part, discid);
	if (!disc)
	{
		ClosePart(part);
		return NULL;
	}

	return disc;
}

void Wbfs_Fat::CloseDisc(wbfs_disc_t *disc)
{
	if (!disc)
		return;
	wbfs_t *part = disc->p;

	// Is this really a .iso file?
	if (part == &wbfs_iso_file)
	{
		close(disc->i);
		free(disc->header);
		free(disc);
		return;
	}

	wbfs_close_disc(disc);
	ClosePart(part);
	return;
}

s32 Wbfs_Fat::GetCount(u32 *count)
{
	GetHeadersCount();
	*count = fat_hdr_vector.size();
	return 0;
}

s32 Wbfs_Fat::GetHeaders(struct discHdr *outbuf, u32 cnt, u32 len)
{
	if (cnt * len > fat_hdr_vector.size() * sizeof(struct discHdr))
		return -1;

	memcpy(outbuf, fat_hdr_vector.data(), cnt * len);
	fat_hdr_vector.clear();
	return 0;
}

s32 Wbfs_Fat::AddGame(void)
{
	static struct discHdr header ATTRIBUTE_ALIGN(32);
	char path[MAX_FAT_PATH];
	wbfs_t *part = NULL;
	s32 ret;

	// Read ID from DVD
	Disc_ReadHeader(&header);
	// Path
	GetDir(&header, path);
	// Create wbfs 'partition' file
	part = CreatePart(header.id, path);
	if (!part)
		return -1;
	// Add game to device
	partition_selector_t part_sel = (partition_selector_t)Settings.InstallPartitions;

	ret = wbfs_add_disc(part, __ReadDVD, NULL, ShowProgress, part_sel, 0);
	wbfs_trim(part);
	ClosePart(part);

	if (install_abort_signal)
		RemoveGame(header.id);
	if (ret < 0)
		return ret;

	return 0;
}

s32 Wbfs_Fat::RemoveGame(u8 *discid)
{
	// wbfs 'partition' file
	std::string path = FindFilename(discid);
	if (path.empty())
		return -1;
	split_create(&split, const_cast<char *>(path.c_str()), 0, 0, true);
	split_close(&split);

	// Done if not in subdir
	size_t lastslash = path.find_last_of('/');
	if (lastslash == std::string::npos || lastslash <= 0)
		return 0;

	// Remove optional .txt file in subdir
	std::string dirpath = path.substr(0, lastslash);
	DIR *dir = opendir(dirpath.c_str());
	if (!dir)
		return 0;
	struct dirent *dirent = NULL;
	while ((dirent = readdir(dir)) != 0)
	{
		if (dirent->d_name[0] == '.')
			continue;
		if (strlen(dirent->d_name) < 7)
			continue;
		if (dirent->d_name[6] != '_')
			continue;
		if (strncasecmp(dirent->d_name, (char *)discid, 6) != 0)
			continue;
		const char *p = strrchr(dirent->d_name, '.');
		if (!p)
			continue;
		if (strcasecmp(p, ".txt") != 0)
			continue;
		std::string xpath = dirpath + "/" + dirent->d_name;
		remove(xpath.c_str());
		break;
	}
	closedir(dir);
	remove(dirpath.c_str());
	rmdir(dirpath.c_str());
	return 0;
}

s32 Wbfs_Fat::DiskSpace(f32 *used, f32 *free)
{
	static f32 used_cached = 0.0;
	static f32 free_cached = 0.0;
	static int game_count = 0;

	// Since it's freaken slow, only refresh on new gamecount
	if (used_cached == 0.0 || game_count != gameList.GameCount())
	{
		game_count = gameList.GameCount();
	}
	else
	{
		*used = used_cached;
		*free = free_cached;
		return 0;
	}

	f32 size;
	int ret;
	struct statvfs wbfs_fat_vfs;

	*used = used_cached = 0.0;
	*free = free_cached = 0.0;
	ret = statvfs(wbfs_fs_drive, &wbfs_fat_vfs);
	if (ret)
		return -1;

	// FS size in GB
	size = (f32)wbfs_fat_vfs.f_frsize * (f32)wbfs_fat_vfs.f_blocks / GB_SIZE;
	*free = free_cached = (f32)wbfs_fat_vfs.f_frsize * (f32)wbfs_fat_vfs.f_bfree / GB_SIZE;
	*used = used_cached = size - *free;

	return 0;
}

s32 Wbfs_Fat::RenameGame(u8 *discid, const void *newname)
{
	wbfs_t *part = OpenPart((char *)discid);
	if (!part)
		return -1;

	s32 ret = wbfs_ren_disc(part, discid, (u8 *)newname);

	ClosePart(part);

	return ret;
}

s32 Wbfs_Fat::ReIDGame(u8 *discid, const void *newID)
{
	wbfs_t *part = OpenPart((char *)discid);
	if (!part)
		return -1;

	s32 ret = wbfs_rID_disc(part, discid, (u8 *)newID);

	ClosePart(part);

	if (ret == 0)
	{
		char fname[100];
		char fnamenew[100];
		s32 cnt = 0x31;

		Filename(discid, fname, sizeof(fname), NULL);
		Filename((u8 *)newID, fnamenew, sizeof(fnamenew), NULL);

		int stringlength = strlen(fname);

		while (rename(fname, fnamenew) == 0)
		{
			fname[stringlength] = cnt;
			fname[stringlength + 1] = 0;
			fnamenew[stringlength] = cnt;
			fnamenew[stringlength + 1] = 0;
			cnt++;
		}
	}

	return ret;
}

u64 Wbfs_Fat::EstimateGameSize()
{
	wbfs_t *part = NULL;
	u64 size = (u64)143432 * 2 * 0x8000ULL;
	u32 n_sector = size / hdd_sector_size[usbport];

	// Init a temporary dummy part as a placeholder for wbfs_size_disc
	wbfs_set_force_mode(1);
	part = wbfs_open_partition(nop_rw_sector, nop_rw_sector, NULL, hdd_sector_size[usbport], n_sector, 0, 1);
	wbfs_set_force_mode(0);
	if (!part)
		return -1;

	partition_selector_t part_sel = (partition_selector_t)Settings.InstallPartitions;

	u64 estimated_size = wbfs_estimate_disc(part, __ReadDVD, NULL, part_sel);

	wbfs_close(part);

	return estimated_size;
}

bool Wbfs_Fat::CheckLayoutB(char *fname, int len, u8 *id, char *fname_title)
{
	if (len <= 8)
		return false;
	if (fname[len - 8] != '[' || fname[len - 1] != ']')
		return false;
	if (!isGameID(&fname[len - 7]))
		return false;
	strncpy(fname_title, fname, TITLE_LEN);
	// Cut at '['
	fname_title[len - 8] = 0;
	int n = strlen(fname_title);
	if (n == 0)
		return false;
	// Cut trailing _ or ' '
	if (fname_title[n - 1] == ' ' || fname_title[n - 1] == '_')
	{
		fname_title[n - 1] = 0;
	}
	if (strlen(fname_title) == 0)
		return false;
	if (id)
	{
		memcpy(id, &fname[len - 7], 6);
		id[6] = 0;
	}
	return true;
}

void Wbfs_Fat::AddHeader(struct discHdr *discHeader)
{
	for (int j = 0; j < 6; ++j)
		discHeader->id[j] = toupper((int)discHeader->id[j]);

	std::string title(discHeader->title);
	title.erase(0, title.find_first_not_of(' '));
	snprintf(discHeader->title, sizeof(discHeader->title), "%s", title.c_str());

	fat_hdr_vector.push_back(*discHeader);
	if ((Settings.TitlesType == TITLETYPE_FORCED_DISC && GameTitles.GetTitleType((const char *)discHeader->id) != TITLETYPE_MANUAL_OVERRIDE))
		GameTitles.SetGameTitle((const char *)discHeader->id, discHeader->title, TITLETYPE_FORCED_DISC);
}

bool Wbfs_Fat::IsDuplicateID(const u8 *id)
{
	for (const auto &hdr : fat_hdr_vector)
	{
		if (memcmp(hdr.id, id, 6) == 0)
			return true;
	}
	return false;
}

void Wbfs_Fat::GetHeadersCount()
{
	std::string base_path = std::string(wbfs_fs_drive) + wbfs_fat_dir;
	char fname_title[TITLE_LEN];
	struct discHdr tmpHdr;
	struct stat st;
	u8 id[8];
	memset(id, 0, sizeof(id));
	DIR *dir_iter;
	struct dirent *dirent;

	fat_hdr_vector.clear();

	dir_iter = opendir(base_path.c_str());
	if (!dir_iter)
		return;

	while ((dirent = readdir(dir_iter)) != 0)
	{
		if (dirent->d_name[0] == '.')
			continue;

		std::string entry_name = dirent->d_name;
		std::string fileext;
		memset(id, 0, sizeof(id));
		*fname_title = 0;

		size_t dot = entry_name.rfind('.');
		if (dot != std::string::npos)
			fileext = entry_name.substr(dot);

		std::string fpath;
		bool is_dir = false;

		if (!fileext.empty() &&
			(strcasecmp(fileext.c_str(), ".wbfs") == 0 ||
			 strcasecmp(fileext.c_str(), ".iso") == 0 ||
			 strcasecmp(fileext.c_str(), ".ciso") == 0))
		{
			int n = dot;
			memcpy(id, entry_name.c_str(), 6);
			if (n != 6)
			{
				if (!CheckLayoutB((char *)entry_name.c_str(), n, id, fname_title))
					continue;
			}
			fpath = base_path + "/" + entry_name;
		}
		else
		{
			std::string full_dir = base_path + "/" + entry_name;
			if (stat(full_dir.c_str(), &st) != 0)
				continue;
			is_dir = S_ISDIR(st.st_mode);
			if (!is_dir)
				continue;

			int len = entry_name.length();
			if (len < 6)
				continue;

			if (len == 6)
			{
				if (!isGameID(entry_name.c_str()))
					continue;
				memcpy(id, entry_name.c_str(), 6);
			}
			else if (len >= 8)
			{
				int lay_a = 0;
				int lay_b = 0;
				if (CheckLayoutB((char *)entry_name.c_str(), len, id, fname_title))
				{
					lay_b = 1;
				}
				else if (entry_name[6] == '_')
				{
					memcpy(id, entry_name.c_str(), 6);

					if (isGameID((char *)id))
					{
						lay_a = 1;
						snprintf(fname_title, sizeof(fname_title), "%s", entry_name.c_str() + 7);
					}
				}

				if (!lay_a && !lay_b)
					continue;
			}
			else
				continue;

			// Only add if a valid file exists in the subdir
			bool found = false;
			const char *exts[] = {".wbfs", ".iso", ".ciso"};
			for (int i = 0; i < 3; ++i)
			{
				std::string testpath = full_dir + "/" + std::string((char *)id, 6) + exts[i];
				if (stat(testpath.c_str(), &st) == 0)
				{
					fpath = testpath;
					found = true;
					break;
				}
			}
			if (!found)
				continue;
			// Set fileext for later
			dot = fpath.rfind('.');
			if (dot != std::string::npos)
				fileext = fpath.substr(dot);
		}

		if (fpath.empty() || fileext.empty())
			continue;

		// Check the path isn't too long
		if (fpath.length() >= sizeof(tmpHdr.path))
			continue;

		if (IsDuplicateID(id))
			continue;

		memset(&tmpHdr, 0, sizeof(tmpHdr));
		memcpy(tmpHdr.id, id, sizeof(tmpHdr.id));

		std::string title = "";
		if (Settings.TitlesType == TITLETYPE_FORCED_DISC && GameTitles.GetTitleType((const char *)id) == TITLETYPE_FORCED_DISC)
			title.assign(GameTitles.GetTitle((const char *)id));
		if (title.length() == 0 && Settings.TitlesType != TITLETYPE_FORCED_DISC && strlen(fname_title) > 0)
			title.assign(fname_title);

		if (*id != 0 && title.length() > 0 && title.length() < 64)
		{
			snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
			snprintf(tmpHdr.title, sizeof(tmpHdr.title), "%s", title.c_str());
			if (strcasecmp(fileext.c_str(), ".ciso") == 0)
				tmpHdr.is_ciso = 1;
			else
				tmpHdr.is_ciso = 0;
			tmpHdr.magic = 0x5D1C9EA3;
			AddHeader(&tmpHdr);
			continue;
		}

		if (strcasecmp(fileext.c_str(), ".wbfs") == 0)
		{
			FILE *fp = fopen(fpath.c_str(), "rb");
			if (fp != NULL)
			{
				fseek(fp, 512, SEEK_SET);
				fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
				fclose(fp);
				if ((tmpHdr.magic == 0x5D1C9EA3) && (memcmp(tmpHdr.id, id, 6) == 0))
				{
					snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
					tmpHdr.is_ciso = 0;
					AddHeader(&tmpHdr);
					continue;
				}
			}
			wbfs_t *part = OpenPart((char *)fpath.c_str());
			if (!part)
				continue;

			u32 size;
			int ret = wbfs_get_disc_info(part, 0, (u8 *)&tmpHdr, sizeof(struct discHdr), &size);
			ClosePart(part);
			if (ret == 0)
			{
				snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
				tmpHdr.is_ciso = 0;
				AddHeader(&tmpHdr);
				continue;
			}
		}
		else if (strcasecmp(fileext.c_str(), ".iso") == 0)
		{
			FILE *fp = fopen(fpath.c_str(), "rb");
			if (fp != NULL)
			{
				fseek(fp, 0, SEEK_SET);
				fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
				fclose(fp);
				if ((tmpHdr.magic == 0x5D1C9EA3) && (memcmp(tmpHdr.id, id, 6) == 0))
				{
					snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
					tmpHdr.is_ciso = 0;
					AddHeader(&tmpHdr);
					continue;
				}
			}
		}
		else if (strcasecmp(fileext.c_str(), ".ciso") == 0)
		{
			FILE *fp = fopen(fpath.c_str(), "rb");
			if (fp != NULL)
			{
				fseek(fp, 0x8000, SEEK_SET);
				fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
				fclose(fp);
				if ((tmpHdr.magic == 0x5D1C9EA3) && (memcmp(tmpHdr.id, id, 6) == 0))
				{
					snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
					tmpHdr.is_ciso = 1;
					AddHeader(&tmpHdr);
					continue;
				}
			}
		}
	}

	closedir(dir_iter);
}

std::string Wbfs_Fat::FindFilename(u8 *id)
{
	extern GameList gameList;
	const struct discHdr *hdr = gameList.GetDiscHeader((char *)id);
	if (hdr && strlen(hdr->path) > 0 && strlen(hdr->path) < MAX_FAT_PATH)
		return hdr->path;

	return "";
}

wbfs_t *Wbfs_Fat::OpenPart(char *fname)
{
	wbfs_t *part = NULL;
	int ret;

	// wbfs 'partition' file
	ret = split_open(&split, fname);
	if (ret)
		return NULL;

	wbfs_set_force_mode(1);

	part = wbfs_open_partition(split_read_sector, nop_rw_sector, // readonly //split_write_sector,
							   &split, hdd_sector_size[usbport], split.total_sec, 0, 0);

	wbfs_set_force_mode(0);

	if (!part)
		split_close(&split);

	return part;
}

void Wbfs_Fat::ClosePart(wbfs_t *part)
{
	if (!part)
		return;
	split_info_t *s = (split_info_t *)part->callback_data;
	wbfs_close(part);
	if (s)
		split_close(s);
}

void Wbfs_Fat::Filename(u8 *id, char *fname, int len, char *path)
{
	if (path == NULL)
	{
		snprintf(fname, len, "%s%s/%.6s.wbfs", wbfs_fs_drive, wbfs_fat_dir, id);
	}
	else
	{
		snprintf(fname, len, "%s/%.6s.wbfs", path, id);
	}
}

void Wbfs_Fat::GetDir(struct discHdr *header, char *path)
{
	strcpy(path, wbfs_fs_drive);
	strcat(path, wbfs_fat_dir);
	if (Settings.InstallToDir)
	{
		strcat(path, "/");
		int layout = 0;
		if (Settings.InstallToDir == 2)
			layout = 1;
		mk_gameid_title(header, path + strlen(path), 0, layout);
	}
}

wbfs_t *Wbfs_Fat::CreatePart(u8 *id, char *path)
{
	char fname[MAX_FAT_PATH];
	wbfs_t *part = NULL;
	u64 size = (u64)143432 * 2 * 0x8000ULL;
	u32 n_sector = size / 512;
	int ret;

	// Game subdir
	if (!CreateSubfolder(path))
	{
		ProgressStop();
		ShowError(tr("Error creating path: %s"), path);
		return NULL;
	}

	// 1 cluster less than 4GB
	u64 OPT_split_size = 4LL * 1024 * 1024 * 1024 - 32 * 1024;

	if (Settings.SDMode && Settings.GameSplit == GAMESPLIT_NONE && DeviceHandler::GetFilesystemType(SD) != PART_FS_FAT)
		OPT_split_size = (u64)100LL * 1024 * 1024 * 1024 - 32 * 1024;

	else if (Settings.GameSplit == GAMESPLIT_NONE && DeviceHandler::GetFilesystemType(USB1 + Settings.partition) != PART_FS_FAT)
		OPT_split_size = (u64)100LL * 1024 * 1024 * 1024 - 32 * 1024;

	else if (Settings.GameSplit == GAMESPLIT_2GB)
		// 1 cluster less than 2GB
		OPT_split_size = (u64)2LL * 1024 * 1024 * 1024 - 32 * 1024;

	Filename(id, fname, sizeof(fname), path);
	printf("Writing to %s\n", fname);
	ret = split_create(&split, fname, OPT_split_size, size, true);
	if (ret)
		return NULL;

	// Force create first file
	u32 scnt = 0;
	int fd = split_get_file(&split, 0, &scnt, 0);
	if (fd < 0)
	{
		printf("ERROR creating file\n");
		sleep(2);
		split_close(&split);
		return NULL;
	}

	wbfs_set_force_mode(1);

	part = wbfs_open_partition(split_read_sector, split_write_sector, &split, hdd_sector_size[usbport], n_sector, 0, 1);

	wbfs_set_force_mode(0);

	if (!part)
		split_close(&split);

	return part;
}

void Wbfs_Fat::mk_gameid_title(struct discHdr *header, char *name, int re_space, int layout)
{
	int i, len;
	char title[100];
	char id[7];

	snprintf(id, sizeof(id), (char *)header->id);
	snprintf(title, sizeof(title), header->title);
	CleanTitleCharacters(title);

	if (layout == 0)
	{
		sprintf(name, "%s_%s", id, title);
	}
	else
	{
		sprintf(name, "%s [%s]", title, id);
	}

	// Replace space with '_'
	if (re_space)
	{
		len = strlen(name);
		for (i = 0; i < len; i++)
		{
			if (name[i] == ' ')
				name[i] = '_';
		}
	}
}

void Wbfs_Fat::CleanTitleCharacters(char *title)
{
	int i, len;
	// Trim leading space
	len = strlen(title);
	while (*title == ' ')
	{
		memmove(title, title + 1, len);
		len--;
	}
	// Trim trailing space - not allowed on windows directories
	while (len && title[len - 1] == ' ')
	{
		title[len - 1] = 0;
		len--;
	}
	// Replace silly chars with '_'
	for (i = 0; i < len; i++)
	{
		if (strchr(invalid_path, title[i]) || iscntrl((int)title[i]))
		{
			title[i] = '_';
		}
	}
}

s32 Wbfs_Fat::GetFragList(u8 *id)
{
	std::string fname = FindFilename(id);
	if (fname.empty())
		return -1;
	return get_frag_list_for_file(const_cast<char *>(fname.c_str()), id, GetFSType(), lba, hdd_sector_size[usbport]);
}
