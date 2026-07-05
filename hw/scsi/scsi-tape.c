/*
 *  SCSI Tape Device Emulation
 *
 *  Copyright (c) 2026 - Emmanuel Ugwu <emmanuelugwu121@gmail.com>
 *                       Ashirvad Mohanty <ashirvadm04@gmail.com>
 *
 *  Based on scsi-disk.c
 *  Mentored-by: Helge Deller <deller@gmx.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/hw-version.h"
#include "qemu/memalign.h"
#include "qemu/target-info.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/emulation.h"
#include "scsi/constants.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "hw/block/block.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/dma.h"
#include "system/system.h"
#include "qemu/cutils.h"
#include "trace.h"
#include "qom/object.h"

#define MAX_SERIAL_LEN           36
#define MAX_SERIAL_LEN_FOR_DEVID 20
#define TYPE_SCSI_TAPE_BASE      "scsi-tape"

#define SCSI_MAX_INQUIRY_LEN     256
#define SCSI_TAPE_BLOCK_SIZE     512

OBJECT_DECLARE_TYPE(SCSITapeState, SCSITapeClass, SCSI_TAPE_BASE)

typedef struct SCSITapeClass {
    SCSIDeviceClass parent_class;
} SCSITapeClass;

typedef struct SCSITapeReq {
    SCSIRequest req;
    struct iovec iov;
    uint32_t buflen;
    QEMUIOVector qiov;
    BlockAcctCookie acct;
} SCSITapeReq;

typedef struct SCSITapeState {
    SCSIDevice qdev;
    uint64_t position;
    bool at_filemark;
    bool eof;
    bool bot;
    bool eot;
    bool loaded;
    char *vendor;
    char *version;
    char *serial;
    char *product;
    char *device_id;
} SCSITapeState;

static void scsi_free_request(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);

    qemu_vfree(r->iov.iov_base);
}


/* Helper function for command completion with sense.  */
static void scsi_check_condition(SCSITapeReq *r, SCSISense sense)
{
    trace_scsi_tape_check_condition(r->req.tag, sense.key, sense.asc,
                                    sense.ascq);
    scsi_req_build_sense(&r->req, sense);
    scsi_req_complete(&r->req, CHECK_CONDITION);
}

static void scsi_tape_read_data(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    uint32_t buflen;

    buflen = r->iov.iov_len;
    if (buflen) {
        r->iov.iov_len = 0;  /* prevent loop during read */
        scsi_req_data(&r->req, buflen);
    } else {
        scsi_req_complete(&r->req, GOOD);
    }
}

static void scsi_tape_write_data(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    int ret;

    if (!s->loaded || !blk_is_available(s->qdev.conf.blk)) {
        scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
        return;
    }

    ret = blk_pwrite(s->qdev.conf.blk, s->position, r->iov.iov_len,
                     r->iov.iov_base, 0);
    if (ret < 0) {
        scsi_check_condition(r, SENSE_CODE(IO_ERROR));
        return;
    }

    s->position += r->iov.iov_len;
    scsi_req_complete(&r->req, GOOD);
}

static uint8_t *scsi_tape_get_buf(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    return (uint8_t *)r->iov.iov_base;
}

static void scsi_tape_emulate_rewind(SCSIRequest *r)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, r->dev);
    s->position = 0;
    s->bot = true;
    s->eot = false;
    s->eof = false;
    s->at_filemark = false;
}

static int scsi_tape_emulate_vpd_page(SCSIRequest *req, uint8_t *outbuf)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    uint8_t page_code = req->cmd.buf[2];
    int start, buflen = 0;

    outbuf[buflen++] = s->qdev.type & 0x1f;
    outbuf[buflen++] = page_code;
    outbuf[buflen++] = 0x00;
    outbuf[buflen++] = 0x00;
    start = buflen;

    switch (page_code) {
    case 0x00:
    {
        trace_scsi_tape_emulate_vpd_page_00(req->cmd.xfer);
        outbuf[buflen++] = 0x00;
        if (s->serial) {
            outbuf[buflen++] = 0x80;
        }
        outbuf[buflen++] = 0x83;
        break;
    }
    case 0x80:
    {
        int l;

        if (!s->serial) {
            trace_scsi_tape_emulate_vpd_page_80_not_supported();
            return -1;
        }
        l = strlen(s->serial);

        if (l > MAX_SERIAL_LEN) {
            l = MAX_SERIAL_LEN;
        }

        trace_scsi_tape_emulate_vpd_page_80(req->cmd.xfer);
        memcpy(outbuf + buflen, s->serial, l);
        buflen += l;
        break;
    }
    case 0x83:
    {
        int id_len = s->device_id ? MIN(strlen(s->device_id), 255 - 8) : 0;

        trace_scsi_tape_emulate_vpd_page_83(req->cmd.xfer);

        if (id_len) {
            outbuf[buflen++] = 0x2; /* ASCII */
            outbuf[buflen++] = 0;   /* not officially assigned */
            outbuf[buflen++] = 0;   /* reserved */
            outbuf[buflen++] = id_len; /* length of data following */
            memcpy(outbuf + buflen, s->device_id, id_len);
            buflen += id_len;
        }

        break;

    }
    default:
        return -1;
    }
    assert(buflen - start <= 255);
    outbuf[start - 1] = buflen - start;
    return buflen;
}

