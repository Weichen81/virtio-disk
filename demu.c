/*  
 * Copyright (c) 2014, Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <xenctrl.h>
#include <xenforeignmemory.h>
#include <xenevtchn.h>
#include <xendevicemodel.h>
#include <xen/hvm/ioreq.h>

#include "debug.h"
#include "device.h"
#include "demu.h"
#include "mapcache.h"
#include "xs_dev.h"

#include "kvm/kvm.h"

#define XS_DISK_TYPE	"virtio_disk"

static struct disk_image_params disk_image[MAX_DISK_IMAGES];
static u8 image_count;

/*
 * XXX:
 * 1. This file should be refactored heavily.
 */

bool do_debug_print = true;

#define __max(_x, _y) (((_x) > (_y)) ? (_x) : (_y))

typedef enum {
    DEMU_SEQ_UNINITIALIZED = 0,
    DEMU_SEQ_XENSTORE_ATTACHED,
    DEMU_SEQ_XENCTRL_OPEN,
    DEMU_SEQ_XENEVTCHN_OPEN,
    DEMU_SEQ_XENFOREIGNMEMORY_OPEN,
    DEMU_SEQ_XENDEVICEMODEL_OPEN,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_RESOURCE_MAPPED,
    DEMU_SEQ_SERVER_ENABLED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_BUF_PORT_BOUND,
    DEMU_SEQ_DEVICE_INITIALIZED,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;

typedef struct demu_space demu_space_t;

struct demu_space {
    demu_space_t	*next;
    uint64_t		start;
    uint64_t		end;
    void			(*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr);
    void			*ptr;
};

typedef struct demu_state {
    demu_seq_t                       seq;
    xc_interface                     *xch;
    xenevtchn_handle                 *xeh;
    xenforeignmemory_handle          *xfh;
    xendevicemodel_handle            *xdh;
    domid_t                          domid;
    domid_t                          be_domid;
    unsigned int                     vcpus;
    ioservid_t                       ioservid;
    xenforeignmemory_resource_handle *resource;
    shared_iopage_t                  *shared_iopage;
    evtchn_port_t                    *ioreq_local_port;
    buffered_iopage_t                *buffered_iopage;
    evtchn_port_t                    buf_ioreq_port;
    evtchn_port_t                    buf_ioreq_local_port;
    demu_space_t                     *memory;
    struct xs_dev                    *xs_dev;
} demu_state_t;

static demu_state_t demu_state;

void
demu_set_irq(int irq, int level)
{
    xendevicemodel_set_irq_level(demu_state.xdh, demu_state.domid,
                                 irq, level);
}

void *
demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n)
{
    return xenforeignmemory_map(demu_state.xfh, demu_state.domid,
                                PROT_READ | PROT_WRITE, n,
                                pfn, NULL);
}

void *
demu_map_guest_range(uint64_t addr, uint64_t size)
{
    xen_pfn_t   *pfn;
    int         i, n;
    void        *ptr;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

    pfn = malloc(sizeof (xen_pfn_t) * n);
    if (pfn == NULL)
        goto fail1;

    for (i = 0; i < n; i++)
        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;

    ptr = demu_map_guest_pages(pfn, n);
    if (ptr == NULL)
        goto fail2;

    free(pfn);

    return ptr + (addr & ~TARGET_PAGE_MASK);

fail2:
    DBG("fail2\n");
    
    free(pfn);
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
}

void
demu_unmap_guest_pages(void *ptr, unsigned int n)
{
    xenforeignmemory_unmap(demu_state.xfh, ptr, n);
}

int
demu_unmap_guest_range(void *ptr, uint64_t size)
{
    int n;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

    demu_unmap_guest_pages((void *)((unsigned long)ptr & TARGET_PAGE_MASK), n);

    return 0;
}

static demu_space_t *
demu_find_space(demu_space_t *head, uint64_t addr)
{
    demu_space_t    *space;

    for (space = head; space != NULL; space = space->next)
        if (addr >= space->start && addr <= space->end)
            return space;

    return NULL;
}

static demu_space_t *
demu_find_memory_space(uint64_t addr)
{
    demu_space_t    *space;

    space = demu_find_space(demu_state.memory, addr);

    if (space == NULL)
        DBG("failed to find space for 0x%"PRIx64"\n", addr);

    return space;
}

