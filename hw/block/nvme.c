/*
 * QEMU NVM Express Controller
 *
 * Copyright (c) 2012, Intel Corporation
 *
 * Written by Keith Busch <keith.busch@intel.com>
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

/**
 * Reference Specs: http://www.nvmexpress.org, 1.2, 1.1, 1.0e
 *
 *  http://www.nvmexpress.org/resources/
 */

/**
 * Usage: add options:
 *      -drive file=<file>,if=none,id=<drive_id>
 *      -device nvme,drive=<drive_id>,serial=<serial>,id=<id[optional]>, \
 *              cmb_size_mb=<cmb_size_mb[optional]>, \
 *              num_queues=<N[optional]>
 *
 * Note cmb_size_mb denotes size of CMB in MB. CMB is assumed to be at
 * offset 0 in BAR2 and supports only WDS, RDS and SQS for now.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/block/block.h"
#include "hw/hw.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/block-backend.h"

#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/qemu-print.h"
#include "monitor/monitor.h"
#include "trace.h"
#include "nvme.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

static const uint32_t nvme_ced_admin[] = {
    NVME_CED_SET_CSUPP, // 00h: Delete I/O Submission Queue
    NVME_CED_SET_CSUPP, // 01h: Create I/O Submission Queue
    NVME_CED_SET_CSUPP, // 02h: Get Log Page
    0,                  // 03h:
    NVME_CED_SET_CSUPP, // 04h: Delete I/O Completion Queue
    NVME_CED_SET_CSUPP, // 05h: Create I/O Completion Queue
    NVME_CED_SET_CSUPP, // 06h: Identify
    0,                  // 07h:
    0,                  // 08h: Abort
    NVME_CED_SET_CSUPP, // 09h: Set Features
    NVME_CED_SET_CSUPP, // 0Ah: Get Features
    0,                  // 0Bh:
    0,                  // 0Ch: Asynchronous Event Request
    0,                  // 0Dh: Namespace Management
    0, 0,               // 0Eh, 0Fh
    0,                  // 10h: Firmware Commit
    0,                  // 11h: Firmware Image Download
    0, 0, 0,            // 12h, 13h, 14h
    0,                  // 15h: Namespace Attachment
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                        // 16h -- 1Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 20h -- 2Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 30h -- 3Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 40h -- 4Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 50h -- 5Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 60h -- 6Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 70h -- 7Fh
    0,                                                   // 80h: Format NVM
    0,                                                   // 81h: Security Send
    0,                                                   // 82h: Security Receive
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,               // 83h -- 8Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 90h -- 9Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // A0h -- AFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // B0h -- BFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // C0h -- CFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // D0h -- DFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // E0h -- EFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // F0h -- FFh
};

static const uint32_t nvme_ced_io[] = {
    NVME_CED_SET_CSUPP,                     // 00h: Flush
    NVME_CED_SET_CSUPP | NVME_CED_SET_LBCC, // 01h: Write
    NVME_CED_SET_CSUPP,                     // 02h: Read
    0,                                      // 03h:
    0,                                      // 04h: Write Uncorrectable
    0,                                      // 05h: Compare
    0, 0,                                   // 06h, 07h:
    NVME_CED_SET_CSUPP | NVME_CED_SET_LBCC, // 08h: Write Zeroes
    NVME_CED_SET_CSUPP | NVME_CED_SET_LBCC, // 09h: Dataset Management
    0, 0, 0,                                // 0Ah, 0Bh, 0Ch
    0,                                      // 0Dh: Reservation Register
    0,                                      // 0Eh: Reservation Report
    0, 0,                                   // 0Fh, 10h
    0,                                      // 11h: Reservation Acquire
    0, 0, 0,                                // 12h, 13h, 14h
    0,                                      // 15h: Reservation Release
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           // 16h -- 1Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 20h -- 2Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 30h -- 3Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 40h -- 4Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 50h -- 5Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 60h -- 6Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 70h -- 7Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 80h -- 8Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 90h -- 9Fh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // A0h -- AFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // B0h -- BFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // C0h -- CFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // D0h -- DFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // E0h -- EFh
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // F0h -- FFh
};

#define NVME_SMART_LOG_FILE "smartlog.bin"

static void nvme_smart_inc_num_power_cycle(NvmeCtrl *_ctrl)
{
    (_ctrl->smart.power_cycles[0])++;

    if ( _ctrl->smart.power_cycles[0] == 0 ) {
        (_ctrl->smart.power_cycles[1])++;
    }
}

static void nvme_smart_save(NvmeCtrl *_ctrl)
{
    int fd = creat( NVME_SMART_LOG_FILE, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH );
    if ( fd != -1 ) {
	ssize_t wn = write( fd, (const void*)&(_ctrl->smart), sizeof(NvmeSmartLog) );
	if ( wn != sizeof(NvmeSmartLog) ) {
	    qemu_printf( "[NVME] something wrong occurred in writing file: %s\n", strerror(errno) );
	}
    } else {
	qemu_printf( "[NVME] something wrong occurred in creating file: %s\n", strerror(errno) );
    }
}

static void nvme_smart_load(NvmeCtrl *_ctrl)
{
    memset( &(_ctrl->smart), 0, sizeof(NvmeSmartLog) ); // clear
    int fd = open( NVME_SMART_LOG_FILE, O_RDONLY );
    if ( fd != -1 ) {
	ssize_t rn = read( fd, (void*)&(_ctrl->smart), sizeof(NvmeSmartLog) );
	if ( rn != sizeof(NvmeSmartLog) ) {
	    qemu_printf( "[NVME] something wrong occurred in reading file: %s\n", strerror(errno) );
	}
    } else {
	qemu_printf( "[NVME] something wrong occurred in opening file: %s\n", strerror(errno) );
    }
}

#define NVME_GUEST_ERR(trace, fmt, ...) \
    do { \
        (trace_##trace)(__VA_ARGS__); \
        qemu_log_mask(LOG_GUEST_ERROR, #trace \
            " in %s: " fmt "\n", __func__, ## __VA_ARGS__); \
    } while (0)

static void nvme_process_sq(void *opaque);

static void nvme_addr_read(NvmeCtrl *n, hwaddr addr, void *buf, int size)
{
    if (n->cmbsz && addr >= n->ctrl_mem.addr &&
                addr < (n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size))) {
        memcpy(buf, (void *)&n->cmbuf[addr - n->ctrl_mem.addr], size);
    } else {
        pci_dma_read(&n->parent_obj, addr, buf, size);
    }
}

static int nvme_check_sqid(NvmeCtrl *n, uint16_t sqid)
{
    return sqid < n->num_queues && n->sq[sqid] != NULL ? 0 : -1;
}

static int nvme_check_cqid(NvmeCtrl *n, uint16_t cqid)
{
    return cqid < n->num_queues && n->cq[cqid] != NULL ? 0 : -1;
}

static void nvme_inc_cq_tail(NvmeCQueue *cq)
{
    cq->tail++;
    if (cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase = !cq->phase;
    }
}

static void nvme_inc_sq_head(NvmeSQueue *sq)
{
    sq->head = (sq->head + 1) % sq->size;
}

static uint8_t nvme_cq_full(NvmeCQueue *cq)
{
    return (cq->tail + 1) % cq->size == cq->head;
}

static uint8_t nvme_sq_empty(NvmeSQueue *sq)
{
    return sq->head == sq->tail;
}

static void nvme_irq_check(NvmeCtrl *n)
{
    if (msix_enabled(&(n->parent_obj))) {
        return;
    }
    if (~n->bar.intms & n->irq_status) {
        pci_irq_assert(&n->parent_obj);
    } else {
        pci_irq_deassert(&n->parent_obj);
    }
}

static void nvme_irq_assert(NvmeCtrl *n, NvmeCQueue *cq)
{
    if (cq->irq_enabled) {
        if (msix_enabled(&(n->parent_obj))) {
            trace_nvme_irq_msix(cq->vector);
            msix_notify(&(n->parent_obj), cq->vector);
        } else {
            trace_nvme_irq_pin();
            assert(cq->cqid < 64);
            n->irq_status |= 1 << cq->cqid;
            nvme_irq_check(n);
        }
    } else {
        trace_nvme_irq_masked();
    }
}

static void nvme_irq_deassert(NvmeCtrl *n, NvmeCQueue *cq)
{
    if (cq->irq_enabled) {
        if (msix_enabled(&(n->parent_obj))) {
            return;
        } else {
            assert(cq->cqid < 64);
            n->irq_status &= ~(1 << cq->cqid);
            nvme_irq_check(n);
        }
    }
}

static uint16_t nvme_map_prp(QEMUSGList *qsg, QEMUIOVector *iov, uint64_t prp1,
                             uint64_t prp2, uint32_t len, NvmeCtrl *n)
{
    hwaddr trans_len = n->page_size - (prp1 % n->page_size);
    trans_len = MIN(len, trans_len);
    int num_prps = (len >> n->page_bits) + 1;

    if (unlikely(!prp1)) {
        trace_nvme_err_invalid_prp();
        return NVME_INVALID_FIELD | NVME_DNR;
    } else if (n->cmbsz && prp1 >= n->ctrl_mem.addr &&
               prp1 < n->ctrl_mem.addr + int128_get64(n->ctrl_mem.size)) {
        qsg->nsg = 0;
        qemu_iovec_init(iov, num_prps);
        qemu_iovec_add(iov, (void *)&n->cmbuf[prp1 - n->ctrl_mem.addr], trans_len);
    } else {
        pci_dma_sglist_init(qsg, &n->parent_obj, num_prps);
        qemu_sglist_add(qsg, prp1, trans_len);
    }
    len -= trans_len;
    if (len) {
        if (unlikely(!prp2)) {
            trace_nvme_err_invalid_prp2_missing();
            goto unmap;
        }
        if (len > n->page_size) {
            uint64_t prp_list[n->max_prp_ents];
            uint32_t nents, prp_trans;
            int i = 0;

            nents = (len + n->page_size - 1) >> n->page_bits;
            prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
            nvme_addr_read(n, prp2, (void *)prp_list, prp_trans);
            while (len != 0) {
                uint64_t prp_ent = le64_to_cpu(prp_list[i]);

                if (i == n->max_prp_ents - 1 && len > n->page_size) {
                    if (unlikely(!prp_ent || prp_ent & (n->page_size - 1))) {
                        trace_nvme_err_invalid_prplist_ent(prp_ent);
                        goto unmap;
                    }

                    i = 0;
                    nents = (len + n->page_size - 1) >> n->page_bits;
                    prp_trans = MIN(n->max_prp_ents, nents) * sizeof(uint64_t);
                    nvme_addr_read(n, prp_ent, (void *)prp_list,
                        prp_trans);
                    prp_ent = le64_to_cpu(prp_list[i]);
                }

                if (unlikely(!prp_ent || prp_ent & (n->page_size - 1))) {
                    trace_nvme_err_invalid_prplist_ent(prp_ent);
                    goto unmap;
                }

                trans_len = MIN(len, n->page_size);
                if (qsg->nsg){
                    qemu_sglist_add(qsg, prp_ent, trans_len);
                } else {
                    qemu_iovec_add(iov, (void *)&n->cmbuf[prp_ent - n->ctrl_mem.addr], trans_len);
                }
                len -= trans_len;
                i++;
            }
        } else {
            if (unlikely(prp2 & (n->page_size - 1))) {
                trace_nvme_err_invalid_prp2_align(prp2);
                goto unmap;
            }
            if (qsg->nsg) {
                qemu_sglist_add(qsg, prp2, len);
            } else {
                qemu_iovec_add(iov, (void *)&n->cmbuf[prp2 - n->ctrl_mem.addr], trans_len);
            }
        }
    }
    return NVME_SUCCESS;

 unmap:
    qemu_sglist_destroy(qsg);
    return NVME_INVALID_FIELD | NVME_DNR;
}

static uint16_t nvme_dma_write_prp(NvmeCtrl *n, uint8_t *ptr, uint32_t len,
                                   uint64_t prp1, uint64_t prp2)
{
    QEMUSGList qsg;
    QEMUIOVector iov;
    uint16_t status = NVME_SUCCESS;

    if (nvme_map_prp(&qsg, &iov, prp1, prp2, len, n)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (qsg.nsg > 0) {
        if (dma_buf_write(ptr, len, &qsg)) {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_sglist_destroy(&qsg);
    } else {
        if (qemu_iovec_to_buf(&iov, 0, ptr, len) != len) {
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_iovec_destroy(&iov);
    }
    return status;
}

static uint16_t nvme_dma_read_prp(NvmeCtrl *n, uint8_t *ptr, uint32_t len,
    uint64_t prp1, uint64_t prp2)
{
    QEMUSGList qsg;
    QEMUIOVector iov;
    uint16_t status = NVME_SUCCESS;

    trace_nvme_dma_read(prp1, prp2);

    if (nvme_map_prp(&qsg, &iov, prp1, prp2, len, n)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (qsg.nsg > 0) {
        if (unlikely(dma_buf_read(ptr, len, &qsg))) {
            trace_nvme_err_invalid_dma();
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_sglist_destroy(&qsg);
    } else {
        if (unlikely(qemu_iovec_from_buf(&iov, 0, ptr, len) != len)) {
            trace_nvme_err_invalid_dma();
            status = NVME_INVALID_FIELD | NVME_DNR;
        }
        qemu_iovec_destroy(&iov);
    }
    return status;
}

static void nvme_post_cqes(void *opaque)
{
    NvmeCQueue *cq = opaque;
    NvmeCtrl *n = cq->ctrl;
    NvmeRequest *req, *next;

    QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
        NvmeSQueue *sq;
        hwaddr addr;

        if (nvme_cq_full(cq)) {
            break;
        }

        QTAILQ_REMOVE(&cq->req_list, req, entry);
        sq = req->sq;
        req->cqe.status = cpu_to_le16((req->status << 1) | cq->phase);
        req->cqe.sq_id = cpu_to_le16(sq->sqid);
        req->cqe.sq_head = cpu_to_le16(sq->head);
        addr = cq->dma_addr + cq->tail * n->cqe_size;
        nvme_inc_cq_tail(cq);
        pci_dma_write(&n->parent_obj, addr, (void *)&req->cqe,
            sizeof(req->cqe));
        QTAILQ_INSERT_TAIL(&sq->req_list, req, entry);
    }
    if (cq->tail != cq->head) {
        nvme_irq_assert(n, cq);
    }
}

static void nvme_enqueue_req_completion(NvmeCQueue *cq, NvmeRequest *req)
{
    assert(cq->cqid == req->sq->cqid);
    QTAILQ_REMOVE(&req->sq->out_req_list, req, entry);
    QTAILQ_INSERT_TAIL(&cq->req_list, req, entry);
    timer_mod(cq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
}

static void nvme_rw_cb(void *opaque, int ret)
{
    NvmeRequest *req = opaque;
    NvmeSQueue *sq = req->sq;
    NvmeCtrl *n = sq->ctrl;
    NvmeCQueue *cq = n->cq[sq->cqid];

    if (!ret) {
        block_acct_done(blk_get_stats(n->conf.blk), &req->acct);
        req->status = NVME_SUCCESS;
    } else {
        block_acct_failed(blk_get_stats(n->conf.blk), &req->acct);
        req->status = NVME_INTERNAL_DEV_ERROR;
    }
    if (req->has_sg) {
        qemu_sglist_destroy(&req->qsg);
    }
    nvme_enqueue_req_completion(cq, req);
}

static uint16_t nvme_flush(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
    req->has_sg = false;
    block_acct_start(blk_get_stats(n->conf.blk), &req->acct, 0,
         BLOCK_ACCT_FLUSH);
    req->aiocb = blk_aio_flush(n->conf.blk, nvme_rw_cb, req);

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_write_zeros(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t offset = slba << data_shift;
    uint32_t count = nlb << data_shift;

    if (unlikely(slba + nlb > ns->id_ns.nsze)) {
        trace_nvme_err_invalid_lba_range(slba, nlb, ns->id_ns.nsze);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    req->has_sg = false;
    block_acct_start(blk_get_stats(n->conf.blk), &req->acct, 0,
                     BLOCK_ACCT_WRITE);
    req->aiocb = blk_aio_pwrite_zeroes(n->conf.blk, offset, count,
                                        BDRV_REQ_MAY_UNMAP, nvme_rw_cb, req);
    return NVME_NO_COMPLETE;
}

static uint16_t nvme_dsm(NvmeCtrl *_ctrl, NvmeNamespace *_ns, NvmeCmd *_cmd,
			 NvmeRequest *_req)
{
    NvmeDsmCmd *thisCmd = (NvmeDsmCmd *)_cmd;
    const uint8_t lba_index  = NVME_ID_NS_FLBAS_INDEX(_ns->id_ns.flbas);
    const uint8_t data_shift = _ns->id_ns.lbaf[lba_index].ds;

    uint32_t nr   = le32_to_cpu( thisCmd->nr ) & 0xFF;        // CDW10[07:00]
    uint64_t attr = le32_to_cpu( thisCmd->attributes ) & 0x7; // CDW11[02:00]
    uint64_t prp1 = le64_to_cpu( thisCmd->prp1 );
    uint64_t prp2 = le64_to_cpu( thisCmd->prp2 );

    uint16_t ret;

    NvmeDsmRange *ranges = g_malloc0( sizeof(NvmeDsmRange) * NVME_NUM_MAX_DSM_RANGES );
    if ( ranges == NULL ) {
	qemu_printf( "[NVME] [DSM] failed in g_malloc(): %s\n", strerror(errno) );
        return NVME_INTERNAL_DEV_ERROR | NVME_DNR;
    }

    // get LBA ranges
    ret = nvme_dma_write_prp( _ctrl, (uint8_t *)ranges,
			     sizeof(NvmeDsmRange) * ( nr + 1 ), prp1, prp2);

    if (ret != NVME_SUCCESS) {
	return ret;
    }

    for ( int i = 0; i <= nr; i++ ) {
	uint32_t nlb    = le32_to_cpu( ranges[ i ].nlb );
	uint64_t slba   = le64_to_cpu( ranges[ i ].slba );
	uint64_t offset = slba << data_shift;
	uint32_t count  =  nlb << data_shift;

	if ( unlikely( slba + nlb > _ns->id_ns.nsze ) ) {
	    trace_nvme_err_invalid_lba_range( slba, nlb, _ns->id_ns.nsze );
	    return NVME_LBA_RANGE | NVME_DNR;
	}

	/**
	 * NVMe spec implicitly states that a host may specify
	 * all combinations of attributes
	 */
	if ( attr & ( NVME_DSMGMT_IDR | NVME_DSMGMT_IDW ) ) {
	    // nothing to do
	}

	if ( attr & NVME_DSMGMT_AD ) {
	    _req->has_sg = false;
	    block_acct_start( blk_get_stats( _ctrl->conf.blk), &_req->acct, 0, BLOCK_ACCT_WRITE );
	    ret = blk_pwrite_zeroes( _ctrl->conf.blk, offset, count, BDRV_REQ_MAY_UNMAP );
	    if ( ret == 0 ) {
		block_acct_done( blk_get_stats( _ctrl->conf.blk ), &_req->acct );
	    } else {
		block_acct_failed( blk_get_stats( _ctrl->conf.blk ), &_req->acct );
		ret = NVME_INTERNAL_DEV_ERROR;
		break;
	    }
	}
    }

    g_free( ranges );
    return ret;
}

