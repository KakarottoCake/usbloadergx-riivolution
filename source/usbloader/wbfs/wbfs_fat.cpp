// WBFS FAT by oggzee

#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <ogcsys.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <ctype.h>

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

#include "gecko.h"

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
		// create a fake wbfs_disc
		int fd = open(fname.c_str(), O_RDONLY);
		if (fd == -1)
			return NULL;
		wbfs_disc_t *iso_file = (wbfs_disc_t *)calloc(1, sizeof(wbfs_disc_t));
		if (iso_file == NULL)
			return NULL;
		// mark with a special wbfs_part
		wbfs_iso_file.wbfs_sec_sz = hdd_sector_size[usbport];
		iso_file->p = &wbfs_iso_file;
		iso_file->header = (wbfs_disc_info_t *)malloc(sizeof(wbfs_disc_info_t));
		if (!iso_file->header)
		{
			free(iso_file);
			return NULL;
		}
		read(fd, iso_file->header, sizeof(wbfs_disc_info_t));
		iso_file->i = fd;
		return iso_file;
	}

	wbfs_t *part = OpenPart(const_cast<char *>(fname.c_str()));
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

	// is this really a .iso file?
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
	wbfs_t *part = NULL;
	s32 ret;

	// read ID from DVD
	Disc_ReadHeader(&header);
	// path
	std::string path = GetDir(&header);
	// create wbfs 'partition' file
	part = CreatePart(header.id, const_cast<char *>(path.c_str()));
	if (!part)
		return -1;
	/* Add game to device */
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

