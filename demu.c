/*  
 * Copyright (c) 2012, Citrix Systems Inc.
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

#include <locale.h>

#include <xenctrl.h>
#include <xen/hvm/ioreq.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include "debug.h"
#include "mapcache.h"
#include "device.h"
#include "pci.h"
#include "demu.h"

#define mb() asm volatile ("" : : : "memory")

enum {
    DEMU_OPT_DOMAIN,
    DEMU_OPT_DEVICE,
    DEMU_NR_OPTS
    };

static struct option demu_option[] = {
    {"domain", 1, NULL, 0},
    {"device", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *demu_option_text[] = {
    "<domid>",
    "<device>",
    NULL
};

static const char *prog;

static void
usage(void)
{
    int i;

    fprintf(stderr, "Usage: %s <options>\n\n", prog);

    for (i = 0; i < DEMU_NR_OPTS; i++)
        fprintf(stderr, "\t--%s %s\n",
                demu_option[i].name,
                demu_option_text[i]);

    fprintf(stderr, "\n");

    exit(2);
}

typedef enum {
    DEMU_SEQ_UNINITIALIZED = 0,
    DEMU_SEQ_INTERFACE_OPEN,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_SHARED_IOPAGE_MAPPED,
    DEMU_SEQ_BUFFERED_IOPAGE_MAPPED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_EVTCHN_OPEN,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_BUF_PORT_BOUND,
    DEMU_SEQ_VNC_INITIALIZED,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;

typedef struct demu_space demu_space_t;

struct demu_space {
    demu_space_t	*next;
    uint64_t		start;
    uint64_t		end;
    const io_ops_t	*ops;
    void		    *priv;
};

typedef struct demu_state {
    demu_seq_t          seq;
    xc_interface        *xch;
    xc_interface        *xceh;
    domid_t             domid;
    unsigned int        vcpus;
    ioservid_t          ioservid;
    shared_iopage_t     *shared_iopage;
    evtchn_port_t       *ioreq_local_port;
    buffered_iopage_t   *buffered_iopage;
    evtchn_port_t       buf_ioreq_port;
    evtchn_port_t       buf_ioreq_local_port;
    uint8_t             *default_framebuffer;
    uint8_t             *framebuffer;
    rfbScreenInfoPtr    screen;
    demu_space_t	    *memory;
    demu_space_t        *port;
    demu_space_t        *pci_config;
} demu_state_t;

#define DEMU_VNC_DEFAULT_WIDTH  640
#define DEMU_VNC_DEFAULT_HEIGHT 480
#define DEMU_VNC_DEFAULT_DEPTH  4

static demu_state_t demu_state;

void *
demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n)
{
    void *ptr;

    ptr = xc_map_foreign_pages(demu_state.xch, demu_state.domid,
                               PROT_READ | PROT_WRITE,
                               pfn, n);
    if (ptr == NULL)
        goto fail1;
    
    return ptr;
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
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
demu_find_pci_config_space(uint8_t bdf)
{
    return demu_find_space(demu_state.pci_config, bdf);
}

static demu_space_t *
demu_find_port_space(uint64_t addr)
{
    return demu_find_space(demu_state.port, addr);
}

static demu_space_t *
demu_find_memory_space(uint64_t addr)
{
    return demu_find_space(demu_state.memory, addr);
}

static int
demu_register_space(demu_space_t **headp, uint64_t start, uint64_t end,
                    const io_ops_t *ops, void *priv)
{
    demu_space_t    *space;

    if (demu_find_space(*headp, start) || demu_find_space(*headp, end))
        goto fail1;

    space = malloc(sizeof (demu_space_t));
    if (space == NULL)
        goto fail2;

    space->start = start;
    space->end = end;
    space->ops = ops;
    space->priv = priv;

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
demu_deregister_space(demu_space_t **headp, uint64_t start)
{
    demu_space_t    **spacep;
    demu_space_t    *space;

    spacep = headp;
    while ((space = *spacep) != NULL) {
        if (start == space->start) {
            *spacep = space->next;
            free(space);
            return;
        }
        spacep = &(space->next);
    }
    assert(FALSE);
}

int
demu_register_pci_config_space(uint8_t bus, uint8_t device, uint8_t function,
                               const io_ops_t *ops, void *priv)
{
    uint16_t    bdf;
    int         rc;

    DBG("%02x:%02x:%02x\n", bus, device, function);

    bdf = (bus << 8) | (device << 3) | function;

    rc = demu_register_space(&demu_state.pci_config, bdf, bdf, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_pcidev_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                           bdf);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.pci_config, bdf);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

int
demu_register_port_space(uint64_t start, uint64_t size, const io_ops_t *ops, void *priv)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.port, start, start + size - 1, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                             0, start, start + size - 1);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.port, start);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

int
demu_register_memory_space(uint64_t start, uint64_t size, const io_ops_t *ops, void *priv)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.memory, start, start + size - 1, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                             1, start, start + size - 1);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.memory, start);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
demu_deregister_pci_config_space(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t    bdf;

    DBG("%02x:%02x:%02x\n", bus, device, function);

    bdf = (bus << 8) | (device << 3) | function;

    xc_hvm_unmap_pcidev_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                          bdf);
    demu_deregister_space(&demu_state.pci_config, bdf);
}

void
demu_deregister_port_space(uint64_t start)
{
    DBG("%"PRIx64"\n", start);

    xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                            0, start);
    demu_deregister_space(&demu_state.port, start);
}

void
demu_deregister_memory_space(uint64_t start)
{
    DBG("%"PRIx64"\n", start);

    xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                            1, start);
    demu_deregister_space(&demu_state.memory, start);
}

#define DEMU_IO_READ(_fn, _priv, _addr, _size, _count, _val)        \
    do {                                                            \
        int       		_i = 0;                                     \
        unsigned int	_shift = 0;                                 \
                                                                    \
        (_val) = 0;                                                 \
        for (_i = 0; _i < (_count); _i++)                           \
        {                                                           \
            (_val) |= (uint32_t)(_fn)((_priv), (_addr)) << _shift;  \
            _shift += 8 * (_size);                                  \
            (_addr) += (_size);                                     \
        }                                                           \
    } while (FALSE)

static uint32_t
demu_io_read(demu_space_t *space, uint64_t addr, uint64_t size)
{
    uint32_t    val = 0;

    switch (size) {
    case 1:
        val = space->ops->readb(space->priv, addr);
        break;

    case 2:
        if (space->ops->readw == NULL)
            DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 2, val);
        else
            val = space->ops->readw(space->priv, addr);
        break;

    case 4:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 4, val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 2, val);
        } else {
            val = space->ops->readl(space->priv, addr);
        }
        break;

    default:
        assert(FALSE);
    }

    return val;
}

#define DEMU_IO_WRITE(_fn, _priv, _addr, _size, _count, _val)   \
    do {                                                        \
        int             	_i = 0;                             \
        unsigned int	_shift = 0;                             \
                                                                \
        for (_i = 0; _i < (_count); _i++)                       \
        {                                                       \
            (_fn)((_priv), (_addr), (_val) >> _shift);          \
            _shift += 8 * (_size);                              \
            (_addr) += (_size);                                 \
        }                                                       \
    } while (FALSE)

static void
demu_io_write(demu_space_t *space, uint64_t addr, uint64_t size, uint32_t val)
{
    switch (size) {
    case 1:
        space->ops->writeb(space->priv, addr, val);
        break;

    case 2:
        if (space->ops->writew == NULL)
            DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 2, val);
        else
            space->ops->writew(space->priv, addr, val);
        break;

    case 4:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 4, val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 2, val);
        } else {
            space->ops->writel(space->priv, addr, val);
        }
        break;

    default:
        assert(FALSE);
    }
}

static inline void
__copy_to_guest_memory(uint64_t addr, uint64_t size, uint8_t *src)
{
    uint8_t *dst = mapcache_lookup(addr);

    assert(((addr + size - 1) >> TARGET_PAGE_SHIFT) == (addr >> TARGET_PAGE_SHIFT));

    if (dst == NULL)
        goto fail1;

    memcpy(dst, src, size);
    return;

fail1:
    DBG("fail1\n");
}

static inline void
__copy_from_guest_memory(uint64_t addr, uint64_t size, uint8_t *dst)
{
    uint8_t *src = mapcache_lookup(addr);

    assert(((addr + size - 1) >> TARGET_PAGE_SHIFT) == (addr >> TARGET_PAGE_SHIFT));

    if (src == NULL)
        goto fail1;

    memcpy(dst, src, size);
    return;

fail1:
    DBG("fail1\n");

    memset(dst, 0xff, size);
}

static void
demu_handle_io(demu_space_t *space, ioreq_t *ioreq, int is_mmio)
{
    if (space == NULL)
        goto fail1;

    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            ioreq->data = (uint64_t)demu_io_read(space, ioreq->addr, ioreq->size);
        } else {
            int i, sign;

            sign = ioreq->df ? -1 : 1;
            for (i = 0; i < ioreq->count; i++) {
                uint32_t    data;
                
                data = demu_io_read(space, ioreq->addr, ioreq->size);

                __copy_to_guest_memory(ioreq->data + (sign * i * ioreq->size),
                                       ioreq->size, (uint8_t *)&data);

                if (is_mmio)
                    ioreq->addr += sign * ioreq->size;
            }
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            demu_io_write(space, ioreq->addr, ioreq->size, (uint32_t)ioreq->data);
        } else {
            int i, sign;

            sign = ioreq->df ? -1 : 1;
            for (i = 0; i < ioreq->count; i++) {
                uint32_t    data;

                __copy_from_guest_memory(ioreq->data + (sign * i * ioreq->size),
                                         ioreq->size, (uint8_t *)&data);

                demu_io_write(space, ioreq->addr, ioreq->size, data);

                if (is_mmio)
                    ioreq->addr += sign * ioreq->size;
            }
        }
    }

    return;

fail1:
    DBG("fail1\n");
}
 
static void
demu_handle_ioreq(ioreq_t *ioreq)
{
    demu_space_t    *space;

    switch (ioreq->type) {
    case IOREQ_TYPE_PIO:
        space = demu_find_port_space(ioreq->addr);
        demu_handle_io(space, ioreq, FALSE);
        break;

    case IOREQ_TYPE_COPY:
        space = demu_find_memory_space(ioreq->addr);
        demu_handle_io(space, ioreq, TRUE);
        break;

    case IOREQ_TYPE_PCI_CONFIG: {
        uint16_t    bdf;

        bdf = (uint16_t)(ioreq->addr >> 8);

        ioreq->addr &= 0xff;
        ioreq->addr += ioreq->size >> 16;
        ioreq->size &= 0xffff;

        space = demu_find_pci_config_space(bdf);
        demu_handle_io(space, ioreq, FALSE);
        break;
    }
    case IOREQ_TYPE_TIMEOFFSET:
        break;

    case IOREQ_TYPE_INVALIDATE:
        mapcache_invalidate();
        break;

    default:
        assert(FALSE);
        break;
    }
}

static void demu_vnc_mouse(int buttonMask, int x, int y, rfbClientPtr client)
{
    rfbDefaultPtrAddEvent(buttonMask, x, y, client);
}

static void demu_vnc_key(rfbBool down, rfbKeySym keySym, rfbClientPtr client)
{
}

static void demu_vnc_remove_client(rfbClientPtr client)
{
    DBG("\n");
}

static enum rfbNewClientAction demu_vnc_add_client(rfbClientPtr client)
{
    DBG("\n");
    client->clientGoneHook = demu_vnc_remove_client;

    return RFB_CLIENT_ACCEPT;
}

void
demu_vnc_new_framebuffer(uint32_t width, uint32_t height, uint32_t depth)
{
    rfbScreenInfoPtr    screen = demu_state.screen;

    if (demu_state.framebuffer != NULL)
        free(demu_state.framebuffer);

    demu_state.framebuffer = malloc(width * height * depth);

    if (demu_state.framebuffer == NULL) {
        DBG("allocation failed: using default framebuffer (%dx%dx%d)\n",
            DEMU_VNC_DEFAULT_WIDTH,
            DEMU_VNC_DEFAULT_HEIGHT,
            DEMU_VNC_DEFAULT_DEPTH);

        rfbNewFramebuffer(screen,
                          (char *)demu_state.default_framebuffer,
                          DEMU_VNC_DEFAULT_WIDTH,
                          DEMU_VNC_DEFAULT_HEIGHT,
                          8, 3,
                          DEMU_VNC_DEFAULT_DEPTH);
    } else {
        DBG("%dx%dx%d\n", width, height, depth);

        rfbNewFramebuffer(screen,
                          (char *)demu_state.framebuffer,
                          width,
                          height,
                          8, 3,
                          depth);
    }
}

uint8_t *
demu_vnc_get_framebuffer(void)
{
    return demu_state.framebuffer;
}   

static int
demu_vnc_initialize(void)
{
    uint8_t             *framebuffer;
    unsigned int        x, y;
    rfbScreenInfoPtr    screen;

    framebuffer = malloc(DEMU_VNC_DEFAULT_WIDTH *
                         DEMU_VNC_DEFAULT_HEIGHT *
                         DEMU_VNC_DEFAULT_DEPTH);
    if (framebuffer == NULL)
        goto fail1;

    for (y = 0; y < DEMU_VNC_DEFAULT_HEIGHT; y++) {
        for (x = 0; x < DEMU_VNC_DEFAULT_WIDTH; x++) {
            uint8_t *pixel = &framebuffer[(y * DEMU_VNC_DEFAULT_WIDTH + x) *
                                          DEMU_VNC_DEFAULT_DEPTH];
            pixel[0] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* RED */
            pixel[1] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* GREEN */
            pixel[2] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* BLUE */
        }
    }   
   
    demu_state.default_framebuffer = framebuffer;

    screen = rfbGetScreen(NULL, NULL,
                          DEMU_VNC_DEFAULT_WIDTH,
                          DEMU_VNC_DEFAULT_HEIGHT,
                          8, 3,
                          DEMU_VNC_DEFAULT_DEPTH);
    if (screen == NULL)
        goto fail2;
 
    screen->frameBuffer = (char *)demu_state.default_framebuffer;
    screen->desktopName = "DEMU";
    screen->alwaysShared = TRUE;
    screen->autoPort = TRUE;
    screen->ptrAddEvent = demu_vnc_mouse;
    screen->kbdAddEvent = demu_vnc_key;
    screen->newClientHook = demu_vnc_add_client;

    rfbInitServer(screen);
    demu_state.screen = screen;
    return 0;