static uint16_t nvme_rw(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint32_t nlb  = le32_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);

    uint8_t lba_index  = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    int is_write = rw->opcode == NVME_CMD_WRITE ? 1 : 0;
    enum BlockAcctType acct = is_write ? BLOCK_ACCT_WRITE : BLOCK_ACCT_READ;

    trace_nvme_rw(is_write ? "write" : "read", nlb, data_size, slba);

    if (unlikely((slba + nlb) > ns->id_ns.nsze)) {
        block_acct_invalid(blk_get_stats(n->conf.blk), acct);
        trace_nvme_err_invalid_lba_range(slba, nlb, ns->id_ns.nsze);
        return NVME_LBA_RANGE | NVME_DNR;
    }

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        block_acct_invalid(blk_get_stats(n->conf.blk), acct);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    dma_acct_start(n->conf.blk, &req->acct, &req->qsg, acct);
    if (req->qsg.nsg > 0) {
        req->has_sg = true;
        req->aiocb = is_write ?
            dma_blk_write(n->conf.blk, &req->qsg, data_offset, BDRV_SECTOR_SIZE,
                          nvme_rw_cb, req) :
            dma_blk_read(n->conf.blk, &req->qsg, data_offset, BDRV_SECTOR_SIZE,
                         nvme_rw_cb, req);
    } else {
        req->has_sg = false;
        req->aiocb = is_write ?
            blk_aio_pwritev(n->conf.blk, data_offset, &req->iov, 0, nvme_rw_cb,
                            req) :
            blk_aio_preadv(n->conf.blk, data_offset, &req->iov, 0, nvme_rw_cb,
                           req);
    }

    return NVME_NO_COMPLETE;
}

