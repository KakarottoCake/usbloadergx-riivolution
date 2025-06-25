/****************************************************************************
 * Copyright (C) 2025 blackb0x
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
 ***************************************************************************/
#include <dirent.h>
#include "UpdateSM.hpp"
#include "language/gettext.h"
#include "language/UpdateLanguage.h"
#include "network/networkops.h"
#include "network/update.h"
#include "prompts/PromptWindows.h"
#include "sys.h"
#include "utils/ShowError.h"
#include "FileOperations/fileops.h"

UpdateSM::UpdateSM()
	: SettingsMenu(tr("Update Menu"), &GuiOptions, MENU_NONE)
{
	SetOptionNames();
	initNetwork();
	return;
}

void UpdateSM::SetOptionNames()
{
	int Idx = 0;

	Options->SetName(Idx++, "%s", tr("Update USB Loader GX"));
	Options->SetName(Idx++, "%s", tr("Update Language Files"));
	Options->SetName(Idx++, "%s", tr("Update Cheat Files"));
	Options->SetName(Idx++, "%s", tr("Update Cover Files"));
	Options->SetName(Idx++, "%s", tr("Update WiiTDB.xml"));
	Options->SetName(Idx++, "%s", tr("Update Nintendont"));
}

int UpdateSM::GetMenuInternal()
{
	int ret = optionBrowser->GetClickedOption();

	if (ret < 0)
		return MENU_NONE;	

	if(!IsNetworkInit())
	{
		ShowError(tr("Network is not initalized."));
		return MENU_NONE;
	}

	int Idx = -1;

	//! Settings: Update USB Loader GX
	if (ret == ++Idx)
	{
		if (ApplicationDownload() > 0)
		{
			UpdateGameTDB(); // Automatic language selection depends on this, plus better game titles
			// DownloadAllLanguageFiles();
			WindowPrompt(tr("Successfully Updated"), tr("Restarting..."), 0, 0, 0, 0, 150);
			RebootApp();
		}
	}

	//! Settings: Update Language Files
	else if (ret == ++Idx)
	{
		if (UpdateLanguageFiles() > 0)
			WindowPrompt(tr("Successfully Updated"), 0, tr("OK"));
	}

	//! Settings: Update Cheat Files
	else if (ret == ++Idx)
	{
		if (UpdateCheats() > 0)
			WindowPrompt(tr("Successfully Updated"), 0, tr("OK"));
		else
			WindowPrompt(tr("Update Failed"), 0, tr("OK"));
	}

	//! Settings: Update Cover Files
	else if (ret == ++Idx)
	{
		if (!GetDirectorySize(Settings.covers_path))
		{
			UpdateCovers();
		}
		else if (WindowPrompt(tr("Cover Download"), tr("Are you sure you want to redownload everything?"), tr("Yes"), tr("Cancel")) > 0)
		{
			RemoveDirectory(Settings.covers_path);
			UpdateCovers();
		}
	}

	//! Settings: Update WiiTDB.xml
	else if (ret == ++Idx)
	{
		int gameTDB = UpdateGameTDB();
		if (gameTDB == -2)
			WindowPrompt(tr("WiiTDB.xml is up to date."), 0, tr("OK"));
		else if (gameTDB == -1)
			WindowPrompt(tr("Update Failed"), 0, tr("OK"));
		else if (gameTDB > 0)
			WindowPrompt(tr("Successfully Updated"), 0, tr("OK"));
	}

	//! Settings: Update Nintendont
	else if (ret == ++Idx)
	{
		if (UpdateNintendont() > 0)
			WindowPrompt(tr("Successfully Updated"), 0, tr("OK"));
		else
			WindowPrompt(tr("Update Failed"), 0, tr("OK"));
	}

	return MENU_NONE;
}
