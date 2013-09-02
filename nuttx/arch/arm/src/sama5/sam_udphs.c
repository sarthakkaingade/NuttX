/****************************************************************************
 * arch/arm/src/sama5/sam_udphs.c
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.orgr>
 *
 * References:
 *   SAMA5D3 Series Data Sheet
 *   Atmel NoOS sample code.
 *
 * The Atmel sample code has a BSD compatibile license that requires this
 * copyright notice:
 *
 *   Copyright (c) 2009, Atmel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX, Atmel, nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/usbdev_trace.h>

#include <arch/irq.h>

#include "up_arch.h"
#include "up_internal.h"
#include "cache.h"

#include "sam_periphclks.h"
#include "sam_memories.h"
#include "chip/sam_udphs.h"
#include "sam_udphs.h"

#if defined(CONFIG_USBDEV) && defined(CONFIG_SAMA5_UDPHS)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

#ifndef CONFIG_USBDEV_EP0_MAXSIZE
#  define CONFIG_USBDEV_EP0_MAXSIZE 64
#endif

/* Number of DMA transfer descriptors.  Default: 8 */

#ifndef CONFIG_SAMA5_UDPHS_NDTDS
#  define CONFIG_SAMA5_UDPHS_NDTDS 8
#endif

/* Extremely detailed register debug that you would normally never want
 * enabled.
 */

#ifndef CONFIG_DEBUG
#  undef CONFIG_SAMA5_UDPHS_REGDEBUG
#endif

/* Not yet supported */

#undef CONFIG_SAMA5_UDPHS_SCATTERGATHER

/* Driver Definitions *******************************************************/
/* Initial interrupt mask: Reset + Suspend + Correct Transfer */

#define SAM_CNTR_SETUP     (USB_CNTR_RESETM|USB_CNTR_SUSPM|USB_CNTR_CTRM)

/* Endpoint definitions */

#define EP0                 (0)
#define SAM_EPSET_ALL       (0xffff)  /* All endpoints */
#define SAM_EPSET_NOTEP0    (0xfffe)  /* All endpoints except EP0 */
#define SAM_EPSET_DMA       (0x00fe)  /* All endpoints that support DMA transfers */
#define SAM_EP_BIT(ep)      (1 << (ep))
#define SAM_EP0_MAXPACKET   (64)      /* EP0 Max. packet size */

/* DMA FIFO */

#define DMA_MAX_FIFO_SIZE   (65536/1) /* Max size of the FMA FIFO */
#define EPT_VIRTUAL_SIZE    16384     /* FIFO space size in units of 32-bit words */

/* USB-related masks */

#define REQRECIPIENT_MASK     (USB_REQ_TYPE_MASK | USB_REQ_RECIPIENT_MASK)

/* Endpoint register masks (handling toggle fields) */

#define EPR_NOTOG_MASK        (USB_EPR_CTR_RX  | USB_EPR_SETUP  | USB_EPR_EPTYPE_MASK |\
                               USB_EPR_EP_KIND | USB_EPR_CTR_TX | USB_EPR_EA_MASK)
#define EPR_TXDTOG_MASK       (USB_EPR_STATTX_MASK | EPR_NOTOG_MASK)
#define EPR_RXDTOG_MASK       (USB_EPR_STATRX_MASK | EPR_NOTOG_MASK)

/* Request queue operations *************************************************/

#define sam_rqempty(ep)     ((ep)->head == NULL)
#define sam_rqpeek(ep)      ((ep)->head)

/* USB trace ****************************************************************/
/* Trace error codes */

#define SAM_TRACEERR_ALLOCFAIL            0x0001
#define SAM_TRACEERR_BADCLEARFEATURE      0x0002
#define SAM_TRACEERR_BADDEVGETSTATUS      0x0003
#define SAM_TRACEERR_BADEPGETSTATUS       0x0004
#define SAM_TRACEERR_BADEPNO              0x0005
#define SAM_TRACEERR_BADEPTYPE            0x0006
#define SAM_TRACEERR_BADGETCONFIG         0x0007
#define SAM_TRACEERR_BADGETSETDESC        0x0008
#define SAM_TRACEERR_BADGETSTATUS         0x0009
#define SAM_TRACEERR_BADSETADDRESS        0x000a
#define SAM_TRACEERR_BADSETCONFIG         0x000b
#define SAM_TRACEERR_BADSETFEATURE        0x000c
#define SAM_TRACEERR_BINDFAILED           0x000d
#define SAM_TRACEERR_DISPATCHSTALL        0x000e
#define SAM_TRACEERR_DRIVER               0x000f
#define SAM_TRACEERR_DRIVERREGISTERED     0x0010
#define SAM_TRACEERR_EP0SETUPSTALLED      0x0011
#define SAM_TRACEERR_EPINBUSY             0x0012
#define SAM_TRACEERR_EPOUTNULLPACKET      0x0013
#define SAM_TRACEERR_EPRESERVE            0x0014
#define SAM_TRACEERR_EPTCFGMAPD           0x0015
#define SAM_TRACEERR_INVALIDCTRLREQ       0x0016
#define SAM_TRACEERR_INVALIDPARMS         0x0017
#define SAM_TRACEERR_IRQREGISTRATION      0x0018
#define SAM_TRACEERR_NOTCONFIGURED        0x0019
#define SAM_TRACEERR_REQABORTED           0x001a
#define SAM_TRACEERR_TXRDYERR             0x001b

/* Trace interrupt codes */

#define SAM_TRACEINTID_ADDRESSED          0x0001
#define SAM_TRACEINTID_CLEARFEATURE       0x0002
#define SAM_TRACEINTID_DETSUSPD           0x0003
#define SAM_TRACEINTID_DEVGETSTATUS       0x0004
#define SAM_TRACEINTID_DISPATCH           0x0005
#define SAM_TRACEINTID_DMA                0x0006
#define SAM_TRACEINTID_DMAEOB             0x0007
#define SAM_TRACEINTID_DMAEOC             0x0008
#define SAM_TRACEINTID_DMAERR             0x0009
#define SAM_TRACEINTID_ENDRESET           0x000a
#define SAM_TRACEINTID_EP                 0x000b
#define SAM_TRACEINTID_EP0SETUPIN         0x000c
#define SAM_TRACEINTID_EP0SETUPOUT        0x000d
#define SAM_TRACEINTID_EP0SETUPSETADDRESS 0x000e
#define SAM_TRACEINTID_EPGETSTATUS        0x000f
#define SAM_TRACEINTID_EPINDONE           0x0010
#define SAM_TRACEINTID_EPINQEMPTY         0x0011
#define SAM_TRACEINTID_EPOUTQEMPTY        0x0012
#define SAM_TRACEINTID_GETCONFIG          0x0013
#define SAM_TRACEINTID_GETSETDESC         0x0014
#define SAM_TRACEINTID_GETSETIF           0x0015
#define SAM_TRACEINTID_GETSTATUS          0x0016
#define SAM_TRACEINTID_IFGETSTATUS        0x0017
#define SAM_TRACEINTID_INTERRUPT          0x0018
#define SAM_TRACEINTID_INTSOF             0x0019
#define SAM_TRACEINTID_NOSTDREQ           0x001a
#define SAM_TRACEINTID_RXRDY              0x001b
#define SAM_TRACEINTID_RXSETUP            0x001c
#define SAM_TRACEINTID_SETADDRESS         0x001d
#define SAM_TRACEINTID_SETCONFIG          0x001e
#define SAM_TRACEINTID_SETFEATURE         0x001f
#define SAM_TRACEINTID_STALLSNT           0x0020
#define SAM_TRACEINTID_SYNCHFRAME         0x0021
#define SAM_TRACEINTID_TXRDY              0x0022
#define SAM_TRACEINTID_UPSTRRES           0x0023
#define SAM_TRACEINTID_WAKEUP             0x0024

/* Ever-present MIN and MAX macros */

#ifndef MIN
#  define MIN(a,b) (a < b ? a : b)
#endif

#ifndef MAX
#  define MAX(a,b) (a > b ? a : b)
#endif

/* Byte ordering in host-based values */

#ifdef CONFIG_ENDIAN_BIG
#  define LSB 1
#  define MSB 0
#else
#  define LSB 0
#  define MSB 1
#endif

/****************************************************************************
 * Private Type Definitions
 ****************************************************************************/
/* State of an endpoint */

enum sam_epstate_e
{
                              /* --- All Endpoints --- */
  UDPHS_EPSTATE_DISABLED = 0, /* Endpoint is disabled */
  UDPHS_EPSTATE_STALLED,      /* Endpoint is stalled */
  UDPHS_EPSTATE_IDLE,         /* Endpoint is idle (i.e. ready for transmission) */
  UDPHS_EPSTATE_SENDING,      /* Endpoint is sending data */
  UDPHS_EPSTATE_RECEIVING,    /* Endpoint is receiving data */
                              /* --- Endpoint 0 Only --- */
  UDPHS_EPSTATE_EP0DATAOUT,   /* Endpoint 0 is receiving SETUP OUT data */
  UDPHS_EPSTATE_EP0STATUSIN,  /* Endpoint 0 is sending SETUP status */
  UDPHS_EPSTATE_EP0ADDRESS    /* Address change is pending completion of status */
};

/* The overall state of the device */

enum sam_devstate_e
{
  UDPHS_DEVSTATE_SUSPENDED = 0, /* The device is currently suspended */
  UDPHS_DEVSTATE_ATTACHED,      /* USB cable is plugged into the device */
  UDPHS_DEVSTATE_POWERED,       /* Host is providing +5V through the USB cable */
  UDPHS_DEVSTATE_DEFAULT,       /* Device has been reset */
  UDPHS_DEVSTATE_ADDRESS,       /* The device has been given an address on the bus */
  UDPHS_DEVSTATE_CONFIGURED     /* A valid configuration has been selected. */
};

/* The result of EP0 SETUP processing */

enum sam_ep0setup_e
{
  UDPHS_EP0SETUP_SUCCESS = 0,   /* The SETUP was handle without incident */
  UDPHS_EP0SETUP_DISPATCHED,    /* The SETUP was forwarded to the class driver */
  UDPHS_EP0SETUP_ADDRESS,       /* A new device address is pending */
  UDPHS_EP0SETUP_STALL          /* An error occurred */
};

/* DMA transfer descriptor */

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
struct sam_dtd_s
{
  struct udphs_dtd_s hw;     /* These are the fields as seen by the hardware */
  uint32_t pad;              /* Pad to 16 bytes to support arrays of descriptors */
};
#define SIZEOF_SAM_DTD_S 16
#endif

/* The following is used to manage lists of free DMA transfer descriptors */

struct sam_list_s
{
  struct sam_list_s *flink;  /* Link to next entry in the list */
                             /* Variable length entry data follows */
};

union wb_u
{
  uint16_t w;
  uint8_t  b[2];
};

/* A container for a request so that the request make be retained in a list */

struct sam_req_s
{
  struct usbdev_req_s  req;          /* Standard USB request */
  struct sam_req_s    *flink;        /* Supports a singly linked list */
  uint16_t             inflight;     /* Number of TX bytes written to FIFO */
};

/* This is the internal representation of an endpoint */

struct sam_ep_s
{
  /* Common endpoint fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbdev_ep_s
   * to struct sam_ep_s.
   */

  struct usbdev_ep_s   ep;           /* Standard endpoint structure */

  /* SAMA5-specific fields */

  struct sam_usbdev_s *dev;          /* Reference to private driver data */
  struct sam_req_s    *head;         /* Request list for this endpoint */
  struct sam_req_s    *tail;
#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
  struct sam_dtd_s    *dtdll;        /* Head of the DMA transfer descriptor list */
#endif
  volatile uint8_t     epstate;      /* State of the endpoint (see enum sam_epstate_e) */
  volatile uint8_t     bank;         /* Current reception bank (0 or 1) */
  uint8_t              stalled:1;    /* true: Endpoint is stalled */
  uint8_t              halted:1;     /* true: Endpoint feature halted */
  uint8_t              txnullpkt:1;  /* Null packet needed at end of transfer */
};

struct sam_usbdev_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbdev_s
   * to structsam_usbdev_s.
   */

  struct usbdev_s usbdev;

  /* The bound device class driver */

  struct usbdevclass_driver_s *driver;

  /* UDPHS-specific fields */

  struct usb_ctrlreq_s     ctrl;          /* Last EP0 request */
  uint8_t                  devstate;      /* State of the device (see enum sam_devstate_e) */
  uint8_t                  prevstate;     /* Previous state of the device */
  uint8_t                  devaddr;       /* Assigned device address */
  uint8_t                  rxpending:1;   /* 1: OUT data in the FIFO, but no read requests */
  uint8_t                  selfpowered:1; /* 1: Device is self powered */
  uint16_t                 epavail;       /* Bitset of available endpoints */

  /* DMA Transfer descriptors */

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
  struct sam_dtd_s        *tdfree;        /* A list of free transfer descriptors */
#ifndef CONFIG_SAMA5_UDPHS_PREALLOCATE
  struct sam_dtd_s        *tdpool;        /* Pool of allocated DMA transfer descriptors */
#endif
#endif

  /* The endpoint list */

  struct sam_ep_s          eplist[SAM_UDPHS_NENDPOINTS];

  /* EP0 data buffer.  For data that is included in an EP0 SETUP OUT
   * transaction.  In this case, no request is in place from the class
   * driver and the incoming data is caught in this buffer.  The size
   * of valid dat in the buffer is given by ctrlreg.len[].  For the
   * case of EP0 SETUP IN transaction, the normal request mechanism is
   * used and the class driver provides the buffering.
   */

  uint8_t                  ep0out[SAM_EP0_MAXPACKET];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register operations ******************************************************/

#ifdef CONFIG_SAMA5_UDPHS_REGDEBUG
static void   sam_printreg(uintptr_t regaddr, uint32_t regval, bool iswrite);
static void   sam_checkreg(uintptr_t regaddr, uint32_t regval, bool iswrite);
static uint32_t sam_getreg(uintptr_t regaddr);
static void   sam_putreg(uint32_t regval, uintptr_t regaddr);
static void   sam_dumpep(struct sam_usbdev_s *priv, int epno);
#else
static inline uint32_t sam_getreg(uintptr_t regaddr);
static inline void sam_putreg(uint32_t regval, uintptr_t regaddr);
# define sam_dumpep(priv,epno)
#endif

/* Suspend/Resume Helpers ***************************************************/

static void   sam_suspend(struct sam_usbdev_s *priv);
static void   sam_resume(struct sam_usbdev_s *priv);

/* DMA/Request Helpers ******************************************************/

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
static struct sam_dtd_s *sam_dtd_alloc(struct sam_usbdev_s *priv);
static void   sam_dtd_free(struct sam_usbdev_s *priv, struct sam_dtd_s *dtd);
#endif
static void   sam_dma_single(uint8_t epno, struct sam_req_s *privreq,
                uint32_t dmacontrol);
static int    sam_req_wrdma(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep, struct sam_req_s *privreq);
static int    sam_req_rddma(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep, struct sam_req_s *privreq);

/* Request Helpers **********************************************************/

static struct sam_req_s *
              sam_req_dequeue(struct sam_ep_s *privep);
static void   sam_req_enqueue(struct sam_ep_s *privep,
                struct sam_req_s *req);
static inline void
              sam_req_abort(struct sam_ep_s *privep,
                struct sam_req_s *privreq, int16_t result);
static void   sam_req_complete(struct sam_ep_s *privep, int16_t result);
static int    sam_req_wrnodma(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep, struct sam_req_s *privreq);
static int    sam_req_write(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep);
static int    sam_req_rdnodma(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep, struct sam_req_s *privreq,
                uint16_t pktsize);
static int    sam_req_read(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep, uint16_t pktsize);
static void   sam_req_cancel(struct sam_ep_s *privep);

/* Interrupt level processing ***********************************************/

static void   sam_ep0_read(uint8_t *buffer, size_t buflen);
static void   sam_ep0_wrstatus(const uint8_t *buffer, size_t buflen);
static void   sam_ep0_dispatch(struct sam_usbdev_s *priv);
static void   sam_setdevaddr(struct sam_usbdev_s *priv, uint8_t value);
static void   sam_ep0_setup(struct sam_usbdev_s *priv);
static void   sam_dma_interrupt(struct sam_usbdev_s *priv, int chan);
static void   sam_ep_interrupt(struct sam_usbdev_s *priv, int epno);
static int    sam_udphs_interrupt(int irq, void *context);

/* Endpoint helpers *********************************************************/

