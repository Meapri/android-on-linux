/* Hand-rolled to match wayland-scanner 1.25.0 `server-header` output. */
/*
 * Equivalent of:
 *   wayland-scanner server-header \
 *     third_party/protocols/text-input/text-input-unstable-v3.xml \
 *     text-input-unstable-v3-server-protocol.h
 *
 * No wayland-scanner exists on this build host (third_party/VENDORING.md), and
 * the vendored wayland tree does NOT ship scanner.c / the protocol DTD, so this
 * glue was written by hand from the vendored canonical protocol XML
 * (third_party/protocols/text-input/text-input-unstable-v3.xml, version 1). It
 * mirrors the structure/format the scanner emits for the OTHER protocols in this
 * directory (wayland-server-protocol.h, xdg-shell-server-protocol.h): the
 * request listener structs, the `_send_*` inline event helpers, the `*_SINCE_*`
 * macros, and the opcode #defines. The matching marshalling tables /
 * wl_interface symbols live in text-input-unstable-v3-protocol.c (private-code).
 *
 * Only the two interfaces this feature needs are emitted: zwp_text_input_v3 and
 * zwp_text_input_manager_v3.
 */

#ifndef TEXT_INPUT_UNSTABLE_V3_SERVER_PROTOCOL_H
#define TEXT_INPUT_UNSTABLE_V3_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

struct wl_seat;
struct wl_surface;
struct zwp_text_input_v3;
struct zwp_text_input_manager_v3;

#ifndef ZWP_TEXT_INPUT_V3_INTERFACE
#define ZWP_TEXT_INPUT_V3_INTERFACE
extern const struct wl_interface zwp_text_input_v3_interface;
#endif
#ifndef ZWP_TEXT_INPUT_MANAGER_V3_INTERFACE
#define ZWP_TEXT_INPUT_MANAGER_V3_INTERFACE
extern const struct wl_interface zwp_text_input_manager_v3_interface;
#endif

#ifndef ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_ENUM
#define ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_ENUM
/**
 * @ingroup iface_zwp_text_input_v3
 * text change reason
 */
enum zwp_text_input_v3_change_cause {
	ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD = 0,
	ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER = 1,
};
#endif /* ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_ENUM */

#ifndef ZWP_TEXT_INPUT_V3_CONTENT_HINT_ENUM
#define ZWP_TEXT_INPUT_V3_CONTENT_HINT_ENUM
/**
 * @ingroup iface_zwp_text_input_v3
 * content hint
 *
 * Content hint is a bitmask to allow to modify the behavior of the text
 * input.
 */
enum zwp_text_input_v3_content_hint {
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE = 0x0,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_COMPLETION = 0x1,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK = 0x2,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_AUTO_CAPITALIZATION = 0x4,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_LOWERCASE = 0x8,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_UPPERCASE = 0x10,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_TITLECASE = 0x20,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT = 0x40,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA = 0x80,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_LATIN = 0x100,
	ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE = 0x200,
};
#endif /* ZWP_TEXT_INPUT_V3_CONTENT_HINT_ENUM */

#ifndef ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ENUM
#define ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ENUM
/**
 * @ingroup iface_zwp_text_input_v3
 * content purpose
 *
 * The content purpose allows to specify the primary purpose of a text
 * input.
 */
enum zwp_text_input_v3_content_purpose {
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL = 0,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ALPHA = 1,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DIGITS = 2,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER = 3,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE = 4,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL = 5,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL = 6,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NAME = 7,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD = 8,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN = 9,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATE = 10,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TIME = 11,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATETIME = 12,
	ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL = 13,
};
#endif /* ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_ENUM */

/**
 * @ingroup iface_zwp_text_input_v3
 * @struct zwp_text_input_v3_interface
 */
