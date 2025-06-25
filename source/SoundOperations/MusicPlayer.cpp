/****************************************************************************
 * Background music
 * USB Loader GX 2009 - 2025
 ***************************************************************************/
#include <algorithm>
#include <sys/dir.h>
#include <string.h>

#include "prompts/PromptWindows.h"
#include "themes/Resources.h"
#include "FileOperations/fileops.h"
#include "settings/CSettings.h"
#include "MusicPlayer.h"
#include "gecko.h"

// #define DEBUG_BGM

MusicPlayer *MusicPlayer::instance = NULL;

MusicPlayer::MusicPlayer()
	: CThread(40, 16384)
{
	currentPlaying = 0;
	currentPath = NULL;
	rndCurrentPlaying = 0;
	Stopped = false;
	ExitRequested = false;
	Switching = true;

	MainSound = new GuiSound(Resources::GetFile("bg_music.ogg"), Resources::GetFileSize("bg_music.ogg"), false, 0);

	startThread();
}

MusicPlayer::~MusicPlayer()
{
	ExitRequested = true;
	Switching = true;

	MainSound->Stop();
	EmptyList();
	shutdownThread();

	if (MainSound)
		delete MainSound;
	if (currentPath)
		delete[] currentPath;
}

bool MusicPlayer::LoadStandard()
{
	EmptyList();
	if (currentPath)
	{
		delete[] currentPath;
		currentPath = NULL;
	}

	strcpy(Settings.ogg_path, "");

	currentPlaying = 0;
	rndCurrentPlaying = 0;

	MainSound->Load(Resources::GetFile("bg_music.ogg"), Resources::GetFileSize("bg_music.ogg"), false);
	MainSound->Play();
	MainSound->SetLoop(true);
	Stopped = false;
	Paused = false;
	Switching = false;

	return true;
}

bool MusicPlayer::Load(const char *path, const bool forceNewRandom)
{
	if (!path)
	{
		LoadStandard();
		return false;
	}

	if (!CheckFile(path))
	{
		LoadStandard();
		return false;
	}

	if (ParsePath(path))
	{
		if (Settings.musicloopmode == SHUFFLE_MUSIC && forceNewRandom)
		{
			rndCurrentPlaying = 0;
			snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, RandomPlayList.at(rndCurrentPlaying).c_str());
			currentPlaying = findIndex(PlayList, RandomPlayList.at(rndCurrentPlaying));
		}
		else if (Settings.musicloopmode == SHUFFLE_MUSIC)
			snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, RandomPlayList.at(rndCurrentPlaying).c_str());
		else
			snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, PlayList.at(currentPlaying).c_str());

		if (!MainSound->Load(Settings.ogg_path))
		{
			LoadStandard();
			return false;
		}
		MainSound->Play();
		Switching = false;

#ifdef DEBUG_BGM
		gprintf("Loaded BGM: %s\n", Settings.ogg_path);
#endif
		return true;
	}

	return false;
}

bool MusicPlayer::ParsePath(const char *folderpath)
{
	EmptyList();

	if (currentPath)
		delete[] currentPath;

	currentPath = new char[strlen(folderpath) + 1];
	if (!currentPath)
		return false;
	strcpy(currentPath, folderpath);

	char *isdirpath = strrchr(folderpath, '.');
	if (isdirpath)
	{
		char *pathptr = strrchr(currentPath, '/');
		if (pathptr)
		{
			pathptr++;
			pathptr[0] = 0;
		}
	}

	char *LoadedFilename = strrchr(folderpath, '/') + 1;

	char filename[1024];
	struct dirent *dirent = NULL;

	DIR *dir = opendir(currentPath);
	if (dir == NULL)
	{
		LoadStandard();
		return false;
	}

	while ((dirent = readdir(dir)) != 0)
	{
		snprintf(filename, sizeof(filename), dirent->d_name);

		char *fileext = strrchr(filename, '.');
		if (!fileext)
			continue;

		if (strcasecmp(fileext, ".mp3") == 0 || strcasecmp(fileext, ".ogg") == 0 ||
			strcasecmp(fileext, ".wav") == 0 || strcasecmp(fileext, ".aif") == 0)
		{
			AddEntry(filename);
		}
	}

	closedir(dir);

	snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s", folderpath);

	std::sort(PlayList.begin(), PlayList.end());

	Randomize();

	currentPlaying = findIndex(PlayList, LoadedFilename);
	rndCurrentPlaying = findIndex(RandomPlayList, LoadedFilename);

	return true;
}

int MusicPlayer::findIndex(std::vector<std::string> &v, std::string file)
{
	for (int i = 0; i < (int)v.size(); i++)
	{
		if (v[i] == file)
			return i;
	}

	return 0;
}