static void   sam_ep_reset(struct sam_usbdev_s *priv, uint8_t epno);
static void   sam_epset_reset(struct sam_usbdev_s *priv, uint16_t epset);
static inline struct sam_ep_s *
              sam_ep_reserve(struct sam_usbdev_s *priv, uint8_t epset);
static inline void
              sam_ep_unreserve(struct sam_usbdev_s *priv,
                struct sam_ep_s *privep);
static inline bool
              sam_ep_reserved(struct sam_usbdev_s *priv, int epno);
static int    sam_ep_configure_internal(struct sam_ep_s *privep,
                const struct usb_epdesc_s *desc);

/* Endpoint operations ******************************************************/

static int    sam_ep_configure(struct usbdev_ep_s *ep,
                const struct usb_epdesc_s *desc, bool last);
static int    sam_ep_disable(struct usbdev_ep_s *ep);
static struct usbdev_req_s *
              sam_ep_allocreq(struct usbdev_ep_s *ep);
static void   sam_ep_freereq(struct usbdev_ep_s *ep,
                struct usbdev_req_s *);
#ifdef CONFIG_USBDEV_DMA
static void  *sam_ep_allocbuffer(struct usbdev_ep_s *ep, uint16_t nbytes);
static void   sam_ep_freebuffer(struct usbdev_ep_s *ep, void *buf);
#endif
static int    sam_ep_submit(struct usbdev_ep_s *ep,
                struct usbdev_req_s *req);
static int    sam_ep_cancel(struct usbdev_ep_s *ep,
                struct usbdev_req_s *req);
static int    sam_ep_stall(struct usbdev_ep_s *ep, bool resume);

/* USB device controller operations *****************************************/

static struct usbdev_ep_s *
              sam_allocep(struct usbdev_s *dev, uint8_t epno, bool in,
                uint8_t eptype);
static void   sam_freeep(struct usbdev_s *dev, struct usbdev_ep_s *ep);
static int    sam_getframe(struct usbdev_s *dev);
static int    sam_wakeup(struct usbdev_s *dev);
static int    sam_selfpowered(struct usbdev_s *dev, bool selfpowered);
static int    sam_pullup(FAR struct usbdev_s *dev,  bool enable);

/* Initialization/Reset *****************************************************/

static void   sam_reset(struct sam_usbdev_s *priv);
static void   sam_hw_setup(struct sam_usbdev_s *priv);
static void   sam_sw_setup(struct sam_usbdev_s *priv);
static void   sam_hw_shutdown(struct sam_usbdev_s *priv);
static void   sam_sw_shutdown(struct sam_usbdev_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Since there is only a single USB interface, all status information can be
 * be simply retained in a single global instance.
 */

static struct sam_usbdev_s g_udphs;

static const struct usbdev_epops_s g_epops =
{
  .configure     = sam_ep_configure,
  .disable       = sam_ep_disable,
  .allocreq      = sam_ep_allocreq,
  .freereq       = sam_ep_freereq,
#ifdef CONFIG_USBDEV_DMA
  .allocbuffer   = sam_ep_allocbuffer,
  .freebuffer    = sam_ep_freebuffer,
#endif
  .submit        = sam_ep_submit,
  .cancel        = sam_ep_cancel,
  .stall         = sam_ep_stall,
};

static const struct usbdev_ops_s g_devops =
{
  .allocep       = sam_allocep,
  .freeep        = sam_freeep,
  .getframe      = sam_getframe,
  .wakeup        = sam_wakeup,
  .selfpowered   = sam_selfpowered,
  .pullup        = sam_pullup,
};

/* This describes endpoint 0 */

static const struct usb_epdesc_s g_ep0desc =
{
  .len           = USB_SIZEOF_EPDESC,
  .type          = USB_DESC_TYPE_ENDPOINT,
  .addr          = EP0,
  .attr          = USB_EP_ATTR_XFER_CONTROL,
  .mxpacketsize  = {64, 0},
  .interval      = 0
};

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
#ifdef CONFIG_SAMA5_UDPHS_PREALLOCATE
/* This is a properly aligned pool of preallocated DMA transfer desciptors */

static struct sam_dtd_s g_dtdpool[CONFIG_SAMA5_UDPHS_NDTDS]
                        __attribute__ ((aligned(16)));
#endif
#endif


/* Device error strings that may be enabled for more desciptive USB trace
 * output.
 */

#ifdef CONFIG_USBDEV_TRACE_STRINGS
const struct trace_msg_t g_usb_trace_strings_deverror[] =
{
  TRACE_STR(SAM_TRACEERR_ALLOCFAIL),
  TRACE_STR(SAM_TRACEERR_BADCLEARFEATURE),
  TRACE_STR(SAM_TRACEERR_BADDEVGETSTATUS),
  TRACE_STR(SAM_TRACEERR_BADEPGETSTATUS),
  TRACE_STR(SAM_TRACEERR_BADEPNO),
  TRACE_STR(SAM_TRACEERR_BADEPTYPE),
  TRACE_STR(SAM_TRACEERR_BADGETCONFIG),
  TRACE_STR(SAM_TRACEERR_BADGETSETDESC),
  TRACE_STR(SAM_TRACEERR_BADGETSTATUS),
  TRACE_STR(SAM_TRACEERR_BADSETADDRESS),
  TRACE_STR(SAM_TRACEERR_BADSETCONFIG),
  TRACE_STR(SAM_TRACEERR_BADSETFEATURE),
  TRACE_STR(SAM_TRACEERR_BINDFAILED),
  TRACE_STR(SAM_TRACEERR_DISPATCHSTALL),
  TRACE_STR(SAM_TRACEERR_DRIVER),
  TRACE_STR(SAM_TRACEERR_DRIVERREGISTERED),
  TRACE_STR(SAM_TRACEERR_EP0SETUPSTALLED),
  TRACE_STR(SAM_TRACEERR_EPINBUSY),
  TRACE_STR(SAM_TRACEERR_EPOUTNULLPACKET),
  TRACE_STR(SAM_TRACEERR_EPRESERVE),
  TRACE_STR(SAM_TRACEERR_EPTCFGMAPD),
  TRACE_STR(SAM_TRACEERR_INVALIDCTRLREQ),
  TRACE_STR(SAM_TRACEERR_INVALIDPARMS),
  TRACE_STR(SAM_TRACEERR_IRQREGISTRATION),
  TRACE_STR(SAM_TRACEERR_NOTCONFIGURED),
  TRACE_STR(SAM_TRACEERR_REQABORTED),
  TRACE_STR(SAM_TRACEERR_TXRDYERR),
  TRACE_STR_END
};
#endif

/* Interrupt event strings that may be enabled for more desciptive USB trace
 * output.
 */

#ifdef CONFIG_USBDEV_TRACE_STRINGS
const struct trace_msg_t g_usb_trace_strings_intdecode[] =
{
  TRACE_STR(SAM_TRACEINTID_ADDRESSED),
  TRACE_STR(SAM_TRACEINTID_CLEARFEATURE),
  TRACE_STR(SAM_TRACEINTID_DETSUSPD),
  TRACE_STR(SAM_TRACEINTID_DEVGETSTATUS),
  TRACE_STR(SAM_TRACEINTID_DISPATCH),
  TRACE_STR(SAM_TRACEINTID_DMA),
  TRACE_STR(SAM_TRACEINTID_DMAEOB),
  TRACE_STR(SAM_TRACEINTID_DMAEOC),
  TRACE_STR(SAM_TRACEINTID_DMAERR),
  TRACE_STR(SAM_TRACEINTID_ENDRESET),
  TRACE_STR(SAM_TRACEINTID_EP),
  TRACE_STR(SAM_TRACEINTID_EP0SETUPIN),
  TRACE_STR(SAM_TRACEINTID_EP0SETUPOUT),
  TRACE_STR(SAM_TRACEINTID_EP0SETUPSETADDRESS),
  TRACE_STR(SAM_TRACEINTID_EPGETSTATUS),
  TRACE_STR(SAM_TRACEINTID_EPINDONE),
  TRACE_STR(SAM_TRACEINTID_EPINQEMPTY),
  TRACE_STR(SAM_TRACEINTID_EPOUTQEMPTY),
  TRACE_STR(SAM_TRACEINTID_GETCONFIG),
  TRACE_STR(SAM_TRACEINTID_GETSETDESC),
  TRACE_STR(SAM_TRACEINTID_GETSETIF),
  TRACE_STR(SAM_TRACEINTID_GETSTATUS),
  TRACE_STR(SAM_TRACEINTID_IFGETSTATUS),
  TRACE_STR(SAM_TRACEINTID_INTERRUPT),
  TRACE_STR(SAM_TRACEINTID_INTSOF),
  TRACE_STR(SAM_TRACEINTID_NOSTDREQ),
  TRACE_STR(SAM_TRACEINTID_RXRDY),
  TRACE_STR(SAM_TRACEINTID_RXSETUP),
  TRACE_STR(SAM_TRACEINTID_SETADDRESS),
  TRACE_STR(SAM_TRACEINTID_SETCONFIG),
  TRACE_STR(SAM_TRACEINTID_SETFEATURE),
  TRACE_STR(SAM_TRACEINTID_STALLSNT),
  TRACE_STR(SAM_TRACEINTID_SYNCHFRAME),
  TRACE_STR(SAM_TRACEINTID_TXRDY),
  TRACE_STR(SAM_TRACEINTID_UPSTRRES),
  TRACE_STR(SAM_TRACEINTID_WAKEUP),
  TRACE_STR_END
};
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Private Private Functions
 ****************************************************************************/

/****************************************************************************
 * Register Operations
 ****************************************************************************/
/*******************************************************************************
 * Name: sam_printreg
 *
 * Description:
 *   Print the contents of a SAMA5 EHCI register
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_REGDEBUG
static void sam_printreg(uintptr_t regaddr, uint32_t regval, bool iswrite)
{
  lldbg("%p%s%08x\n", regaddr, iswrite ? "<-" : "->", regval);
}
#endif

/*******************************************************************************
 * Name: sam_checkreg
 *
 * Description:
 *   Check if it is time to output debug information for accesses to a SAMA5
 *   EHCI register
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_REGDEBUG
static void sam_checkreg(uintptr_t regaddr, uint32_t regval, bool iswrite)
{
  static uintptr_t prevaddr  = 0;
  static uint32_t  preval    = 0;
  static uint32_t  count     = 0;
  static bool      prevwrite = false;

  /* Is this the same value that we read from/wrote to the same register last time?
   * Are we polling the register?  If so, suppress the output.
   */

  if (regaddr == prevaddr && regval == preval && prevwrite == iswrite)
    {
      /* Yes.. Just increment the count */

      count++;
    }
  else
    {
      /* No this is a new address or value or operation. Were there any
       * duplicate accesses before this one?
       */

      if (count > 0)
        {
          /* Yes.. Just one? */

          if (count == 1)
            {
              /* Yes.. Just one */

              sam_printreg(prevaddr, preval, prevwrite);
            }
          else
            {
              /* No.. More than one. */

              lldbg("[repeats %d more times]\n", count);
            }
        }

      /* Save the new address, value, count, and operation for next time */

      prevaddr  = regaddr;
      preval    = regval;
      count     = 0;
      prevwrite = iswrite;

      /* Show the new register access */

      sam_printreg(regaddr, regval, iswrite);
    }
}
#endif

/*******************************************************************************
 * Name: sam_getreg
 *
 * Description:
 *   Get the contents of an SAMA5 register
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_REGDEBUG
static uint32_t sam_getreg(uintptr_t regaddr)
{
  /* Read the value from the register */

  uint32_t regval = getreg32(regaddr);

  /* Check if we need to print this value */

  sam_checkreg(regaddr, regval, false);
  return regval;
}
#else
static inline uint32_t sam_getreg(uintptr_t regaddr)
{
  return getreg32(regaddr;
}
#endif

/*******************************************************************************
 * Name: sam_putreg
 *
 * Description:
 *   Set the contents of an SAMA5 register to a value
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_REGDEBUG
static void sam_putreg(uint32_t regval, uintptr_t regaddr)
{
  /* Check if we need to print this value */

  sam_checkreg(regaddr, regval, true);

  /* Write the value */

  putreg32(regval, regaddr);
}
#else
static inline void sam_putreg(uint32_t regval, uint32_t regaddr)
{
  putreg32(regval, regaddr);
}
#endif

/****************************************************************************
 * Name: sam_dumpep
 ****************************************************************************/

#if defined(CONFIG_SAMA5_UDPHS_REGDEBUG) && defined(CONFIG_DEBUG)
static void sam_dumpep(struct sam_usbdev_s *priv, int epno)
{
  /* Global Registers */

  lldbg("Global Register:\n");
  lldbg("  CTRL:    %04x\n", sam_getreg(SAM_UDPHS_CTRL));
  lldbg("  FNUM:    %04x\n", sam_getreg(SAM_UDPHS_FNUM));
  lldbg("  IEN:     %04x\n", sam_getreg(SAM_UDPHS_IEN));
  lldbg("  INSTA:   %04x\n", sam_getreg(SAM_UDPHS_INTSTA));
  lldbg("  TST:     %04x\n", sam_getreg(SAM_UDPHS_TST));

  /* Endpoint registers */

  lldbg("Endpoint %d Register:\n", epno);
  lldbg("  CFG:     %04x\n", sam_getreg(SAM_UDPHS_EPTCFG(epno)));
  lldbg("  CTL:     %04x\n", sam_getreg(SAM_UDPHS_EPTCTL(epno)));
  lldbg("  STA:     %04x\n", sam_getreg(SAM_UDPHS_EPTSTA(epno)));

  lldbg("DMA %d Register:\n", epno);
  if ((SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0)
    {
      lldbg("  NXTDSC:  %04x\n", sam_getreg(SAM_UDPHS_DMANXTDSC(epno)));
      lldbg("  ADDRESS: %04x\n", sam_getreg(SAM_UDPHS_DMAADDRESS(epno)));
      lldbg("  CONTROL: %04x\n", sam_getreg(SAM_UDPHS_DMACONTROL(epno)));
      lldbg("  STATUS:  %04x\n", sam_getreg(SAM_UDPHS_DMASTATUS(epno)));
    }
  else
    {
      lldbg("  None\n");
    }
}
#endif

/****************************************************************************
 * DMA
 ****************************************************************************/
/****************************************************************************
 * Name: sam_dtd_alloc
 *
 * Description:
 *   Allocate a DMA transfer descriptor by removing it from the free list
 *
 * Assumption:  Caller holds the exclsem
 *
 ****************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
static struct sam_dtd_s *sam_dtd_alloc(struct sam_usbdev_s *priv)
{
  struct sam_dtd_s *dtd;

  /* Remove the DMA transfer descriptor from the freelist */

  dtd = (struct sam_dtd_s *)g_udphs.dtdfree;
  if (dtd)
    {
      g_udphs.dtdfree = ((struct sam_list_s *)dtd)->flink;
      memset(dtd, 0, sizeof(struct sam_dtd_s));
    }

  return dtd;
}
#endif

/****************************************************************************
 * Name: sam_dtd_free
 *
 * Description:
 *   Free a DMA transfer descriptor by returning it to the free list
 *
 * Assumption:  Caller holds the exclsem
 *
 ****************************************************************************/

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
static void sam_dtd_free(struct sam_usbdev_s *priv, struct sam_dtd_s *dtd)
{
  struct sam_list_s *entry = (struct sam_list_s *)dtd;

  /* Put the dtd structure back into the free list */

  entry->flink  = g_udphs.dtdfree;
  g_udphs.dtdfree = entry;
}
#endif

/****************************************************************************
 * Name: sam_dma_single
 *
 * Description:
 *   Setup a start a single buffer DMA.
 *
 * Assumption:  Called as part of UDPHS interrupt handling
 *
 ****************************************************************************/