static int scsi_tape_emulate_inquiry(SCSIRequest *req, uint8_t *outbuf)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    int buflen = 0;

    if (req->cmd.buf[1] & 0x1) {
        /* Vital product data */
        return scsi_tape_emulate_vpd_page(req, outbuf);
    }
    /* Standard Inquiry data */
    if (req->cmd.buf[2] != 0) {
        return -1;
    }
    /* Page code = 0 */
    buflen = req->cmd.xfer;
    if (buflen > SCSI_MAX_INQUIRY_LEN) {
        buflen = SCSI_MAX_INQUIRY_LEN;
    }

    outbuf[0] = s->qdev.type & 0x1f;
    outbuf[1] = 0x80;

    strpadcpy((char *) &outbuf[16], 16, s->product ?: "HP 35480A", ' ');
    strpadcpy((char *) &outbuf[8],  8,  s->vendor  ?: "HP",        ' ');

    memset(&outbuf[32], 0, 4);
    memcpy(&outbuf[32], s->version, MIN(4, strlen(s->version)));

    outbuf[2] = s->qdev.default_scsi_version;
    outbuf[3] = 2 | 0x10; /* Format 2, HiSup */

    if (buflen > 36) {
        outbuf[4] = buflen - 5; /* Additional Length = (Len - 1) - 4 */
    } else {
        /* If the allocation length of CDB is too small,
         * the additional length is not adjusted */
        outbuf[4] = 36 - 5;
    }

    /* Sync data transfer and TCQ.*/
    outbuf[7] = 0x10 | (req->bus->info->tcq ? 0x02 : 0);
    return buflen;
}

static int scsi_tape_mode_sense_page(SCSITapeState *s, uint8_t page,
                                     uint8_t page_control, uint8_t **p_outbuf)
{
    uint8_t *outbuf = *p_outbuf;

    switch (page) {
    case MODE_PAGE_R_W_ERROR:
        outbuf[0] = page;
        outbuf[1] = 0x0a;
        if (page_control != 1) {
            outbuf[2] = 0x80; /* Automatic write reallocation enabled */
        }
        *p_outbuf += 12;
        return 12;

    case MODE_PAGE_CONTROL:
        outbuf[0] = page;
        outbuf[1] = 0x0a;
        if (page_control != 1) {
            outbuf[3] = s->qdev.default_scsi_version >= 3 ? 0x10 : 0x00;
        }
        *p_outbuf += 12;
        return 12;

    default:
        return -1;
    }
}

static int scsi_tape_emulate_mode_sense(SCSIRequest *req, uint8_t *outbuf)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    bool dbd = req->cmd.buf[1] & 0x08;
    bool mode_sense_10 = req->cmd.buf[0] == MODE_SENSE_10;
    uint8_t page = req->cmd.buf[2] & 0x3f;
    uint8_t page_control = (req->cmd.buf[2] & 0xc0) >> 6;
    uint8_t dev_specific_param = blk_is_writable(s->qdev.conf.blk) ? 0 : 0x80;
    uint8_t *p;
    int header_len = mode_sense_10 ? 8 : 4;
    int block_desc_len = dbd ? 0 : 8;
    int page_len;

    if (page_control == 3) {
        return -1;
    }

    p = outbuf + header_len;

    if (!dbd) {
        p[0] = 0; /* default density */
        p[5] = (s->qdev.blocksize >> 16) & 0xff;
        p[6] = (s->qdev.blocksize >> 8) & 0xff;
        p[7] = s->qdev.blocksize & 0xff;
        p += block_desc_len;
    }

    if (page == MODE_PAGE_ALLS) {
        page_len = scsi_tape_mode_sense_page(s, MODE_PAGE_R_W_ERROR,
                                             page_control, &p);
        if (page_len < 0) {
            return -1;
        }
        page_len = scsi_tape_mode_sense_page(s, MODE_PAGE_CONTROL,
                                             page_control, &p);
        if (page_len < 0) {
            return -1;
        }
    } else {
        page_len = scsi_tape_mode_sense_page(s, page, page_control, &p);
        if (page_len < 0) {
            return -1;
        }
    }

    if (mode_sense_10) {
        stw_be_p(outbuf, p - outbuf - 2);
        outbuf[2] = 0; /* medium type */
        outbuf[3] = dev_specific_param;
        stw_be_p(&outbuf[6], block_desc_len);
    } else {
        outbuf[0] = p - outbuf - 1;
        outbuf[1] = 0; /* medium type */
        outbuf[2] = dev_specific_param;
        outbuf[3] = block_desc_len;
    }

    return p - outbuf;
}