bool Wbfs_Fat::IsDirectFile(const std::string &path)
{
	// Returns true if file is in /wbfs/ and not in a subdir
	size_t lastSlash = path.find_last_of('/');
	if (lastSlash == std::string::npos)
		return false;
	std::string parent = path.substr(0, lastSlash);
	return parent == (std::string(wbfs_fs_drive) + wbfs_fat_dir);
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
	if (IsDirectFile(path))
		return 0;

	// Remove optional .txt file in subdir too
	std::string dirpath = path.substr(0, path.find_last_of('/'));
	DIR *dir = opendir(dirpath.c_str());
	if (!dir)
		return 0;
	struct dirent *dirent;
	while ((dirent = readdir(dir)) != 0)
	{
		std::string name = dirent->d_name;
		if (name[0] == '.')
			continue;
		if (name.length() < 7 || name[6] != '_')
			continue;
		if (strncasecmp(name.c_str(), (char *)discid, 6) != 0)
			continue;
		size_t dot = name.find_last_of('.');
		if (dot == std::string::npos)
			continue;
		if (strcasecmp(name.c_str() + dot, ".txt") != 0)
			continue;
		std::string xpath = dirpath + "/" + name;
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

	//! Since it's freaken slow, only refresh on new gamecount
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

	/* FS size in GB */
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

	// init a temporary dummy part
	// as a placeholder for wbfs_size_disc
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

bool Wbfs_Fat::ValidExtension(const char *filename)
{
	const char *fileext = strrchr(filename, '.');
	if (!fileext)
		return false;
	return (strcasecmp(fileext, ".wbfs") == 0 || strcasecmp(fileext, ".iso") == 0 || strcasecmp(fileext, ".ciso") == 0);
}

void Wbfs_Fat::AddHeader(const struct discHdr &discHeader)
{
	struct discHdr hdr = discHeader;
	for (int j = 0; j < 6; ++j)
		hdr.id[j] = toupper((int)hdr.id[j]);

	std::string title(hdr.title);
	title.erase(0, title.find_first_not_of(' '));
	snprintf(hdr.title, sizeof(hdr.title), "%s", title.c_str());

	fat_hdr_vector.push_back(hdr);

	if ((Settings.TitlesType == TITLETYPE_FORCED_DISC && GameTitles.GetTitleType((const char *)hdr.id) != TITLETYPE_MANUAL_OVERRIDE))
		GameTitles.SetGameTitle((const char *)hdr.id, hdr.title, TITLETYPE_FORCED_DISC);
}

bool Wbfs_Fat::TryAddGameFile(const std::string &fpath, const char *expected_id, const char *folder_title, std::set<std::string> &added_ids, bool use_folder_title)
{
	const char *fileext = strrchr(fpath.c_str(), '.');
	if (!fileext || !ValidExtension(fpath.c_str()))
		return false;

	struct discHdr tmpHdr;
	memset(&tmpHdr, 0, sizeof(tmpHdr));
	FILE *fp = fopen(fpath.c_str(), "rb");
	if (!fp)
		return false;

	if (strcasecmp(fileext, ".wbfs") == 0)
	{
		fseek(fp, 512, SEEK_SET);
		fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
		tmpHdr.is_ciso = 0;
	}
	else if (strcasecmp(fileext, ".iso") == 0)
	{
		fseek(fp, 0, SEEK_SET);
		fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
		tmpHdr.is_ciso = 0;
	}
	else if (strcasecmp(fileext, ".ciso") == 0)
	{
		fseek(fp, 0x8000, SEEK_SET);
		fread(&tmpHdr, sizeof(struct discHdr), 1, fp);
		tmpHdr.is_ciso = 1;
	}
	fclose(fp);

	if (tmpHdr.magic != 0x5D1C9EA3 || memcmp(tmpHdr.id, expected_id, 6) != 0)
		return false;

	std::string idstr(expected_id, 6);
	if (added_ids.count(idstr))
		return false;

	std::string title = "";

	// Forced disc titles (GameTDB or manual override)
	if (Settings.TitlesType == TITLETYPE_FORCED_DISC && GameTitles.GetTitleType(expected_id) == TITLETYPE_FORCED_DISC)
		title.assign(GameTitles.GetTitle(expected_id));

	// Use folder titles if not forced disc and available (not /wbfs/GAMEID.ext)
	if (title.length() == 0 && Settings.TitlesType != TITLETYPE_FORCED_DISC && use_folder_title && folder_title && strlen(folder_title) > 0)
		title.assign(folder_title);

	// Fallback to disc header titles
	if (title.empty() && strlen(tmpHdr.title) > 0)
		title.assign(tmpHdr.title);

	if (*tmpHdr.id != 0 && title.length() > 0 && title.length() < 64)
	{
		snprintf(tmpHdr.title, sizeof(tmpHdr.title), "%s", title.c_str());
		snprintf(tmpHdr.path, sizeof(tmpHdr.path), "%s", fpath.c_str());
		AddHeader(tmpHdr);
		added_ids.insert(idstr);
		return true;
	}
	return false;
}

s32 Wbfs_Fat::GetHeadersCount()
{
	std::string path = std::string(wbfs_fs_drive) + wbfs_fat_dir;
	struct stat st;
	DIR *dir_iter;
	struct dirent *dirent;
	std::set<std::string> added_ids;

	fat_hdr_vector.clear();

	dir_iter = opendir(path.c_str());
	if (!dir_iter)
		return 0;

	while ((dirent = readdir(dir_iter)) != 0)
	{
		if (dirent->d_name[0] == '.')
			continue;

		std::string fname = dirent->d_name;
		std::string fpath = path + "/" + fname;

		if (stat(fpath.c_str(), &st) != 0)
			continue;

		// Handle files in /wbfs root
		if (S_ISREG(st.st_mode))
		{
			const char *fileext = strrchr(fname.c_str(), '.');
			if (!fileext || !ValidExtension(fname.c_str()))
				continue;

			int n = fileext ? (fileext - fname.c_str()) : -1;
			if (n < 6)
				continue;

			char id[7] = {0};
			memcpy(id, fname.c_str(), 6);

			TryAddGameFile(fpath, id, nullptr, added_ids, false);
			continue;
		}

		// Handle subdirectories (1 level deep)
		if (S_ISDIR(st.st_mode))
		{
			int dirlen = fname.length();
			char subdir_id[7] = {0};
			char folder_title[TITLE_LEN] = {0};

			// Use old CheckLayoutB logic to get folder title and id
			if (dirlen > 8 && fname[dirlen - 8] == '[' && fname[dirlen - 1] == ']' && isGameID(&fname[dirlen - 7]))
			{
				memcpy(subdir_id, &fname[dirlen - 7], 6);
				subdir_id[6] = 0;
				strncpy(folder_title, fname.c_str(), dirlen - 8);
				folder_title[dirlen - 8] = 0;
				int n = strlen(folder_title);
				if (n > 0 && (folder_title[n - 1] == ' ' || folder_title[n - 1] == '_'))
					folder_title[n - 1] = 0;
			}
			else if (dirlen > 7 && fname[6] == '_' && isGameID(fname.c_str()))
			{
				memcpy(subdir_id, fname.c_str(), 6);
				subdir_id[6] = 0;
				strncpy(folder_title, fname.c_str() + 7, TITLE_LEN - 1);
				folder_title[TITLE_LEN - 1] = 0;
			}
			else if (dirlen == 6 && isGameID(fname.c_str()))
			{
				memcpy(subdir_id, fname.c_str(), 6);
				subdir_id[6] = 0;
				folder_title[0] = 0;
			}
			else
			{
				continue;
			}

			DIR *subdir = opendir(fpath.c_str());
			if (!subdir)
				continue;
			struct dirent *subent;
			while ((subent = readdir(subdir)) != 0)
			{
				if (subent->d_name[0] == '.')
					continue;
				if (!ValidExtension(subent->d_name))
					continue;
				const char *subfileext = strrchr(subent->d_name, '.');
				int subn = subfileext ? (subfileext - subent->d_name) : -1;
				if (subn != 6 || !isGameID(subent->d_name) || memcmp(subent->d_name, subdir_id, 6) != 0)
					continue;

				std::string subfpath = fpath + "/" + subent->d_name;
				TryAddGameFile(subfpath, subdir_id, folder_title, added_ids, true);
			}
			closedir(subdir);
		}
	}

	closedir(dir_iter);
	return fat_hdr_vector.size();
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

std::string Wbfs_Fat::GetDir(struct discHdr *header)
{
	std::string result = std::string(wbfs_fs_drive) + wbfs_fat_dir;
	if (Settings.InstallToDir)
	{
		result += "/";
		int layout = 0;
		if (Settings.InstallToDir == 2)
			layout = 1;
		char name[256];
		mk_gameid_title(header, name, 0, layout);
		result += name;
	}
	return result;
}

wbfs_t *Wbfs_Fat::CreatePart(u8 *id, char *path)
{
	char fname[MAX_FAT_PATH];
	wbfs_t *part = NULL;
	u64 size = (u64)143432 * 2 * 0x8000ULL;
	u32 n_sector = size / 512;
	int ret;

	if (!CreateSubfolder(path)) // game subdir
	{
		ProgressStop();
		ShowError(tr("Error creating path: %s"), path);
		return NULL;
	}

	// 1 cluster less than 4gb
	u64 OPT_split_size = 4LL * 1024 * 1024 * 1024 - 32 * 1024;

	if (Settings.SDMode && Settings.GameSplit == GAMESPLIT_NONE && DeviceHandler::GetFilesystemType(SD) != PART_FS_FAT)
		OPT_split_size = (u64)100LL * 1024 * 1024 * 1024 - 32 * 1024;

	else if (Settings.GameSplit == GAMESPLIT_NONE && DeviceHandler::GetFilesystemType(USB1 + Settings.partition) != PART_FS_FAT)
		OPT_split_size = (u64)100LL * 1024 * 1024 * 1024 - 32 * 1024;

	else if (Settings.GameSplit == GAMESPLIT_2GB)
		// 1 cluster less than 2gb
		OPT_split_size = (u64)2LL * 1024 * 1024 * 1024 - 32 * 1024;

	Filename(id, fname, sizeof(fname), path);
	printf("Writing to %s\n", fname);
	ret = split_create(&split, fname, OPT_split_size, size, true);
	if (ret)
		return NULL;

	// force create first file
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

	// replace space with '_'
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
	// trim leading space
	len = strlen(title);
	while (*title == ' ')
	{
		memmove(title, title + 1, len);
		len--;
	}
	// trim trailing space - not allowed on windows directories
	while (len && title[len - 1] == ' ')
	{
		title[len - 1] = 0;
		len--;
	}
	// replace silly chars with '_'
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