static void sam_dma_single(uint8_t epno, struct sam_req_s *privreq,
                           uint32_t dmacontrol)
{
  uintptr_t buffer;
  uintptr_t physaddr;
  uint32_t regval;

  /* Not all endpoints support DMA */

  DEBUGASSERT((SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0);

  /* Flush the contents of the DMA buffer to RAM */

  buffer = (uintptr_t)&privreq->req.buf[privreq->req.xfrd];
  cp15_clean_dcache(buffer, buffer + privreq->inflight);

  /* Set up the DMA */

  physaddr = sam_physramaddr(buffer);
  sam_putreg(physaddr, SAM_UDPHS_DMAADDRESS(epno));

  /* Clear any pending interrupts then enable the DMA interrupt */

  (void)sam_getreg(SAM_UDPHS_DMASTATUS(epno));
  regval  = sam_getreg(SAM_UDPHS_IEN);
  regval |= UDPHS_INT_DMA(epno);
  sam_putreg(regval, SAM_UDPHS_IEN);

  /* Setup and enable the DMA */

  sam_putreg(0, SAM_UDPHS_DMACONTROL(epno));

  dmacontrol |= UDPHS_DMACONTROL_BUFLEN(privreq->inflight);
  sam_putreg(dmacontrol, SAM_UDPHS_DMACONTROL(epno));
}

/****************************************************************************
 * Name: sam_req_wrdma
 *
 * Description:
 *   Process the next queued write request for an endpoint that supports DMA.
 *
 ****************************************************************************/

static int sam_req_wrdma(struct sam_usbdev_s *priv, struct sam_ep_s *privep,
                         struct sam_req_s *privreq)
{
  uint32_t regval;
  int remaining;
  int epno;

  /* The endpoint must be IDLE and ready to begin the next transfer */

  if (privep->epstate != UDPHS_EPSTATE_IDLE)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EPINBUSY), privep->epstate);
      return -EBUSY;
    }

  /* Switch to the sending state */

  privep->epstate   = UDPHS_EPSTATE_SENDING;
  privreq->inflight = 0;

  /* Get the endpoint number */

  epno = USB_EPNO(privep->ep.eplog);

  /* How many bytes remain to be transferred in the request? */

  remaining = privreq->req.len - privreq->req.xfrd - privreq->inflight;

  /* If there are no bytes to send, then send a null packet */

  if (remaining > 0)
    {
      /* Clip the transfer to the size of the DMA FIFO */

#if USBDEV_MAXREQUEUST > DMA_MAX_FIFO_SIZE
      if (remaining > DMA_MAX_FIFO_SIZE)
        {
          privreq->inflight = DMA_MAX_FIFO_SIZE;
        }
      else
#endif
        {
          privreq->inflight = remaining;
        }

      /* Single transfer */

      sam_dma_single(epno, privreq, UDPHS_DMACONTROL_ENDBEN
                      | UDPHS_DMACONTROL_ENDBUFFIT
                      | UDPHS_DMACONTROL_CHANNENB);
      return OK;
    }

  /* Enable the endpoint interrupt */

  regval  = sam_getreg(SAM_UDPHS_IEN);
  regval |= UDPHS_INT_EPT(epno);
  sam_putreg(regval, SAM_UDPHS_IEN);

  sam_putreg(UDPHS_EPTCTL_TXRDY, SAM_UDPHS_EPTCTLENB(epno));
  return OK;
}

/****************************************************************************
 * Name: sam_req_rddma
 *
 * Description:
 *   Process the next queued read request for an endpoint that supports DMA.
 *
 ****************************************************************************/

static int sam_req_rddma(struct sam_usbdev_s *priv, struct sam_ep_s *privep,
                         struct sam_req_s *privreq)
{
  uint32_t regval;
  int remaining;
  int epno;

  /* The endpoint must be IDLE and ready to begin the next transfer */

  if (privep->epstate != UDPHS_EPSTATE_IDLE)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EPINBUSY), privep->epstate);
      return -EBUSY;
    }

  /* Get the endpoint number */

  epno = USB_EPNO(privep->ep.eplog);

  /* Switch to the receiving state */

  privep->epstate   = UDPHS_EPSTATE_RECEIVING;
  privep->txnullpkt = 0;
  privreq->inflight = 0;
  privreq->req.xfrd = 0;

  /* How many more bytes can we append to the request buffer? */

  remaining = privreq->req.len - privreq->req.xfrd;
  if (remaining > 0)
    {
      /* Clip the DMA transfer size to the size available in the user buffer */

#if USBDEV_MAXREQUEUST > DMA_MAX_FIFO_SIZE
      if (remaining > DMA_MAX_FIFO_SIZE)
        {
         privreq->inflight = DMA_MAX_FIFO_SIZE;
        }
      else
#endif
        {
          privreq->inflight = remaining;
        }

      /* And perform the single DMA transfer */

      regval = UDPHS_DMACONTROL_ENDBEN | UDPHS_DMACONTROL_ENDBUFFIT |
               UDPHS_DMACONTROL_CHANNENB;
      sam_dma_single(epno, privreq, regval);
      return OK;
    }

  /* Enable the endpoint interrupt */

  regval  = sam_getreg(SAM_UDPHS_IEN);
  regval |= UDPHS_INT_EPT(epno);
  sam_putreg(regval, SAM_UDPHS_IEN);

  sam_putreg(UDPHS_EPTCTL_RXRDYTXKL, SAM_UDPHS_EPTCTLENB(epno));
  return OK;
}

/****************************************************************************
 * Request Helpers
 ****************************************************************************/
/****************************************************************************
 * Name: sam_req_dequeue
 ****************************************************************************/

static struct sam_req_s *sam_req_dequeue(struct sam_ep_s *privep)
{
  struct sam_req_s *ret = privep->head;

  if (ret)
    {
      privep->head = ret->flink;
      if (!privep->head)
        {
          privep->tail = NULL;
        }

      ret->flink = NULL;
    }

  return ret;
}

/****************************************************************************
 * Name: sam_req_enqueue
 ****************************************************************************/

static void sam_req_enqueue(struct sam_ep_s *privep, struct sam_req_s *req)
{
  req->flink = NULL;
  if (!privep->head)
    {
      privep->head = req;
      privep->tail = req;
    }
  else
    {
      privep->tail->flink = req;
      privep->tail        = req;
    }
}

/****************************************************************************
 * Name: sam_req_abort
 ****************************************************************************/

static inline void
sam_req_abort(struct sam_ep_s *privep, struct sam_req_s *privreq, int16_t result)
{
  usbtrace(TRACE_DEVERROR(SAM_TRACEERR_REQABORTED), (uint16_t)USB_EPNO(privep->ep.eplog));

  /* Save the result in the request structure */

  privreq->req.result = result;

  /* Callback to the request completion handler */

  privreq->req.callback(&privep->ep, &privreq->req);
}

/****************************************************************************
 * Name: sam_req_complete
 ****************************************************************************/

static void sam_req_complete(struct sam_ep_s *privep, int16_t result)
{
  struct sam_req_s *privreq;
  irqstate_t flags;

  /* Remove the completed request at the head of the endpoint request list */

  flags = irqsave();
  privreq = sam_req_dequeue(privep);
  irqrestore(flags);

  if (privreq)
    {
      DEBUGASSERT((privep->epstate == UDPHS_EPSTATE_RECEIVING) ||
                  (privep->epstate == UDPHS_EPSTATE_SENDING))

      /* Save the result in the request structure */

      privreq->req.result = result;

      /* Callback to the request completion handler */

      privreq->flink = NULL;
      privreq->req.callback(&privep->ep, &privreq->req);

      /* Reset the endpoint state and restore the stalled indication */

      privep->epstate   = UDPHS_EPSTATE_IDLE;
      privep->txnullpkt = 0;
    }
}

/****************************************************************************
 * Name: sam_req_wrnodma
 *
 * Description:
 *   Process the next queued write request for an endpoint that does not
 *   support DMA.
 *
 ****************************************************************************/

static int sam_req_wrnodma(struct sam_usbdev_s *priv, struct sam_ep_s *privep,
                           struct sam_req_s *privreq)
{
  const uint8_t *buf;
  uint8_t *fifo;
  uint8_t epno;
  int committed;
  int nbytes;
  int bytesleft;

  epno = USB_EPNO(privep->ep.eplog);

  /* Get the number of bytes to send.  The totals bytes remaining to be sent
   * is the the total size of the buffer, minus the number of bytes
   * successfully transferred, minus the number of bytes in-flight.
   */

  committed = privreq->req.xfrd + privreq->inflight;
  bytesleft = privreq->req.len - committed;

  /* Clip the requested transfer size to the number of bytes actually
   * available
   */

  nbytes = bytesleft;
  if (nbytes > bytesleft)
    {
      nbytes = bytesleft;
    }

  /* Either (1) we are committed to sending the null packet (because txnullpkt == 1
   * && nbytes == 0), or (2) we havenot yet sent the last packet (nbytes > 0).
   * In either case, it is appropriate to clear txnullpkt now.
   */

  privep->txnullpkt = 0;

  /* If we are not sending a NULL packet, then clip the size to maxpacket
   * and check if we need to send a following NULL packet.
   */

  nbytes = bytesleft;
  if (nbytes > 0)
    {
      /* Either send the maxpacketsize or all of the remaining data in
       * the request.
       */

      if (nbytes >= privep->ep.maxpacket)
        {
          nbytes =  privep->ep.maxpacket;

          /* Handle the case where this packet is exactly the
           * maxpacketsize.  Do we need to send a zero-length packet
           * in this case?
           */

          if (bytesleft ==  privep->ep.maxpacket &&
             (privreq->req.flags & USBDEV_REQFLAGS_NULLPKT) != 0)
            {
              privep->txnullpkt = 1;
            }
        }

      /* This is the new number of bytes "in-flight" */

      privreq->inflight += nbytes;
      usbtrace(TRACE_WRITE(USB_EPNO(privep->ep.eplog)), nbytes);

      /* The new buffer pointer is the started of the buffer plus the number
       * of bytes successfully transfered plus the number of bytes previously
       * "in-flight".
       */

      buf = privreq->req.buf + committed;

      /* Write packet in the FIFO buffer */

      fifo = (uint8_t *)
        ((uint32_t *)SAM_UDPHSRAM_VSECTION + (EPT_VIRTUAL_SIZE * epno));

      for (; nbytes; nbytes--)
        {
          *fifo++ = *buf++;
        }

        /* Indicate that there is data in the TX packet memory.  This will
         * be cleared when the next data out interrupt is received.
         */

        privep->epstate = UDPHS_EPSTATE_SENDING;
    }

  return OK;
}

/****************************************************************************
 * Name: sam_req_write
 *
 * Description:
 *   Process the next queued write request.  This function is called in one
 *   of three contexts:  (1) When a new write request is submitted (with
 *   interrupts disabled, (2) from interrupt handling when a previous
 *   transfer completes, or (3) resuming a stalled IN endpoint.
 *
 ****************************************************************************/