static int
demu_register_space(demu_space_t **headp, uint64_t start, uint64_t end,
    void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
    void *ptr)
{
    demu_space_t    *space;

    assert(mmio_fn);

    if (demu_find_space(*headp, start) || demu_find_space(*headp, end))
        goto fail1;

    space = malloc(sizeof (demu_space_t));
    if (space == NULL)
        goto fail2;

    space->start = start;
    space->end = end;
    space->mmio_fn = mmio_fn;
    space->ptr = ptr;

    space->next = *headp;
    *headp = space;

    return 0;

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");
    warn("fail");
    return -1;
}

static void
demu_deregister_space(demu_space_t **headp, uint64_t start, uint64_t *end)
{
    demu_space_t    **spacep;
    demu_space_t    *space;

    spacep = headp;
    while ((space = *spacep) != NULL) {
        if (start == space->start) {
            *spacep = space->next;
            if (end != NULL)
                *end = space->end;
            free(space);
            return;
        }
        spacep = &(space->next);
    }
}

int
demu_register_memory_space(uint64_t start, uint64_t size,
    void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
    void *ptr)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.memory, start, start + size - 1, mmio_fn, ptr);
    if (rc < 0)
        goto fail1;

    rc = xendevicemodel_map_io_range_to_ioreq_server(demu_state.xdh,
                                                     demu_state.domid,
                                                     demu_state.ioservid,
                                                     1, start,
                                                     start + size - 1);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.memory, start, NULL);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
demu_deregister_memory_space(uint64_t start)
{
    uint64_t end = 0;

    DBG("%"PRIx64"\n", start);

    demu_deregister_space(&demu_state.memory, start, &end);

    xendevicemodel_unmap_io_range_from_ioreq_server(demu_state.xdh,
                                                    demu_state.domid,
                                                    demu_state.ioservid,
                                                    1, start, end);
}

static void
demu_handle_io(ioreq_t *ioreq)
{
    uint8_t data[8] = {0};
    demu_space_t *space;

    space = demu_find_memory_space(ioreq->addr);
    if (space == NULL) {
        fprintf(stderr, "Ignoring MMIO %s at 0x%lx (size %u)\n",
            ioreq->dir == IOREQ_READ ? "read" : "write", ioreq->addr, ioreq->size);
        return;
    }

    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            space->mmio_fn(ioreq->addr, data, ioreq->size, 0, space->ptr);
            ioreq->data = *(uint64_t *)&data;
        } else
            assert(0);
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            *(uint64_t *)&data = ioreq->data;
            space->mmio_fn(ioreq->addr, data, ioreq->size, 1, space->ptr);
        } else
            assert(0);
    }
}

static void
demu_handle_ioreq(ioreq_t *ioreq)
{
    switch (ioreq->type) {
    case IOREQ_TYPE_COPY:
        demu_handle_io(ioreq);
        break;

    case IOREQ_TYPE_INVALIDATE:
#ifdef USE_MAPCACHE
        mapcache_inval_cnt ++;
#endif
        break;

    default:
        DBG("UNKNOWN (%02x)", ioreq->type);
        break;
    }
}

