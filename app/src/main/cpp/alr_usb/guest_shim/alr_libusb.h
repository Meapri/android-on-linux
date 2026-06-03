/* alr_libusb.h — the SUBSET of the public libusb-1.0 API that the ALR libusb
 * shim implements, declared with the EXACT upstream signatures/struct layouts.
 *
 * This is NOT a fork of upstream <libusb.h>. It is a minimal, ABI-compatible
 * re-declaration of just the ~40 symbols ALR's shim provides (Option B1 of
 * docs/design/android-usb-host.md §3). The struct field order/types and enum
 * values are copied verbatim from upstream libusb 1.0 so that an UNMODIFIED
 * guest app compiled against the distro <libusb.h> binds to our shim's
 * libusb-1.0.so.0 transparently (the public ABI is stable and small).
 *
 * Where a guest app includes the distro's own <libusb.h>, THAT header provides
 * the declarations and this file is irrelevant to the app; this header exists
 * only so the SHIM ITSELF compiles standalone (no libusb-dev on the build host).
 *
 * Source of truth for layouts: libusb/libusb.h, libusb 1.0.x (LGPL-2.1; only
 * the public interface — declarations, not code — is reproduced).
 */
#ifndef ALR_LIBUSB_H
#define ALR_LIBUSB_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* descriptor type sizes per USB 2.0 spec / libusb */
struct libusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

struct libusb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
    uint8_t  bRefresh;
    uint8_t  bSynchAddress;
    const unsigned char* extra;
    int      extra_length;
};

struct libusb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
    const struct libusb_endpoint_descriptor* endpoint;
    const unsigned char* extra;
    int      extra_length;
};

struct libusb_interface {
    const struct libusb_interface_descriptor* altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
    const struct libusb_interface* interface;
    const unsigned char* extra;
    int      extra_length;
};

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

/* enum libusb_error (upstream values) */
enum libusb_error {
    LIBUSB_SUCCESS             =  0,
    LIBUSB_ERROR_IO            = -1,
    LIBUSB_ERROR_INVALID_PARAM = -2,
    LIBUSB_ERROR_ACCESS        = -3,
    LIBUSB_ERROR_NO_DEVICE     = -4,
    LIBUSB_ERROR_NOT_FOUND     = -5,
    LIBUSB_ERROR_BUSY          = -6,
    LIBUSB_ERROR_TIMEOUT       = -7,
    LIBUSB_ERROR_OVERFLOW      = -8,
    LIBUSB_ERROR_PIPE          = -9,
    LIBUSB_ERROR_INTERRUPTED   = -10,
    LIBUSB_ERROR_NO_MEM        = -11,
    LIBUSB_ERROR_NOT_SUPPORTED = -12,
    LIBUSB_ERROR_OTHER         = -99,
};

/* transfer status (upstream values) */
enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED = 0,
    LIBUSB_TRANSFER_ERROR     = 1,
    LIBUSB_TRANSFER_TIMED_OUT = 2,
    LIBUSB_TRANSFER_CANCELLED = 3,
    LIBUSB_TRANSFER_STALL     = 4,
    LIBUSB_TRANSFER_NO_DEVICE = 5,
    LIBUSB_TRANSFER_OVERFLOW  = 6,
};

enum libusb_transfer_type {
    LIBUSB_TRANSFER_TYPE_CONTROL     = 0,
    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS = 1,
    LIBUSB_TRANSFER_TYPE_BULK        = 2,
    LIBUSB_TRANSFER_TYPE_INTERRUPT   = 3,
    LIBUSB_TRANSFER_TYPE_BULK_STREAM = 4,
};

/* endpoint direction bits in bEndpointAddress */
#define LIBUSB_ENDPOINT_IN  0x80
#define LIBUSB_ENDPOINT_OUT 0x00

/* hotplug */
typedef int libusb_hotplug_callback_handle;
enum {
    LIBUSB_HOTPLUG_NO_FLAGS        = 0,
    LIBUSB_HOTPLUG_ENUMERATE       = (1 << 0),
};
enum {
    LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED = (1 << 0),
    LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT    = (1 << 1),
};
#define LIBUSB_HOTPLUG_MATCH_ANY (-1)
typedef int libusb_hotplug_event;
typedef int (*libusb_hotplug_callback_fn)(libusb_context* ctx,
                                          libusb_device* device,
                                          libusb_hotplug_event event,
                                          void* user_data);

/* async transfer */
struct libusb_transfer;
typedef void (*libusb_transfer_cb_fn)(struct libusb_transfer* transfer);