static void scsi_tape_emulate_load_unload(SCSIRequest *req)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    bool load = req->cmd.buf[4] & 0x01;

    s->loaded = load;
    if (!load) {
        scsi_tape_emulate_rewind(req);
    }
}

static int32_t scsi_tape_emulate_command(SCSIRequest *req, uint8_t *buf)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, req->dev);
    int buflen;
    uint8_t *outbuf;
    switch (req->cmd.buf[0]) {
    case TEST_UNIT_READY:
    case INQUIRY:
    case REWIND:
    case LOAD_UNLOAD:
    case MODE_SENSE:
    case MODE_SENSE_10:

        break;
    default:
        if (!blk_is_available(s->qdev.conf.blk) || !s->loaded) {
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return 0;
        }
        break;
    }


    if (req->cmd.xfer > 65536) {
        goto illegal_request;
    }
    r->buflen = MAX(4096, req->cmd.xfer);

    if (!r->iov.iov_base) {
        r->iov.iov_base = blk_blockalign(s->qdev.conf.blk, r->buflen);
    }

    outbuf = r->iov.iov_base;
    memset(outbuf, 0, r->buflen);

    switch (req->cmd.buf[0]) {
    case TEST_UNIT_READY:
        if (!blk_is_available(s->qdev.conf.blk) || !s->loaded) {
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return 0;
        }
        break;
    case INQUIRY:
        buflen = scsi_tape_emulate_inquiry(req, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case REWIND:
        scsi_tape_emulate_rewind(req);
        scsi_req_complete(&r->req, GOOD);
        return 0;
    case LOAD_UNLOAD:
        scsi_tape_emulate_load_unload(req);
        scsi_req_complete(&r->req, GOOD);
        return 0;
    case MODE_SENSE:
    case MODE_SENSE_10:
        buflen = scsi_tape_emulate_mode_sense(req, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        r->iov.iov_len = MIN((size_t)buflen, req->cmd.xfer);
        return r->iov.iov_len;
    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
    {
        int ret = blk_pread(s->qdev.conf.blk, s->position, r->iov.iov_len, outbuf, 0);

        if (ret < 0) {
            scsi_check_condition(r, SENSE_CODE(READ_ERROR));
            return 0;
        }
        s->position += r->iov.iov_len;
        s->bot = false;
        break;
    }
    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
    {
        if (!blk_is_writable(s->qdev.conf.blk)) {
            scsi_check_condition(r, SENSE_CODE(WRITE_PROTECTED));
            return 0;
        }
        break;
    }
    case WRITE_FILEMARKS:
    case WRITE_FILEMARKS_16:
    {
        s->at_filemark = true;
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }

    default:
        scsi_check_condition(r, SENSE_CODE(INVALID_OPCODE));
        return 0;
    }

    assert(!r->req.aiocb);
    if (r->iov.iov_len == 0) {
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }
    if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
        assert(r->iov.iov_len == req->cmd.xfer);
        return -r->iov.iov_len;
    } else {
        return r->iov.iov_len;
    }

illegal_request:
    if (r->req.status == -1) {
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
    }
    return 0;
}


static void scsi_tape_init(SCSITapeState *s)
{
    s->position = 0;
    s->at_filemark = false;
    s->eof = false;
    s->bot = true;
    s->eot = false;
    s->loaded = true;
}


static void scsi_tape_realize(SCSIDevice *dev, Error **errp)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, dev);
    bool read_only;

    s->qdev.type = TYPE_TAPE;
    s->qdev.blocksize = SCSI_TAPE_BLOCK_SIZE;

    if (!s->qdev.conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    if (!blk_attach_dev(s->qdev.conf.blk, &dev->qdev)) {
        error_setg(errp, "failed to attach block backend");
        return;
    }

    read_only = !blk_supports_write_perm(s->qdev.conf.blk);

    if (!blkconf_apply_backend_options(&dev->conf, read_only, true, errp)) {
        return;
    }

    if (!s->vendor) {
        s->vendor = g_strdup("QEMU TAPE");
    }
    if (!s->version) {
        s->version = g_strdup(QEMU_HW_VERSION);
    }

    if (s->serial && strlen(s->serial) > MAX_SERIAL_LEN) {
        error_setg(errp, "The serial number can't be longer than %d characters",
                   MAX_SERIAL_LEN);
        goto fail;
    }

    if (!s->device_id) {
        if (s->serial) {
            if (strlen(s->serial) > MAX_SERIAL_LEN_FOR_DEVID) {
                error_setg(errp, "The serial number can't be longer than %d "
                           "characters when it is also used as the default for "
                           "device_id", MAX_SERIAL_LEN_FOR_DEVID);
                goto fail;
            }
            s->device_id = g_strdup(s->serial);
        }
    }

    scsi_tape_init(s);
    return;

fail:
    g_free(s->vendor);
    s->vendor = NULL;
    g_free(s->version);
    s->version = NULL;
    if (s->qdev.conf.blk) {
        blk_detach_dev(s->qdev.conf.blk, &dev->qdev);
    }
}

