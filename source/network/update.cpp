/***************************************************************************
 * Copyright (C) 2025 by blackb0x
 * Copyright (C) 2009 by Dimok
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 * must not claim that you wrote the original software. If you use
 * this software in a product, an acknowledgment in the product
 * documentation would be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and
 * must not be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source
 * distribution.
 *
 * update.cpp
 *
 * Update operations
 * for Wii-Xplorer 2009
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <ogcsys.h>
#include <string>

#include "update.h"
#include "gecko.h"
#include "ZipFile.h"
#include "https.h"
#include "networkops.h"
#include "ImageDownloader.h"
#include "settings/CSettings.h"
#include "settings/GameTitles.h"
#include "language/gettext.h"
#include "language/UpdateLanguage.h"
#include "utils/StringTools.h"
#include "utils/ShowError.h"
#include "prompts/PromptWindows.h"
#include "prompts/ProgressWindow.h"
#include "FileOperations/fileops.h"
#include "xml/GameTDB.hpp"
#include "usbloader/GameList.h"
#include "version.h"

/****************************************************************************
 * Checking if an Update is available
 ***************************************************************************/
int DownloadFileToPath(const char *url, const char *dest, const bool showprogress)
{
	if (showprogress)
	{
		const char *filename = strrchr(url, '/') + 1;
		ProgressCancelEnable(true);
		StartProgress(tr("Downloading file..."), 0, filename, true, true);
	}

	struct download file = {};
	file.show_progress = showprogress;
	downloadfile(url, &file);
	if (file.size > 0)
	{
		FILE *savefile = fopen(dest, "wb");
		if (!savefile)
		{
			MEM2_free(file.data);
			if (showprogress)
				ShowError(tr("Can't write to destination."));
			ProgressStop();
			ProgressCancelEnable(false);
			return -7;
		}
		fwrite(file.data, 1, file.size, savefile);
		fclose(savefile);
		MEM2_free(file.data);
	}
	if (showprogress)
	{
		ProgressStop();
		ProgressCancelEnable(false);
	}
	return file.size;
}

static bool CheckNewGameTDBVersion(const char *url)
{
	gprintf("Checking GameTDB version...\n");
	struct download file = {};
	file.gametdbcheck = true;
	downloadfile(url, &file);

	if (file.gametdbcheck <= 0)
		return false;

	std::string Filepath(Settings.titlestxt_path);
	if (Filepath.back() != '/')
		Filepath += '/';
	Filepath += "wiitdb.xml";

	GameTDB XML_DB;

	if (!XML_DB.OpenFile(Filepath.c_str()))
		return true; // If no file exists we need the file

	u64 ExistingVersion = XML_DB.GetGameTDBVersion();
	XML_DB.CloseFile();

	gprintf("Existing GameTDB Version: %llu Online GameTDB Version: %llu\n", ExistingVersion, file.gametdbcheck);

	return (ExistingVersion != file.gametdbcheck);
}

bool initNetwork()
{
	if (NetworkInitPrompt())
		return true;
	gprintf("No network\n");
	return false;
}

int UpdateGameTDB()
{
	// Create the directory if it doesn't exist
	CreateSubfolder(Settings.titlestxt_path);

	if (CheckNewGameTDBVersion(Settings.URL_GameTDB) == false)
	{
		gprintf("Not updating GameTDB: Version is the same\n");
		return -2;
	}

	gprintf("Updating GameTDB...\n");

	std::string ZipPath(Settings.titlestxt_path);
	if (ZipPath.back() != '/')
		ZipPath += '/';
	ZipPath += "wiitdb.zip";

	int filesize = DownloadFileToPath(Settings.URL_GameTDB, ZipPath.c_str());

	if (filesize <= 0)
		return -1;

	ZipFile zFile(ZipPath.c_str());

	bool result = zFile.ExtractAll(Settings.titlestxt_path);

	remove(ZipPath.c_str());

	// Reload all titles and reload cached titles because the file changed now.
	GameTitles.Reset();
	GameTitles.LoadTitlesFromGameTDB(Settings.titlestxt_path);
	return (result ? filesize : -1);
}

int UpdateCheats()
{
	std::string url("https://raw.githubusercontent.com/wiidev/cheats/master/data/txt.zip");
	std::string ZipPath(Settings.ConfigPath);
	if (ZipPath.back() != '/')
		ZipPath += '/';
	ZipPath += "txt.zip";

	int filesize = DownloadFileToPath(url.c_str(), ZipPath.c_str());

	if (filesize <= 0)
		return -1;

	ZipFile zFile(ZipPath.c_str());

	bool result = zFile.ExtractAll(Settings.TxtCheatcodespath);

	remove(ZipPath.c_str());
	return (result ? filesize : -1);
}

