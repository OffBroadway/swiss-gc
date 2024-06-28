/* deviceHandler-flippydrive.c
	- device implementation for FlippyDrive ODE
	by radicalplants
	by ChrisPVille
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ogc/dvd.h>
#include <ogc/machine/processor.h>
#include <sdcard/gcsd.h>
#include "deviceHandler.h"
#include "deviceHandler-FAT.h"
#include "gui/FrameBufferMagic.h"
#include "gui/IPLFontWrite.h"
#include "swiss.h"
#include "main.h"
#include "flippy_sync.h"
#include "patcher.h"
#include "dvd.h"

// // TODO: alloc this on heap
// char flippyCurrentFile[PATHNAME_MAX];

static uint8_t flippy_read_speed_emu = IPC_FILE_FLAG_DISABLESPEEDEMU;
static bool flippy_write_open = false;

static void enable_speed_emu() {
	flippy_read_speed_emu = IPC_FILE_FLAG_DISABLESPEEDEMU;
}

static void disable_speed_emu() {
	flippy_read_speed_emu = IPC_FILE_FLAG_NONE;
}

file_handle initial_FlippyDrive =
	{ "fldr:/", // directory
	  0ULL,     // fileBase (u64)
	  0,        // offset
	  0,        // size
	  IS_DIR,
	  0,
	  0
	};

static device_info last_fat_info;
device_info* deviceHandler_FlippyDrive_info(file_handle* file) {
	GCN_ALIGNED(fs_info_t) fsInfo;

	int err = dvd_custom_fs_info(&fsInfo);
	if (err)
	{
		print_gecko("DI error during fs info\n");
		return NULL;
	}

	if(fsInfo.result != 0)
	{
		print_gecko("Unable to determine size\n");
		return NULL;
	}

	last_fat_info.freeSpace = fsInfo.free;
	last_fat_info.totalSpace = fsInfo.total;
	last_fat_info.metric = true;

	return &last_fat_info;
}

static uint32_t close_fd(uint32_t fd)
{
	dvd_custom_close(fd);

	GCN_ALIGNED(file_status_t) lastStatus;
	dvd_custom_status(&lastStatus);
	if (lastStatus.result != 0)
	{
		print_gecko("Unable to close fd %d, returned %d\n", fd, lastStatus.result);
	}

	return lastStatus.result;
}

s32 deviceHandler_FlippyDrive_readDir(file_handle* file, file_handle** dir, u32 type) {

	//Open directory
	int err = dvd_custom_open(getDevicePath(file->name), FILE_ENTRY_TYPE_DIR, IPC_FILE_FLAG_NONE);
	if(err)
	{
		print_gecko("DI error during open dir\n");
		return -1;
	}

	GCN_ALIGNED(file_status_t) lastStatus;
	err = dvd_custom_status(&lastStatus);
	if(lastStatus.result != 0 || err)
	{
		print_gecko("Unable to open dir %s, returned %d\n", getDevicePath(file->name), lastStatus.result);
		return -1;
	}

	uint_fast8_t dir_fd = lastStatus.fd;

	//Allocate structures for reading
	size_t entry_table_size = 1;
	*dir = calloc(entry_table_size, sizeof(file_handle));
	if(!dir)
	{
		print_gecko("Unable to alloc dir array\n");
		close_fd(dir_fd);
		return -1;
	}

	//Setup parent dir
	concat_path((*dir)[0].name, file->name, "..");
	(*dir)[0].fileAttrib = IS_SPECIAL;

	int idx = 1;
	GCN_ALIGNED(file_entry_t) curEntry;
	while(idx < 4096) //Limit to a sane number of entries to avoid memory exhaustion
	{
		err = dvd_custom_readdir(&curEntry, dir_fd);
		if(err || curEntry.last_status)
		{
			print_gecko("error during readdir\n");
			close_fd(dir_fd);
			return idx;
		}

		if(curEntry.name[0] == '\0') break; //End of entries

		if(entry_table_size == idx) //Out of table entries
		{
			entry_table_size *= 2; //Double size of table to avoid pathological n^2 reallocation
			*dir = reallocarray(*dir, entry_table_size, sizeof(file_handle));
			if (!dir)
			{
				print_gecko("Unable to alloc dir array\n");
				close_fd(dir_fd);
				return -1;
			}

			//Clear the new memory
			memset(&(*dir)[entry_table_size / 2], 0, sizeof(file_handle) * (entry_table_size/2));
		}

		if (concat_path((*dir)[idx].name, file->name, curEntry.name) < PATHNAME_MAX)
		{
			(*dir)[idx].size = curEntry.size;
			
			switch(curEntry.type)
			{
				case FILE_ENTRY_TYPE_FILE: (*dir)[idx].fileAttrib = IS_FILE; break;
				case FILE_ENTRY_TYPE_DIR: (*dir)[idx].fileAttrib = IS_DIR; break;
				default: (*dir)[idx].fileAttrib = IS_SPECIAL;
			}

			idx++;
		}
		else
		{
			print_gecko("Failed to concat path\n");
		}
	}

	print_gecko("Read %i entries\n", idx);

	//Close directory (best effort)
	close_fd(dir_fd);

	return idx;
}

s32 deviceHandler_FlippyDrive_makeDir(file_handle *file)
{
	int err = dvd_custom_mkdir(getDevicePath(file->name));
	if(err)
	{
		print_gecko("DI error during open dir\n");
		return -1;
	}

	GCN_ALIGNED(file_status_t) lastStatus;
	err = dvd_custom_status(&lastStatus);
	if ((lastStatus.result != 0 && lastStatus.result != FR_EXIST) || err)
	{
		print_gecko("Unable to makedir %s, returned %d\n", getDevicePath(file->name), lastStatus.result);
		return -1;
	}

	return 0;
}

s64 deviceHandler_FlippyDrive_seekFile(file_handle* file, s64 where, u32 type) {
	print_gecko("CALL deviceHandler_FlippyDrive_seekFile(%s, %x)\n", file->name, where);
	if(type == DEVICE_HANDLER_SEEK_SET) file->offset = where;
	else if(type == DEVICE_HANDLER_SEEK_CUR) file->offset = file->offset + where;
	else if(type == DEVICE_HANDLER_SEEK_END) file->offset = file->size + where;
	return file->offset;
}

s32 deviceHandler_FlippyDrive_readFile(file_handle* file, void* buffer, u32 length) {
	char *filename = getDevicePath(file->name);
	// print_gecko("CALL deviceHandler_FlippyDrive_readFile(%s, %p, %u, %x)\n", filename, buffer, file->offset, length);

	if(!file->fileBase) {
		// open the file
		int err = dvd_custom_open(filename, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK | flippy_read_speed_emu);
		if (err)
		{
			print_gecko("DI error during open dir\n");
			return -1;
		}

		GCN_ALIGNED(file_status_t) status;
		err = dvd_custom_status(&status);
		if (err || status.result != 0) {
			print_gecko("Failed to open file %s\n", filename);
			return -1;
		}

		file->size = status.fsize;
		// file->size = ((u32*)&status)[2]; // Dolphin only
		print_gecko("File size: %x\n", file->size);

		file->fileBase = status.fd;
		print_gecko("File base: %u\n", status.fd);
	}

	if (buffer == NULL) {
		print_gecko("Buffer is NULL\n");
		return 0;
	}

	// read the file
	// print_gecko("CALL dvd_read_data(%p, %x, %x, %x)\n", buffer, length, file->offset, file->fileBase);
	dvd_read_data(buffer, length, file->offset, file->fileBase);

	// TODO: check if this is err
	s32 bytes_read = length;
	if(bytes_read > 0) file->offset += bytes_read;
	return bytes_read;
}

s32 deviceHandler_FlippyDrive_writeFile(file_handle* file, const void* buffer, u32 length) {
	char *filename = getDevicePath(file->name);
	print_gecko("CALL deviceHandler_FlippyDrive_writeFile(%s, %p, %x)\n", filename, buffer, length);

	// char *a = getDevicePath(file->name);
	// print_gecko("TEST deviceHandler_FlippyDrive_writeFile(%s)\n", a);

	// always close before writing
	if (file->fileBase) {
		// close the file
		dvd_custom_close(file->fileBase);
		file->fileBase = 0;
	}

	// open the file
	dvd_custom_open(filename, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_WRITE | IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK | flippy_read_speed_emu);

	GCN_ALIGNED(file_status_t) status;
	int err = dvd_custom_status(&status);
	print_gecko("err: %d, status.result: %d\n", err, status.result);
	if (err || status.result != 0) {
		print_gecko("Failed to open file %s\n", filename);
		return -1;
	}

	file->size = status.fsize;

	// write the file in 16k chunks
	for (u32 i = 0; i < length; i += 0x3fe0) {
		print_gecko("Writing %x bytes to file %s\n", (length - i) > 0x3fe0 ? 0x3fe0 : (length - i), filename);

		GCN_ALIGNED(u8) aligned_buffer[FD_IPC_MAXRESP];
		size_t xfer_len = (length - i) > 0x3fe0 ? 0x3fe0 : (length - i);
		memcpy(aligned_buffer, buffer+i, xfer_len);

		if (dvd_custom_write((char*)aligned_buffer, file->offset + i, xfer_len, status.fd) != 0) {
			print_gecko("Failed to write to file %s\n", filename);
			return -1;
		}
		err = dvd_custom_status(&status);
		if (err || status.result != 0)
		{
			print_gecko("Failed to write to file %s\n", filename);
			return -1;
		}
	}

	print_gecko("Write done\n");
	if (flippy_write_open) {
		file->fileBase = status.fd;
	} else {
		dvd_custom_close(status.fd);
	}

	file->offset += length;
	return length;
}

s32 deviceHandler_FlippyDrive_closeFile(file_handle* file) {
	print_gecko("CALL deviceHandler_FlippyDrive_closeFile(%s)\n", file->name);

	if(file && file->fileBase) {
		dvd_custom_close(file->fileBase);
		file->fileBase = 0;
	}

	return 0;
}

s32 deviceHandler_FlippyDrive_setupFile(file_handle* file, file_handle* file2, ExecutableFile* filesToPatch, int numToPatch) {
	print_gecko("CALL deviceHandler_FlippyDrive_setupFile(%s)\n", file->name);

	// Cleanup files that are opened without speed emulation
	enable_speed_emu();
	devices[DEVICE_CUR]->closeFile(file);
	devices[DEVICE_CUR]->closeFile(file2);

	// Check if there are any fragments in our patch location for this game
	if(devices[DEVICE_PATCHES] != NULL) {
		int i;
		file_frag *fragList = NULL;
		u32 numFrags = 0;
		
		print_gecko("Save Patch device found\r\n");
		
		// Look for patch files, if we find some, open them and add them as fragments
		file_handle patchFile;
		for(i = 0; i < numToPatch; i++) {
			if(!filesToPatch[i].patchFile) continue;

			// Cleanup files that are opened without speed emulation
			if(devices[DEVICE_PATCHES] == devices[DEVICE_CUR]) {
				devices[DEVICE_PATCHES]->closeFile(filesToPatch[i].patchFile);
			}

			// Populate fragments for patch files
			if(!getFragments(DEVICE_PATCHES, filesToPatch[i].patchFile, &fragList, &numFrags, filesToPatch[i].file == file2, filesToPatch[i].offset, filesToPatch[i].size)) {
				free(fragList);
				return 0;
			}
		}
		
		if(!getFragments(DEVICE_CUR, file, &fragList, &numFrags, FRAGS_DISC_1, 0, 0)) {
			free(fragList);
			return 0;
		}

		// set the current default DVD + Audio Streaming file
		dvd_set_default_fd(file->fileBase);
		
		if(file2) {
			if(!getFragments(DEVICE_CUR, file2, &fragList, &numFrags, FRAGS_DISC_2, 0, 0)) {
				free(fragList);
				return 0;
			}
		}

		if(swissSettings.igrType == IGR_BOOTBIN || endsWith(file->name,".tgc")) {
			memset(&patchFile, 0, sizeof(file_handle));
			concat_path(patchFile.name, devices[DEVICE_PATCHES]->initial->name, "swiss/patches/apploader.img");

			ApploaderHeader apploaderHeader;
			if(devices[DEVICE_PATCHES]->readFile(&patchFile, &apploaderHeader, sizeof(ApploaderHeader)) != sizeof(ApploaderHeader) || apploaderHeader.rebootSize != reboot_bin_size) {
				devices[DEVICE_PATCHES]->deleteFile(&patchFile);

				memset(&apploaderHeader, 0, sizeof(ApploaderHeader));
				apploaderHeader.rebootSize = reboot_bin_size;

				devices[DEVICE_PATCHES]->seekFile(&patchFile, 0, DEVICE_HANDLER_SEEK_SET);
				devices[DEVICE_PATCHES]->writeFile(&patchFile, &apploaderHeader, sizeof(ApploaderHeader));
				devices[DEVICE_PATCHES]->writeFile(&patchFile, reboot_bin, reboot_bin_size);
				devices[DEVICE_PATCHES]->closeFile(&patchFile);
			}
			
			getFragments(DEVICE_PATCHES, &patchFile, &fragList, &numFrags, FRAGS_APPLOADER, 0x2440, 0);
			devices[DEVICE_PATCHES]->closeFile(&patchFile);
		}

		if(swissSettings.emulateMemoryCard) {
			flippy_write_open = true; // keep write files open

			if(devices[DEVICE_PATCHES] != &__device_sd_a) {
				memset(&patchFile, 0, sizeof(file_handle));
				concatf_path(patchFile.name, devices[DEVICE_PATCHES]->initial->name, "swiss/saves/MemoryCardA.%s.raw", wodeRegionToString(GCMDisk.RegionCode));
				ensure_path(DEVICE_PATCHES, "swiss/saves", NULL);

				if(devices[DEVICE_PATCHES]->readFile(&patchFile, NULL, 0) != 0) {
					devices[DEVICE_PATCHES]->seekFile(&patchFile, 16*1024*1024, DEVICE_HANDLER_SEEK_SET);
					devices[DEVICE_PATCHES]->writeFile(&patchFile, NULL, 0);
					devices[DEVICE_PATCHES]->closeFile(&patchFile);
				}

				devices[DEVICE_PATCHES]->writeFile(&patchFile, NULL, 0); // open in write mode
				if(getFragments(DEVICE_PATCHES, &patchFile, &fragList, &numFrags, FRAGS_CARD_A, 0, 31.5*1024*1024))
					*(vu8*)VAR_CARD_A_ID = (patchFile.size * 8/1024/1024) & 0xFC;
			}

			if(devices[DEVICE_PATCHES] != &__device_sd_b) {
				memset(&patchFile, 0, sizeof(file_handle));
				concatf_path(patchFile.name, devices[DEVICE_PATCHES]->initial->name, "swiss/saves/MemoryCardB.%s.raw", wodeRegionToString(GCMDisk.RegionCode));
				ensure_path(DEVICE_PATCHES, "swiss/saves", NULL);

				if(devices[DEVICE_PATCHES]->readFile(&patchFile, NULL, 0) != 0) {
					devices[DEVICE_PATCHES]->seekFile(&patchFile, 16*1024*1024, DEVICE_HANDLER_SEEK_SET);
					devices[DEVICE_PATCHES]->writeFile(&patchFile, NULL, 0);
					devices[DEVICE_PATCHES]->closeFile(&patchFile);
				}

				devices[DEVICE_PATCHES]->writeFile(&patchFile, NULL, 0); // open in write mode
				if(getFragments(DEVICE_PATCHES, &patchFile, &fragList, &numFrags, FRAGS_CARD_B, 0, 31.5*1024*1024))
					*(vu8*)VAR_CARD_B_ID = (patchFile.size * 8/1024/1024) & 0xFC;
			}
		}
		
		if(fragList) {
			print_frag_list(fragList, numFrags);
			*(vu32**)VAR_FRAG_LIST = installPatch2(fragList, (numFrags + 1) * sizeof(file_frag));
			free(fragList);
			fragList = NULL;
		}
		
		if(devices[DEVICE_PATCHES] != devices[DEVICE_CUR]) {
			s32 exi_channel, exi_device;
			if(getExiDeviceByLocation(devices[DEVICE_PATCHES]->location, &exi_channel, &exi_device)) {
				exi_device = sdgecko_getDevice(exi_channel);
				// Card Type
				*(vu8*)VAR_SD_SHIFT = sdgecko_getAddressingType(exi_channel) ? 0:9;
				// Copy the actual freq
				*(vu8*)VAR_EXI_CPR = (exi_channel << 6) | ((1 << exi_device) << 3) | sdgecko_getSpeed(exi_channel);
				// Device slot (0, 1 or 2)
				*(vu8*)VAR_EXI_SLOT = (exi_device << 2) | exi_channel;
				*(vu32**)VAR_EXI_REGS = ((vu32(*)[5])0xCC006800)[exi_channel];
			}
		}
	}

	if(file2 && file2->meta)
		memcpy(VAR_DISC_2_ID, &file2->meta->diskId, sizeof(VAR_DISC_2_ID));
	memcpy(VAR_DISC_1_ID, &GCMDisk, sizeof(VAR_DISC_1_ID));
	return 1;
}


s32 deviceHandler_FlippyDrive_init(file_handle* file) {
	disable_speed_emu();
	return 0;
}

s32 deviceHandler_FlippyDrive_deinit(file_handle* file) {
	return 0;
}

s32 deviceHandler_FlippyDrive_deleteFile(file_handle* file) {
	deviceHandler_FlippyDrive_closeFile(file);

	int err = dvd_custom_unlink(getDevicePath(file->name));
	if(err)
	{
		print_gecko("DI error during unlink\n");
		return -1;
	}

	GCN_ALIGNED(file_status_t) lastStatus;
	err = dvd_custom_status(&lastStatus);
	if (lastStatus.result != 0 || err)
	{
		print_gecko("Unable to unlink %s, returned %d\n", getDevicePath(file->name), lastStatus.result);
		return -1;
	}

	return 0;
}

s32 deviceHandler_FlippyDrive_renameFile(file_handle* file, char* name) {
	deviceHandler_FlippyDrive_closeFile(file);

	int err = dvd_custom_rename(getDevicePath(file->name), getDevicePath(name));
	if(err)
	{
		print_gecko("DI error during rename\n");
		return -1;
	}

	GCN_ALIGNED(file_status_t) lastStatus;
	err = dvd_custom_status(&lastStatus);
	if (lastStatus.result != 0 || err)
	{
		print_gecko("Unable to rename %s, returned %d\n", getDevicePath(file->name), lastStatus.result);
		return -1;
	}

	strcpy(file->name, name); //TODO this seems silly given the return codes, why do this?
	return 0;
}

bool deviceHandler_FlippyDrive_test() {
	while(DVD_LowGetCoverStatus() == 0);
	return swissSettings.hasDVDDrive && driveInfo.rel_date == 0x20220426;
}

char* deviceHandler_FlippyDrive_status(file_handle* file) { return 0; }

u32 deviceHandler_FlippyDrive_emulated() {
	if (devices[DEVICE_PATCHES]) {
		if (swissSettings.emulateMemoryCard)
			return EMU_READ | /*EMU_MEMCARD |*/ EMU_BUS_ARBITER;
		else
			return EMU_READ | EMU_BUS_ARBITER;
	} else
		return EMU_READ;
}

