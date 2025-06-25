#include <string.h>
#include <ogcsys.h>
#include <malloc.h>
#include <stdio.h>

#include "lstub.h"
#include "wad/nandtitle.h"
#include "settings/CSettings.h"
#include "stub_bin.h"

s32 Set_Stub(u64 reqID)
{
	if (!hbcStubAvailable())
		return 0;
	if (!NandTitles.Exists(reqID))
		return WII_EINSTALL;

	char *stub = (char *)0x80002662;

	stub[0] = TITLE_7(reqID);
	stub[1] = TITLE_6(reqID);
	stub[8] = TITLE_5(reqID);
	stub[9] = TITLE_4(reqID);
	stub[4] = TITLE_3(reqID);
	stub[5] = TITLE_2(reqID);
	stub[12] = TITLE_1(reqID);
	stub[13] = ((u8)(reqID));

	DCFlushRange(stub, 0x10);
	return 1;
}

void loadStub()
{
	char *stubLoc = (char *)0x80001800;
	memcpy(stubLoc, stub_bin, stub_bin_size);
	DCFlushRange(stubLoc, stub_bin_size);
}

u8 hbcStubAvailable()
{
	char *sig = (char *)0x80001804;
	return (strncmp(sig, "STUBHAXX", 8) == 0);
}

u64 returnTo(bool onlyHBC)
{
	if (!onlyHBC)
	{
		// Custom setting
		u64 tid = NandTitles.FindU32(Settings.returnTo);
		if (tid > 0)
			return tid;
		// UNEO
		if (NandTitles.Exists(0x00010001554E454FLL))
			return 0x00010001554E454FLL;
		// ULNR
		if (NandTitles.Exists(0x00010001554C4E52LL))
			return 0x00010001554C4E52LL;
		// IDCL
		if (NandTitles.Exists(0x000100014944434CLL))
			return 0x000100014944434CLL;
	}
	// OHBC
	if (NandTitles.Exists(0x000100014F484243LL))
		return 0x000100014F484243LL;
	// LULZ
	if (NandTitles.Exists(0x000100014C554C5ALL))
		return 0x000100014C554C5ALL;
	// 1.0.7
	if (NandTitles.Exists(0x00010001AF1BF516LL))
		return 0x00010001AF1BF516LL;
	// JODI
	if (NandTitles.Exists(0x000100014A4F4449LL))
		return 0x000100014A4F4449LL;
	// HAXX
	if (NandTitles.Exists(0x0001000148415858LL))
		return 0x0001000148415858LL;
	// System menu
	return 0x100000002LL;
}