static void
demu_seq_next(void)
{
    assert(demu_state.seq < DEMU_SEQ_INITIALIZED);

    switch (++demu_state.seq) {
    case DEMU_SEQ_XENSTORE_ATTACHED: {
        int i;

        DBG(">XENSTORE_ATTACHED\n");
        DBG("domid = %u\n", demu_state.domid);

        for (i = 0; i < image_count; i++) {
            DBG("filename[%d] = %s\n", i, disk_image[i].filename);
            DBG("readonly[%d] = %d\n", i, disk_image[i].readonly);
            DBG("base[%d]     = 0x%x\n", i, disk_image[i].addr);
            DBG("irq[%d]      = %u\n", i, disk_image[i].irq);
        }
        break;
    }

    case DEMU_SEQ_XENCTRL_OPEN:
        DBG(">XENCTRL_OPEN\n");
        break;

    case DEMU_SEQ_XENEVTCHN_OPEN:
        DBG(">XENEVTCHN_OPEN\n");
        break;

    case DEMU_SEQ_XENFOREIGNMEMORY_OPEN:
        DBG(">XENFOREIGNMEMORY_OPEN\n");
        break;

    case DEMU_SEQ_XENDEVICEMODEL_OPEN:
        DBG(">XENDEVICEMODEL_OPEN\n");
        break;

    case DEMU_SEQ_SERVER_REGISTERED:
        DBG(">SERVER_REGISTERED\n");
        DBG("ioservid = %u\n", demu_state.ioservid);
        break;

    case DEMU_SEQ_RESOURCE_MAPPED:
        DBG(">RESOURCE_MAPPED\n");
        DBG("shared_iopage = %p\n", demu_state.shared_iopage);
        DBG("buffered_iopage = %p\n", demu_state.buffered_iopage);
        break;

    case DEMU_SEQ_SERVER_ENABLED:
        DBG(">SERVER_ENABLED\n");
        break;

    case DEMU_SEQ_PORT_ARRAY_ALLOCATED:
        DBG(">PORT_ARRAY_ALLOCATED\n");
        break;

    case DEMU_SEQ_PORTS_BOUND: {
        int i;

        DBG(">EVTCHN_PORTS_BOUND\n");

        for (i = 0; i < demu_state.vcpus; i++)
            DBG("VCPU%d: %u -> %u\n", i,
                demu_state.shared_iopage->vcpu_ioreq[i].vp_eport,
                demu_state.ioreq_local_port[i]);

        break;
    }

    case DEMU_SEQ_BUF_PORT_BOUND:
        DBG(">EVTCHN_BUF_PORT_BOUND\n");

        DBG("%u -> %u\n",
            demu_state.buf_ioreq_port,
            demu_state.buf_ioreq_local_port);
        break;

    case DEMU_SEQ_DEVICE_INITIALIZED:
        DBG(">DEVICE_INITIALIZED\n");
        break;

    case DEMU_SEQ_INITIALIZED:
        DBG(">INITIALIZED\n");
        break;

    default:
        assert(0);
        break;
    }
}

static void
demu_teardown(void)
{
    if (demu_state.seq == DEMU_SEQ_INITIALIZED) {
        DBG("<INITIALIZED\n");

        demu_state.seq = DEMU_SEQ_DEVICE_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_DEVICE_INITIALIZED) {
        DBG("<DEVICE_INITIALIZED\n");
        device_teardown();

        demu_state.seq = DEMU_SEQ_BUF_PORT_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_BUF_PORT_BOUND) {
        DBG("<EVTCHN_BUF_PORT_BOUND\n");
        evtchn_port_t   port;

        port = demu_state.buf_ioreq_local_port;

        DBG("%u\n", port);
        (void) xenevtchn_unbind(demu_state.xeh, port);

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_PORTS_BOUND\n");

        demu_state.seq = DEMU_SEQ_PORT_ARRAY_ALLOCATED;
    }

    if (demu_state.seq >= DEMU_SEQ_PORT_ARRAY_ALLOCATED) {
        int i;

        DBG("<PORT_ARRAY_ALLOCATED\n");

        for (i = 0; i < demu_state.vcpus; i++) {
            evtchn_port_t   port;

            port = demu_state.ioreq_local_port[i];

            if (port >= 0) {
                DBG("VCPU%d: %u\n", i, port);
                (void) xenevtchn_unbind(demu_state.xeh, port);
            }
        }

        free(demu_state.ioreq_local_port);

        demu_state.seq = DEMU_SEQ_SERVER_ENABLED;
    }

    if (demu_state.seq == DEMU_SEQ_SERVER_ENABLED) {
        DBG("<SERVER_ENABLED\n");
        (void) xendevicemodel_set_ioreq_server_state(demu_state.xdh,
                                                     demu_state.domid,
                                                     demu_state.ioservid,
                                                     0);

        demu_state.seq = DEMU_SEQ_RESOURCE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_RESOURCE_MAPPED) {
        DBG("<RESOURCE_MAPPED\n");

        xenforeignmemory_unmap_resource(demu_state.xfh,
                                        demu_state.resource);

        demu_state.seq = DEMU_SEQ_SERVER_REGISTERED;
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_REGISTERED) {
        DBG("<SERVER_REGISTERED\n");

        (void) xendevicemodel_destroy_ioreq_server(demu_state.xdh,
                                                   demu_state.domid,
                                                   demu_state.ioservid);
        demu_state.seq = DEMU_SEQ_XENDEVICEMODEL_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENDEVICEMODEL_OPEN) {
        DBG("<XENDEVICEMODEL_OPEN\n");

        xendevicemodel_close(demu_state.xdh);

        demu_state.seq = DEMU_SEQ_XENFOREIGNMEMORY_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENFOREIGNMEMORY_OPEN) {
        DBG("<XENFOREIGNMEMORY_OPEN\n");

        xenforeignmemory_close(demu_state.xfh);

        demu_state.seq = DEMU_SEQ_XENEVTCHN_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENEVTCHN_OPEN) {
        DBG("<XENEVTCHN_OPEN\n");

        xenevtchn_close(demu_state.xeh);

        demu_state.seq = DEMU_SEQ_XENCTRL_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENCTRL_OPEN) {
        DBG("<XENCTRL_OPEN\n");

        xc_interface_close(demu_state.xch);

        demu_state.seq = DEMU_SEQ_XENSTORE_ATTACHED;
    }

    if (demu_state.seq >= DEMU_SEQ_XENSTORE_ATTACHED) {
        int i;

        DBG("<XENSTORE_ATTACHED\n");

        for (i = 0; i < MAX_DISK_IMAGES; i++) {
            if (disk_image[i].filename) {
                free((void *)disk_image[i].filename);
                disk_image[i].filename = NULL;
            }
        }

        xenstore_disconnect_dom(demu_state.xs_dev);

        demu_state.seq = DEMU_SEQ_UNINITIALIZED;
    }
}