struct zwp_text_input_v3_interface {
	/**
	 * Destroy the wp_text_input
	 */
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	/**
	 * Request text input to be enabled
	 */
	void (*enable)(struct wl_client *client,
		       struct wl_resource *resource);
	/**
	 * Disable text input on a surface
	 */
	void (*disable)(struct wl_client *client,
			struct wl_resource *resource);
	/**
	 * sets the surrounding text
	 */
	void (*set_surrounding_text)(struct wl_client *client,
				     struct wl_resource *resource,
				     const char *text,
				     int32_t cursor,
				     int32_t anchor);
	/**
	 * indicates the cause of surrounding text change
	 */
	void (*set_text_change_cause)(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t cause);
	/**
	 * set content purpose and hint
	 */
	void (*set_content_type)(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t hint,
				 uint32_t purpose);
	/**
	 * set cursor position
	 */
	void (*set_cursor_rectangle)(struct wl_client *client,
				     struct wl_resource *resource,
				     int32_t x,
				     int32_t y,
				     int32_t width,
				     int32_t height);
	/**
	 * commit state
	 */
	void (*commit)(struct wl_client *client,
		       struct wl_resource *resource);
};

#define ZWP_TEXT_INPUT_V3_ENTER 0
#define ZWP_TEXT_INPUT_V3_LEAVE 1
#define ZWP_TEXT_INPUT_V3_PREEDIT_STRING 2
#define ZWP_TEXT_INPUT_V3_COMMIT_STRING 3
#define ZWP_TEXT_INPUT_V3_DELETE_SURROUNDING_TEXT 4
#define ZWP_TEXT_INPUT_V3_DONE 5

/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_ENTER_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_LEAVE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_PREEDIT_STRING_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_COMMIT_STRING_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_DELETE_SURROUNDING_TEXT_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_DONE_SINCE_VERSION 1

/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_DESTROY_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_ENABLE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_DISABLE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_SET_SURROUNDING_TEXT_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_SET_TEXT_CHANGE_CAUSE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_SET_CONTENT_TYPE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_SET_CURSOR_RECTANGLE_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_v3
 */
#define ZWP_TEXT_INPUT_V3_COMMIT_SINCE_VERSION 1

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an enter event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
zwp_text_input_v3_send_enter(struct wl_resource *resource_, struct wl_resource *surface)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_ENTER, surface);
}

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an leave event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
zwp_text_input_v3_send_leave(struct wl_resource *resource_, struct wl_resource *surface)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_LEAVE, surface);
}

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an preedit_string event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
zwp_text_input_v3_send_preedit_string(struct wl_resource *resource_, const char *text, int32_t cursor_begin, int32_t cursor_end)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_PREEDIT_STRING, text, cursor_begin, cursor_end);
}

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an commit_string event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
zwp_text_input_v3_send_commit_string(struct wl_resource *resource_, const char *text)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_COMMIT_STRING, text);
}

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an delete_surrounding_text event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
zwp_text_input_v3_send_delete_surrounding_text(struct wl_resource *resource_, uint32_t before_length, uint32_t after_length)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_DELETE_SURROUNDING_TEXT, before_length, after_length);
}

/**
 * @ingroup iface_zwp_text_input_v3
 * Sends an done event to the client owning the resource.
 * @param resource_ The client's resource
 * @param serial serial of the last commit request
 */
static inline void
zwp_text_input_v3_send_done(struct wl_resource *resource_, uint32_t serial)
{
	wl_resource_post_event(resource_, ZWP_TEXT_INPUT_V3_DONE, serial);
}

/**
 * @ingroup iface_zwp_text_input_manager_v3
 * @struct zwp_text_input_manager_v3_interface
 */
struct zwp_text_input_manager_v3_interface {
	/**
	 * Destroy the wp_text_input_manager
	 */
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	/**
	 * create a new text input object
	 */
	void (*get_text_input)(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t id,
			       struct wl_resource *seat);
};

/**
 * @ingroup iface_zwp_text_input_manager_v3
 */
#define ZWP_TEXT_INPUT_MANAGER_V3_DESTROY_SINCE_VERSION 1
/**
 * @ingroup iface_zwp_text_input_manager_v3
 */
#define ZWP_TEXT_INPUT_MANAGER_V3_GET_TEXT_INPUT_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