fail2:
    DBG("fail2\n");
    rfbScreenCleanup(screen);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

static void
demu_vnc_teardown(void)
{
    rfbScreenInfoPtr    screen = demu_state.screen;

    rfbShutdownServer(screen, TRUE);
    rfbScreenCleanup(screen);

    if (demu_state.framebuffer != NULL)
        free(demu_state.framebuffer);
 
    free(demu_state.default_framebuffer);
}

static void
demu_seq_next(void)
{
    assert(demu_state.seq < DEMU_SEQ_INITIALIZED);

    switch (++demu_state.seq) {
    case DEMU_SEQ_INTERFACE_OPEN:
        DBG(">INTERFACE_OPEN\n");
        break;

    case DEMU_SEQ_SERVER_REGISTERED:
        DBG(">SERVER_REGISTERED\n");
        DBG("ioservid = %u\n", demu_state.ioservid);
        break;

    case DEMU_SEQ_SHARED_IOPAGE_MAPPED:
        DBG(">SHARED_IOPAGE_MAPPED\n");
        DBG("shared_iopage = %p\n", demu_state.shared_iopage);
        break;

    case DEMU_SEQ_BUFFERED_IOPAGE_MAPPED:
        DBG(">BUFFERED_IOPAGE_MAPPED\n");
        DBG("buffered_iopage = %p\n", demu_state.buffered_iopage);
        break;

    case DEMU_SEQ_PORT_ARRAY_ALLOCATED:
        DBG(">PORT_ARRAY_ALLOCATED\n");
        break;

    case DEMU_SEQ_EVTCHN_OPEN:
        DBG(">EVTCHN_OPEN\n");
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

    case DEMU_SEQ_VNC_INITIALIZED:
        DBG(">VNC_INITIALIZED\n");
        break;

    case DEMU_SEQ_INITIALIZED:
        DBG(">INITIALIZED\n");
        break;

    default:
        assert(FALSE);
        break;
    }
}

