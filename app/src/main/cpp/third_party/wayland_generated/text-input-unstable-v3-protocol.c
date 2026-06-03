/* Hand-rolled to match wayland-scanner 1.25.0 `private-code` output. */
/*
 * Equivalent of:
 *   wayland-scanner private-code \
 *     third_party/protocols/text-input/text-input-unstable-v3.xml \
 *     text-input-unstable-v3-protocol.c
 *
 * See text-input-unstable-v3-server-protocol.h for why this is hand-written
 * (no wayland-scanner / scanner.c on this build host). This file defines the
 * wl_message request/event tables and the two wl_interface symbols
 * (zwp_text_input_v3_interface, zwp_text_input_manager_v3_interface) used by
 * both the server glue and wl_resource_create. `private-code` (not
 * `public-code`): the tables are linked statically into the app and not
 * re-exported, identical to wayland-protocol.c / xdg-shell-protocol.c here.
 *
 * Signatures (from the canonical XML, version 1):
 *   zwp_text_input_v3 requests:
 *     destroy ""  enable ""  disable ""  set_surrounding_text "sii"
 *     set_text_change_cause "u"  set_content_type "uu"
 *     set_cursor_rectangle "iiii"  commit ""
 *   zwp_text_input_v3 events:
 *     enter "o"(wl_surface)  leave "o"(wl_surface)
 *     preedit_string "?sii"  commit_string "?s"
 *     delete_surrounding_text "uu"  done "u"
 *   zwp_text_input_manager_v3 requests:
 *     destroy ""  get_text_input "no"(zwp_text_input_v3, wl_seat)
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

#ifndef __has_attribute
# define __has_attribute(x) 0  /* Compatibility with non-clang compilers. */
#endif

#if (__has_attribute(visibility) || defined(__GNUC__) && __GNUC__ >= 4)
#define WL_PRIVATE __attribute__ ((visibility("hidden")))
#else
#define WL_PRIVATE
#endif

extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface zwp_text_input_v3_interface;

static const struct wl_interface *text_input_unstable_v3_types[] = {
	NULL,                          /* 0 */
	NULL,                          /* 1 */
	NULL,                          /* 2 */
	NULL,                          /* 3 */
	&wl_surface_interface,         /* 4: enter/leave .surface */
	&zwp_text_input_v3_interface,  /* 5: get_text_input .id (new_id) */
	&wl_seat_interface,            /* 6: get_text_input .seat */
};

static const struct wl_message zwp_text_input_v3_requests[] = {
	{ "destroy", "", text_input_unstable_v3_types + 0 },
	{ "enable", "", text_input_unstable_v3_types + 0 },
	{ "disable", "", text_input_unstable_v3_types + 0 },
	{ "set_surrounding_text", "sii", text_input_unstable_v3_types + 0 },
	{ "set_text_change_cause", "u", text_input_unstable_v3_types + 0 },
	{ "set_content_type", "uu", text_input_unstable_v3_types + 0 },
	{ "set_cursor_rectangle", "iiii", text_input_unstable_v3_types + 0 },
	{ "commit", "", text_input_unstable_v3_types + 0 },
};

static const struct wl_message zwp_text_input_v3_events[] = {
	{ "enter", "o", text_input_unstable_v3_types + 4 },
	{ "leave", "o", text_input_unstable_v3_types + 4 },
	{ "preedit_string", "?sii", text_input_unstable_v3_types + 0 },
	{ "commit_string", "?s", text_input_unstable_v3_types + 0 },
	{ "delete_surrounding_text", "uu", text_input_unstable_v3_types + 0 },
	{ "done", "u", text_input_unstable_v3_types + 0 },
};

WL_PRIVATE const struct wl_interface zwp_text_input_v3_interface = {
	"zwp_text_input_v3", 1,
	8, zwp_text_input_v3_requests,
	6, zwp_text_input_v3_events,
};

static const struct wl_message zwp_text_input_manager_v3_requests[] = {
	{ "destroy", "", text_input_unstable_v3_types + 0 },
	{ "get_text_input", "no", text_input_unstable_v3_types + 5 },
};

WL_PRIVATE const struct wl_interface zwp_text_input_manager_v3_interface = {
	"zwp_text_input_manager_v3", 1,
	2, zwp_text_input_manager_v3_requests,
	0, NULL,
};
