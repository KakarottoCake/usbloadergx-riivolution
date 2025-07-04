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
#include <sys/statvfs.h>
#include "settings/CSettings.h"
#include "usbloader/GameList.h"
#include "Controls/PartitionHandle.h"
#include "utils/tools.h"
#include "usbloader/wbfs.h"

static float cached_free = 0.0f;
static float cached_used = 0.0f;
static int cached_gameCount = -1;
static bool cached_multiPartitions = false;
static bool cache_valid = false;

bool GetPartitionDiskSpace(float *total_free, float *total_used)
{
	if (!total_free || !total_used)
		return false;

	int currentGameCount = gameList.GameCount();
	bool currentMultiPartitions = Settings.MultiplePartitions;

	// Return cached results if nothing changed
	if (cache_valid && cached_gameCount == currentGameCount && cached_multiPartitions == currentMultiPartitions)
	{
		*total_free = cached_free;
		*total_used = cached_used;
		return true;
	}

	float free = 0.0f, used = 0.0f;

	if (Settings.SDMode)
	{
		struct statvfs vfs;
		if (statvfs("sd:/", &vfs) == 0)
		{
			u64 total_blocks = vfs.f_blocks ? vfs.f_blocks : vfs.f_bfree;
			u64 free_blocks = vfs.f_bavail ? vfs.f_bavail : vfs.f_bfree;

			u64 free_bytes = (u64)vfs.f_bsize * (u64)free_blocks;
			u64 total_bytes = (u64)vfs.f_bsize * (u64)total_blocks;
			u64 used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
			free = (float)free_bytes / GB_SIZE;
			used = (float)used_bytes / GB_SIZE;
		}
	}
	else
	{
		PartitionHandle *usbHandle = DeviceHandler::Instance()->GetUSBHandleFromPartition(Settings.partition);
		int portPart = DeviceHandler::PartitionToPortPartition(Settings.partition);
		if (usbHandle)
		{
			// Only check the current WBFS partition
			if (strncmp(usbHandle->GetFSName(portPart), "WBFS", 4) == 0)
			{
				float wused = 0.0f, wfree = 0.0f;
				if (WBFS_DiskSpace(&wused, &wfree) >= 0)
				{
					free = wfree;
					used = wused;
				}
			}
			else if (Settings.MultiplePartitions)
			{
				// Sum all FAT/NTFS/EXT partitions
				int partCount = DeviceHandler::GetUSBPartitionCount();
				for (int i = 0; i < partCount; ++i)
				{
					PartitionHandle *usbHandle2 = DeviceHandler::Instance()->GetUSBHandleFromPartition(i);
					int portPart2 = DeviceHandler::PartitionToPortPartition(i);
					if (!usbHandle2)
						continue;

					const char *mountName = usbHandle2->MountName(portPart2);
					if (!mountName || !mountName[0])
						continue;

					char mountPath[32];
					snprintf(mountPath, sizeof(mountPath), "%s:/", mountName);
					struct statvfs vfs;
					if (statvfs(mountPath, &vfs) == 0)
					{
						u64 total_blocks = vfs.f_blocks ? vfs.f_blocks : vfs.f_bfree;
						u64 free_blocks = vfs.f_bavail ? vfs.f_bavail : vfs.f_bfree;

						u64 free_bytes = (u64)vfs.f_bsize * (u64)free_blocks;
						u64 total_bytes = (u64)vfs.f_bsize * (u64)total_blocks;
						u64 used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
						free += (float)free_bytes / GB_SIZE;
						used += (float)used_bytes / GB_SIZE;
					}
				}
			}
			else
			{
				const char *mountName = usbHandle->MountName(portPart);
				if (mountName && mountName[0])
				{
					char mountPath[32];
					snprintf(mountPath, sizeof(mountPath), "%s:/", mountName);
					struct statvfs vfs;
					if (statvfs(mountPath, &vfs) == 0)
					{
						u64 total_blocks = vfs.f_blocks ? vfs.f_blocks : vfs.f_bfree;
						u64 free_blocks = vfs.f_bavail ? vfs.f_bavail : vfs.f_bfree;

						u64 free_bytes = (u64)vfs.f_bsize * (u64)free_blocks;
						u64 total_bytes = (u64)vfs.f_bsize * (u64)total_blocks;
						u64 used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
						free = (float)free_bytes / GB_SIZE;
						used = (float)used_bytes / GB_SIZE;
					}
				}
			}
		}
	}

	// Update cache
	cached_free = free;
	cached_used = used;
	cached_gameCount = currentGameCount;
	cached_multiPartitions = currentMultiPartitions;
	cache_valid = true;

	*total_free = free;
	*total_used = used;
	return true;
}

void InvalidateDiskSpaceCache()
{
	cache_valid = false;
	float free = 0.0f, used = 0.0f;
	GetPartitionDiskSpace(&free, &used);
}
