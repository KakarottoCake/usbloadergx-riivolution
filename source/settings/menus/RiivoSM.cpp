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
#include "riivo/RiivoSdWarning.hpp"
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

//! "usb1:/riivolution/Foo.xml" -> "USB drive 1"; "sd:/..." -> "SD card".
//! Which drive the mod is on is not a detail: the cIOS serves every fragment
//! from one device, so a mod on the other drive is refused at boot. Saying it
//! here is cheaper than finding out from a failed launch.
static std::string DeviceLabel(const std::string &path)
{
	size_t colon = path.find(':');
	if (colon == std::string::npos)
		return "";
	const std::string dev = path.substr(0, colon);
	//! Kept short on purpose: in this browser a value is clipped at 100px
	//! (optionValLen), so "USB drive 1" would arrive as "USB dri...".
	if (dev == "sd")
		return tr( "SD" );
	if (dev.size() > 3 && strncmp(dev.c_str(), "usb", 3) == 0)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%s %s", tr( "USB" ), dev.c_str() + 3);
		return buf;
	}
	return dev;
}

//! The mod's own name for itself - the first <section> that has one. Riivolution
//! XMLs carry no title, and a file name is often just the game's initials, so
//! this is the closest thing to something a person recognises.
static std::string ModLabel(const Riivo::Disc &d, const std::string &path)
{
	for (size_t s = 0; s < d.sections.size(); ++s)
		if (!d.sections[s].name.empty())
			return d.sections[s].name;
	return BaseName(path);
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

	char id[7];
	snprintf(id, sizeof(id), "%.6s", (const char *) Header->id);

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

			//! Read it now and keep it only if it names this game. The <id>
			//! is inside the file, so there is no way to tell from the name -
			//! which is why every mod on the card used to be offered for every
			//! game, and cycling through a Galaxy disc's options walked past
			//! patches for titles it has nothing to do with.
			Riivo::Disc probe;
			std::string err;
			if (!Riivo::ParseFile(full.c_str(), probe, &err))
			{
				gprintf("Riivo: %s did not parse: %s\n", full.c_str(), err.c_str());
				continue;
			}
			if (!probe.IsValidForGame(id, 0, 0))
				continue;

			XmlChoice choice;
			choice.path = full;
			choice.device = DeviceLabel(full);
			choice.label = ModLabel(probe, full);
			xmlFiles.push_back(choice);
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

int RiivoSM::HeaderRows() const
{
	return GameConfig.RiivoPath.empty() ? 1 : 4;
}

void RiivoSM::SetOptionNames()
{
	int Idx = 0;

	//! Row 0: the XML selector.
	Options->SetName(Idx++, "%s", tr( "Riivolution XML" ));

	if (!GameConfig.RiivoPath.empty())
	{
		//! The mod's name and its path each get a row to themselves. As values
		//! they were crushed into the right-hand column, which this browser
		//! clips at 100px (optionValLen) - useless for the two strings you
		//! actually need to read back. A row whose value is a single space has
		//! its NAME drawn across the full width, so both go in the name.
		Options->SetName(Idx++, "%s", discLoaded
						 ? ModLabel(disc, GameConfig.RiivoPath).c_str()
						 : BaseName(GameConfig.RiivoPath));
		Options->SetName(Idx++, "%s", GameConfig.RiivoPath.c_str());
		Options->SetName(Idx++, "%s", tr( "Mod is on" ));
	}

	//! One row per option (section-flattened).
	for (size_t i = 0; i < flatOptions.size(); ++i)
		Options->SetName(Idx++, "%s", flatOptions[i]->name.c_str());
}

void RiivoSM::SetOptionValues()
{
	int Idx = 0;

	//! Row 0: short by necessity - a value wider than 100px is clipped, so the
	//! readable strings live in the full-width rows below instead.
	if (GameConfig.RiivoPath.empty())
		Options->SetValue(Idx++, "%s", xmlFiles.empty()
						  ? tr( "None" ) : tr( "OFF" ));
	else if (!discLoaded)
		Options->SetValue(Idx++, "%s", tr( "Parse error" ));
	else
	{
		char id[7];
		snprintf(id, sizeof(id), "%.6s", (const char *) Header->id);
		//! "Other game" is reachable only from a selection saved before the
		//! scan filtered by game, or from a card changed underneath it.
		Options->SetValue(Idx++, "%s", disc.IsValidForGame(id, 0, 0)
						  ? tr( "ON" ) : tr( "Other game" ));
	}

	if (!GameConfig.RiivoPath.empty())
	{
		//! Exactly one space each: that is what earns the two rows above the
		//! full width for their names.
		Options->SetValue(Idx++, " ");
		Options->SetValue(Idx++, " ");
		Options->SetValue(Idx++, "%s", DeviceLabel(GameConfig.RiivoPath).c_str());
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
			if (xmlFiles[i].path == GameConfig.RiivoPath)
			{
				current = (int) i;
				break;
			}

		int next = current + 1; // -1(OFF)->0, last->size (== OFF)
		std::string newPath = (next >= (int) xmlFiles.size())
							  ? std::string() : xmlFiles[next].path;

		if (newPath != GameConfig.RiivoPath)
		{
			GameConfig.RiivoPath = newPath;
			GameConfig.RiivoConfig.clear(); // selection doesn't carry across files
			ReloadXml();
			Options->ClearList();
			SetOptionNames();
			SetOptionValues();
			//! Said the moment the mod is chosen, not only at launch: the
			//! choice is one button press old here and costs nothing to
			//! undo, whereas finding out at launch costs a reset. The
			//! selection is kept either way - this warns, it does not
			//! refuse - and the same warning appears again before the game
			//! starts, because a choice saved in an earlier session never
			//! passes through this point.
			if (Riivo::ModPathIsOnSd(GameConfig.RiivoPath))
				WindowPrompt(Riivo::SdModWarningTitle(), Riivo::SdModWarning(),
							 tr( "OK" ));
		}
		return MENU_NONE;
	}

	//! The path and device rows are there to be read, not clicked.
	const int head = HeaderRows();
	if (ret < head)
		return MENU_NONE;

	//! Option rows: cycle the choice, wrapping through Disabled (0).
	int optIdx = ret - head;
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