static void scsi_tape_unrealize(SCSIDevice *dev)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, dev);

    g_free(s->vendor);
    g_free(s->serial);
    g_free(s->product);
    g_free(s->device_id);
    g_free(s->version);
    if (s->qdev.conf.blk) {
        blk_detach_dev(s->qdev.conf.blk, &dev->qdev);
    }
}

static const SCSIReqOps scsi_tape_emulate_reqops = {
    .size         = sizeof(SCSITapeReq),
    .free_req     = scsi_free_request,
    .send_command = scsi_tape_emulate_command,
    .read_data    = scsi_tape_read_data,
    .write_data   = scsi_tape_write_data,
    .get_buf      = scsi_tape_get_buf,
};

static const SCSIReqOps *const scsi_tape_reqops_dispatch[256] = {
    [TEST_UNIT_READY]                 = &scsi_tape_emulate_reqops,
    [INQUIRY]                         = &scsi_tape_emulate_reqops,
    [REWIND]                          = &scsi_tape_emulate_reqops,
    [LOAD_UNLOAD]                     = &scsi_tape_emulate_reqops,
    [MODE_SENSE]                      = &scsi_tape_emulate_reqops,
    [MODE_SENSE_10]                   = &scsi_tape_emulate_reqops,
    [READ_6]                          = &scsi_tape_emulate_reqops,
    [READ_10]                         = &scsi_tape_emulate_reqops,
    [READ_12]                         = &scsi_tape_emulate_reqops,
    [READ_16]                         = &scsi_tape_emulate_reqops,
    [WRITE_6]                         = &scsi_tape_emulate_reqops,
    [WRITE_10]                        = &scsi_tape_emulate_reqops,
    [WRITE_12]                        = &scsi_tape_emulate_reqops,
    [WRITE_16]                        = &scsi_tape_emulate_reqops,
    [WRITE_FILEMARKS]                 = &scsi_tape_emulate_reqops,
    [WRITE_FILEMARKS_16]              = &scsi_tape_emulate_reqops,
};

static SCSIRequest *scsi_tape_new_request(SCSIDevice *dev, uint32_t tag,
                                          uint32_t lun, uint8_t *buf,
                                          void *hba_private)
{
    SCSITapeState *s = DO_UPCAST(SCSITapeState, qdev, dev);
    SCSIRequest *req;
    const SCSIReqOps *ops;

    ops = scsi_tape_reqops_dispatch[buf[0]];
    if (!ops) {
        ops = &scsi_tape_emulate_reqops;
    }
    req = scsi_req_alloc(ops, &s->qdev, tag, lun, hba_private);

    return req;
}

#define DEFINE_SCSI_TAPE_PROPERTIES()                                   \
    DEFINE_PROP_DRIVE_IOTHREAD("drive", SCSITapeState, qdev.conf.blk),  \
    DEFINE_BLOCK_PROPERTIES_BASE(SCSITapeState, qdev.conf),             \
    DEFINE_BLOCK_ERROR_PROPERTIES(SCSITapeState, qdev.conf),            \
    DEFINE_PROP_STRING("ver", SCSITapeState, version),                  \
    DEFINE_PROP_STRING("serial", SCSITapeState, serial),                \
    DEFINE_PROP_STRING("vendor", SCSITapeState, vendor),                \
    DEFINE_PROP_STRING("product", SCSITapeState, product),              \
    DEFINE_PROP_STRING("device_id", SCSITapeState, device_id)

static const Property scsi_tape_properties[] = {
        DEFINE_SCSI_TAPE_PROPERTIES(),
};

static void scsi_tape_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize   = scsi_tape_realize;
    sc->unrealize = scsi_tape_unrealize;
    sc->alloc_req = scsi_tape_new_request;
    dc->desc = "virtual SCSI tape";
    device_class_set_props(dc, scsi_tape_properties);
}
static const TypeInfo scsi_tape_info = {
    .name          = TYPE_SCSI_TAPE_BASE,
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSITapeState),
    .class_size    = sizeof(SCSITapeClass),
    .class_init    = scsi_tape_class_init,
};

static void scsi_tape_register_types(void)
{
    type_register_static(&scsi_tape_info);
}

type_init(scsi_tape_register_types)