int ApplicationDownload()
{
	std::string DownloadURL;
	int newrev = 0;
#if defined(GITRELEASE)
	int currentrev = atoi(LOADER_REV);
#else
	// It might be a pre-release version, so always update
	int currentrev = 0;
#endif

	struct download file = {};
#ifdef FULLCHANNEL
	downloadfile("https://raw.githubusercontent.com/wiidev/usbloadergx/updates/update_wad.txt", &file);
#else
	downloadfile("https://raw.githubusercontent.com/wiidev/usbloadergx/updates/update_dol.txt", &file);
#endif

	if (file.size > 0)
	{
		// First line of the text file is the revisionc
		newrev = atoi((char *)file.data);
		// 2nd line of the text file is the url
		char *ptr = strchr((char *)file.data, '\n');
		while (ptr && (*ptr == '\r' || *ptr == '\n' || *ptr == ' '))
			ptr++;
		while (ptr && *ptr != '\0' && *ptr != '\r' && *ptr != '\n')
		{
			DownloadURL.push_back(*ptr);
			ptr++;
		}

		MEM2_free(file.data);
	}

	if (newrev <= currentrev)
	{
		WindowPrompt(tr("No new updates."), 0, tr("OK"));
		return 0;
	}

	bool update_error = false;
	char tmppath[250];

#ifdef FULLCHANNEL
	snprintf(tmppath, sizeof(tmppath), "%s/ULNR.wad", Settings.BootDevice);
#else
	char realpath[250];
	snprintf(realpath, sizeof(realpath), "%sboot.dol", Settings.ConfigPath);
	snprintf(tmppath, sizeof(tmppath), "%sboot.tmp", Settings.ConfigPath);
#endif

	int ret = DownloadFileToPath(DownloadURL.c_str(), tmppath);
	if (ret < 1024 * 1024)
	{
		remove(tmppath);
		WindowPrompt(tr("Failed updating"), tr("Error while downloading file"), tr("OK"));
		update_error = true;
	}
	else
	{
#ifdef FULLCHANNEL
		FILE *wadFile = fopen(tmppath, "rb");
		if (!wadFile)
		{
			update_error = true;
			WindowPrompt(tr("Failed updating"), tr("Error opening downloaded file"), tr("OK"));
			return -1;
		}

		int error = Wad_Install(wadFile);
		if (error)
		{
			update_error = true;
			ShowError(tr("The WAD installation failed with error %i"), error);
		}
		else
			WindowPrompt(tr("Success"), tr("The WAD file was installed"), tr("OK"));

		RemoveFile(tmppath);
#else
		gprintf("%s\n%s\n", realpath, tmppath);
		RemoveFile(realpath);
		if (!RenameFile(tmppath, realpath))
			update_error = true;
#endif
	}

	if (update_error)
	{
		ShowError(tr("Error while updating USB Loader GX."));
		return -1;
	}

	snprintf(tmppath, sizeof(tmppath), "%s/icon.png", Settings.ConfigPath);
	DownloadFileToPath("https://raw.githubusercontent.com/wiidev/usbloadergx/updates/icon.png", tmppath, false);

	snprintf(tmppath, sizeof(tmppath), "%s/meta.xml", Settings.ConfigPath);
	DownloadFileToPath("https://raw.githubusercontent.com/wiidev/usbloadergx/updates/meta.xml", tmppath, false);

	return 1;
}

int UpdateNintendont()
{
	char NINUpdatePath[120];
	snprintf(NINUpdatePath, sizeof(NINUpdatePath), "%sboot.dol", Settings.NINLoaderPath);
	char NINUpdatePathBak[120];
	snprintf(NINUpdatePathBak, sizeof(NINUpdatePathBak), "%sboot.bak", Settings.NINLoaderPath);

	// Create the directory if it doesn't exist
	CreateSubfolder(Settings.NINLoaderPath);
	// Rename existing boot.dol file to boot.bak
	if (CheckFile(NINUpdatePath))
		RenameFile(NINUpdatePath, NINUpdatePathBak);

	if (DownloadFileToPath("https://raw.githubusercontent.com/FIX94/Nintendont/master/loader/loader.dol", NINUpdatePath) > 0)
	{
		// Remove existing loader.dol file if found as it has priority over boot.dol, and boot.bak
		snprintf(NINUpdatePath, sizeof(NINUpdatePath), "%s/loader.dol", Settings.NINLoaderPath);
		RemoveFile(NINUpdatePath);
		RemoveFile(NINUpdatePathBak);
		// Download icon.png if it doesn't exist
		snprintf(NINUpdatePath, sizeof(NINUpdatePath), "%s/icon.png", Settings.NINLoaderPath);
		if (!CheckFile(NINUpdatePath))
			DownloadFileToPath("https://raw.githubusercontent.com/FIX94/Nintendont/master/nintendont/icon.png", NINUpdatePath, false);
		// Download meta.xml if it doesn't exist (Nintendont will edit meta.xml when it's launched)
		snprintf(NINUpdatePath, sizeof(NINUpdatePath), "%s/meta.xml", Settings.NINLoaderPath);
		if (!CheckFile(NINUpdatePath))
			DownloadFileToPath("https://raw.githubusercontent.com/FIX94/Nintendont/master/nintendont/meta.xml", NINUpdatePath, false);

		return 1;
	}
	else
	{
		// Restore backup file if found
		RemoveFile(NINUpdatePath);
		if (CheckFile(NINUpdatePathBak))
			RenameFile(NINUpdatePathBak, NINUpdatePath);
	}
	return -1;
}

void UpdateCovers()
{
	ImageDownloader::DownloadImages(true);
}