struct libusb_transfer {
    libusb_device_handle* dev_handle;
    uint8_t  flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    int status;             /* enum libusb_transfer_status */
    int length;
    int actual_length;
    libusb_transfer_cb_fn callback;
    void* user_data;
    unsigned char* buffer;
    int num_iso_packets;
    /* upstream has a flexible iso_packet_desc[] here; the shim never indexes it
     * for bulk/interrupt/control transfers, so it is omitted. */
};

/* ---- public function prototypes implemented by the ALR shim ----
 * (so the single-TU shim has all forward references resolved; the distro
 * <libusb.h> declares the same signatures for guest apps).               */
int  libusb_init(libusb_context** ctx);
void libusb_exit(libusb_context* ctx);
void libusb_set_debug(libusb_context* ctx, int level);
int  libusb_set_option(libusb_context* ctx, int option, ...);
const char* libusb_error_name(int errcode);
const char* libusb_strerror(int errcode);

ssize_t libusb_get_device_list(libusb_context* ctx, libusb_device*** list);
void    libusb_free_device_list(libusb_device** list, int unref_devices);
libusb_device* libusb_ref_device(libusb_device* dev);
void           libusb_unref_device(libusb_device* dev);
uint8_t libusb_get_bus_number(libusb_device* dev);
uint8_t libusb_get_device_address(libusb_device* dev);
int     libusb_get_device_speed(libusb_device* dev);

int libusb_get_device_descriptor(libusb_device* dev, struct libusb_device_descriptor* desc);
int libusb_get_config_descriptor(libusb_device* dev, uint8_t config_index, struct libusb_config_descriptor** config);
int libusb_get_active_config_descriptor(libusb_device* dev, struct libusb_config_descriptor** config);
void libusb_free_config_descriptor(struct libusb_config_descriptor* config);

libusb_device_handle* libusb_open_device_with_vid_pid(libusb_context* ctx, uint16_t vid, uint16_t pid);
int  libusb_open(libusb_device* dev, libusb_device_handle** handle);
void libusb_close(libusb_device_handle* handle);
libusb_device* libusb_get_device(libusb_device_handle* handle);

int libusb_claim_interface(libusb_device_handle* h, int iface);
int libusb_release_interface(libusb_device_handle* h, int iface);
int libusb_set_configuration(libusb_device_handle* h, int configuration);
int libusb_set_interface_alt_setting(libusb_device_handle* h, int iface, int alt);
int libusb_clear_halt(libusb_device_handle* h, unsigned char endpoint);
int libusb_reset_device(libusb_device_handle* h);
int libusb_kernel_driver_active(libusb_device_handle* h, int iface);
int libusb_detach_kernel_driver(libusb_device_handle* h, int iface);
int libusb_attach_kernel_driver(libusb_device_handle* h, int iface);
int libusb_set_auto_detach_kernel_driver(libusb_device_handle* h, int enable);

int libusb_control_transfer(libusb_device_handle* h, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, unsigned char* data,
                            uint16_t wLength, unsigned int timeout);
int libusb_bulk_transfer(libusb_device_handle* h, unsigned char endpoint, unsigned char* data,
                         int length, int* transferred, unsigned int timeout);
int libusb_interrupt_transfer(libusb_device_handle* h, unsigned char endpoint, unsigned char* data,
                              int length, int* transferred, unsigned int timeout);

struct libusb_transfer* libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(struct libusb_transfer* t);
int  libusb_submit_transfer(struct libusb_transfer* xfer);
int  libusb_cancel_transfer(struct libusb_transfer* xfer);
int  libusb_handle_events(libusb_context* ctx);
int  libusb_handle_events_timeout(libusb_context* ctx, struct timeval* tv);
int  libusb_handle_events_completed(libusb_context* ctx, int* completed);

int  libusb_hotplug_register_callback(libusb_context* ctx, int events, int flags,
                                      int vendor_id, int product_id, int dev_class,
                                      libusb_hotplug_callback_fn cb, void* user_data,
                                      libusb_hotplug_callback_handle* handle);
void libusb_hotplug_deregister_callback(libusb_context* ctx, libusb_hotplug_callback_handle handle);
int  libusb_has_capability(uint32_t capability);
int  libusb_get_string_descriptor_ascii(libusb_device_handle* h, uint8_t desc_index,
                                         unsigned char* data, int length);

#ifdef __cplusplus
}
#endif

#endif /* ALR_LIBUSB_H */
