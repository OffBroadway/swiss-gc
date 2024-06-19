/* 
 * Copyright (c) 2019-2024, Extrems <extrems@extremscorner.org>
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
#include "audio.h"
#include "bba/bba.h"
#include "common.h"
#include "dolphin/dvd.h"
#include "dolphin/exi.h"
#include "dolphin/os.h"
#include "emulator.h"
#include "emulator_card.h"
#include "emulator_eth.h"
#include "fifo.h"
#include "frag.h"
#include "interrupt.h"
#include "PowerPC.h"

static struct {
	union {
		uint32_t regs[2];
		struct {
			uint32_t intsr;
			uint32_t intmsk;
		} reg;
	};
} pi;

static void pi_update_interrupts(void)
{
	if (pi.reg.intsr & pi.reg.intmsk)
		assert_interrupt();
	else
		PI[0] = 1;
}

#ifdef CARD_EMULATOR
static struct {
	union {
		uint32_t regs[15];
		struct {
			uint32_t cpr;
			uint32_t mar;
			uint32_t length;
			uint32_t cr;
			uint32_t data;
		} reg[3];
	};

	OSAlarm alarm[EXI_CHANNEL_MAX];
} exi = {
	.reg = {
		{ .cpr = 0b11100000000000 },
		{ .cpr = 0b01100000000000 }
	}
};

static void exi_update_interrupts(void)
{
	if ((exi.reg[EXI_CHANNEL_0].cpr >> 1) & (exi.reg[EXI_CHANNEL_0].cpr & 0b00010000000101) ||
		(exi.reg[EXI_CHANNEL_1].cpr >> 1) & (exi.reg[EXI_CHANNEL_1].cpr & 0b00010000000101) ||
		(exi.reg[EXI_CHANNEL_2].cpr >> 1) & (exi.reg[EXI_CHANNEL_2].cpr & 0b00010000000101))
		pi.reg.intsr |=  0b00000000010000;
	else
		pi.reg.intsr &= ~0b00000000010000;

	pi_update_interrupts();
}

void exi_interrupt(unsigned chan)
{
	exi.reg[chan].cpr |=  0b00000000000010;
	exi_update_interrupts();
}

void exi_complete_transfer(unsigned chan)
{
	if (exi.reg[chan].cr & 0b000010) {
		exi.reg[chan].mar += exi.reg[chan].length;
		exi.reg[chan].length = 0;
	}

	exi.reg[chan].cpr |=  0b00000000001000;
	exi.reg[chan].cr  &= ~0b000001;
	exi_update_interrupts();
}

void exi0_complete_transfer()
{
	exi_complete_transfer(EXI_CHANNEL_0);
}

void exi1_complete_transfer()
{
	exi_complete_transfer(EXI_CHANNEL_1);
}

void exi2_complete_transfer()
{
	exi_complete_transfer(EXI_CHANNEL_2);
}

static void exi_transfer(unsigned chan, uint32_t length)
{
	OSTick ticks = ((length * (OS_TIMER_CLOCK / 843750)) * 8) >> ((exi.reg[chan].cpr >> 4) & 0b111);
	OSAlarmHandler handler = NULL;

	switch (chan) {
		case EXI_CHANNEL_0:
			handler = exi0_complete_transfer;
			break;
		case EXI_CHANNEL_1:
			handler = exi1_complete_transfer;
			break;
		case EXI_CHANNEL_2:
			handler = exi2_complete_transfer;
			break;
	}

	OSSetAlarm(&exi.alarm[chan], ticks, handler);
}

void exi_insert_device(unsigned chan)
{
	exi.reg[chan].cpr |=  0b01000000000000;
}

void exi_remove_device(unsigned chan)
{
	exi.reg[chan].cpr &= ~0b01000000000000;
	exi.reg[chan].cpr |=  0b00100000000000;
	exi_update_interrupts();
}

static void exi_read(unsigned index, uint32_t *value)
{
	unsigned chan = index / 5;
	unsigned dev = (exi.reg[chan].cpr >> 7) & 0b111;

	switch (index) {
		case 0:
			if (dev & ~(1 << EXI_DEVICE_0))
				*value = exi.reg[chan].cpr | (EXI[chan][0] & 0b00000000001100);
			else
				*value = exi.reg[chan].cpr;
			break;
		case 1 ... 4:
			if (dev & ~(1 << EXI_DEVICE_0))
				*value = exi.regs[index] = (*EXI)[index];
			else
				*value = exi.regs[index];
			break;
		#ifndef USB
		case 5:
			if (!dev && chan != *VAR_EXI_SLOT & 0xF) {
				if (exi.reg[chan].cpr & 0b01000000000000) {
					if (EXI[chan][0] & 0b01000000000000)
						exi_remove_device(chan);
				} else if (!(EXI[chan][0] & 0b01100000000000))
					exi_insert_device(chan);
			}

			if (!(exi.reg[chan].cpr & 0b01100000000000))
				*value = exi.reg[chan].cpr | EXI[chan][0];
			else
				*value = exi.reg[chan].cpr;
			break;
		case 6 ... 9:
			if (!(exi.reg[chan].cpr & 0b01000000000000))
				*value = exi.regs[index] = (*EXI)[index];
			else
				*value = exi.regs[index];
			break;
		#endif
		case 10:
			*value = exi.reg[chan].cpr | (EXI[chan][0] & 0b00000000000011);
			break;
		default:
			*value = exi.regs[index];
	}
}

static void exi_write(unsigned index, uint32_t value)
{
	unsigned chan = index / 5;
	unsigned dev = (exi.reg[chan].cpr >> 7) & 0b111;
	unsigned dev2;

	bool ext = !!(exi.reg[chan].cpr & 0b01000000000000);

	switch (index % 5) {
		case 0:
			if (chan == EXI_CHANNEL_1) {
				#ifndef USB
				if (!(exi.reg[chan].cpr & 0b01100000000000))
					EXI[chan][0] = value;
				#endif
			} else if (chan == EXI_CHANNEL_2)
				EXI[chan][0] = (value & 0b00000000000011) | (EXI[chan][0] & 0b00011111110100);

			exi.reg[chan].cpr = ((value & 0b00100000001010) ^ exi.reg[chan].cpr) & exi.reg[chan].cpr;

			if ((value & 0b00001110000000) & ((value & 0b00001110000000) - 1))
				exi.reg[chan].cpr = (value & 0b10010001110101) | (exi.reg[chan].cpr & 0b11100000001010);
			else
				exi.reg[chan].cpr = (value & 0b10011111110101) | (exi.reg[chan].cpr & 0b11100000001010);

			dev2 = (exi.reg[chan].cpr >> 7) & 0b111;

			if (chan == EXI_CHANNEL_0) {
				if ((dev | dev2) & ~(1 << EXI_DEVICE_0)) {
					if (chan == *VAR_EXI_SLOT & 0x3)
						end_read();

					EXI[chan][0] = (value & 0b10001111111100) | (EXI[chan][0] & 0b00010000000001);
				}
			}

			if (ext) {
				if ((~dev & dev2) & (1 << EXI_DEVICE_0))
					card_select(chan);
				if ((dev & ~dev2) & (1 << EXI_DEVICE_0))
					card_deselect(chan);
			}

			exi_update_interrupts();
			break;
		case 1 ... 2:
			exi.regs[index] = value & 0x3FFFFE0;
			break;
		case 3:
			exi.regs[index] = value & 0b111111;

			if (value & 0b000001) {
				#ifdef USB
				if (chan == EXI_CHANNEL_0 && (dev & ~(1 << EXI_DEVICE_0))) {
				#else
				if ((chan == EXI_CHANNEL_0 && (dev & ~(1 << EXI_DEVICE_0))) ||
					(chan == EXI_CHANNEL_1 && !ext)) {
				#endif
					EXI[chan][1] = exi.reg[chan].mar;
					EXI[chan][2] = exi.reg[chan].length;
					EXI[chan][4] = exi.reg[chan].data;
					EXI[chan][3] = exi.reg[chan].cr;
				} else {
					if (value & 0b000010) {
						uint32_t address = exi.reg[chan].mar;
						uint32_t length  = exi.reg[chan].length;
						int type = (exi.reg[chan].cr >> 2) & 0b11;
						bool async = false;

						if (ext && (dev & (1 << EXI_DEVICE_0)))
							async = card_dma(chan, address, length, type);

						if (!async) exi_transfer(chan, length);
					} else {
						int length = (exi.reg[chan].cr >> 4) & 0b11;
						char *data = (char *)&exi.reg[chan].data;

						if (ext && (dev & (1 << EXI_DEVICE_0))) {
							for (int i = 0; i <= length; i++)
								data[i] = card_imm(chan, data[i]);
						} else {
							for (int i = 0; i <= length; i++)
								data[i] = ~0;
						}

						exi_complete_transfer(chan);
					}
				}
			}
			break;
		case 4:
			exi.regs[index] = value;
			break;
	}
}
#elifdef ETH_EMULATOR
static struct {
	union {
		uint32_t regs[15];
		struct {
			uint32_t cpr;
			uint32_t mar;
			uint32_t length;
			uint32_t cr;
			uint32_t data;
		} reg[3];
	};
} exi;

static void exi_update_interrupts(void)
{
	if ((exi.reg[EXI_CHANNEL_0].cpr >> 1) & (exi.reg[EXI_CHANNEL_0].cpr & 0b00010000000101) ||
		(exi.reg[EXI_CHANNEL_1].cpr >> 1) & (exi.reg[EXI_CHANNEL_1].cpr & 0b00010000000101) ||
		(exi.reg[EXI_CHANNEL_2].cpr >> 1) & (exi.reg[EXI_CHANNEL_2].cpr & 0b00010000000101))
		pi.reg.intsr |=  0b00000000010000;
	else
		pi.reg.intsr &= ~0b00000000010000;

	pi_update_interrupts();
}

void exi_interrupt(unsigned chan)
{
	exi.reg[chan].cpr |=  0b00000000000010;
	exi_update_interrupts();
}

void exi_complete_transfer(unsigned chan)
{
	if (exi.reg[chan].cr & 0b000010) {
		exi.reg[chan].mar += exi.reg[chan].length;
		exi.reg[chan].length = 0;
	}

	exi.reg[chan].cpr |=  0b00000000001000;
	exi.reg[chan].cr  &= ~0b000001;
	exi_update_interrupts();
}

static void exi_read(unsigned index, uint32_t *value)
{
	unsigned chan = index / 5;
	unsigned dev = (exi.reg[chan].cpr >> 7) & 0b111;

	uint32_t mask = exi_get_interrupt_mask(chan);
	uint32_t mask2 = (((mask << 1) & 0b00000000010000) * 0b111111) | (mask >> 1);
	mask |= mask2;

	switch (index % 5) {
		case 0:
			if (chan == EXI_CHANNEL_2)
				mask |= 0b00000000000011;
			if (chan == EXI_CHANNEL_0 && (dev & (1 << EXI_DEVICE_2)))
				mask |= 0b00001111111100;
			if (chan == *VAR_EXI_SLOT & 0xF)
				mask |= 0b01000000000000;

			*value = exi.reg[chan].cpr | (EXI[chan][0] & ~mask);
			break;
		case 1 ... 4:
			if (chan == EXI_CHANNEL_0 && (dev & (1 << EXI_DEVICE_2)))
				*value = exi.regs[index];
			else
				*value = exi.regs[index] = (*EXI)[index];
			break;
	}
}

static void exi_write(unsigned index, uint32_t value)
{
	unsigned chan = index / 5;
	unsigned dev = (exi.reg[chan].cpr >> 7) & 0b111;
	unsigned dev2;

	uint32_t mask = exi_get_interrupt_mask(chan);
	uint32_t mask2 = (((mask << 1) & 0b00000000010000) * 0b111111) | (mask >> 1);
	mask |= mask2;

	switch (index % 5) {
		case 0:
			exi.reg[chan].cpr = ((value & 0b00100000001010) ^ exi.reg[chan].cpr) & exi.reg[chan].cpr;

			if ((value & 0b00001110000000) & ((value & 0b00001110000000) - 1))
				exi.reg[chan].cpr = (value & 0b10010001110101) | (exi.reg[chan].cpr & 0b11100000001010);
			else
				exi.reg[chan].cpr = (value & 0b10011111110101) | (exi.reg[chan].cpr & 0b11100000001010);

			dev2 = (exi.reg[chan].cpr >> 7) & 0b111;

			if (chan == EXI_CHANNEL_2)
				mask |= 0b00000000000011;
			if (chan == EXI_CHANNEL_0 && ((dev | dev2) & (1 << EXI_DEVICE_2))) {
				mask |= 0b00001111111100;
			} else if (chan == *VAR_EXI_CPR >> 6) {
				if ((dev | dev2) & *VAR_EXI_CPR >> 3)
					mask |= 0b00001110000000;
				if (~dev & dev2)
					end_read();
			}

			EXI[chan][0] = (value & ~mask) | (EXI[chan][0] & mask2);

			if (chan == EXI_CHANNEL_0) {
				if ((~dev & dev2) & (1 << EXI_DEVICE_2))
					eth_exi_select();
				if ((dev & ~dev2) & (1 << EXI_DEVICE_2))
					eth_exi_deselect();
			}

			exi_update_interrupts();
			break;
		case 1 ... 2:
			exi.regs[index] = value & 0x3FFFFE0;
			break;
		case 3:
			exi.regs[index] = value & 0b111111;

			if (value & 0b000001) {
				if (chan == EXI_CHANNEL_0 && (dev & (1 << EXI_DEVICE_2))) {
					if (value & 0b000010) {
						uint32_t address = exi.reg[chan].mar;
						uint32_t length  = exi.reg[chan].length;
						int type = (exi.reg[chan].cr >> 2) & 0b11;

						eth_exi_dma(address, length, type);

						exi_complete_transfer(chan);
					} else {
						int length = (exi.reg[chan].cr >> 4) & 0b11;
						char *data = (char *)&exi.reg[chan].data;

						for (int i = 0; i <= length; i++)
							data[i] = eth_exi_imm(data[i]);

						exi_complete_transfer(chan);
					}
				} else {
					EXI[chan][1] = exi.reg[chan].mar;
					EXI[chan][2] = exi.reg[chan].length;
					EXI[chan][4] = exi.reg[chan].data;
					EXI[chan][3] = exi.reg[chan].cr;
				}
			}
			break;
		case 4:
			exi.regs[index] = value;
			break;
	}
}
#else
static void exi_read(unsigned index, uint32_t *value)
{
	unsigned chan = index / 5;

	uint32_t mask = exi_get_interrupt_mask(chan);
	uint32_t mask2 = (((mask << 1) & 0b00000000010000) * 0b111111) | (mask >> 1);
	mask |= mask2;

	switch (index % 5) {
		case 0:
			if (chan == *VAR_EXI_SLOT & 0xF)
				mask |= 0b01000000000000;
			#ifdef USB
			if (chan == EXI_CHANNEL_1)
				mask |= 0b01000000000000;
			#endif

			*value = EXI[chan][0] & ~mask;
			break;
		default:
			*value = (*EXI)[index];
	}
}

static void exi_write(unsigned index, uint32_t value)
{
	unsigned chan = index / 5;
	unsigned dev = (EXI[chan][0] >> 7) & 0b111;
	unsigned dev2;

	uint32_t mask = exi_get_interrupt_mask(chan);
	uint32_t mask2 = (((mask << 1) & 0b00000000010000) * 0b111111) | (mask >> 1);
	mask |= mask2;

	switch (index % 5) {
		case 0:
			dev2 = (value >> 7) & 0b111;

			if (~dev & dev2) {
				if (chan == *VAR_EXI_CPR >> 6) {
					if (dev2 & *VAR_EXI_CPR >> 3)
						mask |= 0b00001110000000;
					end_read();
				}
				#ifdef BBA
				if (chan == EXI_CHANNEL_0 && (dev2 & (1 << EXI_DEVICE_2)))
					mask |= 0b00001110000000;
				#endif
				#ifdef USB
				if (chan == EXI_CHANNEL_1 && (dev2 & (1 << EXI_DEVICE_0)))
					mask |= 0b00001110000000;
				#endif
			}

			EXI[chan][0] = (value & ~mask) | (EXI[chan][0] & mask2);
			break;
		default:
			(*EXI)[index] = value;
	}
}
#endif

static struct {
	#ifdef DTK
	adpcm_t adpcm;
	fifo_t fifo;
	uint8_t (*buffer)[512];
	#endif

	bool reading;
	bool flushing;
	bool playing;
	bool stopping;

	struct {
		uint32_t position;
		uint32_t start;
		uint32_t length;
	} current;

	struct {
		uint32_t start;
		uint32_t length;
	} next;
} dtk;

#ifdef DTK
static void dtk_decode_buffer(void *address, uint32_t length)
{
	adpcm_decode(&dtk.adpcm, &dtk.fifo, *dtk.buffer, 448);
	dtk.current.position += sizeof(*dtk.buffer);

	if (dtk.current.position == dtk.current.start + dtk.current.length) {
		dtk.current.position = 
		dtk.current.start  = dtk.next.start;
		dtk.current.length = dtk.next.length;

		if (dtk.stopping) {
			dtk.stopping = false;
			dtk.playing  = false;
		}

		adpcm_reset(&dtk.adpcm);
	}
}

bool dtk_fill_buffer(void)
{
	if (dtk.reading)
		return true;
	if (!dtk.playing)
		return false;
	if (fifo_space(&dtk.fifo) < 448 * sizeof(sample_t))
		return false;

	#ifdef ASYNC_READ
	void read_callback(void *address, uint32_t length)
	{
		dtk.reading = false;
		if (dtk.flushing) dtk.flushing = false;
		else dtk_decode_buffer(address, length);
		dtk_fill_buffer();
	}

	DCInvalidateRange(__builtin_assume_aligned(dtk.buffer, 32), sizeof(*dtk.buffer));
	return dtk.reading = frag_read_async(*VAR_CURRENT_DISC, dtk.buffer, sizeof(*dtk.buffer), dtk.current.position, read_callback);
	#else
	OSCancelAlarm(&read_alarm);
	OSTick start = OSGetTick();

	int size = frag_read(*VAR_CURRENT_DISC, dtk.buffer, sizeof(*dtk.buffer), dtk.current.position);
	if (size == sizeof(*dtk.buffer))
		dtk_decode_buffer(dtk.buffer, size);

	OSTick end = OSGetTick();
	OSSetAlarm(&read_alarm, OSDiffTick(end, start), trickle_read);

	return true;
	#endif
}
#endif

static struct {
	union {
		uint32_t regs[9];
		struct {
			uint32_t sr;
			uint32_t cvr;
			uint32_t cmdbuf0;
			uint32_t cmdbuf1;
			uint32_t cmdbuf2;
			uint32_t mar;
			uint32_t length;
			uint32_t cr;
			uint32_t immbuf;
		} reg;
	};

	uint32_t status :  8;
	uint32_t error  : 24;
	#ifdef DVD
	int reset;
	#endif
} di;

static void di_update_interrupts(void)
{
	if ((di.reg.sr  >> 1) & (di.reg.sr  & 0b0101010) ||
		(di.reg.cvr >> 1) & (di.reg.cvr & 0b010))
		pi.reg.intsr |=  0b00000000000100;
	else
		pi.reg.intsr &= ~0b00000000000100;

	pi_update_interrupts();
}

void di_error(uint32_t error)
{
	di.reg.sr |=  0b0000100;
	di.reg.cr &= ~0b001;
	di.error = error;
	di_update_interrupts();
}

void di_complete_transfer()
{
	if (di.reg.cr & 0b010) {
		di.reg.mar += di.reg.length;
		di.reg.length = 0;
	}

	di.reg.sr |=  0b0010000;
	di.reg.cr &= ~0b001;
	di_update_interrupts();
}

void di_open_cover(void)
{
	di.reg.cvr |=  0b001;
	di.status = 1;
}

void di_close_cover()
{
	di.reg.cvr &= ~0b001;
	di.reg.cvr |=  0b100;
	di.status = 0;
	di_update_interrupts();
}

OSAlarm di_alarm;
OSAlarm cover_alarm;
OSAlarm read_alarm;

#ifndef DI_PASSTHROUGH
#ifdef GCODE
bool gcode_push_queue(void *buffer, uint32_t length, uint32_t offset, uint64_t sector, uint32_t command, frag_callback callback);
#endif

static void di_execute_command()
{
	uint32_t result = 0;

#ifdef FLIPPY
	// extern void gprintf(const char *fmt, ...);
	// gprintf("di_execute_command %08x\n", di.reg.cmdbuf0);
#endif

	switch (di.reg.cmdbuf0 >> 24) {
		case DI_CMD_READ:
		{
			uint32_t address = di.reg.mar;
			uint32_t length  = di.reg.length;
			uint32_t offset  = di.reg.cmdbuf1 << 2;

			if (di.status == 1)
				di_error(0x023A00);
			else if (!length)
				di_error(0x052400);
			else
				perform_read(address, length, offset);
			return;
		}
		case DI_CMD_SEEK:
		{
			uint32_t offset = di.reg.cmdbuf1 << 2;

			if (di.status == 1)
				di_error(0x023A00);
			else
				perform_read(0, 0, offset);
			return;
		}
		case DI_CMD_REQUEST_ERROR:
		{
			result = di.error | di.status << 24;
			di.error = 0;
			break;
		}
		#if defined GCODE && !defined DTK
		case DI_CMD_AUDIO_STREAM:
		case DI_CMD_REQUEST_AUDIO_STATUS:
		{
			gcode_push_queue(&di.reg.immbuf, di.reg.cmdbuf2, di.reg.cmdbuf1, 0, di.reg.cmdbuf0, di_complete_transfer);
			return;
		}
		#else
		case DI_CMD_AUDIO_STREAM:
		{
			switch ((di.reg.cmdbuf0 >> 16) & 0x03) {
				case 0x00:
				{
					uint32_t offset = DVDRoundDown32KB(di.reg.cmdbuf1 << 2);
					uint32_t length = DVDRoundDown32KB(di.reg.cmdbuf2);

					if (!length) {
						dtk.stopping = true;
					} else if (!dtk.stopping) {
						dtk.next.start  = offset;
						dtk.next.length = length;

						if (!dtk.playing) {
							dtk.current.position = 
							dtk.current.start  = offset;
							dtk.current.length = length;

							dtk.playing = true;
							#ifdef DTK
							dtk_fill_buffer();
							#endif
						}
					}
					break;
				}
				case 0x01:
				{
					dtk.stopping = false;
					dtk.playing  = false;
					#ifdef DTK
					dtk.flushing = dtk.reading;

					adpcm_reset(&dtk.adpcm);
					fifo_reset(&dtk.fifo);
					#endif
					break;
				}
			}
			break;
		}
		case DI_CMD_REQUEST_AUDIO_STATUS:
		{
			switch ((di.reg.cmdbuf0 >> 16) & 0x03) {
				#ifndef DTK
				case 0x00: result = dtk.playing; break;
				#else
				case 0x00: result = dtk.playing | !!fifo_size(&dtk.fifo); break;
				#endif
				case 0x01: result = DVDRoundDown32KB(dtk.current.position) >> 2; break;
				case 0x02: result = DVDRoundDown32KB(dtk.current.start) >> 2; break;
				case 0x03: result = dtk.current.length; break;
			}
			break;
		}
		#endif
		case DI_CMD_STOP_MOTOR:
		{
			if (di.status == 0 && change_disc()) {
				di_open_cover();
				#ifndef GCODE
				OSSetAlarm(&cover_alarm, OSSecondsToTicks(1.5), di_close_cover);
				#endif
			}
			break;
		}
	}

	di.reg.immbuf = result;
	di_complete_transfer();
}
#else
static void di_execute_command()
{
	#ifdef DVD
	if (di.reset)
		return;
	#endif

	switch (di.reg.cmdbuf0 >> 24) {
		case DI_CMD_READ:
		{
			uint32_t address = di.reg.mar;
			uint32_t length  = di.reg.length;
			uint32_t offset  = di.reg.cmdbuf1 << 2;

			#ifdef GCODE
			if (!length) {
				di_error(0x052400);
				return;
			}
			#endif

			if ((di.reg.cmdbuf0 & 0xC0) != 0x40) {
				perform_read(address, length, offset);
				return;
			}
			break;
		}
	}

	DI[2] = di.reg.cmdbuf0;
	DI[3] = di.reg.cmdbuf1;
	DI[4] = di.reg.cmdbuf2;
	DI[5] = di.reg.mar;
	DI[6] = di.reg.length;
	DI[8] = di.reg.immbuf;
	DI[7] = di.reg.cr;
}
#endif

static void di_read(unsigned index, uint32_t *value)
{
	#ifndef DI_PASSTHROUGH
	*value = di.regs[index];
	#else
	switch (index) {
		case 0 ... 1:
			*value = di.regs[index] | (DI[index] & 1);
			break;
		case 2 ... 4:
		case 7 ... 8:
			*value = di.regs[index];
			break;
		case 5 ... 6:
			if ((DI[7] & 0b011) == 0b011)
				*value = di.regs[index] = DI[index];
			else
				*value = di.regs[index];
			break;
		default:
			*value = DI[index];
	}
	#endif
}

static void di_write(unsigned index, uint32_t value)
{
	switch (index) {
		case 0:
			di.reg.sr = ((value & 0b1010100) ^ di.reg.sr) & di.reg.sr;
			di.reg.sr = (value & 0b0101011) | (di.reg.sr & 0b1010101);
			di_update_interrupts();
			break;
		case 1:
			di.reg.cvr = ((value & 0b100) ^ di.reg.cvr) & di.reg.cvr;
			di.reg.cvr = (value & 0b010) | (di.reg.cvr & ~0b010);
			di_update_interrupts();
			break;
		case 2 ... 4:
		case 8:
			di.regs[index] = value;
			break;
		case 5 ... 6:
			di.regs[index] = value & 0x3FFFFE0;
			break;
		case 7:
			di.regs[index] = value & 0b111;

			if (value & 0b001) {
				#ifdef DVD
				di_execute_command();
				#else
				OSSetAlarm(&di_alarm, COMMAND_LATENCY_TICKS, di_execute_command);
				#endif
			}
			break;
	}
}

#ifdef DTK
static struct {
	union {
		uint16_t regs[4];
		struct {
			uint32_t aima;
			uint32_t aibl;
		} reg;
	};

	struct {
		int aima;
	} req;

	uint8_t (*buffer[2])[640];
} dsp;

static void dsp_read(unsigned index, uint32_t *value)
{
	switch (index) {
		case 24:
		case 25:
			*value = ((uint16_t *)DSP)[index];
			dsp.req.aima--;
			break;
		default:
			*value = ((uint16_t *)DSP)[index];
	}
}

static void dsp_write(unsigned index, uint16_t value)
{
	switch (index) {
		case 24:
			dsp.regs[index - 24] = value & 0x3FF;
			dsp.req.aima++;
			break;
		case 25:
			dsp.regs[index - 24] = value & 0xFFE0;
			dsp.req.aima++;
			break;
		case 27:
			dsp.regs[index - 24] = value;

			if ((value & 0x8000) && dsp.req.aima >= 0) {
				void *in = OSPhysicalToUncached(dsp.reg.aima);
				int length = (dsp.reg.aibl & 0x7FFF) << 5;
				int count = length / sizeof(sample_t);
				void *out = in;

				if (length <= sizeof(**dsp.buffer)) {
					out = dsp.buffer[0];
					dsp.buffer[0] = dsp.buffer[1];
					dsp.buffer[1] = out;
				}

				uint32_t aicr = AI[0];
				uint32_t aivr = AI[1];

				if (aicr & 0b0000001) mix_samples(out, in, &dtk.fifo, count, aicr & 0b1000000, aivr, aivr >> 8);
				else if (out != in) out = memcpy(out, in, length);

				DSP[12] = (intptr_t)out;
				DSP[13] = ((length >> 5) & 0x7FFF) | 0x8000;

				dtk_fill_buffer();
			} else {
				DSP[12] = dsp.reg.aima;
				DSP[13] = dsp.reg.aibl;
			}

			dsp.req.aima = 0;
			break;
	}
}
#endif

#ifdef DVD
static void di_reset(void)
{
	uint32_t disr  = DI[0];
	uint32_t dicvr = DI[1];

	DI[0] = disr  & 0b1111110;
	DI[1] = dicvr & 0b110;

	if (di.reset > 0 && !(disr & 0b0010100)) return;
	if (di.reset > 3 &&  (disr & 0b0000100))
		di.reset = 3;

	switch (di.reset++) {
		case 0:
			DI[2] = 0xFF014D41;
			DI[3] = 0x54534849;
			DI[4] = 0x54410200;
			DI[7] = 0b001;
			break;
		case 1:
			DI[2] = 0xFF004456;
			DI[3] = 0x442D4741;
			DI[4] = 0x4D450300;
			DI[7] = 0b001;
			break;
		case 2:
			DI[2] = 0x55010000;
			DI[3] = 0;
			DI[4] = 0;
			DI[7] = 0b001;
			break;
		case 3:
			DI[2] = 0xFE114100;
			DI[3] = 0;
			DI[4] = 0;
			DI[7] = 0b001;
			break;
		case 4:
			DI[2] = 0xEE060300;
			DI[3] = 0;
			DI[4] = 0;
			DI[7] = 0b001;
			break;
		case 5:
			di.reset = 0;

			if (di.reg.cr & 0b001)
				di_execute_command();
			break;
	}
}
#endif

#ifdef DI_PASSTHROUGH
static void di_interrupt_handler(OSInterrupt interrupt, OSContext *context)
{
	#ifdef DVD
	if (di.reset) {
		di_reset();
		return;
	}
	#endif

	uint32_t disr  = DI[0];
	uint32_t dicvr = DI[1];

	DI[0] = disr  & 0b1111110;
	DI[1] = dicvr & 0b110;

	if (disr & 0b1010100) {
		di.reg.cr = DI[7];

		if (di.reg.cr & 0b010) {
			di.reg.mar    = DI[5];
			di.reg.length = DI[6];

			switch (di.reg.cmdbuf0 >> 24) {
				case DI_CMD_READ:
				{
					di.reg.length += OSRoundDown32B(di.reg.cmdbuf2 - DI[4]);
					break;
				}
			}
		} else {
			di.reg.immbuf = DI[8];

			switch (di.reg.cmdbuf0 >> 24) {
				case DI_CMD_REQUEST_ERROR:
				{
					if (!(di.reg.immbuf & 0xFFFFFF))
						di.reg.immbuf = di.error | (di.reg.immbuf & ~0xFFFFFF);
					di.error = 0;
					break;
				}
			}
		}
	}

	di.reg.sr  |= disr  & 0b1010100;
	di.reg.cvr |= dicvr & 0b100;
	di_update_interrupts();
}
#endif

static void pi_read(unsigned index, uint32_t *value)
{
	uint32_t mask = pi_get_interrupt_mask();

	switch (index) {
		case 0:
			if (!(pi.reg.intsr & pi.reg.intmsk))
				PI[0] = 1;
			*value = pi.reg.intsr | (PI[0] & ~mask);
			break;
		case 1:
			*value = pi.reg.intmsk;
			break;
		default:
			*value = PI[index];
	}
}

static void pi_write(unsigned index, uint32_t value)
{
	uint32_t mask = pi_get_interrupt_mask();

	switch (index) {
		case 0:
			PI[0] = value & ~mask;
			pi.reg.intsr = ((value & 0b11000000000011) ^ pi.reg.intsr) & pi.reg.intsr;
			pi_update_interrupts();
			break;
		case 1:
			PI[1] = (value & ~mask) | (PI[1] & mask);
			pi.reg.intmsk = value & 0b11111111111111;
			pi_update_interrupts();
			break;
		case 9:
			#ifndef DVD
			PI[index] = ((value << 2) & 0b100) | (value & ~0b100);
			break;
			#else
			if (*VAR_DRIVE_PATCHED) {
				PI[index] = ((value << 2) & 0b100) | (value & ~0b100);

				if (!di.reset && !(value & 0b100))
					di_reset();
				break;
			}
			#endif
		default:
			PI[index] = value;
	}
}

#ifndef NO_VIDEO
static OSAlarm vi_alarm;

static void vi_alarm_handler(OSAlarm *alarm, OSContext *context)
{
	VI[24] &= ~0x8000;
	VI[26] &= ~0x8000;

	if (unmask_user_interrupts(OS_INTERRUPTMASK_PI_VI) & OS_INTERRUPTMASK_DSP_AI)
		DSP[13] |= 0x8000;
}

static void vi_write(unsigned index, uint16_t value)
{
	switch (index) {
		case 0:
			if (VI[0] != value && ((value >> 4) & 0x3FF)) {
				if (mask_user_interrupts(OS_INTERRUPTMASK_PI_VI) & OS_INTERRUPTMASK_DSP_AI)
					DSP[13] &= ~0x8000;

				OSSetAlarm(&vi_alarm, OSSecondsToTicks(2), vi_alarm_handler);
			}
		default:
			VI[index] = value;
	}
}
#endif

static void efb_read(uint32_t address, uint32_t *value)
{
	uint16_t zmode = PE[0];
	PE[0] = 0;
	*(volatile uint32_t *)OSPhysicalToUncached(address) = address;
	*value = *(volatile uint32_t *)OSPhysicalToUncached(address);
	PE[0] = zmode;
}

static bool ppc_load32(uint32_t address, uint32_t *value)
{
	if ((address & ~0x3FFFFC) == 0x08400000) {
		efb_read(address, value);
		return true;
	}
	if ((address & ~0xFFC) == 0x0C003000) {
		pi_read((address >> 2) & 0x7F, value);
		return true;
	}
	if ((address & ~0x3FC) == 0x0C006000) {
		di_read((address >> 2) & 0xF, value);
		return true;
	}
	if ((address & ~0x3FC) == 0x0C006800) {
		exi_read((address >> 2) & 0xF, value);
		return true;
	}
	return false;
}

static bool ppc_store32(uint32_t address, uint32_t value)
{
	if ((address & ~0xFFC) == 0x0C003000) {
		pi_write((address >> 2) & 0x7F, value);
		return true;
	}
	if ((address & ~0x3FC) == 0x0C006000) {
		di_write((address >> 2) & 0xF, value);
		return true;
	}
	if ((address & ~0x3FC) == 0x0C006800) {
		exi_write((address >> 2) & 0xF, value);
		return true;
	}
	if ((address & ~0b111) == 0x800000E8) {
		if (value == 0xfee1dead) {
			asm volatile("mtdabr %0" :: "r" (0x800000E8));
			reset_devices();
		}
		return true;
	}
	return false;
}

static bool ppc_store8(uint32_t address, uint8_t value)
{
	if ((address & ~0b111) == 0x800000E8) {
		return true;
	}
	return false;
}

static bool ppc_load16(uint32_t address, uint32_t *value)
{
	#ifdef DTK
	if ((address & ~0xFFE) == 0x0C005000) {
		dsp_read((address >> 1) & 0xFF, value);
		return true;
	}
	#endif
	return false;
}

static bool ppc_store16(uint32_t address, uint16_t value)
{
	#ifndef NO_VIDEO
	if ((address & ~0xFFE) == 0x0C002000) {
		vi_write((address >> 1) & 0xFF, value);
		return true;
	}
	#endif
	#ifdef DTK
	if ((address & ~0xFFE) == 0x0C005000) {
		dsp_write((address >> 1) & 0xFF, value);
		return true;
	}
	#endif
	return false;
}

static bool ppc_step(ppc_context_t *context)
{
	PowerPC_instr instr = *(PowerPC_instr *)context->srr0;

	switch (PPC_GET_OPCODE(instr)) {
		case PPC_OPCODE_X:
		{
			switch (PPC_GET_FUNC(instr)) {
				case PPC_FUNC_LWZX:
				{
					int rd = PPC_GET_RD(instr);
					int ra = PPC_GET_RA(instr);
					int rb = PPC_GET_RB(instr);
					return ppc_load32(ra ? context->gpr[ra] + context->gpr[rb] : context->gpr[rb], &context->gpr[rd]);
				}
				case PPC_FUNC_STWX:
				{
					int rs = PPC_GET_RS(instr);
					int ra = PPC_GET_RA(instr);
					int rb = PPC_GET_RB(instr);
					return ppc_store32(ra ? context->gpr[ra] + context->gpr[rb] : context->gpr[rb], context->gpr[rs]);
				}
				#ifndef NO_VIDEO
				case PPC_FUNC_STHX:
				{
					int rs = PPC_GET_RS(instr);
					int ra = PPC_GET_RA(instr);
					int rb = PPC_GET_RB(instr);
					return ppc_store16(ra ? context->gpr[ra] + context->gpr[rb] : context->gpr[rb], context->gpr[rs]);
				}
				#endif
			}
			break;
		}
		case PPC_OPCODE_LWZ:
		{
			int rd = PPC_GET_RD(instr);
			int ra = PPC_GET_RA(instr);
			short d = PPC_GET_IMMED(instr);
			return ppc_load32(ra ? context->gpr[ra] + d : d, &context->gpr[rd]);
		}
		case PPC_OPCODE_STW:
		{
			int rs = PPC_GET_RS(instr);
			int ra = PPC_GET_RA(instr);
			short d = PPC_GET_IMMED(instr);
			return ppc_store32(ra ? context->gpr[ra] + d : d, context->gpr[rs]);
		}
		case PPC_OPCODE_STBU:
		{
			int rs = PPC_GET_RS(instr);
			int ra = PPC_GET_RA(instr);
			short d = PPC_GET_IMMED(instr);
			return ppc_store8(ra ? context->gpr[ra] += d : d, context->gpr[rs]);
		}
		#ifdef DTK
		case PPC_OPCODE_LHZ:
		{
			int rd = PPC_GET_RD(instr);
			int ra = PPC_GET_RA(instr);
			short d = PPC_GET_IMMED(instr);
			return ppc_load16(ra ? context->gpr[ra] + d : d, &context->gpr[rd]);
		}
		case PPC_OPCODE_STH:
		{
			int rs = PPC_GET_RS(instr);
			int ra = PPC_GET_RA(instr);
			short d = PPC_GET_IMMED(instr);
			return ppc_store16(ra ? context->gpr[ra] + d : d, context->gpr[rs]);
		}
		#endif
	}

	return false;
}

ppc_context_t *service_exception(ppc_context_t *context)
{
	if (ppc_step(context))
		context->srr0 += sizeof(PowerPC_instr);
	else
		__builtin_trap();

	return context;
}

void dsi_exception_vector(void);

void write_branch(void *a, void *b)
{
	PowerPC_instr instr = (PowerPC_instr)(b - (OS_BASE_CACHED - 0x48000002));

	if (*(PowerPC_instr *)a != instr) {
		*(PowerPC_instr *)a  = instr;
		asm volatile("dcbst 0,%0; sync; icbi 0,%0" :: "r" (a));
	}
}

void init(void **arenaLo, void **arenaHi)
{
	OSCreateAlarm(&di_alarm);
	OSCreateAlarm(&cover_alarm);
	OSCreateAlarm(&read_alarm);

	write_branch((void *)0x80000300, dsi_exception_vector);

	#ifdef BBA
	bba_init(arenaLo, arenaHi);
	#endif
	#ifdef ETH_EMULATOR
	eth_init(arenaLo, arenaHi);
	#endif
	#ifdef DI_PASSTHROUGH
	DI[0] = 0b0101010;
	DI[1] = 0b010;

	set_interrupt_handler(OS_INTERRUPT_PI_DI, di_interrupt_handler);
	unmask_interrupts(OS_INTERRUPTMASK_PI_DI);
	#endif
	unmask_interrupts(OS_INTERRUPTMASK_PI_ERROR);
	#ifdef DTK
	*arenaHi -= sizeof(*dsp.buffer[0]); dsp.buffer[0] = OSCachedToUncached(*arenaHi);
	*arenaHi -= sizeof(*dsp.buffer[1]); dsp.buffer[1] = OSCachedToUncached(*arenaHi);
	*arenaHi -= sizeof(*dtk.buffer); dtk.buffer = *arenaHi;
	*arenaHi -= 7168; fifo_init(&dtk.fifo, *arenaHi, 7168);
	#endif
}

void idle_thread(void)
{
	OSDisableInterrupts();
	#ifndef ASYNC_READ
	trickle_read();
	#endif
	enable_interrupts_idle();
}

void fini(void)
{
	OSDisableInterrupts();
	reset_devices();
}
