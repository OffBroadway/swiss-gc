#include <stddef.h> // bug ipc doesn't include this
#include <string.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "flippy_sync.h"

#ifdef NOSYS
// we don't know where this is defined
extern void DCInvalidateRange(void *startaddress, u32 len);
#endif

// DI regs from YAGCD
#define DI_SR      0 // 0xCC006000 - DI Status Register
#define DI_SR_BRKINT     (1 << 6) // Break Complete Interrupt Status
#define DI_SR_BRKINTMASK (1 << 5) // Break Complete Interrupt Mask. 0:masked, 1:enabled
#define DI_SR_TCINT      (1 << 4) // Transfer Complete Interrupt Status
#define DI_SR_TCINTMASK  (1 << 3) // Transfer Complete Interrupt Mask. 0:masked, 1:enabled
#define DI_SR_DEINT      (1 << 2) // Device Error Interrupt Status
#define DI_SR_DEINTMASK  (1 << 1) // Device Error Interrupt Mask. 0:masked, 1:enabled
#define DI_SR_BRK        (1 << 0) // DI Break

#define DI_CVR     1 // 0xCC006004 - DI Cover Register (status2)
#define DI_CMDBUF0 2 // 0xCC006008 - DI Command Buffer 0
#define DI_CMDBUF1 3 // 0xCC00600c - DI Command Buffer 1 (offset in 32 bit words)
#define DI_CMDBUF2 4 // 0xCC006010 - DI Command Buffer 2 (source length)
#define DI_MAR     5 // 0xCC006014 - DMA Memory Address Register
#define DI_LENGTH  6 // 0xCC006018 - DI DMA Transfer Length Register
#define DI_CR      7 // 0xCC00601c - DI Control Register
#define DI_CR_RW     (1 << 2) // access mode, 0:read, 1:write
#define DI_CR_DMA    (1 << 1) // 0: immediate mode, 1: DMA mode (*1)
#define DI_CR_TSTART (1 << 0) // transfer start. write 1: start transfer, read 1: transfer pending (*2)

#define DI_IMMBUF  8 // 0xCC006020 - DI immediate data buffer (error code ?)
#define DI_CFG     9 // 0xCC006024 - DI Configuration Register

// DI Commands
#define DVD_OEM_INQUIRY 0x12000000
#define DVD_OEM_READ 0xA8000000
#define DVD_FLIPPY_BOOTLOADER_STATUS 0xB4000000
#define DVD_FLIPPY_FILEAPI_BASE 0xB5000000

static vu32* const _di_regs = (vu32*)0xCC006000;

// === OEM commands

static GCN_ALIGNED(dvd_info_t) info = { 0 };
dvd_info_t *dvd_inquiry() {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = ((u32)&info) & 0x1FFFFFFF; // Cached -> Effective
    _di_regs[DI_LENGTH] = sizeof(dvd_info_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register
    
    DCInvalidateRange((u8 *)&info, sizeof(dvd_info_t));

    return &info;
}

// === bootloader commands

int dvd_bootloader_status(firmware_status_blob_t* dst) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_BOOTLOADER_STATUS;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(firmware_status_blob_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register

    DCInvalidateRange(dst, sizeof(firmware_status_blob_t));

    // fix string termination
    dst->status_text[63] = 0;
    dst->status_sub[63] = 0;

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

void dvd_bootloader_boot() {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
    _di_regs[DI_CMDBUF1] = 0xabadbeef;
    _di_regs[DI_CMDBUF2] = 0xcafe6969;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART;

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register

    return;
}

void dvd_bootloader_update() {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
    _di_regs[DI_CMDBUF1] = 0xabadbeef;
    _di_regs[DI_CMDBUF2] = 0xdabfed69;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART;

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register

    return;
}

void dvd_bootloader_noupdate() {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
    _di_regs[DI_CMDBUF1] = 0xabadbeef;
    _di_regs[DI_CMDBUF2] = 0xdecaf420;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART;

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register

    return;
}

// === flippy custom commands

void dvd_custom_close(uint32_t fd) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_CLOSE | ((fd & 0xFF) << 16);
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART; // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register
}

void dvd_set_default_fd(uint32_t fd) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_SET_DEFAULT_FD | ((fd & 0xFF) << 16);
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART; // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register
}