static void
demu_teardown(void)
{
    if (demu_state.seq == DEMU_SEQ_INITIALIZED) {
        DBG("<INITIALIZED\n");
        device_teardown();

        demu_state.seq = DEMU_SEQ_VNC_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_VNC_INITIALIZED) {
        DBG("<VNC_INITIALIZED\n");
        demu_vnc_teardown();

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_BUF_PORT_BOUND\n");
        evtchn_port_t   port;

        port = demu_state.buf_ioreq_local_port;

        DBG("%u\n", port);
        (void) xc_evtchn_unbind(demu_state.xceh, port);

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_PORTS_BOUND\n");

        demu_state.seq = DEMU_SEQ_EVTCHN_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_EVTCHN_OPEN) {
        int i;

        DBG("<EVTCHN_OPEN\n");

        for (i = 0; i < demu_state.vcpus; i++) {
            evtchn_port_t   port;

            port = demu_state.ioreq_local_port[i];

            if (port >= 0) {
                DBG("VCPU%d: %u\n", i, port);
                (void) xc_evtchn_unbind(demu_state.xceh, port);
            }
        }

        xc_evtchn_close(demu_state.xceh);

        demu_state.seq = DEMU_SEQ_PORT_ARRAY_ALLOCATED;
    }

    if (demu_state.seq >= DEMU_SEQ_PORT_ARRAY_ALLOCATED) {
        DBG("<PORT_ARRAY_ALLOCATED\n");

        free(demu_state.ioreq_local_port);

        demu_state.seq = DEMU_SEQ_BUFFERED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_BUFFERED_IOPAGE_MAPPED) {
        DBG("<BUFFERED_IOPAGE_MAPPED\n");

        munmap(demu_state.buffered_iopage, XC_PAGE_SIZE);

        demu_state.seq = DEMU_SEQ_SHARED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_SHARED_IOPAGE_MAPPED) {
        DBG("<SHARED_IOPAGE_MAPPED\n");

        munmap(demu_state.shared_iopage, XC_PAGE_SIZE);

        demu_state.seq = DEMU_SEQ_SERVER_REGISTERED;
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_REGISTERED) {
        DBG("<SERVER_REGISTERED\n");

        (void) xc_hvm_destroy_ioreq_server(demu_state.xch,
                                           demu_state.domid,
                                           demu_state.ioservid);
        demu_state.seq = DEMU_SEQ_INTERFACE_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_INTERFACE_OPEN) {
        DBG("<INTERFACE_OPEN\n");

        xc_interface_close(demu_state.xch);

        demu_state.seq = DEMU_SEQ_UNINITIALIZED;
    }
}

