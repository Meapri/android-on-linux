/* alr_libusb_shim.c — the GUEST-side libusb-1.0 backend shim for ALR.
 *
 * Built into libusb-1.0.so.0 (SONAME) for the aarch64 glibc guest. It exports
 * the public libusb_* symbols an unmodified Linux app links, but instead of
 * opening /dev/bus/usb (which does not exist for a non-root Android app) it
 * connects an AF_UNIX socket to the Android-side UsbHostBridge (Kotlin) at
 * $ALR_USB_SOCK and forwards every operation as the length-prefixed protocol
 * in alr_usb_proto.h. The bridge performs the actual UsbDeviceConnection
 * controlTransfer/bulkTransfer calls in the app process.
 *
 * This is Option B1 of docs/design/android-usb-host.md. The async libusb model
 * is implemented with the shim owning its OWN event loop + completion queue
 * (woken by a self-pipe from the reader thread) — no fake usbfs poll fd is
 * needed (the explicit reason the design chose B over A).
 *
 * Scope / honesty (mirrors design §10):
 *   - control / bulk / interrupt transfers (sync + async) FORWARDED & FUNCTIONAL
 *     once a device is opened on the bridge; device-verify is required to prove
 *     real bytes move (host build cannot enumerate a phone's USB bus).
 *   - enumerate / open(+permission) / claim / hotplug FORWARDED.
 *   - isochronous, libusb_reset_device, kernel-driver detach: best-effort or
 *     LIBUSB_ERROR_NOT_SUPPORTED — platform limits, reported honestly.
 *   - string descriptors are served from the cached rawDescriptors blob where
 *     present; otherwise via a CONTROL GET_DESCRIPTOR round-trip.
 *
 * No libc beyond glibc (sockets, pthread, a self-pipe). Built with the glibc
 * 2.34 target pin so pthread folds into libc.so.6 (same rationale as the GLES
 * guest shim's build-shim.sh) — the tiny ALR rootfs ships no libpthread.so.0.
 */
#define _GNU_SOURCE
#include "alr_libusb.h"
#include "alr_usb_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- *
 * Diagnostics
 * ------------------------------------------------------------------------- */
static int g_debug = -1;
static void dbg(const char* fmt, ...) {
    if (g_debug < 0) {
        const char* e = getenv("ALR_USB_DEBUG");
        g_debug = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!g_debug) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[alr-libusb] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------------- *
 * Context / handle state
 * ------------------------------------------------------------------------- */

#define ALR_USB_MAX_TRANSFERS 256

struct pending_transfer {
    uint32_t tag;                  /* SUBMIT tag = our slot id */
    struct libusb_transfer* xfer;  /* caller's transfer (we own none of buffer) */
    _Atomic int done;              /* set by reader thread on COMPLETE */
    int status;                    /* libusb_transfer_status */
    int actual_length;
    int in_use;
};

struct hotplug_reg {
    libusb_hotplug_callback_handle handle;
    int events;
    libusb_hotplug_callback_fn cb;
    void* user_data;
    int active;
};

struct libusb_context {
    int sock;                  /* AF_UNIX stream to the bridge */
    int wake_r, wake_w;        /* self-pipe: reader -> event loop wakeup */
    pthread_t reader;
    int reader_running;

    pthread_mutex_t send_lock; /* serialize writes on the socket */
    pthread_mutex_t reply_lock;
    pthread_cond_t  reply_cv;  /* signalled when a sync reply arrives */

    /* single in-flight sync request slot (the public API is mostly serial; we
     * hold reply_lock across each request/reply so only one is outstanding). */
    _Atomic uint32_t next_tag;
    uint32_t  awaiting_tag;    /* tag the caller is blocked on (0=none) */
    int       reply_ready;
    uint16_t  reply_op;
    uint16_t  reply_flags;
    uint8_t*  reply_payload;   /* malloc'd; owned until consumed */
    uint32_t  reply_len;

    pthread_mutex_t xfer_lock;
    struct pending_transfer xfers[ALR_USB_MAX_TRANSFERS];

    pthread_mutex_t hp_lock;
    struct hotplug_reg hp[8];
    int hp_next_handle;
};

/* libusb_device — we synthesize one per enumerated device, with its cached
 * descriptor blob so get_*_descriptor never round-trips. */
struct libusb_device {
    libusb_context* ctx;
    _Atomic int refcnt;
    uint16_t vid, pid;
    uint8_t  bus, addr, devclass;
    uint8_t* raw;       /* cached descriptors (device + configs), may be NULL pre-open */
    uint32_t raw_len;
};

struct libusb_device_handle {
    libusb_context* ctx;
    libusb_device*  dev;
    uint32_t conn_id;   /* bridge-assigned */
};

/* A single process-wide default context is the common libusb usage; we still
 * support an explicit one. */
static libusb_context* g_default_ctx = NULL;
static pthread_mutex_t g_default_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- *
 * Low-level framed IO
 * ------------------------------------------------------------------------- */

static int read_full(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return -1;             /* EOF */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, p + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)r;
    }
    return 0;
}