static int sam_req_write(struct sam_usbdev_s *priv, struct sam_ep_s *privep)
{
  struct sam_req_s *privreq;
  uint32_t regval;
  uint32_t eptype;
  uint8_t epno;
  int bytesleft;
  int ret;

  /* We get here when an IN endpoint interrupt occurs.  So now we know that
   * there is no TX transfer in progress.
   */

  while (privep->epstate == UDPHS_EPSTATE_IDLE);
    {
      /* Check the request from the head of the endpoint request queue */

      privreq = sam_rqpeek(privep);
      if (!privreq)
        {
          /* There is no TX transfer in progress and no new pending TX
           * requests to send.
           */

          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EPINQEMPTY), 0);
          return -ENOENT;
        }

      epno = USB_EPNO(privep->ep.eplog);
      ullvdbg("epno=%d req=%p: len=%d xfrd=%d inflight=%dnullpkt=%d\n",
              epno, privreq, privreq->req.len, privreq->req.xfrd,
              privreq->inflight, privep->txnullpkt);

      /* Were there bytes in flight? */

      if (privreq->inflight)
        {
          privreq->req.xfrd += privreq->inflight;
          privreq->inflight = 0;
        }

      /* Get the number of bytes left to be sent in the packet */

      bytesleft = privreq->req.len - privreq->req.xfrd;
      if (bytesleft > 0)
        {
          /* If the size is exactly a full packet, then note if we need to
           * send a zero length packet next.
           */

          if (bytesleft == privep->ep.maxpacket &&
             (privreq->req.flags & USBDEV_REQFLAGS_NULLPKT) != 0)
            {
              /* Next time we get here, bytesleft will be zero and txnullpkt
               * will be set.
               */

              privep->txnullpkt = 1;
            }
          else
            {
              /* No zero packet is forthcoming (maybe later) */

              privep->txnullpkt = 0;
            }

          /* The way that we handle the transfer is going to depend on
           * whether or not this endpoint supports DMA.
           */

          if ((SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0)
            {
              ret = sam_req_wrdma(priv, privep, privreq);
            }
          else
            {
              ret = sam_req_wrnodma(priv, privep, privreq);
            }

          /* Check if the transfer was successfully initiated */

          if (ret < 0)
            {
              return ret;
            }
        }

      /* No data to send... is there a trailing zero length packet transfer
       * pending?
       */

      else if (privep->txnullpkt)
        {
          /* If we get here, then we sent the last of the data on the
           * previous pass and we need to send the zero length packet now.
           *
           * A Zero Length Packet can be sent by setting just the TXRDY flag
           * in* the UDPHS_EPTSETSTAx register
           */

          privep->epstate   = UDPHS_EPSTATE_SENDING;
          privep->txnullpkt = 0;
          privreq->inflight = 0;
          sam_putreg(UDPHS_EPTSETSTA_TXRDY, SAM_UDPHS_EPTSETSTA(epno));
        }

      /* If all of the bytes were sent (including any final null packet)
       * then we are finished with the request buffer).
       */

      if (privreq->req.len >= privreq->req.xfrd &&
          privep->epstate == UDPHS_EPSTATE_IDLE)
        {
          /* Return the write request to the class driver */

          usbtrace(TRACE_COMPLETE(USB_EPNO(privep->ep.eplog)),
                   privreq->req.xfrd);

          /* Get the endpoint type */

          regval = sam_getreg(SAM_UDPHS_EPTCFG(epno));
          eptype = regval & UDPHS_EPTCFG_TYPE_MASK;

          /* Disable interrupts on non-control endpoints */

          if (eptype != UDPHS_EPTCFG_TYPE_CTRL8)
            {
              regval  = sam_getreg(SAM_UDPHS_IEN);
              regval &= ~UDPHS_INT_EPT(epno);
              sam_putreg(regval, SAM_UDPHS_IEN);
            }

          sam_putreg(UDPHS_EPTCTL_TXRDY, SAM_UDPHS_EPTCTLDIS(epno));
          privep->txnullpkt = 0;
          sam_req_complete(privep, OK);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: sam_req_wrnodma
 *
 * Description:
 *   Process the next queued write request for an endpoint that does not
 *   support DMA.
 *
 ****************************************************************************/

static int sam_req_rdnodma(struct sam_usbdev_s *priv, struct sam_ep_s *privep,
                           struct sam_req_s *privreq, uint16_t pktsize)
{
  const uint8_t *fifo;
  uint8_t *dest;
  int remaining;
  int readlen;
  int epno;

  privep->epstate   = UDPHS_EPSTATE_IDLE;
  privreq->inflight = 0;

  /* Get the number of bytes that can be received.  This is the size of the
   * user-provided request buffer, minus the number of bytes already
   * transferred to the user-buffer.
   */

  remaining = privreq->req.len - privreq->req.xfrd;

  /* Read the smaller of the number of bytes available in FIFO and the
   * size remaining in the request buffer provided by the caller.
   */

  readlen = MIN(remaining, pktsize);
  privreq->req.xfrd += readlen;

  /* Get the source and destination transfer addresses */

  epno = USB_EPNO(privep->ep.eplog);
  fifo = (const uint8_t *)
         ((uint32_t *)SAM_UDPHSRAM_VSECTION + (EPT_VIRTUAL_SIZE * epno));
  dest = privreq->req.buf + privreq->req.xfrd;

  /* Retrieve packet from the FIFO */

  for (; readlen > 0; readlen--)
    {
      *dest++ = *fifo++;
    }

  return OK;
}

/****************************************************************************
 * Name: sam_req_read
 *
 * Description:
 *   Called only from interrupt handling logic when on OUT packet is received
 *   on an endpoint in the RECEIVING state.
 *
 ****************************************************************************/

static int sam_req_read(struct sam_usbdev_s *priv, struct sam_ep_s *privep,
                        uint16_t pktsize)
{
  struct sam_req_s *privreq;
  uint32_t regval;
  uint32_t eptype;
  uint8_t epno;
  int ret;

  /* Check the request from the head of the endpoint request queue */

  epno    = USB_EPNO(privep->ep.eplog);
  privreq = sam_rqpeek(privep);
  if (!privreq)
    {
      /* Incoming data available in the FIFO, but no packet to receive the data.
       * Mark that the RX data is pending and hope that a packet is returned
       * soon.
       */

      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EPOUTQEMPTY), epno);
      priv->rxpending = true;
      return -ENOENT;
    }

  ullvdbg("EP%d: len=%d xfrd=%d\n", epno, privreq->req.len, privreq->req.xfrd);

  /* Ignore any attempt to receive a zero length packet */

  if (privreq->req.len == 0)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EPOUTNULLPACKET), 0);
      sam_req_complete(privep, OK);
      return OK;
    }

  usbtrace(TRACE_READ(USB_EPNO(privep->ep.eplog)), privreq->req.xfrd);

  /* The way that we handle the transfer is going to depend on whether
   * or not this endpoint supports DMA.
   */

  if ((SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0)
    {
      ret = sam_req_rddma(priv, privep, privreq);
    }
  else
    {
      ret = sam_req_rdnodma(priv, privep, privreq, pktsize);
    }

  if (ret == OK)
    {
      /* If the receive buffer is full or this is a partial packet,
       * then we are finished with the request buffer).
       */

      if (privreq->inflight < privep->ep.maxpacket ||
          privreq->req.xfrd >= privreq->req.len)
        {
          /* Return the read request to the class driver. */

          usbtrace(TRACE_COMPLETE(epno), privreq->req.xfrd);
          sam_putreg(UDPHS_EPTCTL_RXRDYTXKL, SAM_UDPHS_EPTCTLDIS(epno));

          /* Get the endpoint type */

          regval = sam_getreg(SAM_UDPHS_EPTCFG(epno));
          eptype = regval & UDPHS_EPTCFG_TYPE_MASK;

          /* Disable interrupt if not control EP */

          if (UDPHS_EPTCFG_TYPE_CTRL8 != eptype)
            {
              regval  = sam_getreg(SAM_UDPHS_IEN);
              regval &= ~UDPHS_INT_EPT(epno);
              sam_putreg(regval, SAM_UDPHS_IEN);
            }

          /* And complete the request */

          privep->epstate = UDPHS_EPSTATE_IDLE;
          sam_req_complete(privep, OK);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: sam_req_cancel
 ****************************************************************************/

static void sam_req_cancel(struct sam_ep_s *privep)
{
  uint32_t regval;
  uint8_t epno;

  /* Disable endpoint interrupts */

  epno    = USB_EPNO(privep->ep.eplog);
  regval  = sam_getreg(SAM_UDPHS_IEN);
  regval &= ~UDPHS_INT_DMA(epno);
  sam_putreg(regval, SAM_UDPHS_IEN);

  /* Then complete every queued request with -ESHUTDOWN status */

  while (!sam_rqempty(privep))
    {
      usbtrace(TRACE_COMPLETE(USB_EPNO(privep->ep.eplog)),
               (sam_rqpeek(privep))->req.xfrd);
      sam_req_complete(privep, -ESHUTDOWN);
    }
}

/****************************************************************************
 * Interrupt Level Processing
 ****************************************************************************/
/****************************************************************************
 * Name: sam_ep0_read
 *
 * Description:
 *   Read a general USB request from the UDPHS FIFO
 *
 ****************************************************************************/

static void sam_ep0_read(uint8_t *buffer, size_t buflen)
{
  volatile const uint8_t *fifo;

  /* Retrieve packet from the FIFO */

  fifo = (volatile const uint8_t *)SAM_UDPHSRAM_VSECTION;
  for (; buflen > 0; buflen--)
    {
      *buffer++ = *fifo++;
    }
}

/****************************************************************************
 * Name: sam_ep0_wrstatus
 *
 * Description:
 *   Process the next queued write request for an endpoint that does not
 *   support DMA.
 *
 ****************************************************************************/

static void sam_ep0_wrstatus(const uint8_t *buffer, size_t buflen)
{
  volatile uint8_t *fifo;

  /* Write packet in the FIFO buffer */

  fifo = (volatile uint8_t *)SAM_UDPHSRAM_VSECTION;
  for (; buflen > 0; buflen--)
    {
      *fifo++ = *buffer++;
    }
}

/****************************************************************************
 * Name: sam_ep0_dispatch
 ****************************************************************************/

static void sam_ep0_dispatch(struct sam_usbdev_s *priv)
{
  uint8_t *dataout;
  size_t outlen;
  int ret;

  usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DISPATCH), 0);
  if (priv && priv->driver)
    {
      /* Assume IN SETUP (or OUT SETUP with no data) */

      dataout = NULL;
      outlen  = 0;

      /* Was this an OUT SETUP command? */

      if (USB_REQ_ISOUT(priv->ctrl.type))
        {
          uint16_t tmplen = GETUINT16(priv->ctrl.len);
          if (tmplen > 0)
            {
              dataout = priv->ep0out;
              outlen  = tmplen;
            }
        }

      /* Forward to the control request to the class driver implementation */

      ret = CLASS_SETUP(priv->driver, &priv->usbdev, &priv->ctrl,
                        dataout, outlen);
      if (ret < 0)
        {
          /* Stall on failure */

          usbtrace(TRACE_DEVERROR(SAM_TRACEERR_DISPATCHSTALL), 0);
          (void)sam_ep_stall(&priv->eplist[EP0].ep, true);
        }
    }
}

/****************************************************************************
 * Name: sam_setdevaddr
 ****************************************************************************/

static void sam_setdevaddr(struct sam_usbdev_s *priv, uint8_t address)
{
  uint32_t regval;

  if (address)
    {
      /* Enable the address */

      regval  = sam_getreg(SAM_UDPHS_CTRL);
      regval &= ~UDPHS_CTRL_DEVADDR_MASK;
      regval |= UDPHS_CTRL_DEVADDR(address) | UDPHS_CTRL_FADDREN;
      sam_putreg(regval, SAM_UDPHS_CTRL);

      /* Go to the addressed state */

      priv->devstate = UDPHS_DEVSTATE_ADDRESS;
    }
  else
    {
      /* Disable address */

      regval  = sam_getreg(SAM_UDPHS_CTRL);
      regval &= ~UDPHS_CTRL_FADDREN;
      sam_putreg(regval, SAM_UDPHS_CTRL);

      /* Revert to the un-addressed, default state */

      priv->devstate = UDPHS_DEVSTATE_DEFAULT;
    }
}

/****************************************************************************
 * Name: sam_ep0_setup
 ****************************************************************************/

static void sam_ep0_setup(struct sam_usbdev_s *priv)
{
  struct sam_ep_s     *ep0     = &priv->eplist[EP0];
  struct sam_req_s    *privreq = sam_rqpeek(ep0);
  struct sam_ep_s     *privep;
  union wb_u           value;
  union wb_u           index;
  union wb_u           len;
  union wb_u           response;
  enum sam_ep0setup_e  ep0result;
  uint8_t              epno;
  int                  nbytes = 0; /* Assume zero-length packet */
  int                  ret;

  /* Terminate any pending requests (doesn't work if the pending request
   * was a zero-length transfer!)
   */

  while (!sam_rqempty(ep0))
    {
      int16_t result = OK;
      if (privreq->req.xfrd != privreq->req.len)
        {
          result = -EPROTO;
        }

      usbtrace(TRACE_COMPLETE(ep0->ep.eplog), privreq->req.xfrd);
      sam_req_complete(ep0, result);
    }

  /* Assume NOT stalled; no TX in progress */

  ep0->stalled  = 0;
  ep0->epstate  = UDPHS_EPSTATE_IDLE;

  /* And extract the little-endian 16-bit values to host order */

  value.w = GETUINT16(priv->ctrl.value);
  index.w = GETUINT16(priv->ctrl.index);
  len.w   = GETUINT16(priv->ctrl.len);

  ullvdbg("SETUP: type=%02x req=%02x value=%04x index=%04x len=%04x\n",
          priv->ctrl.type, priv->ctrl.req, value.w, index.w, len.w);

  /* Dispatch any non-standard requests */

  if ((priv->ctrl.type & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_STANDARD)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_NOSTDREQ), priv->ctrl.type);

      /* Let the class implementation handle all non-standar requests */

      sam_ep0_dispatch(priv);
      return;
    }

  /* Handle standard request.  Pick off the things of interest to the
   * USB device controller driver; pass what is left to the class driver
   */

  ep0result = UDPHS_EP0SETUP_SUCCESS;
  switch (priv->ctrl.req)
    {
    case USB_REQ_GETSTATUS:
      {
        /* type:  device-to-host; recipient = device, interface, endpoint
         * value: 0
         * index: zero interface endpoint
         * len:   2; data = status
         */

        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_GETSTATUS), priv->ctrl.type);
        if (len.w != 2 || (priv->ctrl.type & USB_REQ_DIR_IN) == 0 ||
            index.b[MSB] != 0 || value.w != 0)
          {
            usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADEPGETSTATUS), 0);
            ep0result = UDPHS_EP0SETUP_STALL;
          }
        else
          {
            switch (priv->ctrl.type & USB_REQ_RECIPIENT_MASK)
              {
               case USB_REQ_RECIPIENT_ENDPOINT:
                {
                  epno = USB_EPNO(index.b[LSB]);
                  usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EPGETSTATUS), epno);
                  if (epno >= SAM_UDPHS_NENDPOINTS)
                    {
                      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADEPGETSTATUS), epno);
                      ep0result = UDPHS_EP0SETUP_STALL;
                    }
                  else
                    {
                      privep     = &priv->eplist[epno];
                      response.w = 0; /* Not stalled */
                      nbytes     = 2; /* Response size: 2 bytes */

                      if (privep->stalled)
                        {
                          /* Endpoint stalled */

                          response.b[LSB] = 1; /* Stalled */
                        }
                    }
                }
                break;

              case USB_REQ_RECIPIENT_DEVICE:
                {
                 if (index.w == 0)
                    {
                      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DEVGETSTATUS), 0);

                      /* Features:  Remote Wakeup=YES; selfpowered=? */

                      response.w      = 0;
                      response.b[LSB] = (priv->selfpowered << USB_FEATURE_SELFPOWERED) |
                                        (1 << USB_FEATURE_REMOTEWAKEUP);
                      nbytes          = 2; /* Response size: 2 bytes */
                    }
                  else
                    {
                      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADDEVGETSTATUS), 0);
                      ep0result = UDPHS_EP0SETUP_STALL;
                    }
                }
                break;

              case USB_REQ_RECIPIENT_INTERFACE:
                {
                  usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_IFGETSTATUS), 0);
                  response.w = 0;
                  nbytes     = 2; /* Response size: 2 bytes */
                }
                break;

              default:
                {
                  usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADGETSTATUS), 0);
                  ep0result = UDPHS_EP0SETUP_STALL;
                }
                break;
              }
          }
      }
      break;

    case USB_REQ_CLEARFEATURE:
      {
        /* type:  host-to-device; recipient = device, interface or endpoint
         * value: feature selector
         * index: zero interface endpoint;
         * len:   zero, data = none
         */

        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_CLEARFEATURE), priv->ctrl.type);
        if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) != USB_REQ_RECIPIENT_ENDPOINT)
          {
            /* Let the class implementation handle all recipients (except for the
             * endpoint recipient)
             */

            sam_ep0_dispatch(priv);
            ep0result = UDPHS_EP0SETUP_DISPATCHED;
          }
        else
          {
            /* Endpoint recipient */

            epno = USB_EPNO(index.b[LSB]);
            if (epno < SAM_UDPHS_NENDPOINTS && index.b[MSB] == 0 &&
                value.w == USB_FEATURE_ENDPOINTHALT && len.w == 0)
              {
                privep         = &priv->eplist[epno];
                privep->halted = 0;

                ret = sam_ep_stall(&privep->ep, true);
                if (ret < 0)
                  {
                    ep0result = UDPHS_EP0SETUP_STALL;
                  }
              }
            else
              {
                usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADCLEARFEATURE), 0);
                ep0result = UDPHS_EP0SETUP_STALL;
              }
          }
      }
      break;

    case USB_REQ_SETFEATURE:
      {
        /* type:  host-to-device; recipient = device, interface, endpoint
         * value: feature selector
         * index: zero interface endpoint;
         * len:   0; data = none
         */

        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_SETFEATURE), priv->ctrl.type);
        if (((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_DEVICE) &&
            value.w == USB_FEATURE_TESTMODE)
          {
            /* Special case recipient=device test mode */

            ullvdbg("test mode: %d\n", index.w);
          }
        else if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) != USB_REQ_RECIPIENT_ENDPOINT)
          {
            /* The class driver handles all recipients except recipient=endpoint */

            sam_ep0_dispatch(priv);
            ep0result = UDPHS_EP0SETUP_DISPATCHED;
          }
        else
          {
            /* Handler recipient=endpoint */

            epno = USB_EPNO(index.b[LSB]);
            if (epno < SAM_UDPHS_NENDPOINTS && index.b[MSB] == 0 &&
                value.w == USB_FEATURE_ENDPOINTHALT && len.w == 0)
              {
                privep         = &priv->eplist[epno];
                privep->halted = 1;

                ret = sam_ep_stall(&privep->ep, false);
                if (ret < 0)
                  {
                    ep0result = UDPHS_EP0SETUP_STALL;
                  }
              }
            else
              {
                usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADSETFEATURE), 0);
                ep0result = UDPHS_EP0SETUP_STALL;
              }
          }
      }
      break;

    case USB_REQ_SETADDRESS:
      {
        /* type:  host-to-device; recipient = device
         * value: device address
         * index: 0
         * len:   0; data = none
         */

        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EP0SETUPSETADDRESS), value.w);
        if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) != USB_REQ_RECIPIENT_DEVICE ||
            index.w != 0 || len.w != 0 || value.w > 127)
          {
            usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADSETADDRESS), 0);
            ep0result = UDPHS_EP0SETUP_STALL;
          }
        else
          {
            /* Note that setting of the device address will be deferred.  A
             * zero-length packet will be sent and the device address will
             * be set when the zero-length packet transfer completes.
             */

            usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_SETADDRESS), value.w);
            priv->devaddr = value.w;
            ep0result     = UDPHS_EP0SETUP_ADDRESS;
          }
      }
      break;

    case USB_REQ_GETDESCRIPTOR:
      /* type:  device-to-host; recipient = device
       * value: descriptor type and index
       * index: 0 or language ID;
       * len:   descriptor len; data = descriptor
       */
    case USB_REQ_SETDESCRIPTOR:
      /* type:  host-to-device; recipient = device
       * value: descriptor type and index
       * index: 0 or language ID;
       * len:   descriptor len; data = descriptor
       */

      {
        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_GETSETDESC), priv->ctrl.type);
        if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_DEVICE)
          {
            /* The request seems valid... let the class implementation handle it */

            sam_ep0_dispatch(priv);
            ep0result = UDPHS_EP0SETUP_DISPATCHED;
          }
        else
          {
            usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADGETSETDESC), 0);
            ep0result = UDPHS_EP0SETUP_STALL;
          }
      }
      break;

    case USB_REQ_GETCONFIGURATION:
      /* type:  device-to-host; recipient = device
       * value: 0;
       * index: 0;
       * len:   1; data = configuration value
       */

      {
        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_GETCONFIG), priv->ctrl.type);
        if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_DEVICE &&
            value.w == 0 && index.w == 0 && len.w == 1)
          {
            /* The request seems valid... let the class implementation handle it */

            sam_ep0_dispatch(priv);
            ep0result = UDPHS_EP0SETUP_DISPATCHED;
          }
        else
          {
            usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADGETCONFIG), 0);
            ep0result = UDPHS_EP0SETUP_STALL;
          }
      }
      break;

    case USB_REQ_SETCONFIGURATION:
      /* type:  host-to-device; recipient = device
       * value: configuration value
       * index: 0;
       * len:   0; data = none
       */

      {
        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_SETCONFIG), priv->ctrl.type);
        if ((priv->ctrl.type & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_DEVICE &&
            index.w == 0 && len.w == 0)
          {
             /* The request seems valid... let the class implementation handle it.
              * If the class implementation accespts it new configuration, it will
              * call sam_ep_configure() to configure the endpoints.
              */

             sam_ep0_dispatch(priv);
            ep0result = UDPHS_EP0SETUP_DISPATCHED;
          }
        else
          {
            usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADSETCONFIG), 0);
            ep0result = UDPHS_EP0SETUP_STALL;
          }
      }
      break;

    case USB_REQ_GETINTERFACE:
      /* type:  device-to-host; recipient = interface
       * value: 0
       * index: interface;
       * len:   1; data = alt interface
       */
    case USB_REQ_SETINTERFACE:
      /* type:  host-to-device; recipient = interface
       * value: alternate setting
       * index: interface;
       * len:   0; data = none
       */

      {
        /* Let the class implementation handle the request */

        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_GETSETIF), priv->ctrl.type);
        sam_ep0_dispatch(priv);
        ep0result = UDPHS_EP0SETUP_DISPATCHED;
      }
      break;

    case USB_REQ_SYNCHFRAME:
      /* type:  device-to-host; recipient = endpoint
       * value: 0
       * index: endpoint;
       * len:   2; data = frame number
       */

      {
        usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_SYNCHFRAME), 0);
      }
      break;

    default:
      {
        usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDCTRLREQ), priv->ctrl.req);
        ep0result = UDPHS_EP0SETUP_STALL;
      }
      break;
    }

  /* Restrict the data length to the length requested in the setup packet */

  if (nbytes > len.w)
    {
      nbytes = len.w;
    }

  /* At this point, the request has been handled and there are three
   * (or four) possible outcomes:
   *
   * 1a. ep0result == UDPHS_EP0SETUP_SUCCESS
   *
   *    The setup request was successfully handled above and a response
   *    packet must be sent (may be a zero length packet).
   *
   * 1b. ep0result == UDPHS_EP0SETUP_ADDRESS
   *
   *    A special case is the case where epstate=UDPHS_EPSTATE_EP0ADDRESS.
   *    This that the above processing generated an additional state where
   *    we need to wait to obtain our device address.
   *
   * 2. ep0result == UDPHS_EP0SETUP_DISPATCHED;
   *
   *    The request was forwarded to the class implementation.  In case,
   *    EP0 IN data may have already been sent and the EP0 IN response
   *    has already been queued?  Or perhaps the endpoint has already
   *    been stalled?  This is all under the control of the class driver.
   *
   *    NOTE that for the case of non-standard SETUP requested, those
   *    requests were forwarded to the class driver and we don't even get
   *    to this logic.
   *
   * 3. ep0result == UDPHS_EP0SETUP_STALL;
   *
   *    An error was detected in either the above logic or by the class
   *    implementation logic.
   */

  switch (ep0result)
    {
      case UDPHS_EP0SETUP_SUCCESS:
        {
          /* Send the response (might be a zero-length packet) */

          ep0->epstate = UDPHS_EPSTATE_EP0STATUSIN;
          sam_ep0_wrstatus(response.b, nbytes);
        }
        break;

      case UDPHS_EP0SETUP_ADDRESS:
        {
          /* Send the response (might be a zero-length packet) */

          ep0->epstate = UDPHS_EPSTATE_EP0ADDRESS;
          sam_ep0_wrstatus(response.b, nbytes);
        }
        break;

      case UDPHS_EP0SETUP_STALL:
        {
          /* Stall EP0 */

          usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EP0SETUPSTALLED),
                   priv->ctrl.req);

          (void)sam_ep_stall(&priv->eplist[EP0].ep, true);
        }
        break;

      case UDPHS_EP0SETUP_DISPATCHED:
      default:
        break;
    }
}