static struct sigaction sigterm_handler;

static void
demu_sigterm(int num)
{
    DBG("%s\n", strsignal(num));

    demu_teardown();

    exit(0);
}

static struct sigaction sigusr1_handler;

static void
demu_sigusr1(int num)
{
    DBG("%s\n", strsignal(num));

    sigaction(SIGHUP, &sigusr1_handler, NULL);

    pci_device_dump();
}

static int
demu_initialize(domid_t domid, unsigned int bus, unsigned int device, unsigned int function)
{
    int             rc;
    xc_dominfo_t    dominfo;
    unsigned long   pfn;
    unsigned long   buf_pfn;
    evtchn_port_t   port;
    evtchn_port_t   buf_port;
    int             i;

    demu_state.domid = domid;

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL)
        goto fail1;

    demu_seq_next();

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid)
        goto fail2;

    demu_state.vcpus = dominfo.max_vcpu_id + 1;

    DBG("%d vCPU(s)\n", demu_state.vcpus);

    rc = xc_hvm_create_ioreq_server(demu_state.xch, demu_state.domid, &demu_state.ioservid);
    if (rc < 0)
        goto fail3;
    
    demu_seq_next();

    rc = xc_hvm_get_ioreq_server_info(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, &pfn, &buf_pfn, &buf_port);
    if (rc < 0)
        goto fail4;

    demu_state.shared_iopage = xc_map_foreign_range(demu_state.xch,
                                                    demu_state.domid,
                                                    XC_PAGE_SIZE,
                                                    PROT_READ | PROT_WRITE,
                                                    pfn);
    if (demu_state.shared_iopage == NULL)
        goto fail5;

    demu_seq_next();

    demu_state.buffered_iopage = xc_map_foreign_range(demu_state.xch,
                                                      demu_state.domid,
                                                      XC_PAGE_SIZE,
                                                      PROT_READ | PROT_WRITE,
                                                      buf_pfn);
    if (demu_state.buffered_iopage == NULL)
        goto fail6;

    demu_seq_next();

    demu_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL)
        goto fail7;

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next();

    demu_state.xceh = xc_evtchn_open(NULL, 0);
    if (demu_state.xceh == NULL)
        goto fail8;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail9;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

    rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail10;

    demu_state.buf_ioreq_local_port = rc;

    demu_seq_next();

    rc = demu_vnc_initialize();
    if (rc < 0)
        goto fail11;

    demu_seq_next();

    rc = device_initialize(bus, device, function, 0x01000000);
    if (rc < 0)
        goto fail12;

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

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
        mb();

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
            mb();
        }

        demu_state.buffered_iopage->read_pointer = read_pointer;
        mb();
    }
}

