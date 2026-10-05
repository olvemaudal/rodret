/*
 * dci.c - EFR32 Series 2 (xG21) DCI driver in pure C, over a CMSIS-DAP probe.
 *
 * A direct port of dci.py. Where dci.py leans on pyOCD to talk to the probe,
 * this talks CMSIS-DAP v2 (USB bulk) to the probe itself via libusb, implements
 * the ADIv5 DP/AP register transfers, and layers the DCI mailbox protocol on top.
 *
 *   dci status   - read Secure Engine lock status (non-destructive)
 *   dci erase    - device erase: wipes all flash+RAM, clears the debug lock
 *
 * Build:  make           (see Makefile), or:
 *   cc -O2 -Wall dci.c -o dci \
 *      -I/opt/homebrew/opt/libusb/include/libusb-1.0 \
 *      -L/opt/homebrew/opt/libusb/lib -lusb-1.0
 *
 * This is deliberately written to read like the Python version; see JOURNAL.md
 * and README.md for the protocol explanation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <libusb.h>

/* ------------------------------------------------------------------ *
 *  CMSIS-DAP command IDs (subset we need)                            *
 * ------------------------------------------------------------------ */
#define DAP_Info               0x00
#define DAP_Connect            0x02
#define DAP_Disconnect         0x03
#define DAP_TransferConfigure  0x04
#define DAP_Transfer           0x05
#define DAP_SWJ_Clock          0x11
#define DAP_SWJ_Sequence       0x12

#define DAP_PORT_SWD           1

/* ADIv5 transfer request bits (CMSIS-DAP DAP_Transfer) */
#define REQ_APnDP   (1u << 0)
#define REQ_RnW     (1u << 1)
/* register address bits [3:2] live in bits [3:2] of the request byte  */

/* SWD ACK (low 3 bits of the Transfer Response byte) */
#define ACK_OK      0x1

/* ------------------------------------------------------------------ *
 *  USB / CMSIS-DAP transport                                         *
 * ------------------------------------------------------------------ */
static libusb_context       *g_ctx;
static libusb_device_handle *g_dev;
static int   g_iface   = -1;
static uint8_t g_ep_out = 0, g_ep_in = 0;
static uint16_t g_pktsize = 512;

static void die(const char *msg) { fprintf(stderr, "error: %s\n", msg); exit(1); }

/* Find a CMSIS-DAP v2 interface: a vendor (0xFF) interface whose string
 * descriptor contains "CMSIS-DAP", with bulk OUT+IN endpoints. */
static int open_probe(void)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(g_ctx, &list);
    if (n < 0) return -1;

    for (ssize_t i = 0; i < n && g_dev == NULL; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;

        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) continue;

        libusb_device_handle *h = NULL;
        if (libusb_open(list[i], &h) != 0) { libusb_free_config_descriptor(cfg); continue; }

        for (int ii = 0; ii < cfg->bNumInterfaces && g_dev == NULL; ii++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[ii].altsetting[0];
            if (id->bInterfaceClass != 0xFF) continue;      /* vendor-specific */

            unsigned char name[128] = {0};
            if (id->iInterface) {
                libusb_get_string_descriptor_ascii(h, id->iInterface, name, sizeof name);
            }
            if (strstr((char *)name, "CMSIS-DAP") == NULL) continue;

            uint8_t epo = 0, epi = 0; uint16_t mps = 512;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                if ((ep->bmAttributes & 0x3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep->bEndpointAddress & 0x80) { epi = ep->bEndpointAddress; }
                else                             { epo = ep->bEndpointAddress; }
                mps = ep->wMaxPacketSize;
            }
            if (!epo || !epi) continue;

            /* claim it */
            libusb_set_auto_detach_kernel_driver(h, 1);
            if (libusb_claim_interface(h, id->bInterfaceNumber) != 0) continue;

            g_dev = h; g_iface = id->bInterfaceNumber;
            g_ep_out = epo; g_ep_in = epi; g_pktsize = mps ? mps : 512;
            printf("CMSIS-DAP v2 probe: %04x:%04x iface %d (out=0x%02x in=0x%02x)\n",
                   dd.idVendor, dd.idProduct, g_iface, g_ep_out, g_ep_in);
        }
        libusb_free_config_descriptor(cfg);
        if (g_dev != h) libusb_close(h);
    }
    libusb_free_device_list(list, 1);
    return g_dev ? 0 : -1;
}

