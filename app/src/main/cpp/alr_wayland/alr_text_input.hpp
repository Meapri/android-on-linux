// ALR Android IME <-> Wayland text-input bridge — public boundary (NEW, design
// asset for docs/design/android-ime-text-input.md, branch auto/host-ime).
//
// This header declares the TWO-WAY boundary that closes the soft-keyboard loop:
//
//   guest text field focus  --(zwp_text_input_v3.enable)-->  compositor
//        --(this header: IME-show callback)-->  Kotlin InputMethodManager.showSoftInput
//
//   user types on soft keyboard  -->  InputConnection.commitText / sendKeyEvent
//        --(this header: commit / key inject)-->  compositor
//        --(wl_keyboard.key  OR  zwp_text_input_v3.commit_string)-->  guest
//
// It is intentionally header-only and free of any Wayland or Android type so it
// can be included from BOTH:
//   * alr_compositor.cpp  (the compositor owns the zwp_text_input_v3 server glue
//     and CALLS alr_ime_emit_*; it IMPLEMENTS alr_ime_commit_text / *_key)
//   * runtime_report.cpp  (the JNI bridge: it REGISTERS the show/hide callback
//     so a guest enable upcalls into Kotlin, and forwards Kotlin commitText /
//     deleteSurroundingText / sendKeyEvent into the compositor)
//
// NOTHING here is implemented in this file (it would belong in alr_compositor.cpp,
// which this worker must not edit). The functions below are the agreed signatures;
// the owner wiring section of the design doc gives the bodies. Treated as a spec.
//
// Threading contract (mirrors alr_compositor.hpp's inject API):
//   - alr_ime_commit_text / *_delete_surrounding / *_key are callable from ANY
//     thread (the Android UI thread). They enqueue onto the existing g_inject
//     mechanism and wake the compositor; the actual wl_resource sends happen on
//     the compositor thread, exactly like alr_wayland_inject_key today.
//   - alr_ime_set_state_callback is called once, on startup, from the JNI thread.
//   - The registered callback (AlrImeStateFn) is INVOKED ON THE COMPOSITOR THREAD
//     (from the zwp_text_input_v3.commit handler). The JNI implementation MUST NOT
//     touch JNIEnv directly there; it must AttachCurrentThread / post to the main
//     looper before calling InputMethodManager (see the doc's MainActivity wiring).

#ifndef ALR_WAYLAND_ALR_TEXT_INPUT_HPP
#define ALR_WAYLAND_ALR_TEXT_INPUT_HPP

#include <cstdint>

namespace alr::wayland {

// ---- (a) guest -> Android: text-input enable/disable upcall ----------------
//
// content_purpose / content_hint mirror zwp_text_input_v3's enums verbatim (see
// text-input-unstable-v3.xml), so the Android side can pick the right IME
// inputType (password -> TYPE_TEXT_VARIATION_PASSWORD, number -> TYPE_CLASS_NUMBER,
// url/email/...); 0/0 == normal/none. cursor_{x,y,w,h} are the surface-local
// cursor rectangle from set_cursor_rectangle (output px after the compositor
// scales surface-local -> SurfaceView px), so the IME can avoid covering the
// caret; all -1 when the client did not provide one.
struct AlrImeState {
    bool enabled = false;             // true => show soft keyboard; false => hide
    uint32_t content_purpose = 0;     // zwp_text_input_v3 content_purpose
    uint32_t content_hint = 0;        // zwp_text_input_v3 content_hint bitfield
    int32_t cursor_x = -1;            // surface-local cursor rect, output px (-1=none)
    int32_t cursor_y = -1;
    int32_t cursor_w = -1;
    int32_t cursor_h = -1;
};

// Callback invoked (on the COMPOSITOR thread) whenever the focused guest's
// text-input enable/disable state changes (its zwp_text_input_v3.commit applied
// an enable or disable, or focus moved off a text-input surface => synthesized
// disable). user_data is the pointer passed to alr_ime_set_state_callback.
using AlrImeStateFn = void (*)(const AlrImeState& state, void* user_data);

// Register (or clear, fn=nullptr) the IME-state callback. Idempotent; the last
// registration wins. Call once from JNI startup (after the compositor starts).
void alr_ime_set_state_callback(AlrImeStateFn fn, void* user_data);

// ---- (b)/(c) Android -> guest: route IME edits back to the focused guest ----
//
// Direct-text path (preferred when the focused guest bound zwp_text_input_v3):
// send a zwp_text_input_v3.commit_string + done to the guest. utf8 is a
// NUL-terminated UTF-8 buffer; len is its byte length (excluding NUL).
void alr_ime_commit_text(const char* utf8, int32_t len);

// zwp_text_input_v3.delete_surrounding_text + done. before/after are byte counts
// (the InputConnection deleteSurroundingText callback gives char counts; the
// Kotlin side converts to UTF-8 bytes — see the doc). No-op if no text-input
// guest is focused.
void alr_ime_delete_surrounding(uint32_t before_bytes, uint32_t after_bytes);

// Keysym/keymap path (the FALLBACK + the only path for guests that don't bind
// zwp_text_input_v3, e.g. foot terminal, SDL games, raw xkb apps): inject one
// committed Unicode code point as a synthesized wl_keyboard key press+release on
// a dedicated "IME scratch" keysym slot, with the correct level (Shift) derived
// from the keymap. codepoint is a Unicode scalar (U+XXXX). Returns true if a
// keymap mapping was found and the key was injected, false if the code point is
// not reachable on the shipped keymap (caller should then fall back to
// commit_string, or queue a keymap-reload — see the doc's "direct-text vs
// keymap" section).
bool alr_ime_inject_codepoint(uint32_t codepoint);

// Whether the currently focused guest has an ENABLED zwp_text_input_v3 (so the
// Android InputConnection should prefer commit_string over key synthesis). False
// => no text-input guest; the InputConnection must translate to wl_keyboard keys
// (alr_ime_inject_codepoint / the existing evdev key path).
bool alr_ime_focus_has_text_input();

}  // namespace alr::wayland

#endif  // ALR_WAYLAND_ALR_TEXT_INPUT_HPP