/****************************************************************************
 * Name: sam_dma_interrupt
 *
 * Description:
 *   Handle the UDPHS DMA interrupt
 *
 ****************************************************************************/

static void sam_dma_interrupt(struct sam_usbdev_s *priv, int epno)
{
  struct sam_ep_s  *privep;
  struct sam_req_s *privreq;
  uintptr_t regaddr;
  uint32_t regval;
  uint32_t dmastatus;
  uint8_t *buf;
  int bufcnt;
  int xfrd;
  int16_t result = OK;

  /* Not all endpoints support DMA */

  DEBUGASSERT((unsigned)epno < SAM_UDPHS_NENDPOINTS &&
              (SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0);

  /* Get the endpoint structure */

  privep = &priv->eplist[epno];

  /* Get the request from the head of the endpoint request queue */

  privreq = sam_rqpeek(privep);
  DEBUGASSERT(privreq);

  /* Invalidate the data cache for region that just completed DMA.
   * This will force the buffer data to be reloaded from RAM.
   */

  buf = &privreq->req.buf[privreq->req.xfrd];
  cp15_invalidate_dcache((uintptr_t)buf, (uintptr_t)buf + privreq->inflight);

  /* Get the result of the DMA operation */

  dmastatus = sam_getreg(SAM_UDPHS_DMASTATUS(epno));
  uvdbg("DMA%d DMASTATUS: %08x\n", epno, dmastatus);

  /* Disable DMA interrupt to avoid receiving 2 (B_EN and TR_EN) */

  regaddr = SAM_UDPHS_DMACONTROL(epno);
  regval  = sam_getreg(regaddr);
  regval &= ~(UDPHS_DMACONTROL_ENDTREN | UDPHS_DMACONTROL_ENDBEN);
  sam_putreg(regval, regaddr);

  /* Check for end of the buffer.  Set by hardware when the
   * BUFF_COUNT downcount reach zero.
   */

  if ((dmastatus & UDPHS_DMASTATUS_ENDBFST) != 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DMAEOB), (uint16_t)dmastatus);

      /* BUFF_COUNT holds the number of untransmitted bytes. BUFF_COUNT is
       * equal to zero in case of good transfer
       */

      bufcnt = (dmastatus & UDPHS_DMASTATUS_BUFCNT_MASK)
                >> UDPHS_DMASTATUS_BUFCNT_SHIFT;

      xfrd               = privreq->inflight - bufcnt;
      privreq->req.xfrd += xfrd;
      privreq->inflight  = bufcnt;

      /* Is there more data to send? */

      bufcnt = privreq->req.len - privreq->req.xfrd - privreq->inflight;
      if (bufcnt > 0)
        {
          /* Yes, clip to the size of the DMA FIFO */

#if USBDEV_MAXREQUEUST > DMA_MAX_FIFO_SIZE
          if (bufcnt > DMA_MAX_FIFO_SIZE)
            {
              privreq->inflight = DMA_MAX_FIFO_SIZE;
            }
          else
#endif
            {
              privreq->inflight = bufcnt;
            }

          /* And perform the DMA transfer */

          regval = UDPHS_DMACONTROL_ENDTREN | UDPHS_DMACONTROL_ENDTRIT |
                   UDPHS_DMACONTROL_ENDBEN | UDPHS_DMACONTROL_ENDBUFFIT |
                   UDPHS_DMACONTROL_CHANNENB;
          sam_dma_single(epno, privreq, regval);
        }
    }

  /* Check for end of channel transfer. Set by hardware when the last
   * packet transfer is complete
   */

  else if ((dmastatus & UDPHS_DMASTATUS_ENDTRST) != 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DMAEOC), (uint16_t)dmastatus);

      /* Get the number of btyes transferred from the DMA status */

      bufcnt = ((dmastatus & UDPHS_DMASTATUS_BUFCNT_MASK)
                >> UDPHS_DMASTATUS_BUFCNT_SHIFT);

      xfrd               = privreq->inflight - bufcnt;
      privreq->req.xfrd += xfrd;
      privreq->inflight -= bufcnt;
    }
  else
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DMAERR), (uint16_t)dmastatus);
      result = -EIO;
    }

  /* Check if we are finished with this requrest */

  if (privreq->req.len == privreq->req.xfrd)
    {
      /* Return the request buffer to the class implementation */

      sam_req_complete(privep, result);

      /* Try to start the transfer for the next request */
#warning Missing logic
    }
}

/****************************************************************************
 * Name: sam_ep_interrupt
 *
 * Description:
 *   Handle the UDPHS endpoint interrupt
 *
 ****************************************************************************/

static void sam_ep_interrupt(struct sam_usbdev_s *priv, int epno)
{
  struct sam_ep_s *privep;
  struct sam_req_s *privreq;
  uint32_t eptsta;
  uint32_t eptype;
  uint32_t regval;
  uint16_t pktsize;

  DEBUGASSERT((unsigned)epno < SAM_UDPHS_NENDPOINTS);

  /* Get the endpoint structure */

  privep = &priv->eplist[epno];

  /* Get the request from the head of the endpoint request queue */

  privreq = sam_rqpeek(privep);
  DEBUGASSERT(privreq);

  /* Get the endpoint status */

  eptsta = sam_getreg(SAM_UDPHS_EPTSTA(epno));

  /* Get the endpoint type */

  regval = sam_getreg(SAM_UDPHS_EPTCFG(epno));
  eptype = regval & UDPHS_EPTCFG_TYPE_MASK;

  /* IN packet sent */

  if ((sam_getreg(SAM_UDPHS_EPTCTL(epno)) & UDPHS_EPTCTL_TXRDY) != 0 &&
      (eptsta & UDPHS_EPTSTA_TXRDY) == 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_TXRDY), (uint16_t)eptsta);

      /* Sending state.  This is the completion of a "normal" write request
       * transfer.  In this case, we need to resume request processing in
       * order to send the next outgoing packet.
       */

      if (privep->epstate == UDPHS_EPSTATE_SENDING ||
          privep->epstate == UDPHS_EPSTATE_EP0STATUSIN)
        {
          /* Continue/resume processing the write requests */

          privep->epstate = UDPHS_EPSTATE_IDLE;
          (void)sam_req_write(priv, privep);
        }

      /* Setting of the device address is a special case.  The address was
       * obtained when a preceding SETADDRESS SETUP command was processed.
       * But the address is not set until the final SETUP status phase
       * completes.  This interrupt indicates the completion of that status
       * phase and now we set the address.
       */

      else if (privep->epstate == UDPHS_EPSTATE_EP0ADDRESS)
        {
          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_ADDRESSED), priv->devaddr);
          sam_setdevaddr(priv, priv->devaddr);
        }
      else
        {
          usbtrace(TRACE_DEVERROR(SAM_TRACEERR_TXRDYERR), privep->epstate);
        }
    }

  /* OUT packet received */

  if ((eptsta & UDPHS_EPTSTA_RXRDYTXKL) != 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_RXRDY), (uint16_t)eptsta);

      /* Are we receiving data for a read request? */

      if (privep->epstate == UDPHS_EPSTATE_RECEIVING)
        {
          /* Yes, get the size of the packet that we just received */

          pktsize = (uint16_t)
            ((eptsta & UDPHS_EPTSTA_BYTECNT_MASK) >>
            UDPHS_EPTSTA_BYTECNT_SHIFT);

          /* And continue processing the read request */

          privep->epstate = UDPHS_EPSTATE_IDLE;
          sam_req_read(priv, privep, pktsize);
          sam_putreg(UDPHS_EPTSTA_RXRDYTXKL, SAM_UDPHS_EPTCLRSTA(epno));
        }

      /* Did we just receive the data associated with an OUT SETUP command? */

      else if (privep->epstate != UDPHS_EPSTATE_EP0DATAOUT)
        {
          uint16_t len;

#ifdef CONFIG_DEBUG
          /* Yes.. get the size of the packet that we just received */

          pktsize = (uint16_t)
            ((eptsta & UDPHS_EPTSTA_BYTECNT_MASK) >>
            UDPHS_EPTSTA_BYTECNT_SHIFT);
#endif

          /* Copy the OUT data from the EP0 FIFO into special EP0 buffer. */

          len = GETUINT16(priv->ctrl.len);
          DEBUGASSERT(len > 0 && len == pktsize);
          sam_ep0_read(priv->ep0out, len);

          /* And handle the EP0 SETUP now. */

          privep->epstate = UDPHS_EPSTATE_IDLE;
          sam_ep0_setup(priv);
        }
      else
        {
          /* Check if ACK received on a Control EP */

          if (eptype == UDPHS_EPTCFG_TYPE_CTRL8 &&
              (eptsta & UDPHS_EPTSTA_BYTECNT_MASK) == 0)
            {
              sam_putreg(UDPHS_EPTSTA_RXRDYTXKL, SAM_UDPHS_EPTCLRSTA(epno));
            }

          /* Data has been STALLed */

          else if ((eptsta & UDPHS_EPTSTA_FRCESTALL) != 0)
            {
              sam_putreg(UDPHS_EPTSTA_RXRDYTXKL, SAM_UDPHS_EPTCLRSTA(epno));
            }

          /* NAK the data */

          else
            {
              regval  = sam_getreg(SAM_UDPHS_IEN);
              regval &= ~UDPHS_INT_EPT(epno);
              sam_putreg(regval, SAM_UDPHS_IEN);
            }
        }
    }

  /* STALL sent */

  if ((eptsta & UDPHS_EPTSTA_STALLSNT) != 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_STALLSNT), (uint16_t)eptsta);

      /* Acknowledge */

      sam_putreg(UDPHS_EPTSTA_STALLSNT, SAM_UDPHS_EPTCLRSTA(epno));

      /* ISO error */

      if (eptype == UDPHS_EPTCFG_TYPE_ISO)
        {
          sam_req_complete(privep, -EIO);
        }

      /* If EP is not halted, clear STALL */

      else
        {
          if (privep->epstate != UDPHS_EPSTATE_STALLED)
            {
              sam_putreg(UDPHS_EPTSTA_FRCESTALL, SAM_UDPHS_EPTCLRSTA(epno));
            }
        }
    }

  /* SETUP packet received */

  if ((eptsta & UDPHS_EPTSTA_RXSETUP) != 0)
    {
      usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_RXSETUP), (uint16_t)eptsta);

      /* If a request transfer was pending, complete it. Handle the case
       * where during the status phase of a control write transfer, the host
       * receives the device ZLP and ack it, but the ack is not received by the
       * device
       */

      if (privep->epstate == UDPHS_EPSTATE_RECEIVING ||
          privep->epstate == UDPHS_EPSTATE_SENDING)
        {
          sam_req_complete(privep, OK);
        }

      /* ISO Err Flow */

      if (eptype == UDPHS_EPTCFG_TYPE_ISO)
        {
          /* Acknowledge setup packet */

          sam_putreg(UDPHS_EPTSTA_RXSETUP, SAM_UDPHS_EPTCLRSTA(epno));
        }
      else
        {
          uint16_t len;

          /* Copy setup data from the EP0 FIFO into the driver structure. */

          sam_ep0_read((uint8_t *)&priv->ctrl, USB_SIZEOF_CTRLREQ);

          /* Acknowledge setup packet */

          sam_putreg(UDPHS_EPTSTA_RXSETUP, SAM_UDPHS_EPTCLRSTA(epno));

          /* Check for a SETUP IN transaction */

          len = GETUINT16(priv->ctrl.len);
          if (USB_REQ_ISOUT(priv->ctrl.type) && len > 0)
            {
              /* Yes.. then we have to wait for the IN data phase to
               * complete before processing the SETUP command.
               */

              usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EP0SETUPIN), len);
              privep->epstate = UDPHS_EPSTATE_EP0DATAOUT;
            }
          else
            {
              /* This is an SETUP OUT command (or a SETUP IN with no data).
               * Handle the EP0 SETUP now.
               */

              usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EP0SETUPOUT), priv->ctrl.req);
              privep->epstate = UDPHS_EPSTATE_IDLE;
              sam_ep0_setup(priv);
            }
        }
    }
}

/****************************************************************************
 * Name: sam_udphs_interrupt
 *
 * Description:
 *   Handle the UDPHS interrupt
 *
 ****************************************************************************/