/* One CMSIS-DAP command/response round trip over bulk. Returns response len. */
static int dap_xfer(const uint8_t *cmd, int clen, uint8_t *rsp, int rmax)
{
    int tx = 0;
    if (libusb_bulk_transfer(g_dev, g_ep_out, (uint8_t *)cmd, clen, &tx, 2000) != 0 || tx != clen)
        die("USB bulk write failed");
    int rx = 0;
    if (libusb_bulk_transfer(g_dev, g_ep_in, rsp, rmax, &rx, 2000) != 0)
        die("USB bulk read failed");
    if (rx < 1 || rsp[0] != cmd[0])
        die("CMSIS-DAP response mismatch");
    return rx;
}

/* ------------------------------------------------------------------ *
 *  DAP setup                                                         *
 * ------------------------------------------------------------------ */
static void dap_setup(void)
{
    uint8_t c[64], r[64];

    /* Connect in SWD mode */
    c[0] = DAP_Connect; c[1] = DAP_PORT_SWD;
    dap_xfer(c, 2, r, sizeof r);
    if (r[1] != DAP_PORT_SWD) die("probe failed to select SWD");

    /* SWJ clock = 1 MHz (little-endian u32) */
    uint32_t hz = 1000000;
    c[0] = DAP_SWJ_Clock;
    c[1] = hz & 0xff; c[2] = (hz >> 8) & 0xff; c[3] = (hz >> 16) & 0xff; c[4] = (hz >> 24) & 0xff;
    dap_xfer(c, 5, r, sizeof r);

    /* TransferConfigure: idle=0, wait retry=100, match retry=0 */
    c[0] = DAP_TransferConfigure; c[1] = 0; c[2] = 100; c[3] = 0; c[4] = 0; c[5] = 0;
    dap_xfer(c, 6, r, sizeof r);

    /* Line reset + JTAG-to-SWD switch via SWJ_Sequence:
     *   56 clk high, 0x9E 0xE7, 56 clk high, 16 idle                 */
    uint8_t seq[1 + 1 + 18];
    int p = 0;
    seq[p++] = DAP_SWJ_Sequence;
    seq[p++] = 144;                               /* bit count */
    for (int i = 0; i < 7; i++) seq[p++] = 0xFF;  /* 56 ones  */
    seq[p++] = 0x9E; seq[p++] = 0xE7;             /* switch   */
    for (int i = 0; i < 7; i++) seq[p++] = 0xFF;  /* 56 ones  */
    seq[p++] = 0x00; seq[p++] = 0x00;             /* 16 idle  */
    dap_xfer(seq, p, r, sizeof r);
}

/* ------------------------------------------------------------------ *
 *  ADIv5 register access via DAP_Transfer                            *
 * ------------------------------------------------------------------ */
/* Single-transfer helpers. reg is the byte offset (0x0,0x4,0x8,0xC). */
static uint32_t do_read(uint8_t req)
{
    uint8_t c[4], r[16];
    c[0] = DAP_Transfer; c[1] = 0 /*DAP index*/; c[2] = 1 /*count*/; c[3] = req;
    int rx = dap_xfer(c, 4, r, sizeof r);
    if (rx < 3 || r[1] != 1 || (r[2] & 0x7) != ACK_OK)
        die("DAP read: bad ACK");
    return (uint32_t)r[3] | (r[4] << 8) | (r[5] << 16) | (r[6] << 24);
}
static void do_write(uint8_t req, uint32_t val)
{
    uint8_t c[8], r[16];
    c[0] = DAP_Transfer; c[1] = 0; c[2] = 1; c[3] = req;
    c[4] = val & 0xff; c[5] = (val >> 8) & 0xff; c[6] = (val >> 16) & 0xff; c[7] = (val >> 24) & 0xff;
    dap_xfer(c, 8, r, sizeof r);
    if (r[1] != 1 || (r[2] & 0x7) != ACK_OK)
        die("DAP write: bad ACK");
}

static void     dp_write(uint8_t reg, uint32_t v) { do_write((reg & 0x0C),            v); }
static uint32_t dp_read (uint8_t reg)             { return do_read(REQ_RnW | (reg & 0x0C)); }
static void     ap_write(uint8_t reg, uint32_t v) { do_write(REQ_APnDP | (reg & 0x0C), v); }

/* AP reads are posted on SWD: queue the AP read, then read DP RDBUFF (0x0C)
 * in the SAME DAP_Transfer, and take the RDBUFF word as the real value.     */
static uint32_t ap_read(uint8_t reg)
{
    uint8_t c[8], r[32];
    c[0] = DAP_Transfer; c[1] = 0; c[2] = 2;
    c[3] = REQ_APnDP | REQ_RnW | (reg & 0x0C);   /* AP read (posted)      */
    c[4] = REQ_RnW   | 0x0C;                     /* DP RDBUFF read        */
    int rx = dap_xfer(c, 5, r, sizeof r);
    if (rx < 11 || r[1] != 2 || (r[2] & 0x7) != ACK_OK)
        die("DAP ap_read: bad ACK");
    /* r[3..6] = stale AP data, r[7..10] = RDBUFF = real value */
    return (uint32_t)r[7] | (r[8] << 8) | (r[9] << 16) | (r[10] << 24);
}

