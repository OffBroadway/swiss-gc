#include <gctypes.h>
#include <ogc/cache.h>
#include "flippy_ipc.h"

#include "util.h"

// ============================================================================

typedef struct __attribute__((packed, aligned(32)))
{
	uint32_t magic;
	uint8_t show_video;
	uint8_t show_progress_bar;
	uint16_t current_progress;
	char status_text[64];
	char status_sub[64];
	uint8_t padding[120];
} firmware_status_blob_t;

typedef struct __attribute__((__packed__)) {
    uint8_t major;
    uint8_t minor;
    uint16_t build;
} flippy_version_parts_t;

typedef struct __attribute__((__packed__)) {
    u16 rev_level;
    u16 dev_code;
    u32 rel_date;
    u32 fd_flags;
    flippy_version_parts_t fw_ver;
    u8 pad1[16];
} dvd_info_t;

// oem
dvd_info_t *dvd_inquiry();

// bootloader
int dvd_bootloader_status(firmware_status_blob_t*dst);
void dvd_bootloader_boot();
void dvd_bootloader_update();
void dvd_bootloader_noupdate();

// custom
void dvd_custom_close(uint32_t fd);
void dvd_set_default_fd(uint32_t fd);
int dvd_custom_write(char *buf, uint32_t offset, uint32_t length, uint32_t fd);
int dvd_read(void *dst, unsigned int len, uint64_t offset, uint32_t fd);
int dvd_custom_status(file_status_t* status);
int dvd_custom_status_flash(file_status_t *dst);
int dvd_custom_readdir(file_entry_t *dst, uint32_t fd);
int dvd_custom_unlink(char *path);
int dvd_custom_unlink_flash(char *path);
int dvd_custom_open(char *path, uint8_t type, uint8_t flags);
int dvd_custom_open_flash(char *path, uint8_t type, uint8_t flags);
void dvd_custom_bypass();

// utils
int dvd_read_data(void* dst, unsigned int len, uint64_t offset, unsigned int fd);
