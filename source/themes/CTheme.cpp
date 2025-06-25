/****************************************************************************
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
 ***************************************************************************/
#include <ogcsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CTheme.h"
#include "GUI/gui.h"
#include "settings/CSettings.h"
#include "banner/OpeningBNR.hpp"
#include "FileOperations/fileops.h"
#include "SystemMenu/SystemMenuResources.h"
#include "menu/menus.h"
#include "wad/nandtitle.h"
#include "FreeTypeGX.h"

FreeTypeGX *fontSystem = NULL;
static FT_Byte *customFont = NULL;
static u32 customFontSize = 0;

bool Theme::ShowTooltips = true;

void Theme::Reload()
{
	HaltGui();
	mainWindow->Remove(bgImg);
	for (int i = 0; i < 4; ++i)
	{
		char image[50];
		snprintf(image, sizeof(image), "player%i_point.png", i + 1);
		pointer[i]->SetImage(image);
	}
	delete btnSoundClick;
	delete btnSoundClick2;
	delete btnSoundOver;
	btnSoundClick = new GuiSound(Resources::GetFile("button_click.ogg"), Resources::GetFileSize("button_click.ogg"), Settings.sfxvolume);
	btnSoundClick2 = new GuiSound(Resources::GetFile("button_click2.ogg"), Resources::GetFileSize("button_click2.ogg"), Settings.sfxvolume);
	btnSoundOver = new GuiSound(Resources::GetFile("button_over.ogg"), Resources::GetFileSize("button_over.ogg"), Settings.sfxvolume);
	delete background;
	background = Resources::GetImageData(Settings.widescreen ? "wbackground.png" : "background.png");
	delete bgImg;
	bgImg = new GuiImage(background);
	mainWindow->Append(bgImg);
	ResumeGui();
}

void Theme::CleanUp()
{
	ThemeCleanUp();
	Resources::Clear();
	ClearFontData();
}

void Theme::SetDefault()
{
	ShowTooltips = true;
	CleanUp();
	strcpy(Settings.theme, "");
	LoadFont("");
	LoadNewTheme();
	Settings.LayoutVersion = 2;
}

bool Theme::Load(const char *theme_file_path)
{
	u8 ThemeLayoutVersion = 1;
	char Foldername[256] = {0};
	bool foundLayout = false, foundFolder = false;

	FILE *file = fopen(theme_file_path, "rb");
	if (!file)
		return false;

	char line[300];
	while (fgets(line, sizeof(line), file))
	{
		if (!foundLayout)
		{
			char *ptr = strcasestr(line, "Theme-Layout:");
			if (ptr)
			{
				ptr += strlen("Theme-Layout:");
				while (*ptr == ' ')
					ptr++;
				ThemeLayoutVersion = atoi(ptr);
				foundLayout = true;
			}
		}
		if (!foundFolder)
		{
			char *ptr = strcasestr(line, "Image-Folder:");
			if (ptr)
			{
				ptr += strlen("Image-Folder:");
				while (*ptr == ' ')
					ptr++;
				char *end = ptr;
				while (*end != '\\' && *end != '"' && *end != '\0' && *end != '\n' && *end != '\r')
					end++;
				size_t len = end - ptr;
				if (len > 0 && len < sizeof(Foldername))
				{
					strncpy(Foldername, ptr, len);
					Foldername[len] = '\0';
					foundFolder = true;
				}
			}
		}
		if (foundLayout && foundFolder)
			break;
	}
	fclose(file);

	if (!foundFolder || strlen(Foldername) == 0)
	{
		if (ThemeLayoutVersion >= 2)
			LoadNewTheme();
		return false;
	}

	// Checks passed, now apply theme
	ThemeCleanUp();

	if (ThemeLayoutVersion >= 2)
		LoadNewTheme();

	if (!LoadTheme(theme_file_path))
	{
		if (ThemeLayoutVersion >= 2)
			LoadNewTheme();
		return false;
	}

	Theme::ShowTooltips = (thInt("1 - Enable tooltips: 0 for off and 1 for on") != 0);

	// Build theme path
	std::string themePath(theme_file_path);
	size_t lastSlash = themePath.find_last_of('/');
	if (lastSlash != std::string::npos)
		themePath = themePath.substr(0, lastSlash);
	themePath += "/";
	themePath += Foldername;

	// Try loading resources
	if (!Resources::LoadFiles(themePath.c_str()))
	{
		std::string themeFilePathStr(theme_file_path);
		size_t fileNameSlash = themeFilePathStr.find_last_of('/');
		std::string themeFilename = (fileNameSlash != std::string::npos)
										? themeFilePathStr.substr(fileNameSlash + 1)
										: themeFilePathStr;
		size_t dot = themeFilename.find_last_of('.');
		if (dot != std::string::npos)
			themeFilename = themeFilename.substr(0, dot);
		lastSlash = themePath.find_last_of('/');
		if (lastSlash != std::string::npos)
			themePath = themePath.substr(0, lastSlash);
		themePath += "/";
		themePath += themeFilename;
		Resources::LoadFiles(themePath.c_str());
	}

	// Override font.ttf with the themes font.ttf
	std::string fontPath = themePath + "/font.ttf";
	if (CheckFile(fontPath.c_str()))
		Theme::LoadFont(themePath.c_str());
	else
		Theme::LoadFont("");

	Settings.LayoutVersion = ThemeLayoutVersion;
	return true;
}

bool Theme::LoadFont(const char *path)
{
	char FontPath[300];
	bool result = false;
	FILE *pfile = NULL;

	delete[] customFont;
	customFont = NULL;
	customFontSize = 0;

	snprintf(FontPath, sizeof(FontPath), "%s/font.ttf", path);

	pfile = fopen(FontPath, "rb");

	if (pfile)
	{
		fseek(pfile, 0, SEEK_END);
		customFontSize = ftell(pfile);
		rewind(pfile);

		customFont = new (std::nothrow) FT_Byte[customFontSize];
		if (customFont)
		{
			fread(customFont, 1, customFontSize, pfile);
			result = true;
		}
		fclose(pfile);
	}

	bool isSystemFont = false;
	FT_Byte *loadedFont = customFont;
	u32 loadedFontSize = customFontSize;

	if (!loadedFont && Settings.UseSystemFont)
	{
		//! Default to system font if no custom is loaded
		loadedFont = (u8 *)SystemMenuResources::Instance()->GetSystemFont();
		loadedFontSize = SystemMenuResources::Instance()->GetSystemFontSize();
		if (loadedFont)
			isSystemFont = true;
	}
	if (!loadedFont)
	{
		loadedFont = (FT_Byte *)Resources::GetFile("font.ttf");
		loadedFontSize = Resources::GetFileSize("font.ttf");
	}

	delete fontSystem;

	fontSystem = new FreeTypeGX(loadedFont, loadedFontSize, isSystemFont);

	return result;
}

void Theme::ClearFontData()
{
	if (fontSystem)
		delete fontSystem;
	fontSystem = NULL;

	if (customFont)
		delete[] customFont;
	customFont = NULL;
	customFontSize = 0;
}
