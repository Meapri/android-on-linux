/* alr_usb_proto.h — the GUEST(libusb shim) <-> ANDROID(UsbHostBridge) wire
 * contract for ALR USB host access (Option B1 of docs/design/android-usb-host.md).
 *
 * THIS IS THE CONTRACT. The guest-side libusb shim (alr_libusb_shim.c) and the
 * Android-side bridge (dev.chanwoo.androlinux.usb.UsbHostBridge.kt) must agree
 * on every opcode value and field order/type below. If you change one side you
 * MUST change the other, or the bridge silently mis-decodes the stream.
 *
 * WIRE FORMAT
 *   Length-prefixed binary, LITTLE-ENDIAN (the only ABI; aarch64 + the JVM on
 *   this device are LE). Every message is one fixed 12-byte header followed by
 *   an opcode-specific payload:
 *
 *     struct alr_usb_hdr { u32 len; u16 op; u16 flags; u32 tag; }   // 12 bytes
 *
 *   - len   = number of payload bytes that FOLLOW the header (NOT counting the
 *             12 header bytes). A pure ack/error with no payload has len==0.
 *   - op    = one of enum AlrUsbOp below.
 *   - flags = bit0 (ALR_USB_FLAG_ERR) set on a reply means the payload is a
 *             single i32 libusb error code (negative LIBUSB_ERROR_*). Other
 *             bits reserved (0).
 *   - tag   = request/reply correlation id chosen by the shim; the bridge
 *             echoes it on the matching reply. Unsolicited async messages
 *             (COMPLETE/HOTPLUG) carry tag==0 for HOTPLUG and the original
 *             SUBMIT tag (the libusb transfer's user token) for COMPLETE.
 *
 *   Multi-byte payload integers are LE. A "blob" is a u32 length followed by
 *   exactly that many raw bytes (no padding/alignment). Strings are blobs of
 *   UTF-8 with NO trailing NUL.
 *
 * This header is pure C (C99), buildable with zig cc / gcc for the aarch64
 * glibc guest. The Kotlin bridge mirrors these constants by hand (its own
 * copy, kept in sync via the per-op comments here).
 */
#ifndef ALR_USB_PROTO_H
#define ALR_USB_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Header ------------------------------------------------------------- */

#define ALR_USB_HDR_SIZE 12u   /* sizeof on the wire (packed); see note below */

/* The header is sent/received field-by-field little-endian (NOT memcpy'd as a
 * struct) so there is zero dependence on the C struct's natural padding. This
 * type is a convenience for in-memory bookkeeping only. */
struct alr_usb_hdr {
    uint32_t len;    /* payload bytes after this header */
    uint16_t op;     /* enum AlrUsbOp */
    uint16_t flags;  /* ALR_USB_FLAG_* */
    uint32_t tag;    /* request/reply correlation */
};

#define ALR_USB_FLAG_ERR 0x0001u  /* reply payload is a single i32 errno */

/* ---- Opcodes ------------------------------------------------------------ *
 * Request ops are even-ish groups; async (bridge->shim, unsolicited) ops are
 * >= 0x80. Values are the wire contract; do not renumber. The Kotlin bridge
 * has the SAME numbers (UsbHostBridge.Op).                                  */
enum AlrUsbOp {
    ALR_USB_OP_NONE       = 0,

    /* request (shim -> bridge) -> reply (bridge -> shim, same tag) */
    ALR_USB_OP_ENUMERATE  = 1,  /* req: -                              reply: u32 count + count*{u16 vid,u16 pid,u8 bus,u8 addr,u8 class} */
    ALR_USB_OP_OPEN       = 2,  /* req: u16 vid,u16 pid,u8 bus,u8 addr reply: u32 connId + blob(rawDescriptors) | ERR(errno) */
    ALR_USB_OP_CLOSE      = 3,  /* req: u32 connId                     reply: - (ok) */
    ALR_USB_OP_CLAIM      = 4,  /* req: u32 connId,u8 iface,u8 force   reply: - | ERR */
    ALR_USB_OP_RELEASE    = 5,  /* req: u32 connId,u8 iface            reply: - | ERR */
    ALR_USB_OP_SET_CONFIG = 6,  /* req: u32 connId,i32 config          reply: - | ERR */
    ALR_USB_OP_SET_ALT    = 7,  /* req: u32 connId,u8 iface,u8 alt     reply: - | ERR */
    ALR_USB_OP_CONTROL    = 8,  /* req: u32 connId,u8 bmReqType,u8 bReq,u16 wValue,u16 wIndex,u16 wLength,i32 timeout,[blob data if host->dev]
                                 * reply: i32 transferred,[blob data if dev->host] | ERR */
    ALR_USB_OP_XFER       = 9,  /* req: u32 connId,u8 epAddr,u8 type,i32 length,i32 timeout,[blob data if OUT]
                                 * reply: i32 transferred,[blob data if IN] | ERR     (type: 0=bulk,1=interrupt) */
    ALR_USB_OP_CLEAR_HALT = 10, /* req: u32 connId,u8 epAddr           reply: - | ERR */
    ALR_USB_OP_RESET      = 11, /* req: u32 connId                     reply: - | ERR  (best-effort; often NOT_SUPPORTED) */
    ALR_USB_OP_SUBMIT     = 12, /* req: u32 connId,u8 epAddr,u8 type,i32 length,[blob data if OUT]; tag = libusb user token.
                                 * reply: - (immediate ack); result arrives later as ALR_USB_OP_COMPLETE with the same tag */
    ALR_USB_OP_CANCEL     = 13, /* req: u32 connId (tag = the SUBMIT tag to cancel)   reply: - */

