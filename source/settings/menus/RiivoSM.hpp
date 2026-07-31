/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Per-game Riivolution configuration page: pick an XML under
 * <device>:/riivolution and cycle each <option>'s <choice>. Selection is
 * stored per-game in GameCFG (RiivoPath + RiivoConfig).
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
		//! Populate `xmlFiles` with candidate XMLs found on SD/USB.
		void ScanXmlFiles();
		//! Rebuild `flatOptions` (display order) from `disc`.
		void RebuildFlatOptions();
		//! Persist the current selection back into GameConfig.
		void StoreSelection();

		struct discHdr *Header;
		GameCFG GameConfig;
		OptionList GuiOptions;

		Riivo::Disc disc;
		bool discLoaded;
		std::vector<std::string> xmlFiles;        // candidate XML paths
		std::vector<Riivo::Option *> flatOptions; // section-flattened, display order

		GuiText *saveBtnTxt;
		GuiImage *saveBtnImg;
		GuiButton *saveBtn;
};

#endif