static struct sigaction sigterm_handler;

static void
demu_sigterm(int num)
{
    DBG("%s\n", strsignal(num));

    demu_teardown();
    xenstore_destroy(demu_state.xs_dev);

    exit(0);
}

static int demu_read_xenstore_config(void *unused)
{
    char **entries = NULL;
    unsigned int num, i;
    int ret = 0;

    entries = xs_directory(demu_state.xs_dev->xsh, XBT_NULL,
                           demu_state.xs_dev->fe, &num);
    if (!entries)
        return -1;

    image_count = 0;
    for (i = 0; i < num; i++) {
        char *str, *end, node[32];
        int index, val;

        index = strtol(entries[i], &end, 0);
        if (*end != '\0')
           continue;

        if (index >= MAX_DISK_IMAGES || index != image_count) {
            ret = -1;
            break;
        }

        snprintf(node, sizeof(node), "%d/readonly", index);
        ret = xenstore_read_fe_int(demu_state.xs_dev, node, &val);
        if (ret < 0)
            break;
        disk_image[image_count].readonly = val;

        snprintf(node, sizeof(node), "%d/base", index);
        ret = xenstore_read_fe_int(demu_state.xs_dev, node, &val);
        if (ret < 0)
            break;
        disk_image[image_count].addr = val;

        snprintf(node, sizeof(node), "%d/irq", index);
        ret = xenstore_read_fe_int(demu_state.xs_dev, node, &val);
        if (ret < 0)
            break;
        disk_image[image_count].irq = val;

        snprintf(node, sizeof(node), "%d/filename", index);
        str = xenstore_read_fe_str(demu_state.xs_dev, node);
        if (!str) {
            ret = -1;
            break;
        }
        disk_image[image_count].filename = str;

        image_count ++;
    }

    free(entries);

    if (!image_count)
        ret = -1;
    else if (ret < 0) {
        for (i = 0; i < image_count; i++) {
            if (disk_image[i].filename) {
                free((void *)disk_image[i].filename);
                disk_image[i].filename = NULL;
            }
        }
    }

    return ret;
}

