#include <sdcard/wiisd_io.h>

#define SDHC_Init() __io_wiisd.startup()
#define SDHC_Close() __io_wiisd.shutdown()
#define SDHC_IsInserted __io_wiisd.isInserted()
#define SDHC_ReadSectors(x, y, z) __io_wiisd.readSectors(x, y, z)
#define SDHC_WriteSectors(x, y, z) __io_wiisd.writeSectors(x, y, z)