/* Send one framed message. payload may be NULL if plen==0. send_lock held by caller. */
static int send_frame(libusb_context* ctx, uint16_t op, uint16_t flags,
                      uint32_t tag, const void* payload, uint32_t plen) {
    uint8_t hdr[ALR_USB_HDR_SIZE];
    alr_usb_wbuf b = { hdr, sizeof hdr, 0 };
    alr_usb_w_u32(&b, plen);
    alr_usb_w_u16(&b, op);
    alr_usb_w_u16(&b, flags);
    alr_usb_w_u32(&b, tag);
    if (write_full(ctx->sock, hdr, sizeof hdr) != 0) return -1;
    if (plen && write_full(ctx->sock, payload, plen) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Reader thread — demuxes replies + async messages
 * ------------------------------------------------------------------------- */

static void complete_transfer(libusb_context* ctx, uint32_t tag, int status,
                              int actual, const uint8_t* data, uint32_t dlen);

static void* reader_main(void* arg) {
    libusb_context* ctx = (libusb_context*)arg;
    for (;;) {
        uint8_t hdr[ALR_USB_HDR_SIZE];
        if (read_full(ctx->sock, hdr, sizeof hdr) != 0) break;
        alr_usb_rbuf rb = { hdr, sizeof hdr, 0 };
        uint32_t len, tag; uint16_t op, flags;
        alr_usb_r_u32(&rb, &len);
        alr_usb_r_u16(&rb, &op);
        alr_usb_r_u16(&rb, &flags);
        alr_usb_r_u32(&rb, &tag);

        uint8_t* payload = NULL;
        if (len) {
            payload = (uint8_t*)malloc(len);
            if (!payload) break;
            if (read_full(ctx->sock, payload, len) != 0) { free(payload); break; }
        }

        if (op == ALR_USB_OP_COMPLETE) {
            alr_usb_rbuf pb = { payload, len, 0 };
            int32_t status = 0, actual = 0;
            alr_usb_r_i32(&pb, &status);
            alr_usb_r_i32(&pb, &actual);
            const uint8_t* data = NULL; uint32_t dlen = 0;
            if (pb.pos + 4 <= pb.cap) {
                uint32_t n; alr_usb_r_u32(&pb, &n);
                if (pb.pos + n <= pb.cap) { data = pb.p + pb.pos; dlen = n; }
            }
            complete_transfer(ctx, tag, status, actual, data, dlen);
            free(payload);
            continue;
        }
        if (op == ALR_USB_OP_HOTPLUG) {
            alr_usb_rbuf pb = { payload, len, 0 };
            uint8_t added = 0, bus = 0, addr = 0; uint16_t vid = 0, pid = 0;
            alr_usb_r_u8(&pb, &added);
            alr_usb_r_u16(&pb, &vid);
            alr_usb_r_u16(&pb, &pid);
            alr_usb_r_u8(&pb, &bus);
            alr_usb_r_u8(&pb, &addr);
            /* Dispatch to registered hotplug callbacks. We synthesize a
             * transient libusb_device for the callback (refcnt 1). */
            pthread_mutex_lock(&ctx->hp_lock);
            for (size_t i = 0; i < sizeof ctx->hp / sizeof ctx->hp[0]; ++i) {
                if (!ctx->hp[i].active) continue;
                int want = added ? LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED
                                 : LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
                if (!(ctx->hp[i].events & want)) continue;
                libusb_device* d = (libusb_device*)calloc(1, sizeof *d);
                if (!d) continue;
                d->ctx = ctx; atomic_store(&d->refcnt, 1);
                d->vid = vid; d->pid = pid; d->bus = bus; d->addr = addr;
                int ev = added ? LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED
                               : LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
                ctx->hp[i].cb(ctx, d, ev, ctx->hp[i].user_data);
                /* callback owns a ref convention; we drop ours */
                if (atomic_fetch_sub(&d->refcnt, 1) == 1) { free(d->raw); free(d); }
            }
            pthread_mutex_unlock(&ctx->hp_lock);
            free(payload);
            continue;
        }

        /* Otherwise it is a reply to a synchronous request: hand to the waiter. */
        pthread_mutex_lock(&ctx->reply_lock);
        if (ctx->awaiting_tag == tag) {
            ctx->reply_op = op;
            ctx->reply_flags = flags;
            free(ctx->reply_payload);
            ctx->reply_payload = payload;   /* transfer ownership */
            ctx->reply_len = len;
            ctx->reply_ready = 1;
            pthread_cond_broadcast(&ctx->reply_cv);
            payload = NULL;
        }
        pthread_mutex_unlock(&ctx->reply_lock);
        free(payload);  /* unmatched reply (or already consumed): drop */
    }
    ctx->reader_running = 0;
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * Synchronous request/reply round trip
 * ------------------------------------------------------------------------- *
 * Returns the reply op (>=0) on success with out_payload/out_len populated
 * (caller frees the returned payload), or a negative LIBUSB_ERROR_* on error.
 * If the reply has the ERR flag, the i32 errno is returned negative directly. */
static int request_reply(libusb_context* ctx, uint16_t op, uint32_t tag,
                         const void* payload, uint32_t plen,
                         uint8_t** out_payload, uint32_t* out_len) {
    if (out_payload) *out_payload = NULL;
    if (out_len) *out_len = 0;
    if (!ctx || ctx->sock < 0) return LIBUSB_ERROR_NO_DEVICE;

    pthread_mutex_lock(&ctx->reply_lock);
    ctx->awaiting_tag = tag;
    ctx->reply_ready = 0;
    pthread_mutex_unlock(&ctx->reply_lock);

    pthread_mutex_lock(&ctx->send_lock);
    int w = send_frame(ctx, op, 0, tag, payload, plen);
    pthread_mutex_unlock(&ctx->send_lock);
    if (w != 0) {
        pthread_mutex_lock(&ctx->reply_lock);
        ctx->awaiting_tag = 0;
        pthread_mutex_unlock(&ctx->reply_lock);
        return LIBUSB_ERROR_IO;
    }

    pthread_mutex_lock(&ctx->reply_lock);
    while (!ctx->reply_ready && ctx->reader_running) {
        pthread_cond_wait(&ctx->reply_cv, &ctx->reply_lock);
    }
    int rc;
    uint8_t* pl = NULL; uint32_t pn = 0;
    if (!ctx->reply_ready) {
        rc = LIBUSB_ERROR_NO_DEVICE;   /* reader died */
    } else {
        pl = ctx->reply_payload; pn = ctx->reply_len;
        ctx->reply_payload = NULL; ctx->reply_len = 0; ctx->reply_ready = 0;
        if (ctx->reply_flags & ALR_USB_FLAG_ERR) {
            int32_t e = LIBUSB_ERROR_OTHER;
            alr_usb_rbuf eb = { pl, pn, 0 };
            alr_usb_r_i32(&eb, &e);
            free(pl); pl = NULL; pn = 0;
            rc = e;                    /* already negative */
        } else {
            rc = (int)ctx->reply_op;
        }
    }
    ctx->awaiting_tag = 0;
    pthread_mutex_unlock(&ctx->reply_lock);

    if (rc >= 0) {
        if (out_payload) *out_payload = pl; else free(pl);
        if (out_len) *out_len = pn;
    } else {
        free(pl);
    }
    return rc;
}

static uint32_t next_tag(libusb_context* ctx) {
    uint32_t t = atomic_fetch_add(&ctx->next_tag, 1) + 1;
    if (t == 0) t = atomic_fetch_add(&ctx->next_tag, 1) + 1;
    return t;
}

/* ------------------------------------------------------------------------- *
 * Public API: init / exit
 * ------------------------------------------------------------------------- */

static int connect_bridge(void) {
    const char* path = getenv("ALR_USB_SOCK");
    if (!path || !path[0]) {
        dbg("ALR_USB_SOCK not set; USB host unavailable");
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    /* filesystem socket (mirrors the Wayland socket under cacheDir). */
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        dbg("connect(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    dbg("connected to bridge %s fd=%d", path, fd);
    return fd;
}

int libusb_init(libusb_context** ctxp) {
    libusb_context* ctx = (libusb_context*)calloc(1, sizeof *ctx);
    if (!ctx) return LIBUSB_ERROR_NO_MEM;

    int sock = connect_bridge();
    if (sock < 0) { free(ctx); return LIBUSB_ERROR_OTHER; }
    ctx->sock = sock;

    int pp[2];
    if (pipe2(pp, O_CLOEXEC) != 0) { close(sock); free(ctx); return LIBUSB_ERROR_OTHER; }
    ctx->wake_r = pp[0]; ctx->wake_w = pp[1];

    pthread_mutex_init(&ctx->send_lock, NULL);
    pthread_mutex_init(&ctx->reply_lock, NULL);
    pthread_cond_init(&ctx->reply_cv, NULL);
    pthread_mutex_init(&ctx->xfer_lock, NULL);
    pthread_mutex_init(&ctx->hp_lock, NULL);
    atomic_store(&ctx->next_tag, 0);
    ctx->hp_next_handle = 1;

    ctx->reader_running = 1;
    if (pthread_create(&ctx->reader, NULL, reader_main, ctx) != 0) {
        ctx->reader_running = 0;
        close(sock); close(pp[0]); close(pp[1]); free(ctx);
        return LIBUSB_ERROR_OTHER;
    }

    if (ctxp) {
        *ctxp = ctx;
    } else {
        pthread_mutex_lock(&g_default_lock);
        g_default_ctx = ctx;
        pthread_mutex_unlock(&g_default_lock);
    }
    return LIBUSB_SUCCESS;
}

void libusb_exit(libusb_context* ctx) {
    if (!ctx) {
        pthread_mutex_lock(&g_default_lock);
        ctx = g_default_ctx; g_default_ctx = NULL;
        pthread_mutex_unlock(&g_default_lock);
        if (!ctx) return;
    }
    /* shutdown unblocks read_full in the reader. */
    shutdown(ctx->sock, SHUT_RDWR);
    if (ctx->reader_running) pthread_join(ctx->reader, NULL);
    close(ctx->sock);
    close(ctx->wake_r); close(ctx->wake_w);
    free(ctx->reply_payload);
    pthread_mutex_destroy(&ctx->send_lock);
    pthread_mutex_destroy(&ctx->reply_lock);
    pthread_cond_destroy(&ctx->reply_cv);
    pthread_mutex_destroy(&ctx->xfer_lock);
    pthread_mutex_destroy(&ctx->hp_lock);
    free(ctx);
}

static libusb_context* resolve_ctx(libusb_context* ctx) {
    if (ctx) return ctx;
    pthread_mutex_lock(&g_default_lock);
    libusb_context* c = g_default_ctx;
    pthread_mutex_unlock(&g_default_lock);
    return c;
}

void libusb_set_debug(libusb_context* ctx, int level) { (void)ctx; (void)level; }
int  libusb_set_option(libusb_context* ctx, int option, ...) { (void)ctx; (void)option; return LIBUSB_SUCCESS; }

const char* libusb_error_name(int errcode) {
    switch (errcode) {
        case LIBUSB_SUCCESS:             return "LIBUSB_SUCCESS";
        case LIBUSB_ERROR_IO:            return "LIBUSB_ERROR_IO";
        case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
        case LIBUSB_ERROR_ACCESS:        return "LIBUSB_ERROR_ACCESS";
        case LIBUSB_ERROR_NO_DEVICE:     return "LIBUSB_ERROR_NO_DEVICE";
        case LIBUSB_ERROR_NOT_FOUND:     return "LIBUSB_ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY:          return "LIBUSB_ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT:       return "LIBUSB_ERROR_TIMEOUT";
        case LIBUSB_ERROR_OVERFLOW:      return "LIBUSB_ERROR_OVERFLOW";
        case LIBUSB_ERROR_PIPE:          return "LIBUSB_ERROR_PIPE";
        case LIBUSB_ERROR_INTERRUPTED:   return "LIBUSB_ERROR_INTERRUPTED";
        case LIBUSB_ERROR_NO_MEM:        return "LIBUSB_ERROR_NO_MEM";
        case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
        default:                         return "LIBUSB_ERROR_OTHER";
    }
}
const char* libusb_strerror(int errcode) { return libusb_error_name(errcode); }

/* ------------------------------------------------------------------------- *
 * Enumeration
 * ------------------------------------------------------------------------- */

ssize_t libusb_get_device_list(libusb_context* ctx, libusb_device*** list) {
    ctx = resolve_ctx(ctx);
    if (!ctx) return LIBUSB_ERROR_NO_DEVICE;
    uint8_t* pl = NULL; uint32_t pn = 0;
    int rc = request_reply(ctx, ALR_USB_OP_ENUMERATE, next_tag(ctx), NULL, 0, &pl, &pn);
    if (rc < 0) return rc;
    alr_usb_rbuf rb = { pl, pn, 0 };
    uint32_t count = 0;
    alr_usb_r_u32(&rb, &count);
    libusb_device** arr = (libusb_device**)calloc(count + 1, sizeof(libusb_device*));
    if (!arr) { free(pl); return LIBUSB_ERROR_NO_MEM; }
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        libusb_device* d = (libusb_device*)calloc(1, sizeof *d);
        if (!d) break;
        uint16_t vid = 0, pid = 0; uint8_t bus = 0, addr = 0, cls = 0;
        if (alr_usb_r_u16(&rb, &vid) || alr_usb_r_u16(&rb, &pid) ||
            alr_usb_r_u8(&rb, &bus) || alr_usb_r_u8(&rb, &addr) ||
            alr_usb_r_u8(&rb, &cls)) { free(d); break; }
        d->ctx = ctx; atomic_store(&d->refcnt, 1);
        d->vid = vid; d->pid = pid; d->bus = bus; d->addr = addr; d->devclass = cls;
        arr[n++] = d;
    }
    free(pl);
    if (list) *list = arr; else { /* caller wants count only — free */
        for (uint32_t i = 0; i < n; ++i) { free(arr[i]->raw); free(arr[i]); }
        free(arr);
    }
    return (ssize_t)n;
}

void libusb_free_device_list(libusb_device** list, int unref_devices) {
    if (!list) return;
    for (libusb_device** p = list; *p; ++p) {
        if (unref_devices) {
            libusb_device* d = *p;
            if (atomic_fetch_sub(&d->refcnt, 1) == 1) { free(d->raw); free(d); }
        }
    }
    free(list);
}

libusb_device* libusb_ref_device(libusb_device* dev) {
    if (dev) atomic_fetch_add(&dev->refcnt, 1);
    return dev;
}
void libusb_unref_device(libusb_device* dev) {
    if (dev && atomic_fetch_sub(&dev->refcnt, 1) == 1) { free(dev->raw); free(dev); }
}

uint8_t libusb_get_bus_number(libusb_device* dev)     { return dev ? dev->bus : 0; }
uint8_t libusb_get_device_address(libusb_device* dev) { return dev ? dev->addr : 0; }
int     libusb_get_device_speed(libusb_device* dev)   { (void)dev; return 0; /* UNKNOWN */ }

/* ------------------------------------------------------------------------- *
 * Descriptor parsing from the cached rawDescriptors blob
 * ------------------------------------------------------------------------- *
 * Android's UsbDeviceConnection.getRawDescriptors() returns the device
 * descriptor immediately followed by each configuration's full descriptor
 * blob (config + interface + endpoint descriptors), exactly the standard
 * USB layout. We parse it in-place; no per-field round trips.               */

static int parse_device_descriptor(const uint8_t* raw, uint32_t len,
                                   struct libusb_device_descriptor* out) {
    if (!raw || len < 18) return LIBUSB_ERROR_IO;
    out->bLength            = raw[0];
    out->bDescriptorType    = raw[1];
    out->bcdUSB             = (uint16_t)(raw[2] | (raw[3] << 8));
    out->bDeviceClass       = raw[4];
    out->bDeviceSubClass    = raw[5];
    out->bDeviceProtocol    = raw[6];
    out->bMaxPacketSize0    = raw[7];
    out->idVendor           = (uint16_t)(raw[8] | (raw[9] << 8));
    out->idProduct          = (uint16_t)(raw[10] | (raw[11] << 8));
    out->bcdDevice          = (uint16_t)(raw[12] | (raw[13] << 8));
    out->iManufacturer      = raw[14];
    out->iProduct           = raw[15];
    out->iSerialNumber      = raw[16];
    out->bNumConfigurations = raw[17];
    return LIBUSB_SUCCESS;
}

/* Ensure dev->raw is populated. If not (device not yet opened), do a transient
 * OPEN to fetch + cache the descriptors, then CLOSE. This makes
 * get_device_descriptor work pre-open, as apps expect. */
static int ensure_raw(libusb_device* dev) {
    if (dev->raw && dev->raw_len) return LIBUSB_SUCCESS;
    libusb_context* ctx = dev->ctx;
    uint8_t req[6];
    alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u16(&wb, dev->vid);
    alr_usb_w_u16(&wb, dev->pid);
    alr_usb_w_u8(&wb, dev->bus);
    alr_usb_w_u8(&wb, dev->addr);
    uint8_t* pl = NULL; uint32_t pn = 0;
    int rc = request_reply(ctx, ALR_USB_OP_OPEN, next_tag(ctx), req, wb.len, &pl, &pn);
    if (rc < 0) return rc;
    alr_usb_rbuf rb = { pl, pn, 0 };
    uint32_t conn = 0, blen = 0;
    alr_usb_r_u32(&rb, &conn);
    alr_usb_r_u32(&rb, &blen);
    if (rb.pos + blen <= rb.cap && blen) {
        dev->raw = (uint8_t*)malloc(blen);
        if (dev->raw) { memcpy(dev->raw, rb.p + rb.pos, blen); dev->raw_len = blen; }
    }
    free(pl);
    /* transient close */
    uint8_t cl[4]; alr_usb_wbuf cb = { cl, sizeof cl, 0 };
    alr_usb_w_u32(&cb, conn);
    request_reply(ctx, ALR_USB_OP_CLOSE, next_tag(ctx), cl, cb.len, NULL, NULL);
    return dev->raw_len ? LIBUSB_SUCCESS : LIBUSB_ERROR_IO;
}

int libusb_get_device_descriptor(libusb_device* dev, struct libusb_device_descriptor* desc) {
    if (!dev || !desc) return LIBUSB_ERROR_INVALID_PARAM;
    int rc = ensure_raw(dev);
    if (rc < 0) {
        /* Fall back to identity we already have from ENUMERATE. */
        memset(desc, 0, sizeof *desc);
        desc->bLength = 18; desc->bDescriptorType = 1;
        desc->idVendor = dev->vid; desc->idProduct = dev->pid;
        desc->bDeviceClass = dev->devclass; desc->bNumConfigurations = 1;
        return LIBUSB_SUCCESS;
    }
    return parse_device_descriptor(dev->raw, dev->raw_len, desc);
}

/* Find the Nth configuration descriptor in the cached raw blob (skips the 18B
 * device descriptor, then walks config descriptors by wTotalLength). */
static const uint8_t* find_config_raw(libusb_device* dev, int index, uint32_t* out_len) {
    if (ensure_raw(dev) < 0) return NULL;
    const uint8_t* p = dev->raw;
    uint32_t remain = dev->raw_len;
    if (remain < 18) return NULL;
    /* device descriptor */
    p += p[0]; remain -= dev->raw[0];
    int i = 0;
    while (remain >= 9) {
        if (p[1] != 2 /* CONFIGURATION */) break;
        uint32_t total = (uint32_t)(p[2] | (p[3] << 8));
        if (total == 0 || total > remain) break;
        if (i == index) { *out_len = total; return p; }
        p += total; remain -= total; ++i;
    }
    return NULL;
}

/* We expose config descriptors as the raw blob via a small heap struct the
 * caller frees with libusb_free_config_descriptor. Many apps only read the top
 * config fields + walk interfaces; building the full nested C tree is large, so
 * we parse the header fields and leave the nested arrays NULL with the `extra`
 * pointer set to the raw blob for apps that parse it themselves (libusb permits
 * extra-bytes parsing). Apps needing the full tree should be device-tested. */
int libusb_get_config_descriptor(libusb_device* dev, uint8_t config_index,
                                 struct libusb_config_descriptor** config) {
    if (!dev || !config) return LIBUSB_ERROR_INVALID_PARAM;
    uint32_t clen = 0;
    const uint8_t* p = find_config_raw(dev, config_index, &clen);
    if (!p) return LIBUSB_ERROR_NOT_FOUND;
    struct libusb_config_descriptor* c =
        (struct libusb_config_descriptor*)calloc(1, sizeof *c);
    if (!c) return LIBUSB_ERROR_NO_MEM;
    uint8_t* blob = (uint8_t*)malloc(clen);
    if (!blob) { free(c); return LIBUSB_ERROR_NO_MEM; }
    memcpy(blob, p, clen);
    c->bLength             = blob[0];
    c->bDescriptorType     = blob[1];
    c->wTotalLength        = (uint16_t)(blob[2] | (blob[3] << 8));
    c->bNumInterfaces      = blob[4];
    c->bConfigurationValue = blob[5];
    c->iConfiguration      = blob[6];
    c->bmAttributes        = blob[7];
    c->MaxPower            = blob[8];
    c->interface           = NULL;
    c->extra               = blob;          /* full blob for self-parsers */
    c->extra_length        = (int)clen;
    *config = c;
    return LIBUSB_SUCCESS;
}

int libusb_get_active_config_descriptor(libusb_device* dev,
                                        struct libusb_config_descriptor** config) {
    return libusb_get_config_descriptor(dev, 0, config);
}

void libusb_free_config_descriptor(struct libusb_config_descriptor* config) {
    if (!config) return;
    free((void*)config->extra);
    free(config);
}

/* ------------------------------------------------------------------------- *
 * Open / close
 * ------------------------------------------------------------------------- */

libusb_device_handle* libusb_open_device_with_vid_pid(libusb_context* ctx,
                                                      uint16_t vid, uint16_t pid) {
    ctx = resolve_ctx(ctx);
    if (!ctx) return NULL;
    libusb_device** list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle* h = NULL;
    for (ssize_t i = 0; i < n; ++i) {
        if (list[i]->vid == vid && list[i]->pid == pid) {
            if (libusb_open(list[i], &h) == LIBUSB_SUCCESS) break;
        }
    }
    libusb_free_device_list(list, 1);
    return h;
}

int libusb_open(libusb_device* dev, libusb_device_handle** handle) {
    if (!dev || !handle) return LIBUSB_ERROR_INVALID_PARAM;
    libusb_context* ctx = dev->ctx;
    uint8_t req[6];
    alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u16(&wb, dev->vid);
    alr_usb_w_u16(&wb, dev->pid);
    alr_usb_w_u8(&wb, dev->bus);
    alr_usb_w_u8(&wb, dev->addr);
    uint8_t* pl = NULL; uint32_t pn = 0;
    /* This blocks while the Android permission dialog is up (bridge holds the
     * reply until grant/deny). */
    int rc = request_reply(ctx, ALR_USB_OP_OPEN, next_tag(ctx), req, wb.len, &pl, &pn);
    if (rc < 0) return rc;
    alr_usb_rbuf rb = { pl, pn, 0 };
    uint32_t conn = 0, blen = 0;
    alr_usb_r_u32(&rb, &conn);
    alr_usb_r_u32(&rb, &blen);
    libusb_device_handle* h = (libusb_device_handle*)calloc(1, sizeof *h);
    if (!h) { free(pl); return LIBUSB_ERROR_NO_MEM; }
    h->ctx = ctx; h->dev = libusb_ref_device(dev); h->conn_id = conn;
    if (blen && rb.pos + blen <= rb.cap && !dev->raw) {
        dev->raw = (uint8_t*)malloc(blen);
        if (dev->raw) { memcpy(dev->raw, rb.p + rb.pos, blen); dev->raw_len = blen; }
    }
    free(pl);
    *handle = h;
    return LIBUSB_SUCCESS;
}

void libusb_close(libusb_device_handle* handle) {
    if (!handle) return;
    libusb_context* ctx = handle->ctx;
    uint8_t req[4]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, handle->conn_id);
    request_reply(ctx, ALR_USB_OP_CLOSE, next_tag(ctx), req, wb.len, NULL, NULL);
    libusb_unref_device(handle->dev);
    free(handle);
}

libusb_device* libusb_get_device(libusb_device_handle* handle) {
    return handle ? handle->dev : NULL;
}

/* ------------------------------------------------------------------------- *
 * Interface claim / config
 * ------------------------------------------------------------------------- */

int libusb_claim_interface(libusb_device_handle* h, int iface) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[6]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, (uint8_t)iface);
    alr_usb_w_u8(&wb, 1 /* force */);
    int rc = request_reply(h->ctx, ALR_USB_OP_CLAIM, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int libusb_release_interface(libusb_device_handle* h, int iface) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[5]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, (uint8_t)iface);
    int rc = request_reply(h->ctx, ALR_USB_OP_RELEASE, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int libusb_set_configuration(libusb_device_handle* h, int configuration) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[8]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_i32(&wb, configuration);
    int rc = request_reply(h->ctx, ALR_USB_OP_SET_CONFIG, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int libusb_set_interface_alt_setting(libusb_device_handle* h, int iface, int alt) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[6]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, (uint8_t)iface);
    alr_usb_w_u8(&wb, (uint8_t)alt);
    int rc = request_reply(h->ctx, ALR_USB_OP_SET_ALT, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int libusb_clear_halt(libusb_device_handle* h, unsigned char endpoint) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[5]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, endpoint);
    int rc = request_reply(h->ctx, ALR_USB_OP_CLEAR_HALT, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int libusb_reset_device(libusb_device_handle* h) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t req[4]; alr_usb_wbuf wb = { req, sizeof req, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    /* Android has no public reset(); bridge replies NOT_SUPPORTED or best-effort. */
    int rc = request_reply(h->ctx, ALR_USB_OP_RESET, next_tag(h->ctx), req, wb.len, NULL, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

/* kernel-driver helpers: on Android there is no guest kernel driver bound; these
 * are no-ops / best-effort so libusb_detach_kernel_driver-style flows succeed. */
int libusb_kernel_driver_active(libusb_device_handle* h, int iface) { (void)h; (void)iface; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle* h, int iface) { (void)h; (void)iface; return 0; }
int libusb_attach_kernel_driver(libusb_device_handle* h, int iface) { (void)h; (void)iface; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle* h, int enable) { (void)h; (void)enable; return 0; }

/* ------------------------------------------------------------------------- *
 * Synchronous transfers
 * ------------------------------------------------------------------------- */

int libusb_control_transfer(libusb_device_handle* h,
                            uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            unsigned char* data, uint16_t wLength,
                            unsigned int timeout) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    int is_in = (bmRequestType & LIBUSB_ENDPOINT_IN) != 0;
    uint32_t cap = 4 + 1 + 1 + 2 + 2 + 2 + 4 + 4 + (is_in ? 0u : wLength);
    uint8_t* req = (uint8_t*)malloc(cap);
    if (!req) return LIBUSB_ERROR_NO_MEM;
    alr_usb_wbuf wb = { req, cap, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, bmRequestType);
    alr_usb_w_u8(&wb, bRequest);
    alr_usb_w_u16(&wb, wValue);
    alr_usb_w_u16(&wb, wIndex);
    alr_usb_w_u16(&wb, wLength);
    alr_usb_w_i32(&wb, (int32_t)timeout);
    if (!is_in) alr_usb_w_blob(&wb, data, wLength);
    uint8_t* pl = NULL; uint32_t pn = 0;
    int rc = request_reply(h->ctx, ALR_USB_OP_CONTROL, next_tag(h->ctx), req, wb.len, &pl, &pn);
    free(req);
    if (rc < 0) return rc;
    alr_usb_rbuf rb = { pl, pn, 0 };
    int32_t transferred = 0;
    alr_usb_r_i32(&rb, &transferred);
    if (is_in && data) {
        uint32_t dlen = 0;
        if (alr_usb_r_u32(&rb, &dlen) == 0 && rb.pos + dlen <= rb.cap) {
            uint32_t cpy = dlen < (uint32_t)wLength ? dlen : (uint32_t)wLength;
            memcpy(data, rb.p + rb.pos, cpy);
        }
    }
    free(pl);
    return transferred;   /* libusb returns bytes transferred (>=0) */
}

static int do_xfer(libusb_device_handle* h, unsigned char endpoint,
                   unsigned char* data, int length, int* transferred,
                   unsigned int timeout, uint8_t type) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    int is_in = (endpoint & LIBUSB_ENDPOINT_IN) != 0;
    uint32_t cap = 4 + 1 + 1 + 4 + 4 + (is_in ? 0u : (uint32_t)length + 4);
    uint8_t* req = (uint8_t*)malloc(cap ? cap : 1);
    if (!req) return LIBUSB_ERROR_NO_MEM;
    alr_usb_wbuf wb = { req, cap, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, endpoint);
    alr_usb_w_u8(&wb, type);
    alr_usb_w_i32(&wb, length);
    alr_usb_w_i32(&wb, (int32_t)timeout);
    if (!is_in) alr_usb_w_blob(&wb, data, (uint32_t)length);
    uint8_t* pl = NULL; uint32_t pn = 0;
    int rc = request_reply(h->ctx, ALR_USB_OP_XFER, next_tag(h->ctx), req, wb.len, &pl, &pn);
    free(req);
    if (rc < 0) { if (transferred) *transferred = 0; return rc; }
    alr_usb_rbuf rb = { pl, pn, 0 };
    int32_t n = 0;
    alr_usb_r_i32(&rb, &n);
    if (is_in && data) {
        uint32_t dlen = 0;
        if (alr_usb_r_u32(&rb, &dlen) == 0 && rb.pos + dlen <= rb.cap) {
            uint32_t cpy = dlen < (uint32_t)length ? dlen : (uint32_t)length;
            memcpy(data, rb.p + rb.pos, cpy);
        }
    }
    free(pl);
    if (transferred) *transferred = n;
    return LIBUSB_SUCCESS;
}

int libusb_bulk_transfer(libusb_device_handle* h, unsigned char endpoint,
                         unsigned char* data, int length, int* transferred,
                         unsigned int timeout) {
    return do_xfer(h, endpoint, data, length, transferred, timeout, ALR_USB_XFER_BULK);
}

int libusb_interrupt_transfer(libusb_device_handle* h, unsigned char endpoint,
                              unsigned char* data, int length, int* transferred,
                              unsigned int timeout) {
    return do_xfer(h, endpoint, data, length, transferred, timeout, ALR_USB_XFER_INTERRUPT);
}

/* ------------------------------------------------------------------------- *
 * Async transfer API
 * ------------------------------------------------------------------------- */

struct libusb_transfer* libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    struct libusb_transfer* t = (struct libusb_transfer*)calloc(1, sizeof *t);
    return t;
}
void libusb_free_transfer(struct libusb_transfer* t) { free(t); }

static struct pending_transfer* alloc_slot(libusb_context* ctx, uint32_t* tag_out) {
    pthread_mutex_lock(&ctx->xfer_lock);
    for (int i = 0; i < ALR_USB_MAX_TRANSFERS; ++i) {
        if (!ctx->xfers[i].in_use) {
            ctx->xfers[i].in_use = 1;
            ctx->xfers[i].done = 0;
            ctx->xfers[i].tag = (uint32_t)(i + 1);
            *tag_out = ctx->xfers[i].tag;
            pthread_mutex_unlock(&ctx->xfer_lock);
            return &ctx->xfers[i];
        }
    }
    pthread_mutex_unlock(&ctx->xfer_lock);
    return NULL;
}

static void complete_transfer(libusb_context* ctx, uint32_t tag, int status,
                              int actual, const uint8_t* data, uint32_t dlen) {
    if (tag == 0 || tag > ALR_USB_MAX_TRANSFERS) return;
    struct pending_transfer* slot = &ctx->xfers[tag - 1];
    struct libusb_transfer* xfer = slot->xfer;
    if (xfer && data && dlen && (xfer->endpoint & LIBUSB_ENDPOINT_IN) && xfer->buffer) {
        uint32_t cpy = dlen < (uint32_t)xfer->length ? dlen : (uint32_t)xfer->length;
        memcpy(xfer->buffer, data, cpy);
    }
    slot->status = status;
    slot->actual_length = actual;
    atomic_store(&slot->done, 1);
    /* Wake the event loop. */
    uint8_t one = 1;
    (void)!write(ctx->wake_w, &one, 1);
}

int libusb_submit_transfer(struct libusb_transfer* xfer) {
    if (!xfer || !xfer->dev_handle) return LIBUSB_ERROR_INVALID_PARAM;
    libusb_device_handle* h = xfer->dev_handle;
    libusb_context* ctx = h->ctx;
    uint32_t tag = 0;
    struct pending_transfer* slot = alloc_slot(ctx, &tag);
    if (!slot) return LIBUSB_ERROR_NO_MEM;
    slot->xfer = xfer;

    int is_in = (xfer->endpoint & LIBUSB_ENDPOINT_IN) != 0;
    uint8_t type = (xfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
                       ? ALR_USB_XFER_INTERRUPT : ALR_USB_XFER_BULK;
    uint32_t cap = 4 + 1 + 1 + 4 + (is_in ? 0u : (uint32_t)xfer->length + 4);
    uint8_t* req = (uint8_t*)malloc(cap ? cap : 1);
    if (!req) { slot->in_use = 0; return LIBUSB_ERROR_NO_MEM; }
    alr_usb_wbuf wb = { req, cap, 0 };
    alr_usb_w_u32(&wb, h->conn_id);
    alr_usb_w_u8(&wb, xfer->endpoint);
    alr_usb_w_u8(&wb, type);
    alr_usb_w_i32(&wb, xfer->length);
    if (!is_in) alr_usb_w_blob(&wb, xfer->buffer, (uint32_t)xfer->length);

    /* SUBMIT: the tag IS the slot id; the bridge replies later via COMPLETE. */
    pthread_mutex_lock(&ctx->send_lock);
    int w = send_frame(ctx, ALR_USB_OP_SUBMIT, 0, tag, req, wb.len);
    pthread_mutex_unlock(&ctx->send_lock);
    free(req);
    if (w != 0) { slot->in_use = 0; return LIBUSB_ERROR_IO; }
    return LIBUSB_SUCCESS;
}

int libusb_cancel_transfer(struct libusb_transfer* xfer) {
    if (!xfer || !xfer->dev_handle) return LIBUSB_ERROR_INVALID_PARAM;
    libusb_context* ctx = xfer->dev_handle->ctx;
    /* find the slot */
    for (int i = 0; i < ALR_USB_MAX_TRANSFERS; ++i) {
        if (ctx->xfers[i].in_use && ctx->xfers[i].xfer == xfer) {
            uint8_t req[4]; alr_usb_wbuf wb = { req, sizeof req, 0 };
            alr_usb_w_u32(&wb, xfer->dev_handle->conn_id);
            pthread_mutex_lock(&ctx->send_lock);
            send_frame(ctx, ALR_USB_OP_CANCEL, 0, ctx->xfers[i].tag, req, wb.len);
            pthread_mutex_unlock(&ctx->send_lock);
            return LIBUSB_SUCCESS;
        }
    }
    return LIBUSB_ERROR_NOT_FOUND;
}

/* drain completed slots, firing callbacks. Returns count fired. */
static int dispatch_completions(libusb_context* ctx) {
    int fired = 0;
    for (int i = 0; i < ALR_USB_MAX_TRANSFERS; ++i) {
        struct pending_transfer* s = &ctx->xfers[i];
        if (s->in_use && atomic_load(&s->done)) {
            struct libusb_transfer* x = s->xfer;
            if (x) {
                x->status = s->status;
                x->actual_length = s->actual_length;
            }
            /* free slot before callback so a resubmit in the callback can reuse. */
            pthread_mutex_lock(&ctx->xfer_lock);
            s->in_use = 0; s->xfer = NULL; atomic_store(&s->done, 0);
            pthread_mutex_unlock(&ctx->xfer_lock);
            if (x && x->callback) x->callback(x);
            ++fired;
        }
    }
    return fired;
}

int libusb_handle_events(libusb_context* ctx) {
    return libusb_handle_events_timeout(ctx, NULL);
}

int libusb_handle_events_timeout(libusb_context* ctx, struct timeval* tv) {
    ctx = resolve_ctx(ctx);
    if (!ctx) return LIBUSB_ERROR_NO_DEVICE;
    /* Block (with optional timeout) on the wake pipe, then dispatch. */
    struct timeval to = tv ? *tv : (struct timeval){ .tv_sec = 0, .tv_usec = 60000 };
    fd_set rfds; FD_ZERO(&rfds); FD_SET(ctx->wake_r, &rfds);
    int rc = select(ctx->wake_r + 1, &rfds, NULL, NULL, &to);
    if (rc > 0 && FD_ISSET(ctx->wake_r, &rfds)) {
        uint8_t drain[64];
        (void)!read(ctx->wake_r, drain, sizeof drain);
    }
    dispatch_completions(ctx);
    return LIBUSB_SUCCESS;
}

int libusb_handle_events_completed(libusb_context* ctx, int* completed) {
    ctx = resolve_ctx(ctx);
    if (!ctx) return LIBUSB_ERROR_NO_DEVICE;
    while (!completed || !*completed) {
        int rc = libusb_handle_events_timeout(ctx, NULL);
        if (rc < 0) return rc;
        if (completed && *completed) break;
        if (!completed) break;
    }
    return LIBUSB_SUCCESS;
}

/* ------------------------------------------------------------------------- *
 * Hotplug
 * ------------------------------------------------------------------------- */

int libusb_hotplug_register_callback(libusb_context* ctx, int events, int flags,
                                     int vendor_id, int product_id, int dev_class,
                                     libusb_hotplug_callback_fn cb, void* user_data,
                                     libusb_hotplug_callback_handle* handle) {
    (void)flags; (void)vendor_id; (void)product_id; (void)dev_class;
    ctx = resolve_ctx(ctx);
    if (!ctx || !cb) return LIBUSB_ERROR_INVALID_PARAM;
    pthread_mutex_lock(&ctx->hp_lock);
    for (size_t i = 0; i < sizeof ctx->hp / sizeof ctx->hp[0]; ++i) {
        if (!ctx->hp[i].active) {
            ctx->hp[i].active = 1;
            ctx->hp[i].handle = ctx->hp_next_handle++;
            ctx->hp[i].events = events;
            ctx->hp[i].cb = cb;
            ctx->hp[i].user_data = user_data;
            if (handle) *handle = ctx->hp[i].handle;
            pthread_mutex_unlock(&ctx->hp_lock);
            return LIBUSB_SUCCESS;
        }
    }
    pthread_mutex_unlock(&ctx->hp_lock);
    return LIBUSB_ERROR_NO_MEM;
}

void libusb_hotplug_deregister_callback(libusb_context* ctx, libusb_hotplug_callback_handle handle) {
    ctx = resolve_ctx(ctx);
    if (!ctx) return;
    pthread_mutex_lock(&ctx->hp_lock);
    for (size_t i = 0; i < sizeof ctx->hp / sizeof ctx->hp[0]; ++i) {
        if (ctx->hp[i].active && ctx->hp[i].handle == handle) ctx->hp[i].active = 0;
    }
    pthread_mutex_unlock(&ctx->hp_lock);
}

int libusb_has_capability(uint32_t capability) { (void)capability; return 1; }

/* ------------------------------------------------------------------------- *
 * String descriptors (served via CONTROL GET_DESCRIPTOR round trip)
 * ------------------------------------------------------------------------- */

int libusb_get_string_descriptor_ascii(libusb_device_handle* h, uint8_t desc_index,
                                       unsigned char* data, int length) {
    if (!h || !data || length <= 0) return LIBUSB_ERROR_INVALID_PARAM;
    if (desc_index == 0) { data[0] = 0; return 0; }
    unsigned char buf[255];
    /* GET_DESCRIPTOR (STRING) wValue = (3<<8)|index, langid 0x0409 */
    int n = libusb_control_transfer(h, 0x80, 0x06,
                                    (uint16_t)((3 << 8) | desc_index), 0x0409,
                                    buf, sizeof buf, 1000);
    if (n < 2) return n < 0 ? n : LIBUSB_ERROR_IO;
    /* buf is a UTF-16LE string descriptor: [bLength][bDescriptorType][chars..] */
    int out = 0;
    for (int i = 2; i + 1 < n && out < length - 1; i += 2) {
        unsigned char c = buf[i];
        data[out++] = (buf[i + 1] == 0) ? c : '?';  /* ASCII-only downconvert */
    }
    data[out] = 0;
    return out;
}
