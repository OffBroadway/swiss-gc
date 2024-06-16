/* deviceHandler-flippydrive.c
	- device implementation for FlippyDrive ODE
	by radicalplants
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

file_handle initial_FlippyDrive =
	{ "fldr:/", // directory
	  0ULL,     // fileBase (u64)
	  0,        // offset
	  0,        // size
	  IS_DIR,
	  0,
	  0
	};

device_info* deviceHandler_FlippyDrive_info(file_handle* file) { return NULL; }

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
	if (lastStatus.result != 0 && lastStatus.result != FR_EXIST || err)
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
	print_gecko("CALL deviceHandler_FlippyDrive_readFile(%s, %p, %x)\n", filename, buffer, length);

	// open the file
	int err = dvd_custom_open(filename, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK | IPC_FILE_FLAG_DISABLESPEEDEMU);
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

	file->size = __builtin_bswap64(*(u64*)(&status.fsize));

	// // reads will be done in 4k chunks and copied to the buffer
	// static GCN_ALIGNED(u8) read_buffer[0x1000];
	// for (u32 i = 0; i < length; i += 0x1000) {
	// 	u32 read_length = (length - i) > 0x1000 ? 0x1000 : (length - i);
	// 	dvd_read(read_buffer, read_length, file->offset + i, status->fd);
	// 	memcpy((char*)buffer + i, read_buffer, read_length);
	// }

	// read the file
	dvd_read_data(buffer, length, file->offset, status.fd);

	// close the file
	dvd_custom_close(status.fd);

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

	// open the file
	dvd_custom_open(filename, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_WRITE | IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK | IPC_FILE_FLAG_DISABLESPEEDEMU);

	GCN_ALIGNED(file_status_t) status;
	int err = dvd_custom_status(&status);
	if (err || status.result != 0) {
		print_gecko("Failed to open file %s\n", filename);
		return -1;
	}

	file->size = __builtin_bswap64(*(u64*)(&status.fsize));

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

	// close the file
	dvd_custom_close(status.fd);
	print_gecko("Write done\n");

	file->offset += length;
	return length;
}

s32 deviceHandler_FlippyDrive_setupFile(file_handle* file, file_handle* file2, ExecutableFile* filesToPatch, int numToPatch) {
	print_gecko("CALL deviceHandler_FlippyDrive_setupFile(%s)\n", file->name);
	return 1;
}


s32 deviceHandler_FlippyDrive_init(file_handle* file) {
	// memset(flippyCurrentFile, 0, sizeof(flippyCurrentFile));

	// do more??

	return 0;
}

s32 deviceHandler_FlippyDrive_deinit(file_handle* file) { return 0; }

s32 deviceHandler_FlippyDrive_closeFile(file_handle* file) {
	print_gecko("CALL deviceHandler_FlippyDrive_closeFile(%s)\n", file->name);
	return 0;
}

s32 deviceHandler_FlippyDrive_deleteFile(file_handle* file) {
	deviceHandler_FlippyDrive_closeFile(file);

	// TODO: unlink
	return 0;
}

bool deviceHandler_FlippyDrive_test() {
	while(DVD_LowGetCoverStatus() == 0);
	// return swissSettings.hasDVDDrive && driveInfo.rel_date == 0x20080714;
	return true;
}

char* deviceHandler_FlippyDrive_status(file_handle* file) { return 0; }

u32 deviceHandler_FlippyDrive_emulated() {
	if (devices[DEVICE_PATCHES]) {
		if (swissSettings.emulateMemoryCard)
			return EMU_READ | EMU_MEMCARD | EMU_BUS_ARBITER;
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
	.emulable = EMU_READ|EMU_READ_SPEED|EMU_MEMCARD,
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
	// .renameFile = deviceHandler_FAT_renameFile,
	.setupFile = deviceHandler_FlippyDrive_setupFile,
	.deinit = deviceHandler_FlippyDrive_deinit,
	.emulated = deviceHandler_FlippyDrive_emulated,
	.status = deviceHandler_FlippyDrive_status,
};
