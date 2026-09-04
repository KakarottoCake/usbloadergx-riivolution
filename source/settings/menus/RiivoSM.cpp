/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Per-game Riivolution configuration page. Modeled on GameLoadSM.
 ***************************************************************************/
#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <gccore.h>
#include "settings/CSettings.h"
#include "themes/CTheme.h"
#include "prompts/PromptWindows.h"
#include "language/gettext.h"
#include "RiivoSM.hpp"
#include "gecko.h"

//! Devices scanned for a /riivolution folder.
static const char *RiivoDevices[] = { "sd", "usb1", "usb2", "usb3", "usb4" };

static const char *BaseName(const std::string &path)
{
	size_t slash = path.find_last_of('/');
	return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

static bool HasXmlExt(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && strcasecmp(dot, ".xml") == 0;
}

RiivoSM::RiivoSM(struct discHdr *hdr)
	: SettingsMenu(tr("Riivolution"), &GuiOptions, MENU_NONE),
	  Header(hdr), discLoaded(false)
{
	GameConfig = *GameSettings.GetGameCFG((const char *) Header->id);

	ScanXmlFiles();
	ReloadXml();

	if (!btnOutline)
		btnOutline = Resources::GetImageData("button_dialogue_box.png");
	if (!trigA)
		trigA = new GuiTrigger();
	trigA->SetSimpleTrigger(-1, WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A, PAD_BUTTON_A);

	saveBtnTxt = new GuiText(tr( "Save" ), 22, thColor("r=0 g=0 b=0 a=255 - prompt windows button text color"));
	saveBtnTxt->SetMaxWidth(btnOutline->GetWidth() - 30);
	saveBtnImg = new GuiImage(btnOutline);
	if (Settings.wsprompt == ON)
	{
		saveBtnTxt->SetWidescreen(Settings.widescreen);
		saveBtnImg->SetWidescreen(Settings.widescreen);
	}
	saveBtn = new GuiButton(saveBtnImg, saveBtnImg, 2, 3, 180, 400, trigA, btnSoundOver, btnSoundClick2, 1);
	saveBtn->SetLabel(saveBtnTxt);
	Append(saveBtn);

	SetOptionNames();
	SetOptionValues();
}

RiivoSM::~RiivoSM()
{
	HaltGui();
	//! The rest is destroyed in SettingsMenu.cpp
	Remove(saveBtn);
	delete saveBtnTxt;
	delete saveBtnImg;
	delete saveBtn;
	ResumeGui();
}

void RiivoSM::ScanXmlFiles()
{
	xmlFiles.clear();
	for (size_t d = 0; d < sizeof(RiivoDevices) / sizeof(RiivoDevices[0]); ++d)
	{
		char dirpath[64];
		snprintf(dirpath, sizeof(dirpath), "%s:/riivolution", RiivoDevices[d]);
		DIR *dir = opendir(dirpath);
		if (!dir)
			continue;
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL)
		{
			if (!HasXmlExt(ent->d_name))
				continue;
			std::string full = dirpath;
			full += "/";
			full += ent->d_name;
			xmlFiles.push_back(full);
		}
		closedir(dir);
	}
}

void RiivoSM::RebuildFlatOptions()
{
	flatOptions.clear();
	for (size_t s = 0; s < disc.sections.size(); ++s)
		for (size_t o = 0; o < disc.sections[s].options.size(); ++o)
			flatOptions.push_back(&disc.sections[s].options[o]);
}

void RiivoSM::ReloadXml()
{
	disc = Riivo::Disc();
	discLoaded = false;
	flatOptions.clear();

	if (GameConfig.RiivoPath.empty())
		return;

	std::string err;
	if (!Riivo::ParseFile(GameConfig.RiivoPath.c_str(), disc, &err))
	{
		gprintf("Riivo: parse failed: %s\n", err.c_str());
		return;
	}
	discLoaded = true;
	if (!GameConfig.RiivoConfig.empty())
		Riivo::ApplySelection(disc, GameConfig.RiivoConfig);
	RebuildFlatOptions();
}