    /* async (bridge -> shim, unsolicited) */
    ALR_USB_OP_COMPLETE   = 0x80, /* tag = SUBMIT tag; payload: i32 status,i32 transferred,[blob data if IN] */
    ALR_USB_OP_HOTPLUG    = 0x81, /* tag=0; payload: u8 added(1)/removed(0),u16 vid,u16 pid,u8 bus,u8 addr */
};

/* ALR_USB_OP_XFER / SUBMIT transfer-type tags (match Android UsbConstants intent). */
enum AlrUsbXferType {
    ALR_USB_XFER_BULK      = 0,
    ALR_USB_XFER_INTERRUPT = 1,
};

/* ---- libusb error codes (subset used on the wire) ----------------------- *
 * These mirror upstream libusb's enum libusb_error so the shim returns the
 * bridge's i32 verbatim to the caller. Kept here so the bridge can name them. */
enum AlrUsbError {
    ALR_USB_SUCCESS            =  0,
    ALR_USB_ERROR_IO           = -1,
    ALR_USB_ERROR_INVALID_PARAM= -2,
    ALR_USB_ERROR_ACCESS       = -3,
    ALR_USB_ERROR_NO_DEVICE    = -4,
    ALR_USB_ERROR_NOT_FOUND    = -5,
    ALR_USB_ERROR_BUSY         = -6,
    ALR_USB_ERROR_TIMEOUT      = -7,
    ALR_USB_ERROR_OVERFLOW     = -8,
    ALR_USB_ERROR_PIPE         = -9,
    ALR_USB_ERROR_INTERRUPTED  = -10,
    ALR_USB_ERROR_NO_MEM       = -11,
    ALR_USB_ERROR_NOT_SUPPORTED= -12,
    ALR_USB_ERROR_OTHER        = -99,
};

/* ---- Tiny LE cursor encode/decode helpers (header-only, inline) --------- *
 * Used by the shim. The bridge uses java.nio.ByteBuffer(LITTLE_ENDIAN); these
 * produce the identical byte layout.                                        */

typedef struct { uint8_t* p; size_t cap; size_t len; } alr_usb_wbuf;
typedef struct { const uint8_t* p; size_t cap; size_t pos; } alr_usb_rbuf;

static inline int alr_usb_w_u8(alr_usb_wbuf* b, uint8_t v) {
    if (b->len + 1 > b->cap) return -1;
    b->p[b->len++] = v; return 0;
}
static inline int alr_usb_w_u16(alr_usb_wbuf* b, uint16_t v) {
    if (b->len + 2 > b->cap) return -1;
    b->p[b->len++] = (uint8_t)(v & 0xff);
    b->p[b->len++] = (uint8_t)((v >> 8) & 0xff);
    return 0;
}
static inline int alr_usb_w_u32(alr_usb_wbuf* b, uint32_t v) {
    if (b->len + 4 > b->cap) return -1;
    b->p[b->len++] = (uint8_t)(v & 0xff);
    b->p[b->len++] = (uint8_t)((v >> 8) & 0xff);
    b->p[b->len++] = (uint8_t)((v >> 16) & 0xff);
    b->p[b->len++] = (uint8_t)((v >> 24) & 0xff);
    return 0;
}
static inline int alr_usb_w_i32(alr_usb_wbuf* b, int32_t v) {
    return alr_usb_w_u32(b, (uint32_t)v);
}
static inline int alr_usb_w_blob(alr_usb_wbuf* b, const void* data, uint32_t n) {
    if (alr_usb_w_u32(b, n) != 0) return -1;
    if (b->len + n > b->cap) return -1;
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

static inline int alr_usb_r_u8(alr_usb_rbuf* b, uint8_t* out) {
    if (b->pos + 1 > b->cap) return -1;
    *out = b->p[b->pos++]; return 0;
}
static inline int alr_usb_r_u16(alr_usb_rbuf* b, uint16_t* out) {
    if (b->pos + 2 > b->cap) return -1;
    *out = (uint16_t)(b->p[b->pos] | (b->p[b->pos + 1] << 8));
    b->pos += 2; return 0;
}
static inline int alr_usb_r_u32(alr_usb_rbuf* b, uint32_t* out) {
    if (b->pos + 4 > b->cap) return -1;
    *out = (uint32_t)(b->p[b->pos]
                      | (b->p[b->pos + 1] << 8)
                      | (b->p[b->pos + 2] << 16)
                      | ((uint32_t)b->p[b->pos + 3] << 24));
    b->pos += 4; return 0;
}
static inline int alr_usb_r_i32(alr_usb_rbuf* b, int32_t* out) {
    return alr_usb_r_u32(b, (uint32_t*)out);
}

#ifdef __cplusplus
}
#endif

#endif /* ALR_USB_PROTO_H */
