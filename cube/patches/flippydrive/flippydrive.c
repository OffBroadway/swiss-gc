/* 
 * Copyright (c) 2021-2022, Extrems <extrems@extremscorner.org>
 * 
 * This file is part of Swiss.
 * 
 * Swiss is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Swiss is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * with Swiss.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "common.h"
#include "dolphin/dvd.h"
#include "dolphin/exi.h"
#include "dolphin/os.h"
#include "emulator.h"
#include "frag.h"
#include "interrupt.h"
#include "ipl.h"

#include "debug.c"

#define FLIPPY 1
#define DI_CMD_FLIPPY_FILEAPI 0xB5
#define FLIPPY_FILEAPI_RESET 0x05
#define FLIPPY_FILEAPI_DEFAULT_FD 0x01

#ifndef QUEUE_SIZE
#ifdef FLIPPY
#define QUEUE_SIZE 4
#else
#define QUEUE_SIZE 2
#endif
#endif
#define SECTOR_SIZE 512

static struct {
	char (*buffer)[SECTOR_SIZE];
	uint32_t last_sector;
	struct {
		void *buffer;
		uint32_t length;
		uint32_t offset;
		uint32_t sector;
		uint32_t command;
		frag_callback callback;
	} queue[QUEUE_SIZE], *queued;
} gcode = {
	.buffer = &VAR_SECTOR_BUF,
	.last_sector = ~0
};

static struct {
	void *buffer;
	uint32_t length;
	uint32_t offset;
	bool read;
} dvd;

static void gcode_set_default(uint8_t fd)
{
	DI[2] = (DI_CMD_FLIPPY_FILEAPI << 24) | FLIPPY_FILEAPI_DEFAULT_FD | (fd << 16);
	DI[3] = 0;
	DI[4] = 0;
	DI[7] = 0b001;
	while (DI[7] & 0b001);
}

static void gcode_reset()
{
	DI[2] = (DI_CMD_FLIPPY_FILEAPI << 24) | FLIPPY_FILEAPI_RESET;
	DI[3] = 0xAA55F641; // IPC_MAGIC
	DI[7] = 0b001;
	while (DI[7] & 0b001);
}

static void di_interrupt_handler(OSInterrupt interrupt, OSContext *context);

static void gcode_done_queued(void);
static void gcode_read_queued(void)
{
	void *buffer = gcode.queued->buffer;
	uint32_t length = gcode.queued->length;
	uint32_t offset = gcode.queued->offset;
	uint32_t sector = gcode.queued->sector;
	uint32_t command = gcode.queued->command;

	DI[0] = 0b0011000;
	DI[1] = 0;

	switch (command >> 24) {
		case DI_CMD_READ:
			DI[2] = command;
			DI[3] = offset;
			DI[4] = length;
			DI[5] = (uint32_t)buffer;
			DI[6] = length;
			DI[7] = 0b011;
			break;
		case DI_CMD_SEEK:
			DI[2] = command;
			DI[3] = offset;
			DI[7] = 0b001;
			break;
		// case DI_CMD_FLIPPY_FILEAPI:
		// 	switch(command & 0xFF) {
		// 		case FLIPPY_FILEAPI_DEFAULT_FD:
		// 			// _puts("gcode_read_queued DI_CMD_FLIPPY_FILEAPI\n");
		// 			DI[2] = command;
		// 			DI[3] = 0;
		// 			DI[4] = 0;
		// 			DI[7] = 0b001;
		// 			break;
		// 	}
		// 	break;
		// case DI_CMD_GCODE_WRITE_BUFFER:
		// 	DI[2] = command;
		// 	DI[5] = (uint32_t)buffer;
		// 	DI[6] = length;
		// 	DI[7] = 0b111;
		// 	break;
		// case DI_CMD_GCODE_WRITE:
		// 	if (gcode.last_sector == sector)
		// 		gcode.last_sector = ~0;

		// 	DI[2] = command;
		// 	DI[3] = sector;
		// 	DI[7] = 0b001;
		// 	break;
		case DI_CMD_AUDIO_STREAM:
			DI[2] = command;
			DI[3] = offset;
			DI[4] = length;
			DI[7] = 0b001;
			break;
		case DI_CMD_REQUEST_AUDIO_STATUS:
			DI[2] = command;
			DI[7] = 0b001;
			break;
	}

	set_interrupt_handler(OS_INTERRUPT_PI_DI, di_interrupt_handler);
	unmask_interrupts(OS_INTERRUPTMASK_PI_DI);
}

static void gcode_done_queued(void)
{
	void *buffer = gcode.queued->buffer;
	uint32_t length = gcode.queued->length;
	uint32_t offset = gcode.queued->offset;
	uint32_t sector = gcode.queued->sector;
	uint32_t command = gcode.queued->command;

	switch (command >> 24) {
		// case DI_CMD_FLIPPY_FILEAPI:
		// 	switch(command & 0xFF) {
		// 		case FLIPPY_FILEAPI_DEFAULT_FD:
		// 			const frag_t *frag = buffer;

		// 			DI[2] = frag->offset;
		// 			DI[3] = frag->size;
		// 			DI[4] = frag->sector;
		// 			DI[7] = 0b001;
		// 			while (DI[7] & 0b001);
		// 			if (!DI[8]) {
		// 				*VAR_CURRENT_DISC = frag->file;
		// 				break;
		// 			}
		// 			break;
		// 	}
		// 	break;
		// case DI_CMD_GCODE_WRITE_BUFFER:
		// 	gcode.queued->command = DI_CMD_GCODE_WRITE << 24;
		// 	gcode_read_queued();
		// 	return;
		// case DI_CMD_GCODE_WRITE:
		// 	if (DI[8])
		// 		length = 0;
		// 	break;
		case DI_CMD_AUDIO_STREAM:
		case DI_CMD_REQUEST_AUDIO_STATUS:
			*(uint32_t *)buffer = DI[8];
			break;
	}

	gcode.queued->callback(buffer, length);

	gcode.queued->callback = NULL;
	gcode.queued = NULL;

	for (int i = 0; i < QUEUE_SIZE; i++) {
		if (gcode.queue[i].callback != NULL) {
			gcode.queued = &gcode.queue[i];
			gcode_read_queued();
			return;
		}
	}
}

static void di_interrupt_handler(OSInterrupt interrupt, OSContext *context)
{
	mask_interrupts(OS_INTERRUPTMASK_PI_DI);

	gcode_done_queued();
}

bool gcode_push_queue(void *buffer, uint32_t length, uint32_t offset, uint64_t sector, uint32_t command, frag_callback callback)
{
	for (int i = 0; i < QUEUE_SIZE; i++) {
		if (gcode.queue[i].callback == NULL) {
			gcode.queue[i].buffer = buffer;
			gcode.queue[i].length = length;
			gcode.queue[i].offset = offset;
			gcode.queue[i].sector = sector;
			gcode.queue[i].command = command;
			gcode.queue[i].callback = callback;

			if (gcode.queued == NULL) {
				gcode.queued = &gcode.queue[i];
				gcode_read_queued();
			}
			return true;
		}
	}

	return false;
}

bool do_read_write_async(void *buffer, uint32_t length, uint32_t offset, uint64_t sector, bool write, frag_callback callback)
{
	// gprintf("do_read_write_async: buffer=%p, length=%u, offset=%u, sector=%llu, write=%d, callback=%p\n", buffer, length, offset, sector, write, callback);
	// _puts("do_read_write_async\n");

	uint32_t command;

	if (write) {
		// length = SECTOR_SIZE;
		// command = DI_CMD_WRITE << 24;
		while(1);
	}

	// file read
	command = DI_CMD_READ << 24;
	command |= (sector & 0xFF) << 16;
	return gcode_push_queue(buffer, length, offset >> 2, sector, command, callback);
}

bool do_read_disc(void *buffer, uint32_t length, uint32_t offset, const frag_t *frag, frag_callback callback)
{
	//gprintf("do_read_disc: buffer=%p, length=%u, offset=%u, frag=%p, callback=%p\n", buffer, length, offset, frag, callback);
	// _puts("do_read_disc\n");
	uint32_t cmd;
	if (length)
	{
		cmd = DI_CMD_READ << 24;
		cmd |= (frag->sector % 0xFF) << 16; //FD
	}
 	else
	{
		cmd = DI_CMD_SEEK << 24;
		cmd |= (frag->sector % 0xFF) << 16; // FD
	}

	return gcode_push_queue(buffer, length, offset >> 2, frag->sector, cmd, callback);
}

void schedule_read(OSTick ticks)
{
	void read_callback(void *address, uint32_t length)
	{
		dvd.buffer += length;
		dvd.length -= length;
		dvd.offset += length;
		dvd.read = !!dvd.length;

		schedule_read(0);
	}

	if (!dvd.read) {
		di_complete_transfer();
		return;
	}

	// gprintf("schedule_read: dvd.buffer=%p, dvd.length=%u, dvd.offset=%u, dvd.read=%d\n", dvd.buffer, dvd.length, dvd.offset, dvd.read);
	// _puts("schedule_read\n");
	frag_read_async(*VAR_CURRENT_DISC, dvd.buffer, dvd.length, dvd.offset, read_callback);
}

void perform_read(uint32_t address, uint32_t length, uint32_t offset)
{
	// gprintf("perform_read: address=%u, length=%u, offset=%u\n", address, length, offset);
	// _puts("perform_read\n");

	if ((*VAR_IGR_TYPE & 0x80) && offset == 0x2440) {
		*VAR_CURRENT_DISC = FRAGS_APPLOADER;
		*VAR_SECOND_DISC = 0;
	}

	dvd.buffer = OSPhysicalToUncached(address);
	dvd.length = length;
	dvd.offset = offset;
	dvd.read = true;

	#ifdef DVD_MATH
	void alarm_handler(OSAlarm *alarm, OSContext *context)
	{
		schedule_read(0);
	}

	if (*VAR_EMU_READ_SPEED) {
		dvd_schedule_read(offset, length, alarm_handler);
		return;
	}
	#endif

	schedule_read(0);
}

bool change_disc(void)
{
	if (*VAR_SECOND_DISC) {
		// _puts("change_disc\n");
		const frag_t *frag = NULL;
		frag_get_list(*VAR_CURRENT_DISC ^ 1, &frag);

		if (frag) {
			// _puts("change_disc frag\n");
			uint8_t fd = frag->sector & 0xFF;
			gcode_set_default(fd);

			*VAR_CURRENT_DISC ^= 1;
			return true;
		}
	}

	return false;
}

void reset_devices(void)
{
	// _puts("reset_devices\n");

	while (DI[7] & 0b001);

	if (AI[0] & 0b0000001) {
		AI[1] = 0;

		DI[2] = DI_CMD_AUDIO_STREAM << 24 | 0x01 << 16;
		DI[7] = 0b001;
		while (DI[7] & 0b001);

		AI[0] &= ~0b0000001;
	}

	gcode_reset();

	while (EXI[EXI_CHANNEL_0][3] & 0b000001);
	while (EXI[EXI_CHANNEL_1][3] & 0b000001);
	while (EXI[EXI_CHANNEL_2][3] & 0b000001);

	reset_device();
	ipl_set_config(0);
}