static uint16_t nvme_io_cmd(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(cmd->nsid);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    switch (cmd->opcode) {
    case NVME_CMD_FLUSH:
        return nvme_flush(n, ns, cmd, req);
    case NVME_CMD_WRITE_ZEROS:
        return nvme_write_zeros(n, ns, cmd, req);
    case NVME_CMD_WRITE:
    case NVME_CMD_READ:
        return nvme_rw(n, ns, cmd, req);
    case NVME_CMD_DSM:
        return nvme_dsm(n, ns, cmd, req);
    default:
        trace_nvme_err_invalid_opc(cmd->opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void nvme_free_sq(NvmeSQueue *sq, NvmeCtrl *n)
{
    n->sq[sq->sqid] = NULL;
    timer_del(sq->timer);
    timer_free(sq->timer);
    g_free(sq->io_req);
    if (sq->sqid) {
        g_free(sq);
    }
}

static uint16_t nvme_del_sq(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeRequest *req, *next;
    NvmeSQueue *sq;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_sqid(n, qid))) {
        trace_nvme_err_invalid_del_sq(qid);
        return NVME_INVALID_QID | NVME_DNR;
    }

    trace_nvme_del_sq(qid);

    sq = n->sq[qid];
    while (!QTAILQ_EMPTY(&sq->out_req_list)) {
        req = QTAILQ_FIRST(&sq->out_req_list);
        assert(req->aiocb);
        blk_aio_cancel(req->aiocb);
    }
    if (!nvme_check_cqid(n, sq->cqid)) {
        cq = n->cq[sq->cqid];
        QTAILQ_REMOVE(&cq->sq_list, sq, entry);

        nvme_post_cqes(cq);
        QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
            if (req->sq == sq) {
                QTAILQ_REMOVE(&cq->req_list, req, entry);
                QTAILQ_INSERT_TAIL(&sq->req_list, req, entry);
            }
        }
    }

    nvme_free_sq(sq, n);
    return NVME_SUCCESS;
}

static void nvme_init_sq(NvmeSQueue *sq, NvmeCtrl *n, uint64_t dma_addr,
    uint16_t sqid, uint16_t cqid, uint16_t size)
{
    int i;
    NvmeCQueue *cq;

    sq->ctrl = n;
    sq->dma_addr = dma_addr;
    sq->sqid = sqid;
    sq->size = size;
    sq->cqid = cqid;
    sq->head = sq->tail = 0;
    sq->io_req = g_new(NvmeRequest, sq->size);

    QTAILQ_INIT(&sq->req_list);
    QTAILQ_INIT(&sq->out_req_list);
    for (i = 0; i < sq->size; i++) {
        sq->io_req[i].sq = sq;
        QTAILQ_INSERT_TAIL(&(sq->req_list), &sq->io_req[i], entry);
    }
    sq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_process_sq, sq);

    assert(n->cq[cqid]);
    cq = n->cq[cqid];
    QTAILQ_INSERT_TAIL(&(cq->sq_list), sq, entry);
    n->sq[sqid] = sq;
}

static uint16_t nvme_create_sq(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeSQueue *sq;
    NvmeCreateSq *c = (NvmeCreateSq *)cmd;

    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t sqid = le16_to_cpu(c->sqid);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->sq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    trace_nvme_create_sq(prp1, sqid, cqid, qsize, qflags);

    if (unlikely(!cqid || nvme_check_cqid(n, cqid))) {
        trace_nvme_err_invalid_create_sq_cqid(cqid);
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (unlikely(!sqid || !nvme_check_sqid(n, sqid))) {
        trace_nvme_err_invalid_create_sq_sqid(sqid);
        return NVME_INVALID_QID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(n->bar.cap))) {
        trace_nvme_err_invalid_create_sq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(!prp1 || prp1 & (n->page_size - 1))) {
        trace_nvme_err_invalid_create_sq_addr(prp1);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (unlikely(!(NVME_SQ_FLAGS_PC(qflags)))) {
        trace_nvme_err_invalid_create_sq_qflags(NVME_SQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    sq = g_malloc0(sizeof(*sq));
    nvme_init_sq(sq, n, prp1, sqid, cqid, qsize + 1);
    return NVME_SUCCESS;
}

static void nvme_free_cq(NvmeCQueue *cq, NvmeCtrl *n)
{
    n->cq[cq->cqid] = NULL;
    timer_del(cq->timer);
    timer_free(cq->timer);
    msix_vector_unuse(&n->parent_obj, cq->vector);
    if (cq->cqid) {
        g_free(cq);
    }
}

static uint16_t nvme_del_cq(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_cqid(n, qid))) {
        trace_nvme_err_invalid_del_cq_cqid(qid);
        return NVME_INVALID_CQID | NVME_DNR;
    }

    cq = n->cq[qid];
    if (unlikely(!QTAILQ_EMPTY(&cq->sq_list))) {
        trace_nvme_err_invalid_del_cq_notempty(qid);
        return NVME_INVALID_QUEUE_DEL;
    }
    nvme_irq_deassert(n, cq);
    trace_nvme_del_cq(qid);
    nvme_free_cq(cq, n);
    return NVME_SUCCESS;
}

static void nvme_init_cq(NvmeCQueue *cq, NvmeCtrl *n, uint64_t dma_addr,
    uint16_t cqid, uint16_t vector, uint16_t size, uint16_t irq_enabled)
{
    cq->ctrl = n;
    cq->cqid = cqid;
    cq->size = size;
    cq->dma_addr = dma_addr;
    cq->phase = 1;
    cq->irq_enabled = irq_enabled;
    cq->vector = vector;
    cq->head = cq->tail = 0;
    QTAILQ_INIT(&cq->req_list);
    QTAILQ_INIT(&cq->sq_list);
    msix_vector_use(&n->parent_obj, cq->vector);
    n->cq[cqid] = cq;
    cq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_post_cqes, cq);
}