static int
demu_initialize(void)
{
    int             rc;
    xc_dominfo_t    dominfo;
    void            *addr;
    evtchn_port_t   port;
    evtchn_port_t   buf_port;
    int             i;

    rc = xenstore_connect_dom(demu_state.xs_dev, demu_state.be_domid,
            demu_state.domid, demu_read_xenstore_config, NULL);
    if (rc < 0)
        goto fail0;

    demu_seq_next();

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL)
        goto fail1;

    demu_seq_next();

    demu_state.xeh = xenevtchn_open(NULL, 0);
    if (demu_state.xeh == NULL)
        goto fail2;

    demu_seq_next();

    demu_state.xfh = xenforeignmemory_open(NULL, 0);
    if (demu_state.xfh == NULL)
        goto fail3;

    demu_seq_next();

    demu_state.xdh = xendevicemodel_open(NULL, 0);
    if (demu_state.xdh == NULL)
        goto fail4;

    demu_seq_next();

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid)
        goto fail5;

    demu_state.vcpus = dominfo.max_vcpu_id + 1;

    DBG("%d vCPU(s)\n", demu_state.vcpus);

    rc = xc_domain_set_target(demu_state.xch, demu_state.be_domid, demu_state.domid);
    if (rc < 0)
        goto fail5;

    rc = xendevicemodel_create_ioreq_server(demu_state.xdh,
                                            demu_state.domid, 1,
                                            &demu_state.ioservid);
    if (rc < 0)
        goto fail6;
    
    demu_seq_next();

    addr = NULL;
    demu_state.resource =
        xenforeignmemory_map_resource(demu_state.xfh, demu_state.domid,
                                      XENMEM_resource_ioreq_server,
                                      demu_state.ioservid, 0, 2,
                                      &addr,
                                      PROT_READ | PROT_WRITE, 0);
    if (demu_state.resource == NULL)
        goto fail7;

    demu_state.buffered_iopage = addr;
    demu_state.shared_iopage = addr + XC_PAGE_SIZE;

    rc = xendevicemodel_get_ioreq_server_info(demu_state.xdh,
                                              demu_state.domid,
                                              demu_state.ioservid,
                                              NULL, NULL, &buf_port);
    if (rc < 0)
        goto fail8;

    demu_seq_next();

    rc = xendevicemodel_set_ioreq_server_state(demu_state.xdh,
                                               demu_state.domid,
                                               demu_state.ioservid,
                                               1);
    if (rc != 0)
        goto fail9;

    demu_seq_next();

    demu_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL)
        goto fail10;

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        rc = xenevtchn_bind_interdomain(demu_state.xeh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail11;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

    rc = xenevtchn_bind_interdomain(demu_state.xeh, demu_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail12;

    demu_state.buf_ioreq_local_port = rc;

    demu_seq_next();

    rc = device_initialize(disk_image, image_count);
    if (rc < 0)
        goto fail13;

    demu_seq_next();

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

fail13:
    DBG("fail13\n");

fail12:
    DBG("fail12\n");

fail11:
    DBG("fail11\n");

fail10:
    DBG("fail10\n");

fail9:
    DBG("fail9\n");

fail8:
    DBG("fail8\n");

fail7:
    DBG("fail7\n");

fail6:
    DBG("fail6\n");

fail5:
    DBG("fail5\n");

fail4:
    DBG("fail4\n");

fail3:
    DBG("fail3\n");

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");

fail0:
    DBG("fail0\n");

    warn("fail");
    return -1;
}

static void
demu_poll_buffered_iopage(void)
{
    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    for (;;) {
        unsigned int    read_pointer;
        unsigned int    write_pointer;
        
        read_pointer = demu_state.buffered_iopage->read_pointer;
        write_pointer = demu_state.buffered_iopage->write_pointer;
        xen_mb();

        if (read_pointer == write_pointer)
            break;

        while (read_pointer != write_pointer) {
            unsigned int    slot;
            buf_ioreq_t     *buf_ioreq;
            ioreq_t         ioreq;

            slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;

            buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

            ioreq.size = 1UL << buf_ioreq->size;
            ioreq.count = 1;
            ioreq.addr = buf_ioreq->addr;
            ioreq.data = buf_ioreq->data;
            ioreq.state = STATE_IOREQ_READY;
            ioreq.dir = buf_ioreq->dir;
            ioreq.df = 1;
            ioreq.type = buf_ioreq->type;
            ioreq.data_is_ptr = 0;

            read_pointer++;

            if (ioreq.size == 8) {
                slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;
                buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

                ioreq.data |= ((uint64_t)buf_ioreq->data) << 32;

                read_pointer++;
            }

            demu_handle_ioreq(&ioreq);
            xen_mb();
        }

        demu_state.buffered_iopage->read_pointer = read_pointer;
        xen_mb();
    }
}

static void
demu_poll_shared_iopage(unsigned int i)
{
    ioreq_t *ioreq;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    ioreq = &demu_state.shared_iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY) {
        fprintf(stderr, "IO request not ready\n");
        return;
    }

    xen_mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
    xen_mb();

    ioreq->state = STATE_IORESP_READY;
    xen_mb();

    xenevtchn_notify(demu_state.xeh, demu_state.ioreq_local_port[i]);
}