void RiivoSM::StoreSelection()
{
	if (discLoaded)
		GameConfig.RiivoConfig = Riivo::SerializeSelection(disc);
	else
		GameConfig.RiivoConfig.clear();
}

void RiivoSM::SetOptionNames()
{
	int Idx = 0;

	//! Row 0: the XML selector.
	Options->SetName(Idx++, "%s", tr( "Riivolution XML" ));

	//! One row per option (section-flattened).
	for (size_t i = 0; i < flatOptions.size(); ++i)
		Options->SetName(Idx++, "%s", flatOptions[i]->name.c_str());
}

void RiivoSM::SetOptionValues()
{
	int Idx = 0;

	//! Row 0: selected XML file name, or why there isn't one.
	if (GameConfig.RiivoPath.empty())
		Options->SetValue(Idx++, "%s", xmlFiles.empty()
						  ? tr( "None found" ) : tr( "OFF" ));
	else if (!discLoaded)
		Options->SetValue(Idx++, "%s (%s)", BaseName(GameConfig.RiivoPath), tr( "parse error" ));
	else
	{
		char id[7];
		snprintf(id, sizeof(id), "%.6s", (const char *) Header->id);
		if (!disc.IsValidForGame(id, 0, 0))
			Options->SetValue(Idx++, "%s (%s)", BaseName(GameConfig.RiivoPath), tr( "other game" ));
		else
			Options->SetValue(Idx++, "%s", BaseName(GameConfig.RiivoPath));
	}

	//! Option rows: current choice name, or Disabled.
	for (size_t i = 0; i < flatOptions.size(); ++i)
	{
		const Riivo::Option *opt = flatOptions[i];
		int sel = opt->selectedChoice;
		if (sel <= 0 || sel > (int) opt->choices.size())
			Options->SetValue(Idx++, "%s", tr( "Disabled" ));
		else
			Options->SetValue(Idx++, "%s", opt->choices[sel - 1].name.c_str());
	}
}

int RiivoSM::GetMenuInternal()
{
	if (saveBtn->GetState() == STATE_CLICKED)
	{
		StoreSelection();
		if (GameSettings.AddGame(GameConfig) && GameSettings.Save())
			WindowPrompt(tr( "Successfully Saved" ), 0, tr( "OK" ));
		else
			WindowPrompt(tr( "Save Failed. No device inserted?" ), 0, tr( "OK" ));

		saveBtn->ResetState();
	}

	int ret = optionBrowser->GetClickedOption();
	if (ret < 0)
		return MENU_NONE;

	//! Row 0: cycle the XML selection: OFF -> file[0] -> ... -> file[n-1] -> OFF.
	if (ret == 0)
	{
		// Find the current index in the cycle (-1 == OFF).
		int current = -1;
		for (size_t i = 0; i < xmlFiles.size(); ++i)
			if (xmlFiles[i] == GameConfig.RiivoPath)
			{
				current = (int) i;
				break;
			}

		int next = current + 1; // -1(OFF)->0, last->size (== OFF)
		std::string newPath = (next >= (int) xmlFiles.size()) ? std::string() : xmlFiles[next];

		if (newPath != GameConfig.RiivoPath)
		{
			GameConfig.RiivoPath = newPath;
			GameConfig.RiivoConfig.clear(); // selection doesn't carry across files
			ReloadXml();
			Options->ClearList();
			SetOptionNames();
			SetOptionValues();
		}
		return MENU_NONE;
	}

	//! Option rows: cycle the choice, wrapping through Disabled (0).
	int optIdx = ret - 1;
	if (optIdx >= 0 && optIdx < (int) flatOptions.size())
	{
		Riivo::Option *opt = flatOptions[optIdx];
		int count = (int) opt->choices.size();
		if (++opt->selectedChoice > count)
			opt->selectedChoice = 0; // 0 == Disabled
	}

	SetOptionValues();
	return MENU_NONE;
}