void MusicPlayer::Randomize()
{
	RandomPlayList.clear();
	RandomPlayList = PlayList;
	srand(time(NULL));
	for (int i = RandomPlayList.size() - 1; i > 0; i--)
	{
		int n = rand() % (i + 1);
		std::swap(RandomPlayList[i], RandomPlayList[n]);
	}

#ifdef DEBUG_BGM
	gprintf("--------\n");
	for (auto &s : RandomPlayList)
		gprintf("%s\n", s.c_str());
	gprintf("--------\n");
#endif
}

void MusicPlayer::AddEntry(const char *filename)
{
	if (!filename)
		return;

	PlayList.push_back(filename);
}

void MusicPlayer::EmptyList()
{
	PlayList.clear();
	std::vector<std::string>().swap(PlayList);
	RandomPlayList.clear();
	std::vector<std::string>().swap(RandomPlayList);
}

void MusicPlayer::Resume()
{
	if (IsStopped())
		return;

	MainSound->Play();
	Paused = false;
}

void MusicPlayer::Stop()
{
	Paused = false;
	Stopped = true;
	MainSound->Stop();
}

bool MusicPlayer::PlayNext()
{
	if (!currentPath)
		return false;

	if (PlayList.size() == 0)
		return LoadStandard();

	if (Settings.musicloopmode <= LOOP)
		return false;

	if (Settings.musicloopmode == SHUFFLE_MUSIC)
		return PlayRandom();

	currentPlaying++;
	if (currentPlaying >= (int)PlayList.size())
		currentPlaying = 0;

	Switching = true;
	snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, PlayList.at(currentPlaying).c_str());
#ifdef DEBUG_BGM
	gprintf("Play next: %s\n", Settings.ogg_path);
#endif
	if (!MainSound->Load(Settings.ogg_path))
		return false;

	MainSound->SetLoop(Settings.musicloopmode);
	MainSound->Play();
	Switching = false;

	// Sync playlists - avoids repeats on setting changes
	rndCurrentPlaying = findIndex(RandomPlayList, PlayList.at(currentPlaying));

	return true;
}

bool MusicPlayer::PlayPrevious()
{
	if (!currentPath)
		return false;

	if (PlayList.size() == 0)
		return LoadStandard();

	if (Settings.musicloopmode <= LOOP)
		return false;

	if (Settings.musicloopmode == SHUFFLE_MUSIC)
		return PlayRandom();

	currentPlaying--;
	if (currentPlaying < 0)
		currentPlaying = PlayList.size() - 1;

	Switching = true;
	snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, PlayList.at(currentPlaying).c_str());
#ifdef DEBUG_BGM
	gprintf("Play previous: %s\n", Settings.ogg_path);
#endif
	if (!MainSound->Load(Settings.ogg_path))
		return false;

	MainSound->SetLoop(Settings.musicloopmode);
	MainSound->Play();
	Switching = false;

	// Sync playlists - avoids repeats on setting changes
	rndCurrentPlaying = findIndex(RandomPlayList, PlayList.at(currentPlaying));

	return true;
}

bool MusicPlayer::PlayRandom()
{
	if (!currentPath)
		return false;

	if (RandomPlayList.size() == 0)
		return LoadStandard();

	rndCurrentPlaying++;
	if (rndCurrentPlaying >= (int)RandomPlayList.size())
	{
		Randomize();
		rndCurrentPlaying = 0;
	}

	Switching = true;
	snprintf(Settings.ogg_path, sizeof(Settings.ogg_path), "%s%s", currentPath, RandomPlayList.at(rndCurrentPlaying).c_str());
#ifdef DEBUG_BGM
	gprintf("Play random: %s\n", Settings.ogg_path);
#endif
	if (!MainSound->Load(Settings.ogg_path))
		return false;

	MainSound->SetLoop(Settings.musicloopmode);
	MainSound->Play();
	Switching = false;

	// Sync playlists - avoids repeats on setting changes
	currentPlaying = findIndex(PlayList, RandomPlayList.at(rndCurrentPlaying));

	return true;
}

void MusicPlayer::SetLoop(u8 mode)
{
	if (!Settings.ogg_path[0])
		MainSound->SetLoop(true);
	else
		MainSound->SetLoop(mode);
}

void MusicPlayer::executeThread(void)
{
	while (!ExitRequested)
	{
		usleep(100000);

		if (!MainSound->IsPlaying() && !Stopped && !Paused && !Switching)
		{
			if (Settings.musicloopmode == PLAYLIST_LOOP)
				PlayNext();
			else if (Settings.musicloopmode == SHUFFLE_MUSIC)
				PlayRandom();
		}
	}
}