static int sam_udphs_interrupt(int irq, void *context)
{
  /* For now there is only one USB controller, but we will always refer to
   * it using a pointer to make any future ports to multiple UDPHS controllers
   * easier.
   */

  struct sam_usbdev_s *priv = &g_udphs;
  uint32_t intsta;
  uint32_t ien;
  uint32_t pending;
  uint32_t regval;
  int i;

  /* Get the set of pending interrupts */

  intsta  = sam_getreg(SAM_UDPHS_INTSTA);
  usbtrace(TRACE_INTENTRY(SAM_TRACEINTID_INTERRUPT), intsta);

  ien     = sam_getreg(SAM_UDPHS_IEN);
  pending = intsta & ien;

  /* Handle all pending UDPHS interrupts (and new interrupts that become
   * pending)
   */

  while (pending)
    {
      usbtrace(TRACE_INTENTRY(SAM_TRACEINTID_INTERRUPT), intsta);

      /* Suspend, treated last */

      if ((pending == UDPHS_INT_DETSUSPD) != 0)
        {
          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DETSUSPD), (uint16_t)pending);

          /* Enable wakeup interrupts */

          regval  = ien;
          regval &= ~UDPHS_INT_DETSUSPD;
          regval |= (UDPHS_INT_WAKEUP | UDPHS_INT_ENDOFRSM);
          sam_putreg(regval, SAM_UDPHS_IEN);

          /* Acknowledge interrupt */

          sam_putreg(UDPHS_INT_DETSUSPD | UDPHS_INT_WAKEUP, SAM_UDPHS_CLRINT);
          sam_suspend(priv);
        }

      /* SOF interrupt*/

      else if ((pending & UDPHS_INT_INTSOF) != 0)
        {
          /* Acknowledge interrupt */

          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_INTSOF), (uint16_t)pending);
          sam_putreg(UDPHS_INT_INTSOF, SAM_UDPHS_CLRINT);
        }

      /* Resume */

      else if ((pending & UDPHS_INT_WAKEUP) != 0 ||
               (pending & UDPHS_INT_ENDOFRSM) != 0)
        {
          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_WAKEUP), (uint16_t)pending);
          sam_resume(priv);

          /* Acknowledge interrupt */

          sam_putreg(UDPHS_INT_WAKEUP | UDPHS_INT_ENDOFRSM | UDPHS_INT_DETSUSPD,
                     SAM_UDPHS_CLRINT);

          /* Enable suspend interrupts */

          ien &= ~UDPHS_INT_WAKEUP;
          ien |= (UDPHS_INT_ENDOFRSM | UDPHS_INT_DETSUSPD);
          sam_putreg(ien, SAM_UDPHS_IEN);
        }

      /* Bus reset */

      if ((pending & UDPHS_INT_ENDRESET) != 0)
        {
          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_ENDRESET), (uint16_t)pending);

          /* Clear and enable the suspend interrupt */

          sam_putreg(UDPHS_INT_WAKEUP | UDPHS_INT_DETSUSPD, SAM_UDPHS_CLRINT);

          ien |= UDPHS_INT_DETSUSPD;
          sam_putreg(ien, SAM_UDPHS_IEN);

          /* Handle the reset */

          sam_reset(priv);

          /* Acknowledge the interrupt */

          sam_putreg(UDPHS_INT_ENDRESET, SAM_UDPHS_CLRINT);
        }

      /* Upstream resume */

      else if ((pending & UDPHS_INT_UPSTRRES) != 0)
        {
          /* Acknowledge interrupt */

          usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_UPSTRRES), (uint16_t)pending);
          sam_putreg(UDPHS_INT_UPSTRRES, SAM_UDPHS_CLRINT);
        }

      /* DMA interrupts */

      if ((pending & UDPHS_INT_DMA_MASK) != 0)
        {
          for (i = 1; i <= SAM_UDPHS_NDMACHANNELS; i++)
            {
              if ((pending & UDPHS_INT_DMA(i)) != 0)
                {
                  usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_DMA), (uint16_t)i);
                  sam_dma_interrupt(priv, i);
                }
            }
        }

      /* Endpoint Interrupts */

      if ((pending & UDPHS_INT_EPT_MASK) != 0)
        {
          for (i = 1; i <= SAM_UDPHS_NENDPOINTS; i++)
            {
              if ((pending & UDPHS_INT_EPT(i)) != 0)
                {
                  usbtrace(TRACE_INTDECODE(SAM_TRACEINTID_EP), (uint16_t)i);
                  sam_ep_interrupt(priv, i);
                }
            }
        }

      /* Re-sample the set of pending interrupts */

      intsta  = sam_getreg(SAM_UDPHS_INTSTA);
      ien     = sam_getreg(SAM_UDPHS_IEN);
      pending = intsta & ien;
    }

  usbtrace(TRACE_INTEXIT(SAM_TRACEINTID_INTERRUPT), intsta);
  return OK;
}

/****************************************************************************
 * Suspend/Resume Helpers
 ****************************************************************************/
/****************************************************************************
 * Name: sam_suspend
 ****************************************************************************/

static void sam_suspend(struct sam_usbdev_s *priv)
{
  /* Don't do anything if the device is already suspended */

  if (priv->devstate != UDPHS_DEVSTATE_SUSPENDED)
    {
      /* Notify the class driver of the suspend event */

      if (priv->driver)
        {
          CLASS_SUSPEND(priv->driver, &priv->usbdev);
        }

      /* Switch to the Suspended state */

      priv->prevstate = priv->devstate;
      priv->devstate  = UDPHS_DEVSTATE_SUSPENDED;

      /* Disable clocking to the UDPHS peripheral
       *
       * NOTE: The Atmel sample code disables USB clocking here (via the PMC
       * CKGR_UCKR).  However, we cannot really do that here because that
       * clocking is also needed by the UHPHS host.
       */

      sam_udphs_disableclk();

      /* Let the board-specific logic know that we have entered the
       * suspend state.  This may trigger additional reduced power
       * consumuption measures.
       */

      sam_usbsuspend((struct usbdev_s *)priv, false);
    }
}

/****************************************************************************
 * Name: sam_resume
 ****************************************************************************/

static void sam_resume(struct sam_usbdev_s *priv)
{
  /* This function is called when either (1) a WKUP interrupt is received from
   * the host PC, or (2) the class device implementation calls the wakeup()
   * method.
   */

  /* Don't do anything if the device was not suspended */

  if (priv->devstate == UDPHS_DEVSTATE_SUSPENDED)
    {
      /* Enable clocking to the UDPHS peripheral.
       *
       * NOTE: In the Atmel example code, they also enable USB clocking
       * at this point (via the BIAS in the CKGR_UCKR register).  In this
       * implementation, that should not be necessary here because we
       * never disable BIAS to begin with.
       */

      sam_udphs_enableclk();

      /* Revert to the previous state */

      priv->devstate = priv->prevstate;

      /* Restore full power -- whatever that means for this particular board */

      sam_usbsuspend((struct usbdev_s *)priv, true);

      /* Notify the class driver of the resume event */

      if (priv->driver)
        {
          CLASS_RESUME(priv->driver, &priv->usbdev);
        }
    }
}

/****************************************************************************
 * Endpoint Helpers
 ****************************************************************************/

/****************************************************************************
 * Name: sam_ep_reset
 *
 * Description
 *   Reset and disable a set of endpoints.
 *
 ****************************************************************************/

static void sam_ep_reset(struct sam_usbdev_s *priv, uint8_t epno)
{
  struct sam_ep_s *privep = &priv->eplist[epno];
  uint32_t regval;

  /* Disable endpoint interrupt */

  regval = sam_getreg(SAM_UDPHS_IEN);
  regval &= ~UDPHS_INT_EPT(epno);
  sam_putreg(regval, SAM_UDPHS_IEN);

  /* Cancel any queued requests.  Since they are canceled with status
   * -ESHUTDOWN, then will not be requeued until the configuration is reset.
   * NOTE:  This should not be necessary... the CLASS_DISCONNECT above
   * should result in the class implementation calling sam_ep_disable
   * for each of its configured endpoints.
   */

  sam_req_cancel(privep);

  /* Reset endpoint */

  sam_putreg(UDPHS_EPTRST(epno), SAM_UDPHS_EPTRST);

  /* Reset endpoint status */

  privep->epstate   = UDPHS_EPSTATE_DISABLED;
  privep->stalled   = false;
  privep->halted    = false;
  privep->txnullpkt = false;
  privep->bank      = 0;
}

/****************************************************************************
 * Name: sam_epset_reset
 *
 * Description
 *   Reset and disable a set of endpoints.
 *
 ****************************************************************************/

static void sam_epset_reset(struct sam_usbdev_s *priv, uint16_t epset)
{
  uint32_t bit;
  int epno;

  /* Reset each endpoint in the set */

  for (epno = 0, bit = 1, epset &= SAM_EPSET_ALL;
       epno < SAM_UDPHS_NENDPOINTS && epset != 0;
       epno++, bit <<= 1)
    {
      /* Is this endpoint in the set? */

      if ((epset & bit) != 0)
        {
           /* Yes.. reset it */

           sam_ep_reset(priv, epno);
           epset &= ~bit;
        }
    }
}

/****************************************************************************
 * Name: sam_ep_reserve
 *
 * Description:
 *   Find and un-reserved endpoint number and reserve it for the caller.
 *
 ****************************************************************************/

static inline struct sam_ep_s *
sam_ep_reserve(struct sam_usbdev_s *priv, uint8_t epset)
{
  struct sam_ep_s *privep = NULL;
  irqstate_t flags;
  int epndx = 0;

  flags = irqsave();
  epset &= priv->epavail;
  if (epset)
    {
      /* Select the lowest bit in the set of matching, available endpoints
       * (skipping EP0)
       */

      for (epndx = 1; epndx < SAM_UDPHS_NENDPOINTS; epndx++)
        {
          uint8_t bit = SAM_EP_BIT(epndx);
          if ((epset & bit) != 0)
            {
              /* Mark the endpoint no longer available */

              priv->epavail &= ~bit;

              /* And return the pointer to the standard endpoint structure */

              privep = &priv->eplist[epndx];
              break;
            }
        }
    }

  irqrestore(flags);
  return privep;
}

/****************************************************************************
 * Name: sam_ep_unreserve
 *
 * Description:
 *   The endpoint is no long in-used.  It will be un-reserved and can be
 *   re-used if needed.
 *
 ****************************************************************************/

static inline void
sam_ep_unreserve(struct sam_usbdev_s *priv, struct sam_ep_s *privep)
{
  irqstate_t flags = irqsave();
  priv->epavail   |= SAM_EP_BIT(USB_EPNO(privep->ep.eplog));
  irqrestore(flags);
}

/****************************************************************************
 * Name: sam_ep_reserved
 *
 * Description:
 *   Check if the endpoint has already been allocated.
 *
 ****************************************************************************/

static inline bool
sam_ep_reserved(struct sam_usbdev_s *priv, int epno)
{
  return ((priv->epavail & SAM_EP_BIT(epno)) == 0);
}

/****************************************************************************
 * Name: sam_ep_configure
 *
 * Description:
 *   This is the internal implementation of the endpoint configuration logic
 *   and implements the endpoint configuration method of the usbdev_ep_s
 *   interface.  As an internal interface, it will be used to configure
 *   endpoint 0 which is not available to the class implementation.
 *
 ****************************************************************************/

static int sam_ep_configure_internal(struct sam_ep_s *privep,
                                     const struct usb_epdesc_s *desc)
{
  struct sam_usbdev_s *priv;
  uint32_t regval;
  uint8_t epno;
  uint8_t eptype;
  uint8_t nbtrans;
  uint8_t maxpacket;
  bool dirin;
  bool highspeed;

  /* Decode the endpoint descriptor */

  epno      = USB_EPNO(desc->addr);
  dirin     = (desc->addr & USB_DIR_MASK) == USB_REQ_DIR_IN;
  eptype    = (desc->type & USB_REQ_TYPE_MASK) >> USB_REQ_TYPE_SHIFT;
  maxpacket = GETUINT16(desc->mxpacketsize);

  /* Special case high-speed endpoints */

  highspeed = ((sam_getreg(SAM_UDPHS_INTSTA) & UDPHS_INTSTA_SPEED) > 0);
  nbtrans   = 1;

  if (highspeed)
    {
      /* HS Interval, 125us */
      /* MPS: Bits 12:11 specify NB_TRANS, as USB 2.0 Spec. */

      nbtrans = ((maxpacket >> 11) & 3);
      if (nbtrans == 3)
        {
          nbtrans = 1;
        }
      else
        {
          nbtrans++;
        }

      /* Mask, bit 10..0 is the size */

       maxpacket &= 0x7ff;
    }

   /* Initialize the endpoint structure */

   privep->ep.eplog     = desc->addr;              /* Includes direction */
   privep->ep.maxpacket = maxpacket;
   privep->epstate      = UDPHS_EPSTATE_IDLE;
   privep->bank         = SAM_UDPHS_NBANKS(epno);

  /* Initialize the endpoint hardware */
  /* Disable the endpoint */

  sam_putreg(UDPHS_EPTCTL_SHRTPCKT | UDPHS_EPTCTL_BUSYBANK |
             UDPHS_EPTCTL_NAKOUT | UDPHS_EPTCTL_NAKIN |
             UDPHS_EPTCTL_STALLSNT | UDPHS_EPTCTL_STALLSNT |
             UDPHS_EPTCTL_TXRDY | UDPHS_EPTCTL_RXRDYTXKL |
             UDPHS_EPTCTL_ERROVFLW | UDPHS_EPTCTL_MDATARX |
             UDPHS_EPTCTL_DATAXRX | UDPHS_EPTCTL_NYETDIS |
             UDPHS_EPTCTL_INTDISDMA | UDPHS_EPTCTL_AUTOVALID |
             UDPHS_EPTCTL_EPTENABL,
             SAM_UDPHS_EPTCTLDIS(epno));

  /* Reset Endpoint Fifos */

  sam_putreg(UDPHS_EPTSTA_TOGGLESQ_MASK | UDPHS_EPTSTA_FRCESTALL,
             SAM_UDPHS_EPTCLRSTA(epno));
  sam_putreg(UDPHS_EPTRST(epno), SAM_UDPHS_EPTRST);

  /* If this is EP0, disable interrupts now */

  if (eptype == USB_EP_ATTR_XFER_CONTROL)
    {
      regval  = sam_getreg(SAM_UDPHS_IEN);
      regval &= ~UDPHS_INT_EPT(epno);
      sam_putreg(regval, SAM_UDPHS_IEN);
    }

  /* Configure the endpoint */

  if (maxpacket <= 8)
    {
      regval = UDPHS_EPTCFG_SIZE_8;
    }
  else if (maxpacket <= 16)
    {
      regval = UDPHS_EPTCFG_SIZE_16;
    }
  else if (maxpacket <= 32)
    {
      regval = UDPHS_EPTCFG_SIZE_32;
    }
  else if (maxpacket <= 64)
    {
      regval = UDPHS_EPTCFG_SIZE_64;
    }
  else if (maxpacket <= 128)
    {
      regval = UDPHS_EPTCFG_SIZE_128;
    }
  else if (maxpacket <= 256)
    {
      regval = UDPHS_EPTCFG_SIZE_256;
    }
  else if (maxpacket <= 512)
    {
      regval = UDPHS_EPTCFG_SIZE_512;
    }
  else if (privep->ep.maxpacket <= 1024)
    {
      regval = UDPHS_EPTCFG_SIZE_1024;
    }
  else
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADEPTYPE), eptype);
      DEBUGPANIC();
      regval = UDPHS_EPTCFG_SIZE_8;
    }

  regval |= ((uint32_t)dirin << 3) | (eptype << 4) |
            ((privep->bank) << 6) | (nbtrans << 8);
  sam_putreg(regval, SAM_UDPHS_EPTCFG(epno));

  /* Verify that the EPT_MAPD flag is set. This flag is set if the
   * endpoint size and the number of banks are correct compared to
   * the FIFO maximum capacity and the maximum number of allowed banks.
   */

  if ((sam_getreg(SAM_UDPHS_EPTCFG(epno)) & UDPHS_EPTCFG_MAPD) == 0)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EPTCFGMAPD), epno);
      return -EINVAL;
    }

  /* Enable the endpoint */

  if (eptype == USB_EP_ATTR_XFER_CONTROL)
    {
      sam_putreg(UDPHS_EPTCTL_RXRDYTXKL | UDPHS_EPTCTL_RXSETUP |
                 UDPHS_EPTCTL_EPTENABL,
                 SAM_UDPHS_EPTCTLENB(epno));
    }
  else
    {
      sam_putreg(UDPHS_EPTCTL_AUTOVALID | UDPHS_EPTCTL_EPTENABL,
                 SAM_UDPHS_EPTCTLENB(epno));
    }

  /* If this was the last endpoint, then the class driver is fully
   * configured.
   */

  priv           = privep->dev;
  priv->devstate = UDPHS_DEVSTATE_CONFIGURED;
  sam_dumpep(priv, epno);
  return OK;
}

/****************************************************************************
 * Endpoint operations
 ****************************************************************************/
/****************************************************************************
 * Name: sam_ep_configure
 *
 * Description:
 *   This is the endpoint configuration method of the usbdev_ep_s interface.
 *
 ****************************************************************************/

static int sam_ep_configure(struct usbdev_ep_s *ep,
                            const struct usb_epdesc_s *desc,
                            bool last)
{
  struct sam_ep_s *privep = (struct sam_ep_s *)ep;

  /* Verify parameters.  Endpoint 0 is not available at this interface */

#if defined(CONFIG_DEBUG) || defined(CONFIG_USBDEV_TRACE)
  uint8_t epno = USB_EPNO(desc->addr);
  usbtrace(TRACE_EPCONFIGURE, (uint16_t)epno);

  DEBUGASSERT(ep && desc && epno > 0 && epno < SAM_UDPHS_NENDPOINTS);
  DEBUGASSERT(epno == USB_EPNO(ep->eplog));
#endif

  /* This logic is implemented in sam_ep_configure_internal */

  return sam_ep_configure_internal(privep, desc);
}

/****************************************************************************
 * Name: sam_ep_disable
 *
 * Description:
 *   This is the disable() method of the USB device endpoint structure.
 *
 ****************************************************************************/

