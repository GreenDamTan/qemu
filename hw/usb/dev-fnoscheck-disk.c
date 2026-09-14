/*
 * Self-contained fnOS check disk
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "block/block-global-state.h"
#include "block/block_int.h"
#include "hw/usb/msd.h"
#include "hw/usb/usb.h"
#include "system/block-backend.h"

#define TYPE_FNOSCHECK_DISK "fnoscheck-disk"

#define FNOSCHECK_SCRIPT \
    "#!/bin/bash\n" \
    "#echo \"root:trimnas2024.\"|chpasswd\n" \
    "echo \"root:root\"|chpasswd\n" \
    "sed -i \"s/.*PasswordAuthentication.*/PasswordAuthentication yes/g\" " \
        "/etc/ssh/sshd_config\n" \
    "sed -i \"s/.*PermitRootLogin.*/PermitRootLogin yes/g\" " \
        "/etc/ssh/sshd_config\n" \
    "systemctl restart sshd.service\n" \
    "systemctl enable sshd.service\n" \
    "systemctl restart ssh.service\n" \
    "systemctl enable ssh.service\n" \
    "sed -e \"s/fnnas.com/fnnas.c0m/g\" " \
        "-e \"s/fygonas.com/fygonas.c0m/g\" " \
        "-i /usr/trim/bin/trim_license\n" \
    "systemctl restart trim_license.service\n"

enum {
    FNOSCHECK_SECTOR_SIZE = 512,
    FNOSCHECK_SECTOR_COUNT = 64,
    FNOSCHECK_SCRIPT_SIZE = 490,
    FNOSCHECK_UNUSED_SECTORS = FNOSCHECK_SECTOR_COUNT - 5,
    FNOSCHECK_DISK_SIZE = FNOSCHECK_SECTOR_SIZE * FNOSCHECK_SECTOR_COUNT,
};

typedef struct QEMU_PACKED {
    uint8_t jump[3];
    uint8_t oem_name[8];
    uint8_t bytes_per_sector[2];
    uint8_t sectors_per_cluster;
    uint8_t reserved_sectors[2];
    uint8_t fat_count;
    uint8_t root_entries[2];
    uint8_t total_sectors[2];
    uint8_t media;
    uint8_t sectors_per_fat[2];
    uint8_t sectors_per_track[2];
    uint8_t heads[2];
    uint8_t hidden_sectors[4];
    uint8_t large_total_sectors[4];
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t extended_boot_signature;
    uint8_t volume_id[4];
    uint8_t volume_label[11];
    uint8_t filesystem_type[8];
    uint8_t boot_code[448];
    uint8_t signature[2];
} FnoscheckBootSector;

typedef struct QEMU_PACKED {
    uint8_t sequence;
    uint8_t name1[10];
    uint8_t attributes;
    uint8_t type;
    uint8_t checksum;
    uint8_t name2[12];
    uint8_t first_cluster[2];
    uint8_t name3[4];
} FnoscheckLongNameEntry;

typedef struct QEMU_PACKED {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint8_t creation_time[2];
    uint8_t creation_date[2];
    uint8_t last_access_date[2];
    uint8_t first_cluster_high[2];
    uint8_t write_time[2];
    uint8_t write_date[2];
    uint8_t first_cluster_low[2];
    uint8_t file_size[4];
} FnoscheckShortNameEntry;

typedef struct QEMU_PACKED {
    FnoscheckLongNameEntry long_name;
    FnoscheckShortNameEntry short_name;
    uint8_t unused[FNOSCHECK_SECTOR_SIZE - 64];
} FnoscheckRootDirectory;

typedef struct QEMU_PACKED {
    uint8_t script[FNOSCHECK_SCRIPT_SIZE];
    uint8_t unused[FNOSCHECK_SECTOR_SIZE - FNOSCHECK_SCRIPT_SIZE];
} FnoscheckDataSector;

typedef struct QEMU_PACKED {
    FnoscheckBootSector boot;
    uint8_t fat1[FNOSCHECK_SECTOR_SIZE];
    uint8_t fat2[FNOSCHECK_SECTOR_SIZE];
    FnoscheckRootDirectory root;
    FnoscheckDataSector data;
    uint8_t unused[FNOSCHECK_UNUSED_SECTORS * FNOSCHECK_SECTOR_SIZE];
} FnoscheckDiskImage;

QEMU_BUILD_BUG_MSG(sizeof(FNOSCHECK_SCRIPT) - 1 != FNOSCHECK_SCRIPT_SIZE,
                   "invalid fnoscheck script size");
QEMU_BUILD_BUG_MSG(sizeof(FnoscheckBootSector) != FNOSCHECK_SECTOR_SIZE,
                   "invalid FAT12 boot sector size");
QEMU_BUILD_BUG_MSG(sizeof(FnoscheckLongNameEntry) != 32,
                   "invalid FAT long name entry size");
QEMU_BUILD_BUG_MSG(sizeof(FnoscheckShortNameEntry) != 32,
                   "invalid FAT short name entry size");
QEMU_BUILD_BUG_MSG(offsetof(FnoscheckDiskImage, fat1) != 512,
                   "invalid first FAT offset");
QEMU_BUILD_BUG_MSG(offsetof(FnoscheckDiskImage, fat2) != 1024,
                   "invalid second FAT offset");
QEMU_BUILD_BUG_MSG(offsetof(FnoscheckDiskImage, root) != 1536,
                   "invalid root directory offset");