/* ------------------------------------------------------------------ *
 *  DCI mailbox (mirrors dci.py)                                      *
 * ------------------------------------------------------------------ */
#define DCI_AP_CSW   0x00   /* AP register offsets */
#define DCI_AP_TAR   0x04
#define DCI_AP_DRW   0x0C
#define DCIWDATA     0x1000 /* DCI register addresses (via MEM-AP TAR) */
#define DCIRDATA     0x1004
#define DCISTATUS    0x1008
#define DCIID        0x10FC

static uint32_t mem_read(uint32_t addr)  { ap_write(DCI_AP_TAR, addr); return ap_read(DCI_AP_DRW); }
static void     mem_write(uint32_t addr, uint32_t v) { ap_write(DCI_AP_TAR, addr); ap_write(DCI_AP_DRW, v); }

static void dci_connect(void)
{
    uint32_t idr = dp_read(0x00);               /* DPIDR */
    if (idr != 0x6BA02477) {
        fprintf(stderr, "bad DP IDCODE 0x%08X\n", idr); exit(1);
    }
    dp_write(0x00, 0x1E);                       /* ABORT: clear sticky errors      */
    dp_write(0x04, 0x50000000);                 /* CTRL/STAT: power up debug+system */
    dp_write(0x08, 0x01000000);                 /* SELECT: APSEL=1 (APB-AP), bank 0 */
    ap_write(DCI_AP_CSW, 0x22000002);           /* CSW: 32-bit access               */
    uint32_t id = mem_read(DCIID);
    if (id != 0xDC11D) { fprintf(stderr, "DCIID mismatch: 0x%X\n", id); exit(1); }
    printf("DCI connected (DCIID=0x%X)\n", id);
}

static uint32_t dci_status(void) { return mem_read(DCISTATUS); }

static void dci_write_cmd(uint32_t word)
{
    for (int i = 0; i < 100; i++) {
        uint32_t s = dci_status();
        if (s & 0x001) { usleep(10000); continue; }      /* WPENDING */
        if (s & 0x100) die("RDATAVALID set, cannot write DCIWDATA");
        mem_write(DCIWDATA, word);
        return;
    }
    die("DCI write timeout");
}

static uint32_t dci_read_response(void)
{
    for (int i = 0; i < 100; i++) {
        if (dci_status() & 0x100) return mem_read(DCIRDATA);   /* RDATAVALID */
        usleep(10000);
    }
    die("DCI read timeout");
    return 0;
}

static void read_se_status(void)
{
    dci_connect();
    dci_write_cmd(8);                 /* length */
    dci_write_cmd(0xFE010000);        /* cmd: read SE status */
    uint32_t recvlen = dci_read_response();
    if (recvlen & 0xFFFF0000) { fprintf(stderr, "bad cmd response 0x%X\n", recvlen); exit(1); }

    int idx = (recvlen == 0x28) ? 7 : 3;
    uint32_t words[32]; int n = 0;
    recvlen -= 4;
    while ((int32_t)recvlen > 0 && n < 32) { recvlen -= 4; words[n++] = dci_read_response(); }

    printf("SESTATUS words:");
    for (int i = 0; i < n; i++) printf(" 0x%X", words[i]);
    printf("\n");

    uint32_t dl = words[idx];
    printf("  Debug lock (config):   %s\n", (dl & 0x01) ? "Enabled" : "Disabled");
    printf("  Device erase:          %s\n", (dl & 0x02) ? "Enabled" : "Disabled");
    printf("  Secure debug:          %s\n", (dl & 0x04) ? "Enabled" : "Disabled");
    printf("  Debug lock (hw status):%s\n", (dl & 0x20) ? "Enabled" : "Disabled");
}

static void device_erase(void)
{
    dci_connect();
    dci_write_cmd(8);
    dci_write_cmd(0x430F0000);        /* cmd: device erase */
    usleep(2000 * 1000);
    printf("Device erase command sent.\n");
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "status";

    if (libusb_init(&g_ctx) != 0) die("libusb_init failed");
    if (open_probe() != 0) die("no CMSIS-DAP probe found (is it plugged in?)");

    dap_setup();

    if      (!strcmp(cmd, "status")) read_se_status();
    else if (!strcmp(cmd, "erase"))  device_erase();
    else { fprintf(stderr, "usage: %s status|erase\n", argv[0]); }

    libusb_release_interface(g_dev, g_iface);
    libusb_close(g_dev);
    libusb_exit(g_ctx);
    return 0;
}