int dvd_custom_write(char *buf, uint32_t offset, uint32_t length, uint32_t fd) {
    DCFlushRange(buf, length);

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_WRITE | ((fd & 0xFF) << 16);
    _di_regs[DI_CMDBUF1] = offset;
    _di_regs[DI_CMDBUF2] = length;

    _di_regs[DI_MAR] = (u32)buf & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = (length + 31) & 0xFFFFFFE0;
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART);
    while (_di_regs[DI_CR] & DI_CR_TSTART) {
        if (ticks_to_millisecs(gettime()) % 1000 == 0) {
            print_gecko("Still writing: %u/%u\n", (u32)_di_regs[DI_LENGTH], length);
        }
    }

    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_read(void* dst, unsigned int len, uint64_t offset, unsigned int fd) {
    if (offset >> 2 > 0xFFFFFFFF) return -1;

    /* TODO What was going on with this setup code previously? Seems wrong
    if ((((int)dst) & 0xC0000000) == 0x80000000) // cached?
    {
        dvd[0] = 0x2E;
    }
    */
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_OEM_READ | ((fd & 0xFF) << 16);
    _di_regs[DI_CMDBUF1] = offset >> 2;
    _di_regs[DI_CMDBUF2] = len;

    _di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = len;
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART); // transfer complete register

    DCInvalidateRange(dst, len);

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_read_data(void* dst, unsigned int len, uint64_t offset, unsigned int fd) {
    uint64_t current_offset = offset;
    unsigned int total_read = 0;
    unsigned int remaining = len;

    GCN_ALIGNED(u8) aligned_buffer[FD_IPC_MAXRESP];

    while (remaining > 0) {
        unsigned int to_read = remaining > FD_IPC_MAXRESP ? FD_IPC_MAXRESP : remaining;
        int result = dvd_read(aligned_buffer, to_read, current_offset, fd);
        if (result != 0) {
            print_gecko("dvd_read_data failed: %d\n", result);
            return result;  // Return the error code if dvd_read fails
        }

        memcpy(dst + total_read, aligned_buffer, to_read);
        total_read += to_read;
        current_offset += to_read;
        remaining -= to_read;
    }

    return 0;
}

int dvd_custom_status(file_status_t* status) {
    memset(status, 0, sizeof(file_status_t));
    status->result = 1;

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_READ_STATUS;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)(status) & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_status_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    DCInvalidateRange(status, sizeof(file_status_t));

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return -1;
    }
    return 0;
}

int dvd_custom_fs_info(fs_info_t* status) {
    memset(status, 0, sizeof(fs_info_t));
    status->result = 1;

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FS_INFO;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)(status) & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(fs_info_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    DCInvalidateRange(status, sizeof(fs_info_t));

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return -1;
    }
    return 0;
}

int dvd_custom_readdir(file_entry_t* dst, unsigned int fd) {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_READDIR | ((fd & 0xFF) << 16);
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    DCInvalidateRange(dst, sizeof(file_entry_t));

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_custom_unlink(char *path) {
    GCN_ALIGNED(file_entry_t) entry;

    strncpy(entry.name, path, 256);
    entry.name[255] = 0;

    DCFlushRange(&entry, sizeof(file_entry_t));

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_UNLINK;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0; //TODO this was sizeof(file_entry_t) before for no particular reason

    _di_regs[DI_MAR] = (u32)&entry & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_custom_mkdir(char *path) {
    GCN_ALIGNED(file_entry_t) entry;

    strncpy(entry.name, path, 256);
    entry.name[255] = 0;

    DCFlushRange(&entry, sizeof(file_entry_t));

	_di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
	_di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_MKDIR;
    _di_regs[DI_CMDBUF1] = 0;
	_di_regs[DI_CMDBUF2] = 0; //TODO this was sizeof(file_entry_t) before for no particular reason

    _di_regs[DI_MAR] = (u32)&entry & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    // check if ERR was asserted
	if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
	return 0;
}

int dvd_custom_unlink_flash(char *path) {
    GCN_ALIGNED(file_entry_t) entry;

    strncpy(entry.name, path, 256);
    entry.name[255] = 0;

    DCFlushRange(&entry, sizeof(file_entry_t));

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_UNLINK_FLASH;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0; //TODO this was sizeof(file_entry_t) before for no particular reason

    _di_regs[DI_MAR] = (u32)&entry & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_custom_open(char *path, uint8_t type, uint8_t flags) {
    GCN_ALIGNED(file_entry_t) entry;

    strncpy(entry.name, path, 256);
    entry.name[255] = 0;
    entry.type = type;
    entry.flags = flags;

    DCFlushRange(&entry, sizeof(file_entry_t));

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_OPEN;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0; //TODO this was sizeof(file_entry_t) before for no particular reason

    _di_regs[DI_MAR] = (u32)&entry & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

int dvd_custom_open_flash(char *path, uint8_t type, uint8_t flags) {
    GCN_ALIGNED(file_entry_t) entry;

    strncpy(entry.name, path, 256);
    entry.name[255] = 0;
    entry.type = type;
    entry.flags = flags;

    DCFlushRange(&entry, sizeof(file_entry_t));

    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_OPEN_FLASH;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0; //TODO this was sizeof(file_entry_t) before for no particular reason

    _di_regs[DI_MAR] = (u32)&entry & 0x1FFFFFFF;
    _di_regs[DI_LENGTH] = sizeof(file_entry_t);
    _di_regs[DI_CR] = (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART); // start transfer

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    // check if ERR was asserted
    if (_di_regs[DI_SR] & DI_SR_DEINT) {
        return 1;
    }
    return 0;
}

void dvd_custom_bypass() {
    _di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
    _di_regs[DI_CVR] = 0; // clear cover int

    _di_regs[DI_CMDBUF0] = 0xDC000000;
    _di_regs[DI_CMDBUF1] = 0;
    _di_regs[DI_CMDBUF2] = 0;

    _di_regs[DI_MAR] = 0;
    _di_regs[DI_LENGTH] = 0;
    _di_regs[DI_CR] = DI_CR_TSTART;

    while (_di_regs[DI_CR] & DI_CR_TSTART)
        ; // transfer complete register

    return;
}