static int sam_ep_disable(struct usbdev_ep_s *ep)
{
  struct sam_ep_s *privep = (struct sam_ep_s *)ep;
  struct sam_usbdev_s *priv;
  irqstate_t flags;
  uint8_t epno;

#ifdef CONFIG_DEBUG
  if (!ep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      ulldbg("ERROR: ep=%p\n", ep);
      return -EINVAL;
    }
#endif

  epno = USB_EPNO(ep->eplog);
  usbtrace(TRACE_EPDISABLE, epno);

  /* Cancel any ongoing activity */

  flags = irqsave();
  sam_req_cancel(privep);

  /* Reset the endpoint */

  priv = privep->dev;
  sam_ep_reset(priv, epno);

  /* Revert to the addressed-but-not-configured state */

  priv->devstate = UDPHS_DEVSTATE_ADDRESS;
  irqrestore(flags);
  return OK;
}

/****************************************************************************
 * Name: sam_ep_allocreq
 *
 * Description:
 *   This is the allocreq() method of the USB device endpoint structure.
 *
 ****************************************************************************/

static struct usbdev_req_s *sam_ep_allocreq(struct usbdev_ep_s *ep)
{
  struct sam_req_s *privreq;

#ifdef CONFIG_DEBUG
  if (!ep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return NULL;
    }
#endif
  usbtrace(TRACE_EPALLOCREQ, USB_EPNO(ep->eplog));

  privreq = (struct sam_req_s *)kmalloc(sizeof(struct sam_req_s));
  if (!privreq)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_ALLOCFAIL), 0);
      return NULL;
    }

  memset(privreq, 0, sizeof(struct sam_req_s));
  return &privreq->req;
}

/****************************************************************************
 * Name: sam_ep_freereq
 *
 * Description:
 *   This is the freereq() method of the USB device endpoint structure.
 *
 ****************************************************************************/

static void sam_ep_freereq(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  struct sam_req_s *privreq = (struct sam_req_s*)req;

#ifdef CONFIG_DEBUG
  if (!ep || !req)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return;
    }
#endif
  usbtrace(TRACE_EPFREEREQ, USB_EPNO(ep->eplog));

  kfree(privreq);
}

/****************************************************************************
 * Name: sam_ep_submit
 *
 * Description:
 *   This is the allocbuffer() method of the USB device endpoint structure.
 *
 ****************************************************************************/

#ifdef CONFIG_USBDEV_DMA
static void *sam_ep_allocbuffer(struct usbdev_ep_s *ep, uint16_t nbytes)
{
  /* There is not special buffer allocation requirement */

  return kumalloc(nbytes);
}
#endif

/****************************************************************************
 * Name: sam_ep_submit
 *
 * Description:
 *   This is the freebuffer() method of the USB device endpoint structure.
 *
 ****************************************************************************/

#ifdef CONFIG_USBDEV_DMA
static void sam_ep_freebuffer(struct usbdev_ep_s *ep, void *buf)
{
  /* There is not special buffer allocation requirement */

  kufree(buf);
}
#endif

/****************************************************************************
 * Name: sam_ep_submit
 *
 * Description:
 *   This is the submit() method of the USB device endpoint structure.
 *
 ****************************************************************************/

static int sam_ep_submit(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  struct sam_req_s *privreq = (struct sam_req_s *)req;
  struct sam_ep_s *privep = (struct sam_ep_s *)ep;
  struct sam_usbdev_s *priv;
  irqstate_t flags;
  uint8_t epno;
  int ret = OK;

#ifdef CONFIG_DEBUG
  if (!req || !req->callback || !req->buf || !ep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      ulldbg("ERROR: req=%p callback=%p buf=%p ep=%p\n", req, req->callback, req->buf, ep);
      return -EINVAL;
    }
#endif

  usbtrace(TRACE_EPSUBMIT, USB_EPNO(ep->eplog));
  priv = privep->dev;

#ifdef CONFIG_DEBUG
  if (!priv->driver)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_NOTCONFIGURED), priv->usbdev.speed);
      ulldbg("ERROR: driver=%p\n", priv->driver);
      return -ESHUTDOWN;
    }
#endif

  /* Handle the request from the class driver */

  epno              = USB_EPNO(ep->eplog);
  req->result       = -EINPROGRESS;
  req->xfrd         = 0;
  privreq->inflight = 0;
  flags             = irqsave();

  /* If we are stalled, then drop all requests on the floor */

  if (privep->stalled)
    {
      sam_req_abort(privep, privreq, -EBUSY);
      ulldbg("ERROR: stalled\n");
      ret = -EBUSY;
    }

  /* Handle IN (device-to-host) requests.  NOTE:  If the class device is
   * using the bi-directional EP0, then we assume that they intend the EP0
   * IN functionality.
   */

  else if (USB_ISEPIN(ep->eplog) || epno == EP0)
    {
      /* Add the new request to the request queue for the IN endpoint */

      sam_req_enqueue(privep, privreq);
      usbtrace(TRACE_INREQQUEUED(epno), req->len);

      /* If the IN endpoint FIFO is available, then transfer the data now */

      if (privep->epstate == UDPHS_EPSTATE_IDLE)
        {
          ret = sam_req_write(priv, privep);
        }
    }

  /* Handle OUT (host-to-device) requests */

  else
    {
      /* Add the new request to the request queue for the OUT endpoint */

      privep->txnullpkt = 0;
      sam_req_enqueue(privep, privreq);
      usbtrace(TRACE_OUTREQQUEUED(epno), req->len);

      /* This there a incoming data pending the availability of a request? */

      if (priv->rxpending)
        {
          /* NAK any OUT request addressed to the endpoint */
#warning Missing logic

          /* Data is no longer pending */

          priv->rxpending = false;
        }
    }

  irqrestore(flags);
  return ret;
}

/****************************************************************************
 * Name: sam_ep_cancel
 ****************************************************************************/

static int sam_ep_cancel(struct usbdev_ep_s *ep, struct usbdev_req_s *req)
{
  struct sam_ep_s *privep = (struct sam_ep_s *)ep;
  irqstate_t flags;

#ifdef CONFIG_DEBUG
  if (!ep || !req)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif
  usbtrace(TRACE_EPCANCEL, USB_EPNO(ep->eplog));

  flags = irqsave();
  sam_req_cancel(privep);
  irqrestore(flags);
  return OK;
}

/****************************************************************************
 * Name: sam_ep_stall
 ****************************************************************************/

static int sam_ep_stall(struct usbdev_ep_s *ep, bool resume)
{
  struct sam_ep_s *privep;
  struct sam_usbdev_s *priv;
  uint8_t epno = USB_EPNO(ep->eplog);
  uint32_t regval;
  irqstate_t flags;

#ifdef CONFIG_DEBUG
  if (!ep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  /* Check that endpoint is in Idle state */

  privep = (struct sam_ep_s *)ep;
  DEBUGASSERT(privep->epstate == UDPHS_EPSTATE_IDLE && privep->dev);

  priv   = (struct sam_usbdev_s *)privep->dev;
  epno   = USB_EPNO(ep->eplog);

  /* STALL or RESUME the endpoint */

  flags = irqsave();
  usbtrace(resume ? TRACE_EPRESUME : TRACE_EPSTALL, USB_EPNO(ep->eplog));

  /* Handle the resume condition */

  if (resume)
    {
      /* Check if the endpoint is halted */

      if (privep->epstate == UDPHS_EPSTATE_STALLED)
        {
          usbtrace(TRACE_EPRESUME, epno);
          privep->stalled = false;

          /* Return endpoint to Idle state */

          privep->epstate = UDPHS_EPSTATE_IDLE;

          /* Clear FORCESTALL flag */

          sam_putreg(UDPHS_EPTSTA_TOGGLESQ_MASK | UDPHS_EPTSTA_FRCESTALL,
                     SAM_UDPHS_EPTCLRSTA(epno));

          /* Reset endpoint FIFOs */

          sam_putreg(UDPHS_EPTRST(epno), SAM_UDPHS_EPTRST);

          /* Resuming any blocked data transfers on the endpoint */

          if (USB_ISEPIN(ep->eplog))
            {
              /* IN endpoint */
              /* Restart any queued write requests */

              (void)sam_req_write(priv, privep);
            }
        }
    }

  /* Handle the stall condition */

  else
    {
      /* Check that endpoint is enabled and not already in Halt state */

      if ((privep->epstate != UDPHS_EPSTATE_DISABLED) &&
          (privep->epstate != UDPHS_EPSTATE_STALLED))
        {
          usbtrace(TRACE_EPSTALL, epno);

          /* Abort the current transfer if necessary */

          sam_req_complete(privep, -EIO);

          /* Put endpoint into stalled state */

          privep->epstate = UDPHS_EPSTATE_STALLED;
          privep->stalled = true;

          sam_putreg(UDPHS_EPTSETSTA_FRCESTALL, SAM_UDPHS_EPTSETSTA(epno));

          /* Enable endpoint/DMA interrupts */

          regval = sam_getreg(SAM_UDPHS_IEN);
          if ((SAM_EPSET_DMA & SAM_EP_BIT(epno)) != 0)
            {
              /* Enable the endpoint DMA interrupt */

              regval &= ~UDPHS_INT_DMA(epno);
            }
          else
            {
              /* Enable the endpoint interrupt */

              regval &= ~UDPHS_INT_EPT(epno);
            }
          sam_putreg(regval, SAM_UDPHS_IEN);
        }
    }

  irqrestore(flags);
  return OK;
}

/****************************************************************************
 * Device Controller Operations
 ****************************************************************************/
/****************************************************************************
 * Name: sam_allocep
 *
 * Description:
 *   This is the allocep() method of the USB device driver interface
 *
 ****************************************************************************/

static struct usbdev_ep_s *sam_allocep(struct usbdev_s *dev, uint8_t epno,
                                         bool in, uint8_t eptype)
{
  struct sam_usbdev_s *priv = (struct sam_usbdev_s *)dev;
  struct sam_ep_s *privep = NULL;
  uint16_t epset = SAM_EPSET_NOTEP0;

  usbtrace(TRACE_DEVALLOCEP, (uint16_t)epno);
#ifdef CONFIG_DEBUG
  if (!dev)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return NULL;
    }
#endif

  /* Ignore any direction bits in the logical address */

  epno = USB_EPNO(epno);

  /* A logical address of 0 means that any endpoint will do */

  if (epno > 0)
    {
      /* Otherwise, we will return the endpoint structure only for the requested
       * 'logical' endpoint.  All of the other checks will still be performed.
       *
       * First, verify that the logical endpoint is in the range supported by
       * by the hardware.
       */

      if (epno >= SAM_UDPHS_NENDPOINTS)
        {
          usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BADEPNO), (uint16_t)epno);
          return NULL;
        }

      /* Convert the logical address to a physical OUT endpoint address and
       * remove all of the candidate endpoints from the bitset except for the
       * the IN/OUT pair for this logical address.
       */

      epset = SAM_EP_BIT(epno);
    }

  /* Check if the selected endpoint number is available */

  privep = sam_ep_reserve(priv, epset);
  if (!privep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_EPRESERVE), (uint16_t)epset);
      return NULL;
    }

  return &privep->ep;
}

/****************************************************************************
 * Name: sam_freeep
 *
 * Description:
 *   This is the freeep() method of the USB device driver interface
 *
 ****************************************************************************/

static void sam_freeep(struct usbdev_s *dev, struct usbdev_ep_s *ep)
{
  struct sam_usbdev_s *priv;
  struct sam_ep_s *privep;

#ifdef CONFIG_DEBUG
  if (!dev || !ep)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return;
    }
#endif
  priv   = (struct sam_usbdev_s *)dev;
  privep = (struct sam_ep_s *)ep;
  usbtrace(TRACE_DEVFREEEP, (uint16_t)USB_EPNO(ep->eplog));

  if (priv && privep)
    {
      /* Mark the endpoint as available */

      sam_ep_unreserve(priv, privep);
    }
}

/****************************************************************************
 * Name: sam_getframe
 *
 * Description:
 *   This is the getframe() method of the USB device driver interface
 *
 ****************************************************************************/

static int sam_getframe(struct usbdev_s *dev)
{
  uint32_t regval;
  uint16_t frameno;

#ifdef CONFIG_DEBUG
  if (!dev)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  /* Return the last frame number detected by the hardware */

  regval  = sam_getreg(SAM_UDPHS_FNUM);
  frameno = (regval & UDPHS_FNUM_FRAMENUM_MASK) >> UDPHS_FNUM_FRAMENUM_SHIFT;

  usbtrace(TRACE_DEVGETFRAME, frameno);
  return frameno;
}

/****************************************************************************
 * Name: sam_wakeup
 *
 * Description:
 *   This is the wakeup() method of the USB device driver interface
 *
 ****************************************************************************/

static int sam_wakeup(struct usbdev_s *dev)
{
  struct sam_usbdev_s *priv = (struct sam_usbdev_s *)dev;
  irqstate_t flags;
  uint32_t regval;

  usbtrace(TRACE_DEVWAKEUP, 0);
#ifdef CONFIG_DEBUG
  if (!dev)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  /* Resume normal operation */

  flags = irqsave();
  sam_resume(priv);

  /* Activate a remote wakeup.  Setting this bit forces an external interrupt
   * on the UDPHS controller for Remote Wake UP purposes.  An Upstream Resume
   * is sent only after the UDPHS bus has been in SUSPEND state for at least 5
   * ms.
   */

  regval  = sam_getreg(SAM_UDPHS_CTRL);
  regval |= UDPHS_CTRL_REWAKEUP;
  sam_putreg(regval, SAM_UDPHS_CTRL);
  irqrestore(flags);

  /* This bit is automatically cleared by hardware at the end of the Upstream
   * Resume
   */

  while ((sam_getreg(SAM_UDPHS_CTRL) & UDPHS_CTRL_REWAKEUP) != 0);
  return OK;
}

/****************************************************************************
 * Name: sam_selfpowered
 *
 * Description:
 *   This is the selfpowered() method of the USB device driver interface
 *
 ****************************************************************************/

static int sam_selfpowered(struct usbdev_s *dev, bool selfpowered)
{
  struct sam_usbdev_s *priv = (struct sam_usbdev_s *)dev;

  usbtrace(TRACE_DEVSELFPOWERED, (uint16_t)selfpowered);

#ifdef CONFIG_DEBUG
  if (!dev)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -ENODEV;
    }
#endif

  priv->selfpowered = selfpowered;
  return OK;
}

/****************************************************************************
 * Name: sam_pullup
 *
 * Description:
 *   This is the pullup() method of the USB device driver interface
 *
 ****************************************************************************/

static int sam_pullup(FAR struct usbdev_s *dev, bool enable)
{
  struct sam_usbdev_s *priv = (struct sam_usbdev_s *)dev;
  uint32_t regval;

  usbtrace(TRACE_DEVPULLUP, (uint16_t)enable);

  /* DETACH PULLD_DIS DP        DM         Condition
   *
   *   0         1    Pull      High       VBUS present
   *                  Up        Impedance
   *   1         0    Pull      Pull       No VBUS
   *                  Down      Down
   *   1         1    High      High       VBUS present +
   *                  Impedance Imedpance  Disconnect
   */

  regval = sam_getreg(SAM_UDPHS_CTRL);
  if (enable)
    {
      /* PULLD_DIS=1: No pull-Down on DP and DM */

      regval |= UDPHS_CTRL_PULLDDIS;
      sam_putreg(regval, SAM_UDPHS_CTRL);

      /* DETACH=0: UDPHS is attached.  Pulls up the DP line */

      regval &= ~UDPHS_CTRL_DETACH;
      sam_putreg(regval, SAM_UDPHS_CTRL);
    }
  else
    {
      /* DETACH=1: UDPHS is detached, UTMI transceiver is suspended. */

      regval |= UDPHS_CTRL_DETACH;
      sam_putreg(regval, SAM_UDPHS_CTRL);

      /* PULLD_DIS=0: Pull-Down on DP & DM */

      regval &= ~UDPHS_CTRL_PULLDDIS;
      sam_putreg(regval, SAM_UDPHS_CTRL);

      /* Device returns to the Powered state */

      if (priv->devstate > UDPHS_DEVSTATE_POWERED)
        {
          priv->devstate = UDPHS_DEVSTATE_POWERED;
        }
    }

  return OK;
}

/****************************************************************************
 * Initialization/Reset
 ****************************************************************************/

/****************************************************************************
 * Name: sam_reset
 ****************************************************************************/