static void
demu_poll_shared_iopage(unsigned int i)
{
    ioreq_t *ioreq;

    ioreq = &demu_state.shared_iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY)
        return;

    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
    mb();

    ioreq->state = STATE_IORESP_READY;
    mb();

    xc_evtchn_notify(demu_state.xceh, demu_state.ioreq_local_port[i]);
}

static void
demu_poll_iopages(void)
{
    evtchn_port_t   port;
    int             i;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    port = xc_evtchn_pending(demu_state.xceh);
    if (port < 0)
        return;

    if (port == demu_state.buf_ioreq_local_port) {
        xc_evtchn_unmask(demu_state.xceh, port);
        demu_poll_buffered_iopage();
    } else {
        for (i = 0; i < demu_state.vcpus; i++) {
            if (port == demu_state.ioreq_local_port[i]) {
                xc_evtchn_unmask(demu_state.xceh, port);
                demu_poll_shared_iopage(i);
            }
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    char            *domain_str;
    char            *device_str;
    int             index;
    char            *end;
    domid_t         domid;
    unsigned int    device;
    sigset_t        block;
    int             efd;
    int             rc;

    prog = basename(argv[0]);

    domain_str = NULL;
    device_str = NULL;

    for (;;) {
        char    c;

        c = getopt_long(argc, argv, "", demu_option, &index);
        if (c == -1)
            break;

        DBG("--%s = '%s'\n", demu_option[index].name, optarg);

        assert(c == 0);
        switch (index) {
        case DEMU_OPT_DOMAIN:
            domain_str = optarg;
            break;

        case DEMU_OPT_DEVICE:
            device_str = optarg;
            break;

        default:
            assert(FALSE);
            break;
        }
    }

    if (domain_str == NULL ||
        device_str == NULL) {
        usage();
        /*NOTREACHED*/
    }

    domid = (domid_t)strtol(domain_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid domain '%s'\n", domain_str);
        exit(1);
    }

    device = (unsigned int)strtol(device_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid device '%s'\n", device_str);
        exit(1);
    }

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

    memset(&sigusr1_handler, 0, sizeof (struct sigaction));
    sigusr1_handler.sa_handler = demu_sigusr1;

    sigaction(SIGUSR1, &sigusr1_handler, NULL);
    sigdelset(&block, SIGUSR1);

    sigprocmask(SIG_BLOCK, &block, NULL);

    rc = demu_initialize(domid, 0, device, 0);
    if (rc < 0) {
        demu_teardown();
        exit(1);
    }

    efd = xc_evtchn_fd(demu_state.xceh);

    for (;;) {
        fd_set          rfds;
        fd_set          wfds;
        fd_set          xfds;
        int             nfds;
        struct timeval  tv;

        FD_ZERO(&wfds);
        FD_ZERO(&xfds);

        rfds = demu_state.screen->allFds;
        FD_SET(efd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = demu_state.screen->deferUpdateTime * 1000;

        nfds = max(demu_state.screen->maxFd, efd) + 1;
        rc = select(nfds, &rfds, &wfds, &xfds, &tv);

        if (rc > 0) {
            if (FD_ISSET(efd, &rfds))
                demu_poll_iopages();

            if (rfbIsActive(demu_state.screen))
                rfbProcessEvents(demu_state.screen, 0);
        }

        if (rc < 0 && errno != EINTR)
            break;
    }

    demu_teardown();

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