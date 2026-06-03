// ALR in-app PulseAudio-native-protocol server (host-only, design:
// docs/design/android-audio-sink.md §5). A self-contained AF_UNIX server that
// speaks just enough of the PulseAudio native wire protocol to accept playback
// streams from libpulse clients (Firefox/Chromium/mpv/SDL2/GStreamer/Qt) running
// in the guest, and pumps their PCM to an Android AAudio output stream.
//
// This module is INDEPENDENT of the Wayland compositor (audio is not Wayland):
// it owns its own listening socket under ${XDG_RUNTIME_DIR}/pulse/native, its own
// epoll thread, and its own AAudio sink. No SELinux bypass, public NDK API only,
// W^X-safe (no codegen / executable mappings).
//
// Protocol-version pragmatism (design §1b): we announce a MODEST protocol version
// (13) so libpulse downgrades to the simple memblock framing (no memfd/shm/
// srbchannel) and all PCM arrives as plain socket writes we read with recv().
#ifndef ALR_AUDIO_ALR_PULSE_SERVER_H
#define ALR_AUDIO_ALR_PULSE_SERVER_H

#include <cstdint>
#include <string>

namespace alr {
namespace audio {

// Start the server: bind ${xdg_runtime_dir}/pulse/native, listen, and spawn the
// epoll thread. device_rate is the device's native output sample rate (e.g.
// 48000, from AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE); the sink advertises this
// so well-behaved clients pre-resample. Idempotent: a second call while running
// is a no-op that returns the current status. Returns a human-readable status
// line ("ALR AUDIO SINK: started ..." / "... already-running" / "... FAIL ...").
std::string alr_audio_sink_start(const std::string& xdg_runtime_dir, int device_rate);

// One-line status: "ALR AUDIO SINK: <state> streams=<n> rate=<r> clients=<c>
// frames=<written>". Safe to call from any thread at any time.
std::string alr_audio_sink_status();

// Stop the server: break the epoll loop, join the thread, close the AAudio sink
// and the listening socket, unlink the socket path. Idempotent.
std::string alr_audio_sink_stop();

// True iff the epoll thread is running. (Mirrors alr_wayland_compositor_running.)
bool alr_audio_sink_running();

}  // namespace audio
}  // namespace alr

#endif  // ALR_AUDIO_ALR_PULSE_SERVER_H
