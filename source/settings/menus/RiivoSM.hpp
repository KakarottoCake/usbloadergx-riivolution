/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Per-game Riivolution configuration page: pick an XML under
 * <device>:/riivolution and cycle each <option>'s <choice>. Selection is
 * stored per-game in GameCFG (RiivoPath + RiivoConfig).
 *
 * Only XMLs that name THIS game are offered. The scan parses each candidate
 * and keeps the ones whose <id> matches, so a Newer SMBW patch no longer
 * appears in the cycle while a Galaxy disc is selected.
 ***************************************************************************/
#ifndef RIIVO_SM_HPP
#define RIIVO_SM_HPP

#include <vector>
#include "SettingsMenu.hpp"
#include "settings/CGameSettings.h"
#include "riivo/RiivoParser.hpp"
#include "riivo/RiivoConfig.hpp"

class RiivoSM : public SettingsMenu
{
	public:
		RiivoSM(struct discHdr *Header);
		virtual ~RiivoSM();
	protected:
		void SetOptionNames();
		void SetOptionValues();
		int GetMenuInternal();

		//! (Re)parse GameConfig.RiivoPath into `disc` and apply the saved selection.
		void ReloadXml();
		//! Populate `xmlFiles` with the XMLs on SD/USB that name this game.
		void ScanXmlFiles();
		//! Rebuild `flatOptions` (display order) from `disc`.
		void RebuildFlatOptions();
		//! Persist the current selection back into GameConfig.
		void StoreSelection();

		//! One XML that matches this game, with everything the page shows about
		//! it. Gathered during the scan so drawing never re-reads the card.
		struct XmlChoice
		{
			std::string path;    //!< "usb1:/riivolution/Foo.xml"
			std::string device;  //!< "SD card", "USB drive 1"
			std::string label;   //!< the mod's own name, else the file name
		};

		//! Rows above the <option> rows: the selector, and - once something is
		//! selected - the path and the device it lives on. The option rows
		//! start here, and a click below it is on one of the two read-only
		//! lines, so the count has to come from one place.
		int HeaderRows() const;

		struct discHdr *Header;
		GameCFG GameConfig;
		OptionList GuiOptions;

		Riivo::Disc disc;
		bool discLoaded;
		std::vector<XmlChoice> xmlFiles;          // candidates for THIS game
		std::vector<Riivo::Option *> flatOptions; // section-flattened, display order

		GuiText *saveBtnTxt;
		GuiImage *saveBtnImg;
		GuiButton *saveBtn;
};

#endif