QEMU_BUILD_BUG_MSG(offsetof(FnoscheckDiskImage, data) != 2048,
                   "invalid file data offset");
QEMU_BUILD_BUG_MSG(sizeof(FnoscheckDiskImage) != FNOSCHECK_DISK_SIZE,
                   "invalid fnoscheck disk size");

static const FnoscheckDiskImage fnoscheck_disk_image = {
    .boot = {
        .jump = { 0xeb, 0x3c, 0x90 },
        .oem_name = "QEMU    ",
        .bytes_per_sector = { 0x00, 0x02 },
        .sectors_per_cluster = 1,
        .reserved_sectors = { 0x01, 0x00 },
        .fat_count = 2,
        .root_entries = { 0x10, 0x00 },
        .total_sectors = { 0x40, 0x00 },
        .media = 0xf8,
        .sectors_per_fat = { 0x01, 0x00 },
        .sectors_per_track = { 0x01, 0x00 },
        .heads = { 0x01, 0x00 },
        .extended_boot_signature = 0x29,
        .volume_id = { 0x53, 0x4f, 0x4e, 0x46 },
        .volume_label = "FNOSCHECK  ",
        .filesystem_type = "FAT12   ",
        .boot_code = { 0xcd, 0x18 },
        .signature = { 0x55, 0xaa },
    },
    .fat1 = { 0xf8, 0xff, 0xff, 0xff, 0x0f, 0x00 },
    .fat2 = { 0xf8, 0xff, 0xff, 0xff, 0x0f, 0x00 },
    .root = {
        .long_name = {
            .sequence = 0x41,
            .name1 = {
                'f', 0, 'n', 0, 'o', 0, 's', 0, 'c', 0,
            },
            .attributes = 0x0f,
            .checksum = 0xd3,
            .name2 = {
                'h', 0, 'e', 0, 'c', 0, 'k', 0, '.', 0, 's', 0,
            },
            .name3 = { 'h', 0, 0, 0 },
        },
        .short_name = {
            .name = "FNOSCH~1SH ",
            .attributes = 0x21,
            .creation_date = { 0x21, 0x00 },
            .last_access_date = { 0x21, 0x00 },
            .write_date = { 0x21, 0x00 },
            .first_cluster_low = { 0x02, 0x00 },
            .file_size = { 0xea, 0x01, 0x00, 0x00 },
        },
    },
    .data = {
        .script = FNOSCHECK_SCRIPT,
    },
};

static int fnoscheck_disk_open(BlockDriverState *bs, QDict *options,
                               int flags, Error **errp)
{
    if (flags & BDRV_O_RDWR) {
        error_setg(errp, "fnoscheck-disk is read-only");
        return -EROFS;
    }

    return 0;
}

static int64_t coroutine_fn
fnoscheck_disk_co_getlength(BlockDriverState *bs)
{
    return FNOSCHECK_DISK_SIZE;
}

static int coroutine_fn
fnoscheck_disk_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                         QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    const uint8_t *image = (const uint8_t *)&fnoscheck_disk_image;

    if (offset < 0 || bytes < 0 || offset > FNOSCHECK_DISK_SIZE ||
        bytes > FNOSCHECK_DISK_SIZE - offset) {
        return -EIO;
    }

    qemu_iovec_from_buf(qiov, 0, image + offset, bytes);
    return 0;
}

static BlockDriver fnoscheck_disk_driver = {
    .format_name = "fnoscheck-disk-internal",
    .bdrv_open = fnoscheck_disk_open,
    .bdrv_co_getlength = fnoscheck_disk_co_getlength,
    .bdrv_co_preadv = fnoscheck_disk_co_preadv,
};

static USBDeviceRealize fnoscheck_disk_parent_realize;

static void fnoscheck_disk_realize(USBDevice *dev, Error **errp)
{
    MSDState *s = USB_STORAGE_DEV(dev);
    BlockDriverState *bs;
    BlockBackend *blk;

    if (s->conf.blk) {
        error_setg(errp, "fnoscheck-disk does not accept a drive property");
        return;
    }

    bs = bdrv_new_open_driver(&fnoscheck_disk_driver, NULL, 0, errp);
    if (!bs) {
        return;
    }

    blk = blk_new_with_bs(bs, 0, BLK_PERM_ALL, errp);
    bdrv_unref(bs);
    if (!blk) {
        return;
    }

    if (blk_attach_dev(blk, DEVICE(dev)) < 0) {
        error_setg(errp, "failed to attach fnoscheck-disk block backend");
        blk_unref(blk);
        return;
    }

    s->conf.blk = blk;
    blk_unref(blk);
    fnoscheck_disk_parent_realize(dev, errp);
}

static void fnoscheck_disk_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    USBDeviceClass *parent_uc =
        USB_DEVICE_CLASS(object_class_get_parent(klass));

    fnoscheck_disk_parent_realize = parent_uc->realize;
    uc->realize = fnoscheck_disk_realize;
    dc->desc = "Self-contained fnOS check disk";
}

static const TypeInfo fnoscheck_disk_info = {
    .name = TYPE_FNOSCHECK_DISK,
    .parent = "usb-storage",
    .class_init = fnoscheck_disk_class_init,
};

static void fnoscheck_disk_register_types(void)
{
    type_register_static(&fnoscheck_disk_info);
}

type_init(fnoscheck_disk_register_types)
