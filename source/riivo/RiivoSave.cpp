/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <stdio.h>
#include "RiivoSave.hpp"
#include "RiivoConfig.hpp"
#include "FileOperations/fileops.h"
#include "gecko.h"

namespace Riivo
{
	bool SetupSavegame(const ResolvedPatchSet &set, const std::string &device,
					   const char *gameId, bool nandEmuEnabled,
					   const char *defaultNandPath, std::string &outNandPath)
	{
		if (set.savegames.empty())
			return false;

		const ResolvedSavegame &sg = set.savegames[0];
		const std::string modPath = JoinPath(device, sg.root, sg.external);

		if (!nandEmuEnabled)
		{
			gprintf("Riivo save: <savegame> requires NAND emulation enabled; "
					"not redirecting to %s\n", modPath.c_str());
			return false;
		}

		// Save data class for disc games: /title/00010000/<id-hex>
		char idhex[9];
		snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x",
				 (u8) gameId[0], (u8) gameId[1], (u8) gameId[2], (u8) gameId[3]);
		const std::string titleRel = std::string("/title/00010000/") + idhex;
		const std::string modTitle = modPath + titleRel;

		//! Clone the existing save on first run (mod title.tmd absent) if a source exists.
		if (sg.clone && defaultNandPath && *defaultNandPath)
		{
			const std::string modTmd = modTitle + "/title.tmd";
			if (!CheckFile(modTmd.c_str()))
			{
				const std::string srcTitle = std::string(defaultNandPath) + titleRel;
				const std::string srcTmd = srcTitle + "/title.tmd";
				if (CheckFile(srcTmd.c_str()))
				{
					gprintf("Riivo save: cloning %s -> %s\n", srcTitle.c_str(), modTitle.c_str());
					CreateSubfolder(modTitle.c_str());
					CopyDirectory(srcTitle.c_str(), modTitle.c_str());
				}
				else
					gprintf("Riivo save: no existing save to clone at %s\n", srcTitle.c_str());
			}
		}

		outNandPath = modPath;
		gprintf("Riivo save: redirecting NAND-emu path to %s\n", modPath.c_str());
		return true;
	}
}
