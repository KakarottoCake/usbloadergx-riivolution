/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * The one warning that has to be said in two places: a mod on the SD card.
 *
 * A mod must live on the same drive as the game (the cIOS serves every
 * fragment from one device), so choosing a mod on SD also means loading the
 * game from SD. That path does not survive the moment the loader hands its
 * fragment list to the cIOS: stock USB Loader GX unmounts SD around
 * set_frag_list and mounts it again afterwards, and on a Riivolution boot
 * the card does not come back. The boot log stops at the unmount marker -
 * by construction, because the log lives on the card that just went away -
 * and the console sits on a black screen until it is reset.
 *
 * So this is said twice: when the mod is chosen, while the choice is easy
 * to undo, and again before launch, because a choice saved in an earlier
 * session never passes through the first one. Same wording both times, from
 * here, so the two cannot drift apart.
 *
 * Detection delegates to DeviceHandler::PathToDriveType, the same call the
 * boot path uses to decide which drive a mod is on. A private string rule
 * here could disagree with it; this cannot.
 ***************************************************************************/
#ifndef RIIVO_SD_WARNING_HPP_
#define RIIVO_SD_WARNING_HPP_

#include <string>
#include "Controls/DeviceHandler.hpp"
#include "language/gettext.h"

namespace Riivo
{
	//! True when this XML - and therefore the mod beside it - is on SD.
	inline bool ModPathIsOnSd(const std::string &xmlPath)
	{
		if (xmlPath.empty())
			return false;
		return DeviceHandler::PathToDriveType(xmlPath.c_str()) == SD;
	}

	//! The headline goes in the title, where it is drawn larger and cannot
	//! be pushed off by wrapping.
	inline const char *SdModWarningTitle()
	{
		return tr( "Riivolution: the mod is on the SD card" );
	}

	//! Plain language, no jargon: what was detected, what it means, and the
	//! one thing that fixes it. Deliberately states the outcome as certain,
	//! because it is not a risk the user can manage - every SD mod boot ends
	//! the same way, and a hedge here just costs someone a reset.
	//!
	//! Length is not free: the prompt box is 472x320 with the message at
	//! SetMaxWidth(430) in a 22px font, between the title and the buttons,
	//! which is about seven lines. This sits at the same length as the
	//! longest message the loader already ships, so it wraps no worse than
	//! what testers have been reading all along.
	inline const char *SdModWarning()
	{
		return tr( "The mod selected was detected on the SD card. USB Loader GX supports SD loading, but a mod on SD will not work: the loader has to release the card just before the game starts, and on SD it never comes back. The game will NOT load - a black screen, every time. Put the game and the mod on a USB drive." );
	}
}

#endif