static void sam_reset(struct sam_usbdev_s *priv)
{
  uint8_t epno;

  /* Make sure that clocking is enabled to the UDPHS peripheral.
   *
   * NOTE: In the Atmel example code, they also enable USB clocking
   * at this point (via the BIAS in the CKGR_UCKR register).  In this
   * implementation, that should not be necessary here because we
   * never disable BIAS to begin with.
   */

  sam_udphs_enableclk();

  /* Tell the class driver that we are disconnected.  The class driver
   * should then accept any new configurations.
   */

  CLASS_DISCONNECT(priv->driver, &priv->usbdev);

  /* The device enters the Default state */

  priv->devaddr   = 0;
  sam_setdevaddr(priv, 0);

  priv->devstate  = UDPHS_DEVSTATE_DEFAULT;
  priv->rxpending = false;

  /* Reset and disable all endpoints other.  Then re-configure EP0 */

  sam_epset_reset(priv, SAM_EPSET_ALL);
  sam_ep_configure_internal(&priv->eplist[EP0], &g_ep0desc);

  /* Reset endpoint data structures */

  for (epno = 0; epno < SAM_UDPHS_NENDPOINTS; epno++)
    {
      struct sam_ep_s *privep = &priv->eplist[epno];

      /* Cancel any queued requests.  Since they are canceled
       * with status -ESHUTDOWN, then will not be requeued
       * until the configuration is reset.  NOTE:  This should
       * not be necessary... the CLASS_DISCONNECT above should
       * result in the class implementation calling sam_ep_disable
       * for each of its configured endpoints.
       */

      sam_req_cancel(privep);

      /* Reset endpoint status */

      privep->stalled   = false;
      privep->halted    = false;
      privep->txnullpkt = false;
    }

  /* Re-configure the USB controller in its initial, unconnected state */

  priv->usbdev.speed = USB_SPEED_FULL;
  sam_dumpep(priv, EP0);
}

/****************************************************************************
 * Name: sam_hw_setup
 ****************************************************************************/

static void sam_hw_setup(struct sam_usbdev_s *priv)
{
  uint32_t regval;
  int i;

  /* Paragraph 32.5.1, "Power Management".  The UDPHS is not continuously
   * clocked.  For using the UDPHS, the programmer must first enable the
   * UDPHS Clock in the Power Management Controller (PMC_PCER register).
   * Then enable the PLL (PMC_UCKR register). Finally, enable BIAS in
   * PMC_UCKR register. However, if the application does not require UDPHS
   * operations, the UDPHS clock can be stopped when not needed and
   * restarted later.
   *
   * Here, we set only the PCER.  PLL configuration was performed in
   * sam_clockconfig() earlier in the boot sequence.
   */

  sam_udphs_enableclk();

  /* Reset and disable endpoints */

  sam_epset_reset(priv, SAM_EPSET_ALL);

  /* Configure the pull-up on D+ and disconnect it */

  regval  = sam_getreg(SAM_UDPHS_CTRL);
  regval |= UDPHS_CTRL_DETACH;
  sam_putreg(regval, SAM_UDPHS_CTRL);

  regval &= ~UDPHS_CTRL_PULLDDIS;
  sam_putreg(regval, SAM_UDPHS_CTRL);

  /* Reset the UDPHS block
   *
   * Paragraph 33.5.1.  "One transceiver is shared with the USB High Speed
   *   Device (port A). The selection between Host Port A and USB Device is
   *   controlled by the UDPHS enable bit (EN_UDPHS) located in the UDPHS_CTRL
   *   control register.
   *
   *  "In the case the port A is driven by the USB High Speed Device, the ...
   *   transceiver is automatically selected for Device operation once the
   *   USB High Speed Device is enabled."
   */

  regval &= ~UDPHS_CTRL_ENUDPHS;
  sam_putreg(regval, SAM_UDPHS_CTRL);

  regval |= UDPHS_CTRL_ENUDPHS;
  sam_putreg(regval, SAM_UDPHS_CTRL);

  /* REVISIT: Per recommendations and sample code, USB clocking (as
   * configured in the PMC CKGR_UCKR) is set up after reseting the UDHPS.
   * However, that initialation has already been done in sam_clockconfig().
   * Also, that clocking is shared with the UHPHS USB host logic; the
   * device logica cannot autonomously control USB clocking.
   */

  /* Initialize DMA channels */

  for (i = 1; i <= SAM_UDPHS_NDMACHANNELS; i++)
    {
      /* Stop any DMA transfer */

      sam_putreg(0, SAM_UDPHS_DMACONTROL(i));

      /* Reset DMA channel (Buffer count and Control field) */

      sam_putreg(UDPHS_DMACONTROL_LDNXTDSC, SAM_UDPHS_DMACONTROL(i));

      /* Reset DMA channel */

      sam_putreg(0, SAM_UDPHS_DMACONTROL(i));

      /* Clear DMA channel status (read to clear) */

      regval = sam_getreg(SAM_UDPHS_DMASTATUS(i));
      sam_putreg(regval, SAM_UDPHS_DMACONTROL(i));
    }

  /* Initialize Endpoints */

  for (i = 0; i < SAM_UDPHS_NENDPOINTS; i++)
    {
      /* Disable endpoint */

      regval = UDPHS_EPTCTL_SHRTPCKT | UDPHS_EPTCTL_BUSYBANK |
               UDPHS_EPTCTL_NAKOUT | UDPHS_EPTCTL_NAKIN |
               UDPHS_EPTCTL_STALLSNT | UDPHS_EPTCTL_STALLSNT |
               UDPHS_EPTCTL_TXRDY | UDPHS_EPTCTL_TXCOMPLT |
               UDPHS_EPTCTL_RXRDYTXKL | UDPHS_EPTCTL_ERROVFLW |
               UDPHS_EPTCTL_MDATARX | UDPHS_EPTCTL_DATAXRX |
               UDPHS_EPTCTL_NYETDIS | UDPHS_EPTCTL_INTDISDMA |
               UDPHS_EPTCTL_AUTOVALID | UDPHS_EPTCTL_EPTENABL;
      sam_putreg(regval, SAM_UDPHS_EPTCTLDIS(i));

      /* Clear endpoint status */

      regval = UDPHS_EPTSTA_TOGGLESQ_MASK | UDPHS_EPTSTA_FRCESTALL |
               UDPHS_EPTSTA_RXRDYTXKL | UDPHS_EPTSTA_TXCOMPLT |
               UDPHS_EPTSTA_RXSETUP | UDPHS_EPTSTA_STALLSNT |
               UDPHS_EPTSTA_NAKIN | UDPHS_EPTSTA_NAKOUT;
      sam_putreg(regval, SAM_UDPHS_EPTCLRSTA(i));

      /* Reset endpoint configuration */

      sam_putreg(0, SAM_UDPHS_EPTCTLENB(i));
    }

  /* Normal mode (full speed not forced) */

  sam_putreg(0, SAM_UDPHS_TST);

  /* Disable all interrupts */

  sam_putreg(0, SAM_UDPHS_IEN);

  /* Clear all pending interrupt status */

  regval = UDPHS_INT_UPSTRRES | UDPHS_INT_ENDOFRSM | UDPHS_INT_WAKEUP |
           UDPHS_INT_ENDRESET | UDPHS_INT_INTSOF | UDPHS_INT_MICROSOF |
           UDPHS_INT_DETSUSPD;
  sam_putreg(regval, SAM_UDPHS_CLRINT);

  /* Enable interrupts */

  regval = UDPHS_INT_ENDOFRSM | UDPHS_INT_WAKEUP | UDPHS_INT_DETSUSPD;
  sam_putreg(regval, SAM_UDPHS_IEN);

  /* The Atmel sample code disables USB clocking here (via the PMC
   * CKGR_UCKR).  However, we cannot really do that here because that
   * clocking is also needed by the UHPHS host.
   */
}

/****************************************************************************
 * Name: sam_sw_setup
 ****************************************************************************/

static void sam_sw_setup(struct sam_usbdev_s *priv)
{
  int epno;

#ifdef CONFIG_SAMA5_UDPHS_SCATTERGATHER
#ifndef CONFIG_SAMA5_EHCI_PREALLOCATE
  int i;

  /* Allocate a pool of free DMA transfer descriptors */

  priv->dtdpool = (struct sam_dtd_s *)
    kmemalign(16, CONFIG_SAMA5_UDPHS_NDTDS * sizeof(struct sam_dtd_s));
  if (!priv->dtdpool)
     {
      udbg("ERROR: Failed to allocate the DMA transfer descriptor pool\n");
      return NULL;
    }

  /* Initialize the list of free DMA transfer descriptors */

  for (i = 0; i < CONFIG_SAMA5_UDPHS_NDTDS; i++)
    {
      /* Put the transfer descriptor in a free list */

      sam_td_free(&priv->dtdpool[i]);
    }

#else
  /* Initialize the list of free DMA transfer descriptors */

  DEBUGASSERT(((uintptr_t)&g_dtdpool & 15) == 0);
  for (i = 0; i < CONFIG_SAMA5_UDPHS_NDTDS; i++)
    {
      /* Put the transfer descriptor in a free list */

      sam_td_free(&g_dtdpool[i]);
    }

#endif /* CONFIG_SAMA5_EHCI_PREALLOCATE */
#endif /* CONFIG_SAMA5_UDPHS_SCATTERGATHER */

  /* Initialize the device state structure.  NOTE: many fields
   * have the initial value of zero and, hence, are not explicitly
   * initialized here.
   */

  memset(priv, 0, sizeof(struct sam_usbdev_s));
  priv->usbdev.ops = &g_devops;
  priv->usbdev.ep0 = &priv->eplist[EP0].ep;
  priv->epavail    = SAM_EPSET_ALL & ~SAM_EP_BIT(EP0);
  priv->devstate   = UDPHS_DEVSTATE_SUSPENDED;
  priv->prevstate  = UDPHS_DEVSTATE_POWERED;

  /* Initialize the endpoint list */

  for (epno = 0; epno < SAM_UDPHS_NENDPOINTS; epno++)
    {
      /* Set endpoint operations, reference to driver structure (not
       * really necessary because there is only one controller), and
       * the (physical) endpoint number which is just the index to the
       * endpoint.
       */

      priv->eplist[epno].ep.ops    = &g_epops;
      priv->eplist[epno].dev       = priv;
      priv->eplist[epno].ep.eplog  = epno;

      /* We will use a maxpacket size for supported for each endpoint */

      priv->eplist[epno].ep.maxpacket = SAM_UDPHS_MAXPACKETSIZE(epno);
    }

  /* Select a smaller endpoint size for EP0 */

#if SAM_EP0MAXPACKET < SAM_MAXPACKET_SIZE
  priv->eplist[EP0].ep.maxpacket = SAM_EP0MAXPACKET;
#endif
}

/****************************************************************************
 * Name: sam_hw_shutdown
 ****************************************************************************/

static void sam_hw_shutdown(struct sam_usbdev_s *priv)
{
  uint32_t regval;

  priv->usbdev.speed = USB_SPEED_UNKNOWN;

  /* Disable all interrupts */

  sam_putreg(0, SAM_UDPHS_IEN);

  /* Clear all pending interrupt status */

  regval = UDPHS_INT_UPSTRRES | UDPHS_INT_ENDOFRSM | UDPHS_INT_WAKEUP |
           UDPHS_INT_ENDRESET | UDPHS_INT_INTSOF | UDPHS_INT_MICROSOF |
           UDPHS_INT_DETSUSPD;
  sam_putreg(regval, SAM_UDPHS_CLRINT);

  /* Disconnect the device / disable the pull-up */

  sam_pullup(&priv->usbdev, false);

  /* Disable clocking to the UDPHS peripheral */

  sam_udphs_disableclk();
}

/****************************************************************************
 * Name: sam_sw_shutdown
 ****************************************************************************/

static void sam_sw_shutdown(struct sam_usbdev_s *priv)
{
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/
/****************************************************************************
 * Name: up_usbinitialize
 * Description:
 *   Initialize the USB driver
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_usbinitialize(void)
{
  /* For now there is only one USB controller, but we will always refer to
   * it using a pointer to make any future ports to multiple USB controllers
   * easier.
   */

  struct sam_usbdev_s *priv = &g_udphs;

  usbtrace(TRACE_DEVINIT, 0);

  /* Software initialization */

  sam_sw_setup(priv);

  /* Power up and initialize USB controller, but leave it in the reset
   * state.  Interrupts from the UDPHS controller are initialized here,
   * but will not be enabled at the AIC until the class driver is installed.
   */

  sam_hw_setup(priv);

  /* Attach USB controller interrupt handlers.  The hardware will not be
   * initialized and interrupts will not be enabled until the class device
   * driver is bound.  Getting the IRQs here only makes sure that we have
   * them when we need them later.
   */

  if (irq_attach(SAM_IRQ_UDPHS, sam_udphs_interrupt) != 0)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_IRQREGISTRATION),
               (uint16_t)SAM_IRQ_UDPHS);
      goto errout;
    }

  return;

errout:
  up_usbuninitialize();
}

/****************************************************************************
 * Name: up_usbuninitialize
 * Description:
 *   Initialize the USB driver
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_usbuninitialize(void)
{
  /* For now there is only one USB controller, but we will always refer to
   * it using a pointer to make any future ports to multiple USB controllers
   * easier.
   */

  struct sam_usbdev_s *priv = &g_udphs;
  irqstate_t flags;

  flags = irqsave();
  usbtrace(TRACE_DEVUNINIT, 0);

  /* Disable and detach the UDPHS IRQ */

  up_disable_irq(SAM_IRQ_UDPHS);
  irq_detach(SAM_IRQ_UDPHS);

  if (priv->driver)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_DRIVERREGISTERED), 0);
      usbdev_unregister(priv->driver);
    }

  /* Put the hardware in an inactive state */

  sam_hw_shutdown(priv);
  sam_sw_shutdown(priv);
  irqrestore(flags);
}

/****************************************************************************
 * Name: usbdev_register
 *
 * Description:
 *   Register a USB device class driver. The class driver's bind() method will be
 *   called to bind it to a USB device driver.
 *
 ****************************************************************************/

int usbdev_register(struct usbdevclass_driver_s *driver)
{
  /* For now there is only one USB controller, but we will always refer to
   * it using a pointer to make any future ports to multiple USB controllers
   * easier.
   */

  struct sam_usbdev_s *priv = &g_udphs;
  int ret;

  usbtrace(TRACE_DEVREGISTER, 0);

#ifdef CONFIG_DEBUG
  if (!driver || !driver->ops->bind || !driver->ops->unbind ||
      !driver->ops->disconnect || !driver->ops->setup)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }

  if (priv->driver)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_DRIVER), 0);
      return -EBUSY;
    }
#endif

  /* First hook up the driver */

  priv->driver = driver;

  /* Then bind the class driver */

  ret = CLASS_BIND(driver, &priv->usbdev);
  if (ret)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_BINDFAILED), (uint16_t)-ret);
      priv->driver = NULL;
    }
  else
    {
      /* Setup the USB controller -- enabling interrupts at the USB controller */

      sam_reset(priv);

      /* Enable USB controller interrupts at the NVIC */

      up_enable_irq(SAM_IRQ_UDPHS);

      /* Enable pull-up to connect the device.  The host should enumerate us
       * some time after this
       */

      sam_pullup(&priv->usbdev, true);
      priv->usbdev.speed = USB_SPEED_FULL;
   }

  return ret;
}

/****************************************************************************
 * Name: usbdev_unregister
 *
 * Description:
 *   Un-register usbdev class driver. If the USB device is connected to a
 *   USB host, it will first disconnect().  The driver is also requested to
 *   unbind() and clean up any device state, before this procedure finally
 *   returns.
 *
 ****************************************************************************/

int usbdev_unregister(struct usbdevclass_driver_s *driver)
{
  /* For now there is only one USB controller, but we will always refer to
   * it using a pointer to make any future ports to multiple USB controllers
   * easier.
   */

  struct sam_usbdev_s *priv = &g_udphs;
  irqstate_t flags;

  usbtrace(TRACE_DEVUNREGISTER, 0);

#ifdef CONFIG_DEBUG
  if (driver != priv->driver)
    {
      usbtrace(TRACE_DEVERROR(SAM_TRACEERR_INVALIDPARMS), 0);
      return -EINVAL;
    }
#endif

  /* Reset the hardware and cancel all requests.  All requests must be
   * canceled while the class driver is still bound.
   */

  flags = irqsave();
  sam_reset(priv);

  /* Unbind the class driver */

  CLASS_UNBIND(driver, &priv->usbdev);

  /* Disable USB controller interrupts (but keep them attached) */

  up_disable_irq(SAM_IRQ_UDPHS);

  /* Put the hardware in an inactive state.  Then bring the hardware back up
   * in the reset state (this is probably not necessary, the sam_reset()
   * call above was probably sufficient).
   */

  sam_hw_shutdown(priv);
  sam_sw_shutdown(priv);

  sam_sw_setup(priv);
  sam_hw_setup(priv);

  /* Unhook the driver */

  priv->driver = NULL;
  irqrestore(flags);
  return OK;
}

#endif /* CONFIG_USBDEV && CONFIG_SAMA5_UDPHS */
