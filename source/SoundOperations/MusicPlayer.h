/***************************************************************************
 * Copyright (C) 2010
 * by Dimok
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
 * for WiiXplorer 2010
 ***************************************************************************/
#ifndef _MUSICPLAYER_H_
#define _MUSICPLAYER_H_

#include <vector>
#include <string>
#include "Controls/CThread.h"
#include "GUI/sigslot.h"
#include "SoundOperations/gui_sound.h"

enum
{
	ONCE = 0,
	LOOP,
	SHUFFLE_MUSIC,
	PLAYLIST_LOOP,
	MAX_LOOP_MODES
};

class MusicPlayer : public CThread, public sigslot::has_slots<>
{
	public:
		static MusicPlayer * Instance() { if(!instance) instance = new MusicPlayer(); return instance; }
		static void DestroyInstance() { delete instance; instance = NULL; }

		bool Load(const char *path, const bool forceNewRandom = false);
		bool LoadStandard();
		bool ParsePath(const char * folderpath);
		void Resume();
		void Play() { MainSound->Play(); }
		bool PlayNext();
		bool PlayPrevious();
		bool PlayRandom();
		void Pause() { if(IsStopped()) return; MainSound->Pause(); Paused = true; }
		void Stop();
		void SetLoop(u8 mode);
		void SetVolume(int volume) { MainSound->SetVolume(volume); }
		bool IsStopped() { return Stopped; }
		int GetPlayListCount() { return (int)PlayList.size(); }
	protected:
		MusicPlayer();
		virtual ~MusicPlayer();
		void executeThread(void);
		int findIndex(std::vector<std::string> &v, std::string file);
		void Randomize();
		void AddEntry(const char * filename);
		void EmptyList();

		bool Paused;
		bool Stopped;
		bool ExitRequested;
		bool Switching;

		std::vector<std::string> PlayList;
		std::vector<std::string> RandomPlayList;
		std::string loadPathThreaded;

		GuiSound * MainSound;

		static MusicPlayer *instance;
		int currentPlaying;
		char * currentPath;
		int rndCurrentPlaying;
};

#endif