static uint16_t nvme_create_cq(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeCQueue *cq;
    NvmeCreateCq *c = (NvmeCreateCq *)cmd;
    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t vector = le16_to_cpu(c->irq_vector);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->cq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    trace_nvme_create_cq(prp1, cqid, vector, qsize, qflags,
                         NVME_CQ_FLAGS_IEN(qflags) != 0);

    if (unlikely(!cqid || !nvme_check_cqid(n, cqid))) {
        trace_nvme_err_invalid_create_cq_cqid(cqid);
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(n->bar.cap))) {
        trace_nvme_err_invalid_create_cq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(!prp1)) {
        trace_nvme_err_invalid_create_cq_addr(prp1);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (unlikely(vector > n->num_queues)) {
        trace_nvme_err_invalid_create_cq_vector(vector);
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (unlikely(!(NVME_CQ_FLAGS_PC(qflags)))) {
        trace_nvme_err_invalid_create_cq_qflags(NVME_CQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    cq = g_malloc0(sizeof(*cq));
    nvme_init_cq(cq, n, prp1, cqid, vector, qsize + 1,
        NVME_CQ_FLAGS_IEN(qflags));
    return NVME_SUCCESS;
}

static uint16_t nvme_identify_ctrl(NvmeCtrl *n, NvmeIdentify *c)
{
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    trace_nvme_identify_ctrl();

    return nvme_dma_read_prp(n, (uint8_t *)&n->id_ctrl, sizeof(n->id_ctrl),
        prp1, prp2);
}

static uint16_t nvme_identify_ns(NvmeCtrl *n, NvmeIdentify *c)
{
    NvmeNamespace *ns;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    trace_nvme_identify_ns(nsid);

    if (unlikely(nsid == 0 || nsid > n->num_namespaces)) {
        trace_nvme_err_invalid_ns(nsid, n->num_namespaces);
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];

    return nvme_dma_read_prp(n, (uint8_t *)&ns->id_ns, sizeof(ns->id_ns),
        prp1, prp2);
}

static uint16_t nvme_identify_nslist(NvmeCtrl *n, NvmeIdentify *c)
{
    static const int data_len = 4 * KiB;
    uint32_t min_nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);
    uint32_t *list;
    uint16_t ret;
    int i, j = 0;

    trace_nvme_identify_nslist(min_nsid);

    list = g_malloc0(data_len);
    for (i = 0; i < n->num_namespaces; i++) {
        if (i < min_nsid) {
            continue;
        }
        list[j++] = cpu_to_le32(i + 1);
        if (j == data_len / sizeof(uint32_t)) {
            break;
        }
    }
    ret = nvme_dma_read_prp(n, (uint8_t *)list, data_len, prp1, prp2);
    g_free(list);
    return ret;
}

static uint16_t nvme_identify(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeIdentify *c = (NvmeIdentify *)cmd;

    switch (le32_to_cpu(c->cns)) {
    case 0x00:
        return nvme_identify_ns(n, c);
    case 0x01:
        return nvme_identify_ctrl(n, c);
    case 0x02:
        return nvme_identify_nslist(n, c);
    default:
        trace_nvme_err_invalid_identify_cns(le32_to_cpu(c->cns));
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static inline void nvme_set_timestamp(NvmeCtrl *n, uint64_t ts)
{
    trace_nvme_setfeat_timestamp(ts);

    n->host_timestamp = le64_to_cpu(ts);
    n->timestamp_set_qemu_clock_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static inline uint64_t nvme_get_timestamp(const NvmeCtrl *n)
{
    uint64_t current_time = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed_time = current_time - n->timestamp_set_qemu_clock_ms;

    union nvme_timestamp {
        struct {
            uint64_t timestamp:48;
            uint64_t sync:1;
            uint64_t origin:3;
            uint64_t rsvd1:12;
        };
        uint64_t all;
    };

    union nvme_timestamp ts;
    ts.all = 0;

    /*
     * If the sum of the Timestamp value set by the host and the elapsed
     * time exceeds 2^48, the value returned should be reduced modulo 2^48.
     */
    ts.timestamp = (n->host_timestamp + elapsed_time) & 0xffffffffffff;

    /* If the host timestamp is non-zero, set the timestamp origin */
    ts.origin = n->host_timestamp ? 0x01 : 0x00;

    trace_nvme_getfeat_timestamp(ts.all);

    return cpu_to_le64(ts.all);
}

static uint16_t nvme_get_feature_timestamp(NvmeCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
    uint64_t prp2 = le64_to_cpu(cmd->prp2);

    uint64_t timestamp = nvme_get_timestamp(n);

    return nvme_dma_read_prp(n, (uint8_t *)&timestamp,
                                 sizeof(timestamp), prp1, prp2);
}

static uint16_t nvme_get_feature(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t result;

    switch (dw10) {
    case NVME_VOLATILE_WRITE_CACHE:
        result = blk_enable_write_cache(n->conf.blk);
        trace_nvme_getfeat_vwcache(result ? "enabled" : "disabled");
        break;
    case NVME_NUMBER_OF_QUEUES:
        result = cpu_to_le32((n->num_queues - 2) | ((n->num_queues - 2) << 16));
        trace_nvme_getfeat_numq(result);
        break;
    case NVME_TIMESTAMP:
        return nvme_get_feature_timestamp(n, cmd);
        break;
    default:
        trace_nvme_err_invalid_getfeat(dw10);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    req->cqe.result = result;
    return NVME_SUCCESS;
}

static uint16_t nvme_set_feature_timestamp(NvmeCtrl *n, NvmeCmd *cmd)
{
    uint16_t ret;
    uint64_t timestamp;
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
    uint64_t prp2 = le64_to_cpu(cmd->prp2);

    ret = nvme_dma_write_prp(n, (uint8_t *)&timestamp,
                                sizeof(timestamp), prp1, prp2);
    if (ret != NVME_SUCCESS) {
        return ret;
    }

    nvme_set_timestamp(n, timestamp);

    return NVME_SUCCESS;
}

static uint16_t nvme_set_feature(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);

    switch (dw10) {
    case NVME_VOLATILE_WRITE_CACHE:
        blk_set_enable_write_cache(n->conf.blk, dw11 & 1);
        break;
    case NVME_NUMBER_OF_QUEUES:
        trace_nvme_setfeat_numq((dw11 & 0xFFFF) + 1,
                                ((dw11 >> 16) & 0xFFFF) + 1,
                                n->num_queues - 1, n->num_queues - 1);
        req->cqe.result =
            cpu_to_le32((n->num_queues - 2) | ((n->num_queues - 2) << 16));
        break;

    case NVME_TIMESTAMP:
        return nvme_set_feature_timestamp(n, cmd);
        break;

    default:
        trace_nvme_err_invalid_setfeat(dw10);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t nvme_get_smart(NvmeCtrl *_ctrl, NvmeGetLogPageCmd *_cmd, NvmeRequest *_req)
{
    uint64_t prp1 = le64_to_cpu( _cmd->prp1 );
    uint64_t prp2 = le64_to_cpu( _cmd->prp2 );
    uint16_t numd = le16_to_cpu( _cmd->numd ) & 0x0FFF;

    if ( sizeof(NvmeSmartLog) < ( ( numd + 1 ) << 2 ) ) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return nvme_dma_read_prp(_ctrl, (uint8_t *)&_ctrl->smart, ( numd + 1 ) << 2, prp1, prp2);
}

static uint16_t nvme_get_error_info(NvmeCtrl *_ctrl, NvmeGetLogPageCmd *_cmd, NvmeRequest *_req)
{
    uint64_t prp1 = le64_to_cpu( _cmd->prp1 );
    uint64_t prp2 = le64_to_cpu( _cmd->prp2 );
    uint16_t numd = le16_to_cpu( _cmd->numd ) & 0x0FFF;

    if ( sizeof(NvmeErrorLog) * NVME_NUM_ERROR_LOG < ( ( numd + 1 ) << 2 ) ) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return nvme_dma_read_prp(_ctrl, (uint8_t *)(_ctrl->error_info), ( numd + 1 ) << 2, prp1, prp2);
}

static uint16_t nvme_get_fw_slot_info(NvmeCtrl *_ctrl, NvmeGetLogPageCmd *_cmd, NvmeRequest *_req)
{
    uint64_t prp1 = le64_to_cpu( _cmd->prp1 );
    uint64_t prp2 = le64_to_cpu( _cmd->prp2 );
    uint16_t numd = le16_to_cpu( _cmd->numd ) & 0x0FFF;

    if ( sizeof(NvmeFwSlotInfoLog) < ( ( numd + 1 ) << 2 ) ) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return nvme_dma_read_prp(_ctrl, (uint8_t *)&(_ctrl->fw_slot_info), ( numd + 1 ) << 2, prp1, prp2);
}

static uint16_t nvme_get_cse_info(NvmeCtrl *_ctrl, NvmeGetLogPageCmd *_cmd, NvmeRequest *_req)
{
    uint64_t prp1 = le64_to_cpu( _cmd->prp1 );
    uint64_t prp2 = le64_to_cpu( _cmd->prp2 );
    uint16_t numd = le16_to_cpu( _cmd->numd ) & 0x0FFF;

    if ( NVME_CED_SZ_BYTE < ( ( numd + 1 ) << 2 ) ) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    uint8_t *tmp = g_malloc0(NVME_CED_SZ_BYTE);
    if ( tmp == NULL ) {
	qemu_printf( "[NVME] [ Get Log Page / CSE ] failed in g_malloc(): %s\n", strerror(errno) );
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    memset( tmp, 0, NVME_CED_SZ_BYTE ); // clear
    memcpy( (void *)tmp, (const void *)nvme_ced_admin, NVME_CED_NUM_ADM_CMD << 2 );
    memcpy( (void *)( tmp + (NVME_CED_NUM_ADM_CMD << 2) ), (const void *)nvme_ced_io, NVME_CED_NUM_IO_CMD << 2 );

    uint16_t ret = nvme_dma_read_prp(_ctrl, (uint8_t *)tmp, ( numd + 1 ) << 2, prp1, prp2);
    g_free( tmp );
    return ret;
}

static uint16_t nvme_get_telemetry(NvmeCtrl *_ctrl, NvmeGetLogPageCmd *_cmd, NvmeRequest *_req)
{
    uint64_t prp1 = le64_to_cpu( _cmd->prp1 );
    uint64_t prp2 = le64_to_cpu( _cmd->prp2 );
    uint8_t  bCreate = ( _cmd->res2 ) & 0x1;
    unsigned int uiAllocSize = 0;
    uint8_t  *res = NULL;

    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] Command received\n" );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW10[11: 8] LSP   = 0x%X\n", bCreate );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW10[   15] RAE   = 0x%X\n", ((_cmd->res2) >> 15) & 0x1 );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW10[31:16] NUMDL = 0x%04X\n", le16_to_cpu(_cmd->numd) );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW11[15: 0] NUMDU = 0x%08X\n", le32_to_cpu(_cmd->cdw11) & 0x0000FFFF );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW12[31: 0] LPOL  = 0x%08X\n", le32_to_cpu(_cmd->cdw12) );
    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] CDW13[31: 0] LPOU  = 0x%08X\n", le32_to_cpu(_cmd->cdw13) );

    if ( bCreate )
    {
	/**
	 * Windows sets "Create Telemetry Host-Initiated Data" bit to 1
	 * ONLY IF calling DeviceIoControl() with IOCTL_STORAGE_GET_DEVICE_INTERNAL_LOG.
	 *
	 * In this case, the data layout for Telemetry Header is different from the one of NVMe.
	 */
	uiAllocSize = sizeof(DeviceInternalStatusData);
	qemu_printf( "[NVME] [ Get Log Page / Telemetry ] uiAllocSize = %d\n", uiAllocSize);
	res = g_malloc0(uiAllocSize);
	if ( res == NULL ) {
	    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] failed in g_malloc(): %s\n", strerror(errno) );
	    return NVME_INVALID_FIELD | NVME_DNR;
	}
	memset( res, 0, uiAllocSize ); // clear

	DeviceInternalStatusData *tmp = (DeviceInternalStatusData *)res;
//      tmp->T10VendorId        = 0x0000000000000001; // NG
//      tmp->T10VendorId        = 0x0000000000010000; // NG
        tmp->T10VendorId        = 0x0000000100000000; // OK
//      tmp->T10VendorId        = 0x0001000000000000; // NG
	// other fields are left zero
    }
    else
    {
	uiAllocSize = sizeof(NvmeTelemetryLogHeader);
	qemu_printf( "[NVME] [ Get Log Page / Telemetry ] uiAllocSize = %d\n", uiAllocSize);
	res = g_malloc0(uiAllocSize);
	if ( res == NULL ) {
	    qemu_printf( "[NVME] [ Get Log Page / Telemetry ] failed in g_malloc(): %s\n", strerror(errno) );
	    return NVME_INVALID_FIELD | NVME_DNR;
	}
	memset( res, 0, uiAllocSize ); // clear

	NvmeTelemetryLogHeader *tmp = (NvmeTelemetryLogHeader *)res;
	tmp->log_id = _cmd->lid; // shall be 07h (Host-Initiated) or 08h (Controller-Initiated)
	// other fields are left zero
    }

    uint16_t ret = nvme_dma_read_prp(_ctrl, res, uiAllocSize, prp1, prp2);

    g_free( res );
    return ret;
}

static uint16_t nvme_get_log_page(NvmeCtrl *_ctrl, NvmeCmd *_cmd, NvmeRequest *_req)
{
    NvmeGetLogPageCmd *thisCmd = (NvmeGetLogPageCmd *)_cmd;

    switch ( thisCmd->lid ) {
    case NVME_LOG_ERROR_INFO:
        return nvme_get_error_info(_ctrl, thisCmd, _req);
    case NVME_LOG_SMART_INFO:
        return nvme_get_smart(_ctrl, thisCmd, _req);
    case NVME_LOG_FW_SLOT_INFO:
        return nvme_get_fw_slot_info(_ctrl, thisCmd, _req);
    case NVME_LOG_CSE_INFO:
        return nvme_get_cse_info(_ctrl, thisCmd, _req);
    case NVME_LOG_TELEMETRY_HOST:
	qemu_printf( "[NVME] Get Log Page: Telemetry Host-Initiated\n" );
        return nvme_get_telemetry(_ctrl, thisCmd, _req);
    case NVME_LOG_TELEMETRY_CTLR:
	qemu_printf( "[NVME] Get Log Page: Telemetry Controller-Initiated\n" );
        return nvme_get_telemetry(_ctrl, thisCmd, _req);

    default:
        // REVISIT: need to implement trace event like "trace_nvme_err_invalid_logid(cdw10)"
        return NVME_INVALID_LOG_ID | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t nvme_admin_cmd(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_DELETE_SQ:
        return nvme_del_sq(n, cmd);
    case NVME_ADM_CMD_CREATE_SQ:
        return nvme_create_sq(n, cmd);
    case NVME_ADM_CMD_GET_LOG_PAGE:
        return nvme_get_log_page(n, cmd, req);
    case NVME_ADM_CMD_DELETE_CQ:
        return nvme_del_cq(n, cmd);
    case NVME_ADM_CMD_CREATE_CQ:
        return nvme_create_cq(n, cmd);
    case NVME_ADM_CMD_IDENTIFY:
        return nvme_identify(n, cmd);
    case NVME_ADM_CMD_SET_FEATURES:
        return nvme_set_feature(n, cmd, req);
    case NVME_ADM_CMD_GET_FEATURES:
        return nvme_get_feature(n, cmd, req);
    default:
        trace_nvme_err_invalid_admin_opc(cmd->opcode);
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void nvme_process_sq(void *opaque)
{
    NvmeSQueue *sq = opaque;
    NvmeCtrl *n = sq->ctrl;
    NvmeCQueue *cq = n->cq[sq->cqid];

    uint16_t status;
    hwaddr addr;
    NvmeCmd cmd;
    NvmeRequest *req;

    while (!(nvme_sq_empty(sq) || QTAILQ_EMPTY(&sq->req_list))) {
        addr = sq->dma_addr + sq->head * n->sqe_size;
        nvme_addr_read(n, addr, (void *)&cmd, sizeof(cmd));
        nvme_inc_sq_head(sq);

        req = QTAILQ_FIRST(&sq->req_list);
        QTAILQ_REMOVE(&sq->req_list, req, entry);
        QTAILQ_INSERT_TAIL(&sq->out_req_list, req, entry);
        memset(&req->cqe, 0, sizeof(req->cqe));
        req->cqe.cid = cmd.cid;

        status = sq->sqid ? nvme_io_cmd(n, &cmd, req) :
            nvme_admin_cmd(n, &cmd, req);
        if (status != NVME_NO_COMPLETE) {
            req->status = status;
            nvme_enqueue_req_completion(cq, req);
        }
    }
}

static void nvme_clear_ctrl(NvmeCtrl *n)
{
    int i;

    blk_drain(n->conf.blk);

    for (i = 0; i < n->num_queues; i++) {
        if (n->sq[i] != NULL) {
            nvme_free_sq(n->sq[i], n);
        }
    }
    for (i = 0; i < n->num_queues; i++) {
        if (n->cq[i] != NULL) {
            nvme_free_cq(n->cq[i], n);
        }
    }

    blk_flush(n->conf.blk);
    n->bar.cc = 0;
}

static int nvme_start_ctrl(NvmeCtrl *n)
{
    uint32_t page_bits = NVME_CC_MPS(n->bar.cc) + 12;
    uint32_t page_size = 1 << page_bits;

    if (unlikely(n->cq[0])) {
        trace_nvme_err_startfail_cq();
        return -1;
    }
    if (unlikely(n->sq[0])) {
        trace_nvme_err_startfail_sq();
        return -1;
    }
    if (unlikely(!n->bar.asq)) {
        trace_nvme_err_startfail_nbarasq();
        return -1;
    }
    if (unlikely(!n->bar.acq)) {
        trace_nvme_err_startfail_nbaracq();
        return -1;
    }
    if (unlikely(n->bar.asq & (page_size - 1))) {
        trace_nvme_err_startfail_asq_misaligned(n->bar.asq);
        return -1;
    }
    if (unlikely(n->bar.acq & (page_size - 1))) {
        trace_nvme_err_startfail_acq_misaligned(n->bar.acq);
        return -1;
    }
    if (unlikely(NVME_CC_MPS(n->bar.cc) <
                 NVME_CAP_MPSMIN(n->bar.cap))) {
        trace_nvme_err_startfail_page_too_small(
                    NVME_CC_MPS(n->bar.cc),
                    NVME_CAP_MPSMIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_MPS(n->bar.cc) >
                 NVME_CAP_MPSMAX(n->bar.cap))) {
        trace_nvme_err_startfail_page_too_large(
                    NVME_CC_MPS(n->bar.cc),
                    NVME_CAP_MPSMAX(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOCQES(n->bar.cc) <
                 NVME_CTRL_CQES_MIN(n->id_ctrl.cqes))) {
        trace_nvme_err_startfail_cqent_too_small(
                    NVME_CC_IOCQES(n->bar.cc),
                    NVME_CTRL_CQES_MIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOCQES(n->bar.cc) >
                 NVME_CTRL_CQES_MAX(n->id_ctrl.cqes))) {
        trace_nvme_err_startfail_cqent_too_large(
                    NVME_CC_IOCQES(n->bar.cc),
                    NVME_CTRL_CQES_MAX(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOSQES(n->bar.cc) <
                 NVME_CTRL_SQES_MIN(n->id_ctrl.sqes))) {
        trace_nvme_err_startfail_sqent_too_small(
                    NVME_CC_IOSQES(n->bar.cc),
                    NVME_CTRL_SQES_MIN(n->bar.cap));
        return -1;
    }
    if (unlikely(NVME_CC_IOSQES(n->bar.cc) >
                 NVME_CTRL_SQES_MAX(n->id_ctrl.sqes))) {
        trace_nvme_err_startfail_sqent_too_large(
                    NVME_CC_IOSQES(n->bar.cc),
                    NVME_CTRL_SQES_MAX(n->bar.cap));
        return -1;
    }
    if (unlikely(!NVME_AQA_ASQS(n->bar.aqa))) {
        trace_nvme_err_startfail_asqent_sz_zero();
        return -1;
    }
    if (unlikely(!NVME_AQA_ACQS(n->bar.aqa))) {
        trace_nvme_err_startfail_acqent_sz_zero();
        return -1;
    }

    n->page_bits = page_bits;
    n->page_size = page_size;
    n->max_prp_ents = n->page_size / sizeof(uint64_t);
    n->cqe_size = 1 << NVME_CC_IOCQES(n->bar.cc);
    n->sqe_size = 1 << NVME_CC_IOSQES(n->bar.cc);
    nvme_init_cq(&n->admin_cq, n, n->bar.acq, 0, 0,
        NVME_AQA_ACQS(n->bar.aqa) + 1, 1);
    nvme_init_sq(&n->admin_sq, n, n->bar.asq, 0, 0,
        NVME_AQA_ASQS(n->bar.aqa) + 1);

    nvme_set_timestamp(n, 0ULL);

    return 0;
}

static void nvme_write_bar(NvmeCtrl *n, hwaddr offset, uint64_t data,
    unsigned size)
{
    if (unlikely(offset & (sizeof(uint32_t) - 1))) {
        NVME_GUEST_ERR(nvme_ub_mmiowr_misaligned32,
                       "MMIO write not 32-bit aligned,"
                       " offset=0x%"PRIx64"", offset);
        /* should be ignored, fall through for now */
    }

    if (unlikely(size < sizeof(uint32_t))) {
        NVME_GUEST_ERR(nvme_ub_mmiowr_toosmall,
                       "MMIO write smaller than 32-bits,"
                       " offset=0x%"PRIx64", size=%u",
                       offset, size);
        /* should be ignored, fall through for now */
    }

    switch (offset) {
    case 0xc:   /* INTMS */
        if (unlikely(msix_enabled(&(n->parent_obj)))) {
            NVME_GUEST_ERR(nvme_ub_mmiowr_intmask_with_msix,
                           "undefined access to interrupt mask set"
                           " when MSI-X is enabled");
            /* should be ignored, fall through for now */
        }
        n->bar.intms |= data & 0xffffffff;
        n->bar.intmc = n->bar.intms;
        trace_nvme_mmio_intm_set(data & 0xffffffff,
                                 n->bar.intmc);
        nvme_irq_check(n);
        break;
    case 0x10:  /* INTMC */
        if (unlikely(msix_enabled(&(n->parent_obj)))) {
            NVME_GUEST_ERR(nvme_ub_mmiowr_intmask_with_msix,
                           "undefined access to interrupt mask clr"
                           " when MSI-X is enabled");
            /* should be ignored, fall through for now */
        }
        n->bar.intms &= ~(data & 0xffffffff);
        n->bar.intmc = n->bar.intms;
        trace_nvme_mmio_intm_clr(data & 0xffffffff,
                                 n->bar.intmc);
        nvme_irq_check(n);
        break;
    case 0x14:  /* CC */
        trace_nvme_mmio_cfg(data & 0xffffffff);
        /* Windows first sends data, then sends enable bit */
        if (!NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc) &&
            !NVME_CC_SHN(data) && !NVME_CC_SHN(n->bar.cc))
        {
            n->bar.cc = data;
        }

        if (NVME_CC_EN(data) && !NVME_CC_EN(n->bar.cc)) {
            n->bar.cc = data;
            if (unlikely(nvme_start_ctrl(n))) {
                trace_nvme_err_startfail();
                n->bar.csts = NVME_CSTS_FAILED;
            } else {
                trace_nvme_mmio_start_success();
                n->bar.csts = NVME_CSTS_READY;
            }
        } else if (!NVME_CC_EN(data) && NVME_CC_EN(n->bar.cc)) {
            trace_nvme_mmio_stopped();
            nvme_clear_ctrl(n);
            n->bar.csts &= ~NVME_CSTS_READY;
        }
        if (NVME_CC_SHN(data) && !(NVME_CC_SHN(n->bar.cc))) {
            trace_nvme_mmio_shutdown_set();
            nvme_clear_ctrl(n);
            n->bar.cc = data;
            n->bar.csts |= NVME_CSTS_SHST_COMPLETE;

            nvme_smart_inc_num_power_cycle( n ); // record as "Power Cycle"
            nvme_smart_save(n); // save SMART data at shutdown event
        } else if (!NVME_CC_SHN(data) && NVME_CC_SHN(n->bar.cc)) {
            trace_nvme_mmio_shutdown_cleared();
            n->bar.csts &= ~NVME_CSTS_SHST_COMPLETE;
            n->bar.cc = data;
        }
        break;
    case 0x1C:  /* CSTS */
        if (data & (1 << 4)) {
            NVME_GUEST_ERR(nvme_ub_mmiowr_ssreset_w1c_unsupported,
                           "attempted to W1C CSTS.NSSRO"
                           " but CAP.NSSRS is zero (not supported)");
        } else if (data != 0) {
            NVME_GUEST_ERR(nvme_ub_mmiowr_ro_csts,
                           "attempted to set a read only bit"
                           " of controller status");
        }
        break;
    case 0x20:  /* NSSR */
        if (data == 0x4E564D65) {
            trace_nvme_ub_mmiowr_ssreset_unsupported();
        } else {
            /* The spec says that writes of other values have no effect */
            return;
        }
        break;
    case 0x24:  /* AQA */
        n->bar.aqa = data & 0xffffffff;
        trace_nvme_mmio_aqattr(data & 0xffffffff);
        break;
    case 0x28:  /* ASQ */
        n->bar.asq = data;
        trace_nvme_mmio_asqaddr(data);
        break;
    case 0x2c:  /* ASQ hi */
        n->bar.asq |= data << 32;
        trace_nvme_mmio_asqaddr_hi(data, n->bar.asq);
        break;
    case 0x30:  /* ACQ */
        trace_nvme_mmio_acqaddr(data);
        n->bar.acq = data;
        break;
    case 0x34:  /* ACQ hi */
        n->bar.acq |= data << 32;
        trace_nvme_mmio_acqaddr_hi(data, n->bar.acq);
        break;
    case 0x38:  /* CMBLOC */
        NVME_GUEST_ERR(nvme_ub_mmiowr_cmbloc_reserved,
                       "invalid write to reserved CMBLOC"
                       " when CMBSZ is zero, ignored");
        return;
    case 0x3C:  /* CMBSZ */
        NVME_GUEST_ERR(nvme_ub_mmiowr_cmbsz_readonly,
                       "invalid write to read only CMBSZ, ignored");
        return;
    default:
        NVME_GUEST_ERR(nvme_ub_mmiowr_invalid,
                       "invalid MMIO write,"
                       " offset=0x%"PRIx64", data=%"PRIx64"",
                       offset, data);
        break;
    }
}

static uint64_t nvme_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    uint8_t *ptr = (uint8_t *)&n->bar;
    uint64_t val = 0;

    if (unlikely(addr & (sizeof(uint32_t) - 1))) {
        NVME_GUEST_ERR(nvme_ub_mmiord_misaligned32,
                       "MMIO read not 32-bit aligned,"
                       " offset=0x%"PRIx64"", addr);
        /* should RAZ, fall through for now */
    } else if (unlikely(size < sizeof(uint32_t))) {
        NVME_GUEST_ERR(nvme_ub_mmiord_toosmall,
                       "MMIO read smaller than 32-bits,"
                       " offset=0x%"PRIx64"", addr);
        /* should RAZ, fall through for now */
    }

    if (addr < sizeof(n->bar)) {
        memcpy(&val, ptr + addr, size);
    } else {
        NVME_GUEST_ERR(nvme_ub_mmiord_invalid_ofs,
                       "MMIO read beyond last register,"
                       " offset=0x%"PRIx64", returning 0", addr);
    }

    return val;
}

static void nvme_process_db(NvmeCtrl *n, hwaddr addr, int val)
{
    uint32_t qid;

    if (unlikely(addr & ((1 << 2) - 1))) {
        NVME_GUEST_ERR(nvme_ub_db_wr_misaligned,
                       "doorbell write not 32-bit aligned,"
                       " offset=0x%"PRIx64", ignoring", addr);
        return;
    }

    if (((addr - 0x1000) >> 2) & 1) {
        /* Completion queue doorbell write */

        uint16_t new_head = val & 0xffff;
        int start_sqs;
        NvmeCQueue *cq;

        qid = (addr - (0x1000 + (1 << 2))) >> 3;
        if (unlikely(nvme_check_cqid(n, qid))) {
            NVME_GUEST_ERR(nvme_ub_db_wr_invalid_cq,
                           "completion queue doorbell write"
                           " for nonexistent queue,"
                           " sqid=%"PRIu32", ignoring", qid);
            return;
        }

        cq = n->cq[qid];
        if (unlikely(new_head >= cq->size)) {
            NVME_GUEST_ERR(nvme_ub_db_wr_invalid_cqhead,
                           "completion queue doorbell write value"
                           " beyond queue size, sqid=%"PRIu32","
                           " new_head=%"PRIu16", ignoring",
                           qid, new_head);
            return;
        }

        start_sqs = nvme_cq_full(cq) ? 1 : 0;
        cq->head = new_head;
        if (start_sqs) {
            NvmeSQueue *sq;
            QTAILQ_FOREACH(sq, &cq->sq_list, entry) {
                timer_mod(sq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
            }
            timer_mod(cq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
        }

        if (cq->tail == cq->head) {
            nvme_irq_deassert(n, cq);
        }
    } else {
        /* Submission queue doorbell write */

        uint16_t new_tail = val & 0xffff;
        NvmeSQueue *sq;

        qid = (addr - 0x1000) >> 3;
        if (unlikely(nvme_check_sqid(n, qid))) {
            NVME_GUEST_ERR(nvme_ub_db_wr_invalid_sq,
                           "submission queue doorbell write"
                           " for nonexistent queue,"
                           " sqid=%"PRIu32", ignoring", qid);
            return;
        }

        sq = n->sq[qid];
        if (unlikely(new_tail >= sq->size)) {
            NVME_GUEST_ERR(nvme_ub_db_wr_invalid_sqtail,
                           "submission queue doorbell write value"
                           " beyond queue size, sqid=%"PRIu32","
                           " new_tail=%"PRIu16", ignoring",
                           qid, new_tail);
            return;
        }

        sq->tail = new_tail;
        timer_mod(sq->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500);
    }
}

static void nvme_mmio_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    if (addr < sizeof(n->bar)) {
        nvme_write_bar(n, addr, data, size);
    } else if (addr >= 0x1000) {
        nvme_process_db(n, addr, data);
    }
}

static const MemoryRegionOps nvme_mmio_ops = {
    .read = nvme_mmio_read,
    .write = nvme_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 8,
    },
};

static void nvme_cmb_write(void *opaque, hwaddr addr, uint64_t data,
    unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    stn_le_p(&n->cmbuf[addr], size, data);
}

static uint64_t nvme_cmb_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    return ldn_le_p(&n->cmbuf[addr], size);
}

static const MemoryRegionOps nvme_cmb_ops = {
    .read = nvme_cmb_read,
    .write = nvme_cmb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void nvme_realize_error_info_log(NvmeCtrl *_ctrl)
{
    NvmeErrorLog *elog = _ctrl->error_info;

    // followings are just temporary...
    for ( int i = 0; i < NVME_NUM_ERROR_LOG; i++ ) {
        elog->error_count  = cpu_to_le64( 0 );
        elog->sqid         = cpu_to_le16( 0xFFFF );
        elog->cid          = cpu_to_le16( 0xFFFF );
        elog->status_field = cpu_to_le16( 0 );
        elog->param_error_location = cpu_to_le16( 0 );
        elog->lba          = cpu_to_le64( 0 );
        elog->nsid         = cpu_to_le32( 0 );
        elog->vs           = 0;
        elog->cmd_specific_info    = cpu_to_le64( 0 );
	elog++;
    }
}

static void nvme_realize_smart_log(NvmeCtrl *_ctrl)
{
    NvmeSmartLog *log = &(_ctrl->smart);

    nvme_smart_load(_ctrl);

    // followings are just temporary...?
    log->temperature[0] = ( cpu_to_le16( 273 + 30 ) & 0xFF );
    log->temperature[1] = ( ( cpu_to_le16( 273 + 30 ) >> 8 ) & 0xFF );
    log->available_spare = 100;
    log->available_spare_threshold = 10;
    log->temperature_sensor[0] = cpu_to_le16( 273 + 30 );
}

static void nvme_realize_fw_slot_info_log(NvmeCtrl *_ctrl)
{
    NvmeFwSlotInfoLog *log = &(_ctrl->fw_slot_info);

    memset( log, 0, sizeof(NvmeFwSlotInfoLog) ); // clear
    log->afi = 1; // firmware in slot 1 is running (active slot number)

    // only slot 1 has valid firmware revision
    strpadcpy((char *)(log->frs1), sizeof(log->frs1), "1.0", ' ');
}

static void nvme_realize_id_ctrl(NvmeCtrl *_ctrl, uint8_t *_pci_conf)
{
    NvmeIdCtrl *id = &_ctrl->id_ctrl;

    // PCI Vendor ID (VID)
    id->vid = cpu_to_le16(pci_get_word(_pci_conf + PCI_VENDOR_ID));

    // PCI Subsystem Vendor ID (SSVID)
    id->ssvid = cpu_to_le16(pci_get_word(_pci_conf + PCI_SUBSYSTEM_VENDOR_ID));

    // Serial Number (SN)
    strpadcpy((char *)id->sn, sizeof(id->sn), _ctrl->serial, ' ');

    // Model Number (MN)
    strpadcpy((char *)id->mn, sizeof(id->mn), "QEMU NVMe Ctrl", ' ');

    // Firmware Revision (FR)
    strpadcpy((char *)id->fr, sizeof(id->fr), "1.0", ' ');

    // Recommended Arbitration Burst (RAB)
    id->rab = 6;

    // IEEE OUI Identifier (IEEE)
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x02;
    id->ieee[2] = 0xb3;

    // Controller Multi-Path I/O and Namespace Sharing Capabilities (CMIC)
    id->cmic = 0;

    // Maximum Data Transfer Size (MDTS)
    id->mdts = 0; // no restrictions

    // Controller ID (CNTLID)
    id->cntlid = 0;

    // Version (VER)
    id->ver = cpu_to_le32(0x00010300);

    // RTD3 Resume Latency (RTD3R)
    id->rtd3r = 1000;

    // RTD3 Entry Latency (RTD3E)
    id->rtd3e = 1000;

    // Optional Asynchronous Events Supported (OAES)
    id->oaes = 0;

    // Optional Admin Command Support (OACS)
    id->oacs = 0;

    // Abort Command Limit (ACL)
    id->acl = 0;

    // Asynchronous Event Request Limit (AERL)
    id->aerl = 0;

    // Firmware Updates (FRMW)
    //  - controller requires a reset to activate downloaded firmware
    //  - number of firmware slot is seven
    //  - the first firmware slot (slot 1) is read/write
    id->frmw = 7 << 1;

    // Log Page Attributes (LPA)
    //  - Telemetry supported (only header)
    //  - Command Effects log page is supported
    //  - SMART log page is not per namespace basis
    id->lpa = NVME_LPA_CSE | NVME_LPA_TELEMETRY;

    // Error Log Page Entries (ELPE)
    id->elpe = (NVME_NUM_ERROR_LOG - 1);

    // Number of Power States Support (NPSS)
    id->npss = 0;

    // Admin Vendor Specific Command Configuration (AVSCC)
    id->avscc = 0;

    // Autonomous Power State Transition Attributes (APSTA)
    id->apsta = 0;

    // Warning Composite Temperature Threshold (WCTEMP)
    id->wctemp = 363; // 90 degrees Celcius

    // Critical Composite Temperature Threshold (CCTEMP)
    id->cctemp = 373; // 100 degrees Celcius

    // Maximum Time for Firmware Activation (MTFA)
    id->mtfa = 0;

    // Host Memory Buffer Preferred Size (HMPRE)
    id->hmpre = 0;

    // Host Memory Buffer Minimum Size (HMMIN)
    id->hmmin = 0;

    // Total NVM Capacity (TNVMCAP)
    id->tnvmcap[0] = 0;
    id->tnvmcap[1] = 0;

    // Unallocated NVM Capacity (UNVMCAP)
    id->unvmcap[0] = 0;
    id->unvmcap[1] = 0;

    // Replay Protected Memory Block Support (RPMBS)
    id->rpmbs = 0;

    // Submission Queue Entry Size (SQES)
    id->sqes = (0x6 << 4) | 0x6;

    // Completion Queue Entry Size (CQES)
    id->cqes = (0x4 << 4) | 0x4;

    // Number of Namespaces (NN)
    id->nn = cpu_to_le32(_ctrl->num_namespaces);

    // Optional NVM Command Support (ONCS)
    id->oncs = cpu_to_le16(NVME_ONCS_WRITE_ZEROS | NVME_ONCS_TIMESTAMP | NVME_ONCS_DSM);

    // Fused Operation Support (FUSES)
    id->fuses = 0;

    // Format NVM Attributes (FNA)
    id->fna = 0;

    // Volatile Write Cache (VWC)
    if (blk_enable_write_cache(_ctrl->conf.blk)) {
        id->vwc = 1;
    }

    // Atomic Write Unit Normal (AWUN)
    id->awun = 0;

    // Atomic Write Unit Power Fail (AWUPF)
    id->awupf = 0;

    // NVM Vendor Specific Command Configuration (NVSCC)
    id->nvscc = 0;

    // Atomic Compare Write Unit (ACWU)
    id->acwu = 0;

    // SGL Support (SGLS)
    id->sgls = 0;

    // Power State Descriptors
    id->psd[0].mp    = cpu_to_le16(0x9c4);
    id->psd[0].enlat = cpu_to_le32(0x10);
    id->psd[0].exlat = cpu_to_le32(0x4);
}

static void nvme_realize(PCIDevice *pci_dev, Error **errp)
{
    NvmeCtrl *n = NVME(pci_dev);

    int i;
    int64_t bs_size;
    uint8_t *pci_conf;

    if (!n->num_queues) {
        error_setg(errp, "num_queues can't be zero");
        return;
    }

    if (!n->conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    bs_size = blk_getlength(n->conf.blk);
    if (bs_size < 0) {
        error_setg(errp, "could not get backing file size");
        return;
    }

    if (!n->serial) {
        error_setg(errp, "serial property not set");
        return;
    }
    blkconf_blocksizes(&n->conf);
    if (!blkconf_apply_backend_options(&n->conf, blk_is_read_only(n->conf.blk),
                                       false, errp)) {
        return;
    }

    pci_conf = pci_dev->config;
    pci_conf[PCI_INTERRUPT_PIN] = 1;
    pci_config_set_prog_interface(pci_dev->config, 0x2);
    pci_config_set_class(pci_dev->config, PCI_CLASS_STORAGE_EXPRESS);
    pcie_endpoint_cap_init(pci_dev, 0x80);

    n->num_namespaces = 1;
    n->reg_size = pow2ceil(0x1004 + 2 * (n->num_queues + 1) * 4);
    n->ns_size = bs_size / (uint64_t)n->num_namespaces;

    n->namespaces = g_new0(NvmeNamespace, n->num_namespaces);
    n->sq = g_new0(NvmeSQueue *, n->num_queues);
    n->cq = g_new0(NvmeCQueue *, n->num_queues);

    memory_region_init_io(&n->iomem, OBJECT(n), &nvme_mmio_ops, n,
                          "nvme", n->reg_size);
    pci_register_bar(pci_dev, 0,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
        &n->iomem);
    msix_init_exclusive_bar(pci_dev, n->num_queues, 4, NULL);

    nvme_realize_id_ctrl(n, pci_conf);
    nvme_realize_smart_log(n);
    nvme_realize_error_info_log(n);
    nvme_realize_fw_slot_info_log(n);

    n->bar.cap = 0;
    NVME_CAP_SET_MQES(n->bar.cap, 0x7ff);
    NVME_CAP_SET_CQR(n->bar.cap, 1);
    NVME_CAP_SET_TO(n->bar.cap, 0xf);
    NVME_CAP_SET_CSS(n->bar.cap, 1);
    NVME_CAP_SET_MPSMAX(n->bar.cap, 4);

    n->bar.vs = 0x00010200;
    n->bar.intmc = n->bar.intms = 0;

    if (n->cmb_size_mb) {

        NVME_CMBLOC_SET_BIR(n->bar.cmbloc, 2);
        NVME_CMBLOC_SET_OFST(n->bar.cmbloc, 0);

        NVME_CMBSZ_SET_SQS(n->bar.cmbsz, 1);
        NVME_CMBSZ_SET_CQS(n->bar.cmbsz, 0);
        NVME_CMBSZ_SET_LISTS(n->bar.cmbsz, 0);
        NVME_CMBSZ_SET_RDS(n->bar.cmbsz, 1);
        NVME_CMBSZ_SET_WDS(n->bar.cmbsz, 1);
        NVME_CMBSZ_SET_SZU(n->bar.cmbsz, 2); /* MBs */
        NVME_CMBSZ_SET_SZ(n->bar.cmbsz, n->cmb_size_mb);

        n->cmbloc = n->bar.cmbloc;
        n->cmbsz = n->bar.cmbsz;

        n->cmbuf = g_malloc0(NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
        memory_region_init_io(&n->ctrl_mem, OBJECT(n), &nvme_cmb_ops, n,
                              "nvme-cmb", NVME_CMBSZ_GETSIZE(n->bar.cmbsz));
        pci_register_bar(pci_dev, NVME_CMBLOC_BIR(n->bar.cmbloc),
            PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
            PCI_BASE_ADDRESS_MEM_PREFETCH, &n->ctrl_mem);

    }

    for (i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        NvmeIdNs *id_ns = &ns->id_ns;
        id_ns->nsfeat = 0;
        id_ns->nlbaf = 0;
        id_ns->flbas = 0;
        id_ns->mc = 0;
        id_ns->dpc = 0;
        id_ns->dps = 0;
        id_ns->lbaf[0].ds = BDRV_SECTOR_BITS;
        id_ns->ncap  = id_ns->nuse = id_ns->nsze =
            cpu_to_le64(n->ns_size >>
                id_ns->lbaf[NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas)].ds);
    }
}

static void nvme_exit(PCIDevice *pci_dev)
{
    NvmeCtrl *n = NVME(pci_dev);

    nvme_clear_ctrl(n);
    g_free(n->namespaces);
    g_free(n->cq);
    g_free(n->sq);

    if (n->cmb_size_mb) {
        g_free(n->cmbuf);
    }
    msix_uninit_exclusive_bar(pci_dev);
}

static Property nvme_props[] = {
    DEFINE_BLOCK_PROPERTIES(NvmeCtrl, conf),
    DEFINE_PROP_STRING("serial", NvmeCtrl, serial),
    DEFINE_PROP_UINT32("cmb_size_mb", NvmeCtrl, cmb_size_mb, 0),
    DEFINE_PROP_UINT32("num_queues", NvmeCtrl, num_queues, 64),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription nvme_vmstate = {
    .name = "nvme",
    .unmigratable = 1,
};

static void nvme_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = nvme_realize;
    pc->exit = nvme_exit;
    pc->class_id = PCI_CLASS_STORAGE_EXPRESS;
    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->device_id = 0x5845;
    pc->revision = 2;

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "Non-Volatile Memory Express";
    dc->props = nvme_props;
    dc->vmsd = &nvme_vmstate;
}

static void nvme_instance_init(Object *obj)
{
    NvmeCtrl *s = NVME(obj);

    device_add_bootindex_property(obj, &s->conf.bootindex,
                                  "bootindex", "/namespace@1,0",
                                  DEVICE(obj), &error_abort);
}

static const TypeInfo nvme_info = {
    .name          = TYPE_NVME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NvmeCtrl),
    .class_init    = nvme_class_init,
    .instance_init = nvme_instance_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void nvme_register_types(void)
{
    type_register_static(&nvme_info);
}

type_init(nvme_register_types)