static void
demu_poll_iopages(void)
{
    evtchn_port_t   port;
    int             i;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    port = xenevtchn_pending(demu_state.xeh);
    if (port < 0)
        return;

    if (port == demu_state.buf_ioreq_local_port) {
        xenevtchn_unmask(demu_state.xeh, port);
        demu_poll_buffered_iopage();
    } else {
        for (i = 0; i < demu_state.vcpus; i++) {
            if (port == demu_state.ioreq_local_port[i]) {
                xenevtchn_unmask(demu_state.xeh, port);
                demu_poll_shared_iopage(i);
            }
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    sigset_t        block;
    int             rc;
    int             efd, xfd;

    sigfillset(&block);

    memset(&sigterm_handler, 0, sizeof (struct sigaction));
    sigterm_handler.sa_handler = demu_sigterm;

    sigaction(SIGTERM, &sigterm_handler, NULL);
    sigdelset(&block, SIGTERM);

    sigaction(SIGINT, &sigterm_handler, NULL);
    sigdelset(&block, SIGINT);

    sigaction(SIGHUP, &sigterm_handler, NULL);
    sigdelset(&block, SIGHUP);

    sigaction(SIGABRT, &sigterm_handler, NULL);
    sigdelset(&block, SIGABRT);

    sigprocmask(SIG_BLOCK, &block, NULL);

    demu_state.xs_dev = xenstore_create(XS_DISK_TYPE);
    if (demu_state.xs_dev == NULL) {
        fprintf(stderr, "failed to create xenstore instance\n");
        exit(1);
    }

    rc = xenstore_get_be_domid(demu_state.xs_dev);
    if (rc < 0) {
        xenstore_destroy(demu_state.xs_dev);
        fprintf(stderr, "failed to read backend domid\n");
        exit(1);
    }
    demu_state.be_domid = rc;
    DBG("read backend domid %u\n", demu_state.be_domid);

    while (1) {
        rc = xenstore_wait_fe_domid(demu_state.xs_dev);
        if (rc < 0) {
            /*fprintf(stderr, "failed to read frontend domid\n");*/
            msleep(100);
            continue;
        }
        demu_state.domid = rc;
        DBG("read frontend domid %u\n", demu_state.domid);

        rc = demu_initialize();
        if (rc < 0) {
            demu_teardown();
            continue;
        }

        efd = xenevtchn_fd(demu_state.xeh);
        xfd = xenstore_get_fd(demu_state.xs_dev);

        while (1) {
            int nfds;
            fd_set fds;
            struct timeval t = { .tv_sec = 1 };

            FD_ZERO(&fds);
            FD_SET(efd, &fds);
            FD_SET(xfd, &fds);
            nfds = __max(efd, xfd) + 1;

            rc = select(nfds, &fds, NULL, NULL, &t);
            if (rc > 0) {
                if (FD_ISSET(efd, &fds))
                    demu_poll_iopages();

                if (FD_ISSET(xfd, &fds)) {
                    rc = xenstore_poll_watches(demu_state.xs_dev);
                    if (rc < 0) {
                        DBG("lost connection to dom%d\n", demu_state.domid);
                        rc = 0;
                        break;
                    }
                }
            }

            if (rc < 0 && errno != EINTR)
                break;
        }

        demu_teardown();

        if (rc < 0)
           break;
    }

    xenstore_destroy(demu_state.xs_dev);

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-tab-always-indent: nil
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