DEVICEHANDLER_INTERFACE __device_flippydrive = {
	.deviceUniqueId = DEVICE_ID_J,
	.hwName = "FlippyDrive ODE",
	.deviceName = "FlippyDrive ODE",
	.deviceDescription = "Supported File System(s): FAT16, FAT32, exFAT",
	.deviceTexture = {TEX_FLIPPY, 160, 160, 160, 160},
	.features = FEAT_READ|FEAT_WRITE|FEAT_BOOT_GCM|FEAT_BOOT_DEVICE|FEAT_CONFIG_DEVICE|FEAT_AUTOLOAD_DOL|FEAT_HYPERVISOR|FEAT_PATCHES|FEAT_AUDIO_STREAMING,
	.emulable = EMU_READ|EMU_READ_SPEED,
	.location = LOC_DVD_CONNECTOR,
	.initial = &initial_FlippyDrive,
	.test = deviceHandler_FlippyDrive_test,
	.info = deviceHandler_FlippyDrive_info,
	.init = deviceHandler_FlippyDrive_init,
	.readDir = deviceHandler_FlippyDrive_readDir,
	.makeDir = deviceHandler_FlippyDrive_makeDir,
	.seekFile = deviceHandler_FlippyDrive_seekFile,
	.readFile = deviceHandler_FlippyDrive_readFile,
	.writeFile = deviceHandler_FlippyDrive_writeFile,
	.closeFile = deviceHandler_FlippyDrive_closeFile,
	.deleteFile = deviceHandler_FlippyDrive_deleteFile,
	.renameFile = deviceHandler_FlippyDrive_renameFile,
	.setupFile = deviceHandler_FlippyDrive_setupFile,
	.deinit = deviceHandler_FlippyDrive_deinit,
	.emulated = deviceHandler_FlippyDrive_emulated,
	.status = deviceHandler_FlippyDrive_status,
};