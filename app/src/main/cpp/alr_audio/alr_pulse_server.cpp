// ALR in-app PulseAudio-native-protocol server (design §5). See alr_pulse_server.h.
//
// One epoll thread accepts AF_UNIX clients on ${XDG_RUNTIME_DIR}/pulse/native and
// drives the playback subset of the PulseAudio native protocol (alr_pulse_proto.h).
// Decoded PCM is converted to the device format (S16, deviceRate, stereo), summed
// across streams into a lock-light ring, and pulled by an AAudio output stream's
// realtime data callback (Option A — no JNI on the audio data path).
//
// Threading model:
//   * epoll thread: accept(), recv() protocol frames, parse, push PCM into rings,
//     send REQUEST credit. Owns Client/Stream maps.
//   * AAudio callback thread: pulls from rings via a per-stream SpinLock-free-ish
//     mutex (short critical section). Writes silence on underrun.
//   * control thread (start/stop): the caller (MainActivity via JNI).
// All cross-thread state is guarded by g_server.mutex except the AAudio callback,
// which takes a short per-server data lock. No SELinux bypass; public NDK API only.

#include "alr_pulse_server.h"
#include "alr_pulse_proto.h"

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define ALR_AUD_TAG "alr_audio"
#define ALR_AUD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ALR_AUD_TAG, __VA_ARGS__)
#define ALR_AUD_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ALR_AUD_TAG, __VA_ARGS__)
#define ALR_AUD_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ALR_AUD_TAG, __VA_ARGS__)

namespace alr {
namespace audio {

using namespace proto;

namespace {

// Device sink fixed format: S16 interleaved stereo at deviceRate. We mix every
// client stream into this; the AAudio stream is opened with the same.
constexpr int kSinkChannels = 2;

// Ring sizing: ~1s of audio per stream is plenty of slack for socket jitter while
// keeping memory bounded (48000 * 4 bytes ≈ 192 KiB).
constexpr size_t kRingFramesDefault = 48000;

// A single playback stream's decode/convert/mix state.
struct PlaybackStream {
    uint32_t index = 0;          // stream index (== sink-input index here)
    uint32_t channel = 0;        // data-frame channel tag == index
    SampleSpec spec;             // client's negotiated spec
    bool corked = false;
    bool started = false;        // STARTED already sent?
    bool draining = false;
    uint32_t drain_tag = 0;      // tag of the in-flight DRAIN to ack on empty

    // Negotiated buffer attrs (bytes, in the CLIENT's sample spec).
    uint32_t tlength = 0;
    uint32_t minreq = 0;
    uint32_t prebuf = 0;
    uint32_t maxlength = 0;
    uint32_t fragsize = 0;       // unused (playback)

    // Credit accounting: how many client bytes we've granted via REQUEST but not
    // yet consumed. We keep ~tlength worth outstanding so the client stays fed.
    uint64_t requested_bytes = 0;   // total REQUEST credit granted
    uint64_t received_bytes = 0;    // total data bytes received from client

    // Device-format ring (S16 stereo @ deviceRate), measured in FRAMES.
    std::vector<int16_t> ring;   // size = ring_frames * kSinkChannels
    size_t ring_frames = 0;
    size_t head = 0;             // read cursor (frames)
    size_t tail = 0;             // write cursor (frames)
    size_t fill = 0;             // frames currently buffered

    // Per-stream resampler phase (linear interp) when client rate != deviceRate.
    double resample_pos = 0.0;
    int16_t last_l = 0, last_r = 0;

    uint64_t frames_played = 0;  // device frames pulled by AAudio for this stream
};

struct Client {
    int fd = -1;
    uint32_t index = 0;          // client index
    bool authed = false;
    std::string name;
    std::vector<uint8_t> rbuf;   // accumulated socket bytes (frame reassembly)
    std::vector<uint8_t> wbuf;   // pending outbound bytes (best-effort, blocking-ish)
    std::map<uint32_t, std::shared_ptr<PlaybackStream>> streams;  // index -> stream
};

struct Server {
    std::mutex mutex;            // guards all maps + ring metadata
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};

    int listen_fd = -1;
    int epoll_fd = -1;
    int wakeup_fd = -1;          // eventfd to break the loop
    std::string socket_path;
    std::string socket_dir;
    int device_rate = 48000;

    std::thread thread;
    std::map<int, std::shared_ptr<Client>> clients;  // fd -> client
    // Flat view of all active streams for the AAudio mixer (shared_ptr keeps them
    // alive even if a client is mid-teardown on the epoll thread).
    std::map<uint32_t, std::shared_ptr<PlaybackStream>> mix_streams;

    uint32_t next_client_index = 1;
    uint32_t next_stream_index = 1;

    AAudioStream* aaudio = nullptr;
    std::atomic<uint64_t> frames_written{0};  // device frames delivered to AAudio
    std::atomic<int> active_streams{0};
};

Server g_server;

// Forward decls for helpers defined further down (used before their definition).
void flush_wbuf(Client& c);

// --------------------------------------------------------------------------- //
// Format conversion: client PCM -> device S16 stereo @ deviceRate.
// --------------------------------------------------------------------------- //

// Decode one interleaved client frame at byte offset into L/R int16 (clamped).
// Returns false on an unsupported format.
inline bool decode_client_frame(const uint8_t* p, const SampleSpec& spec,
                                int16_t* l, int16_t* r) {
    const uint32_t ssz = sample_size_bytes(spec.format);
    auto rd_s16le = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>(q[0] | (q[1] << 8));
    };
    auto rd_s16be = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>(q[1] | (q[0] << 8));
    };
    auto rd_f32le = [](const uint8_t* q) -> int16_t {
        float f;
        std::memcpy(&f, q, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return static_cast<int16_t>(f * 32767.0f);
    };
    auto rd_s32le = [](const uint8_t* q) -> int16_t {
        int32_t v = static_cast<int32_t>(q[0] | (q[1] << 8) | (q[2] << 16) |
                                         (static_cast<uint32_t>(q[3]) << 24));
        return static_cast<int16_t>(v >> 16);
    };
    auto rd_u8 = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>((static_cast<int>(q[0]) - 128) << 8);
    };
    auto sample_at = [&](int ch) -> int16_t {
        const uint8_t* q = p + static_cast<size_t>(ch) * ssz;
        switch (spec.format) {
            case PA_SAMPLE_S16LE: return rd_s16le(q);
            case PA_SAMPLE_S16BE: return rd_s16be(q);
            case PA_SAMPLE_FLOAT32LE: return rd_f32le(q);
            case PA_SAMPLE_S32LE: return rd_s32le(q);
            case PA_SAMPLE_U8: return rd_u8(q);
            default: return 0;
        }
    };
    switch (spec.format) {
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_U8:
            break;
        default:
            return false;  // unsupported (alaw/ulaw/s24) — v1 NAKs at stream create
    }
    if (spec.channels >= 2) {
        *l = sample_at(0);
        *r = sample_at(1);
    } else {
        int16_t m = sample_at(0);
        *l = m;
        *r = m;
    }
    return true;
}

// Push a chunk of client PCM into the stream's device-format ring, resampling
// (linear) from spec.rate to device_rate. Caller holds g_server.mutex.
void push_pcm_to_ring(PlaybackStream& st, const uint8_t* data, size_t len,
                      int device_rate) {
    const uint32_t cf = frame_size_bytes(st.spec);
    if (cf == 0) return;
    const size_t nframes = len / cf;
    const bool need_resample = (st.spec.rate != static_cast<uint32_t>(device_rate)) &&
                               st.spec.rate != 0;
    const double step = need_resample
                            ? static_cast<double>(st.spec.rate) / device_rate
                            : 1.0;

    auto write_frame = [&](int16_t l, int16_t r) {
        if (st.fill >= st.ring_frames) {
            // Ring full: drop oldest (advance head). Bounded memory > perfect audio.
            st.head = (st.head + 1) % st.ring_frames;
            --st.fill;
        }
        st.ring[st.tail * kSinkChannels + 0] = l;
        st.ring[st.tail * kSinkChannels + 1] = r;
        st.tail = (st.tail + 1) % st.ring_frames;
        ++st.fill;
    };

    if (!need_resample) {
        for (size_t i = 0; i < nframes; ++i) {
            int16_t l = 0, r = 0;
            if (!decode_client_frame(data + i * cf, st.spec, &l, &r)) return;
            write_frame(l, r);
        }
        return;
    }

    // Linear resampler: emit device frames while resample_pos < nframes.
    double pos = st.resample_pos;
    int16_t prev_l = st.last_l, prev_r = st.last_r;
    while (pos < static_cast<double>(nframes)) {
        size_t i0 = static_cast<size_t>(pos);
        double frac = pos - static_cast<double>(i0);
        int16_t l0 = prev_l, r0 = prev_r, l1, r1;
        if (i0 == 0) {
            l0 = prev_l; r0 = prev_r;
        } else if (!decode_client_frame(data + (i0 - 1) * cf, st.spec, &l0, &r0)) {
            return;
        }
        if (!decode_client_frame(data + i0 * cf, st.spec, &l1, &r1)) return;
        int16_t l = static_cast<int16_t>(l0 + (l1 - l0) * frac);
        int16_t r = static_cast<int16_t>(r0 + (r1 - r0) * frac);
        write_frame(l, r);
        pos += step;
    }
    // Carry phase + last sample across chunks.
    if (nframes > 0) {
        decode_client_frame(data + (nframes - 1) * cf, st.spec, &prev_l, &prev_r);
        st.last_l = prev_l;
        st.last_r = prev_r;
    }
    st.resample_pos = pos - static_cast<double>(nframes);
}

// --------------------------------------------------------------------------- //
// AAudio data callback — pull mixed PCM (Option A, design §5d).
// --------------------------------------------------------------------------- //
aaudio_data_callback_result_t aaudio_cb(AAudioStream* /*stream*/, void* /*userData*/,
                                        void* audioData, int32_t numFrames) {
    int16_t* out = static_cast<int16_t*>(audioData);
    const size_t total = static_cast<size_t>(numFrames) * kSinkChannels;

    // Mix in an int32 accumulator then clip to int16 (design §5c). A thread-local
    // scratch buffer avoids a per-callback heap allocation on the realtime path.
    static thread_local std::vector<int32_t> acc;
    if (acc.size() < total) acc.resize(total);
    std::fill(acc.begin(), acc.begin() + total, 0);
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        for (auto& kv : g_server.mix_streams) {
            PlaybackStream& st = *kv.second;
            if (st.corked || st.ring_frames == 0) continue;
            size_t take = static_cast<size_t>(numFrames);
            if (take > st.fill) take = st.fill;
            for (size_t f = 0; f < take; ++f) {
                acc[f * kSinkChannels + 0] += st.ring[st.head * kSinkChannels + 0];
                acc[f * kSinkChannels + 1] += st.ring[st.head * kSinkChannels + 1];
                st.head = (st.head + 1) % st.ring_frames;
            }
            st.fill -= take;
            st.frames_played += take;
        }
    }

    for (size_t i = 0; i < total; ++i) {
        int32_t v = acc[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = static_cast<int16_t>(v);
    }
    g_server.frames_written.fetch_add(static_cast<uint64_t>(numFrames),
                                      std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool open_aaudio_locked() {
    if (g_server.aaudio != nullptr) return true;
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || builder == nullptr) {
        ALR_AUD_LOGE("AAudio_createStreamBuilder failed: %s",
                     AAudio_convertResultToText(res));
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, kSinkChannels);
    AAudioStreamBuilder_setSampleRate(builder, g_server.device_rate);
    AAudioStreamBuilder_setDataCallback(builder, aaudio_cb, nullptr);

    AAudioStream* stream = nullptr;
    res = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK || stream == nullptr) {
        ALR_AUD_LOGE("AAudioStreamBuilder_openStream failed: %s",
                     AAudio_convertResultToText(res));
        return false;
    }
    // The HAL may have given us a different rate; trust it for the mixer.
    g_server.device_rate = AAudioStream_getSampleRate(stream);
    res = AAudioStream_requestStart(stream);
    if (res != AAUDIO_OK) {
        ALR_AUD_LOGE("AAudioStream_requestStart failed: %s",
                     AAudio_convertResultToText(res));
        AAudioStream_close(stream);
        return false;
    }
    g_server.aaudio = stream;
    ALR_AUD_LOGI("AAudio output stream open: rate=%d ch=%d fmt=S16",
                 g_server.device_rate, kSinkChannels);
    return true;
}

void close_aaudio_locked() {
    if (g_server.aaudio) {
        AAudioStream_requestStop(g_server.aaudio);
        AAudioStream_close(g_server.aaudio);
        g_server.aaudio = nullptr;
    }
}

// --------------------------------------------------------------------------- //
// Socket framing: build a control-packet frame, queue it on the client.
// --------------------------------------------------------------------------- //

// Frame descriptor (PulseAudio): big-endian [length][channel][offset_hi]
// [offset_lo][flags] then payload. Control packets use channel = 0xFFFFFFFF and
// all-zero offset/flags.
void queue_frame(Client& c, uint32_t channel, const uint8_t* payload, uint32_t len) {
    uint8_t hdr[20];
    auto be = [&](int off, uint32_t v) {
        hdr[off + 0] = static_cast<uint8_t>(v >> 24);
        hdr[off + 1] = static_cast<uint8_t>(v >> 16);
        hdr[off + 2] = static_cast<uint8_t>(v >> 8);
        hdr[off + 3] = static_cast<uint8_t>(v);
    };
    be(0, len);
    be(4, channel);
    be(8, 0);   // offset_hi
    be(12, 0);  // offset_lo
    be(16, 0);  // flags
    c.wbuf.insert(c.wbuf.end(), hdr, hdr + sizeof(hdr));
    if (len) c.wbuf.insert(c.wbuf.end(), payload, payload + len);
}

void queue_command(Client& c, const TagWriter& tw) {
    queue_frame(c, kChannelControl, tw.bytes().data(),
                static_cast<uint32_t>(tw.size()));
}

// REPLY skeleton: U32(PA_COMMAND_REPLY) U32(tag).
TagWriter make_reply(uint32_t tag) {
    TagWriter tw;
    tw.put_u32(PA_COMMAND_REPLY);
    tw.put_u32(tag);
    return tw;
}

void queue_error(Client& c, uint32_t tag, uint32_t err) {
    TagWriter tw;
    tw.put_u32(PA_COMMAND_ERROR);
    tw.put_u32(tag);
    tw.put_u32(err);
    queue_command(c, tw);
}

// server->client REQUEST: "send me <nbytes> more bytes for stream <index>".
void queue_request(Client& c, uint32_t stream_index, uint32_t nbytes) {
    TagWriter tw;
    tw.put_u32(PA_COMMAND_REQUEST);
    tw.put_u32(0xFFFFFFFFu);  // tag (no reply expected)
    tw.put_u32(stream_index);
    tw.put_u32(nbytes);
    queue_command(c, tw);
}

void queue_started(Client& c, uint32_t stream_index) {
    TagWriter tw;
    tw.put_u32(PA_COMMAND_STARTED);
    tw.put_u32(0xFFFFFFFFu);
    tw.put_u32(stream_index);
    queue_command(c, tw);
}

// --------------------------------------------------------------------------- //
// Command handlers
// --------------------------------------------------------------------------- //

void handle_auth(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t client_version = 0;
    rd.get_u32(&client_version);  // protocol version (cookie follows; we ignore it)
    c.authed = true;
    // Reply: the protocol version WE speak (clamped to <= client's, >= ours floor).
    uint32_t ver = kProtocolVersion;
    if (client_version != 0 && (client_version & 0xFFFF) < ver) ver = client_version & 0xFFFF;
    TagWriter tw = make_reply(tag);
    tw.put_u32(ver);  // NO PA_PROTOCOL_FLAG_SHM/memfd bits -> socket-copy path
    queue_command(c, tw);
    ALR_AUD_LOGI("client %u AUTH (client_ver=%u -> server_ver=%u)", c.index,
                 client_version, ver);
}

void handle_set_client_name(Client& c, uint32_t tag, TagReader& rd) {
    // proplist (>= v13) — skip it; we just need to ACK with a client index.
    rd.skip_value();
    TagWriter tw = make_reply(tag);
    tw.put_u32(c.index);  // client index
    queue_command(c, tw);
}

void put_sink_info(TagWriter& tw, const SampleSpec& spec, int rate) {
    // pa_sink_info layout for protocol v13 (the fields libpulse reads, in order).
    tw.put_u32(0);                       // sink index
    tw.put_string("alr-android");        // name
    tw.put_string("ALR Android Sink");   // description
    SampleSpec ss = spec;
    ss.rate = static_cast<uint32_t>(rate);
    tw.put_sample_spec(ss);              // sample spec
    tw.put_channel_map_stereo();         // channel map
    tw.put_u32(PA_INVALID_INDEX);        // owner module
    tw.put_cvolume(2, PA_VOLUME_NORM);   // volume
    tw.put_boolean(false);              // mute
    tw.put_u32(PA_INVALID_INDEX);        // monitor source
    tw.put_string("alr-android.monitor"); // monitor source name
    tw.put_usec(0);                      // latency
    tw.put_string("alr");                // driver
    tw.put_u32(0x0001);                  // flags (HARDWARE)
    // v13 additions:
    tw.put_usec(2000000);                // configured latency (2s safety)
    tw.put_volume(PA_VOLUME_NORM);       // base volume
    tw.put_u32(0);                       // state (RUNNING=0)
    tw.put_u32(1);                       // n_volume_steps
    tw.put_u32(PA_INVALID_INDEX);        // card index
    tw.put_u32(0);                       // n_ports
    // proplist
    tw.put_empty_proplist();
}

void handle_get_sink_info(Client& c, uint32_t tag, const SampleSpec& spec, int rate,
                          bool list) {
    TagWriter tw = make_reply(tag);
    put_sink_info(tw, spec, rate);
    (void)list;  // single sink: LIST and single both return exactly one entry
    queue_command(c, tw);
}

void handle_get_server_info(Client& c, uint32_t tag, int rate) {
    TagWriter tw = make_reply(tag);
    tw.put_string("pulseaudio");           // package name
    tw.put_string("15.0.0");               // package version
    tw.put_string("alr");                  // user name
    tw.put_string("android");              // host name
    SampleSpec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.channels = 2;
    ss.rate = static_cast<uint32_t>(rate);
    tw.put_sample_spec(ss);                // default sample spec
    tw.put_string("alr-android");          // default sink name
    tw.put_string("alr-android.monitor");  // default source name
    tw.put_u32(0);                         // cookie
    tw.put_channel_map_stereo();           // default channel map (>= v15 ignores ok)
    queue_command(c, tw);
}

void handle_get_sink_input_info(Client& c, uint32_t tag, bool list) {
    // v1: report no sink-inputs from introspection (clients drive their own).
    TagWriter tw = make_reply(tag);
    (void)list;  // empty list -> just the REPLY header
    queue_command(c, tw);
}

// CREATE_PLAYBACK_STREAM: parse sample spec + buffer attrs, allocate the ring,
// reply with negotiated attrs + stream index, grant initial REQUEST credit.
void handle_create_playback_stream(Client& c, uint32_t tag, TagReader& rd) {
    auto st = std::make_shared<PlaybackStream>();

    std::string name;
    rd.get_string(&name);              // stream name (>= v13: may be in proplist)
    rd.get_sample_spec(&st->spec);     // pa_sample_spec
    rd.skip_value();                   // channel map
    uint32_t sink_index = PA_INVALID_INDEX;
    rd.get_u32(&sink_index);           // sink index
    std::string sink_name;
    rd.get_string(&sink_name);         // sink name
    rd.get_u32(&st->maxlength);        // maxlength
    bool corked = false;
    rd.get_boolean(&corked);           // corked
    rd.get_u32(&st->tlength);          // tlength (target buffer length, bytes)
    rd.get_u32(&st->prebuf);           // prebuf
    rd.get_u32(&st->minreq);           // minreq
    uint32_t syncid = 0;
    rd.get_u32(&syncid);               // sync id
    rd.skip_value();                   // cvolume (best-effort; rest of args ignored)
    (void)name; (void)sink_index; (void)sink_name; (void)syncid;

    if (!rd.ok()) {
        ALR_AUD_LOGW("client %u CREATE_PLAYBACK_STREAM parse error", c.index);
        queue_error(c, tag, PA_ERR_INVALID);
        return;
    }

    // Validate the sample spec; NAK unsupported formats (v1 path, design §5c/§6).
    int16_t dl, dr;
    uint8_t probe[8] = {0};
    if (!decode_client_frame(probe, st->spec, &dl, &dr) || st->spec.channels == 0 ||
        st->spec.rate == 0 || st->spec.rate > 192000) {
        ALR_AUD_LOGW("client %u unsupported spec fmt=%u ch=%u rate=%u", c.index,
                     st->spec.format, st->spec.channels, st->spec.rate);
        queue_error(c, tag, PA_ERR_NOTSUPPORTED);
        return;
    }

    st->corked = corked;

    // Negotiate buffer attrs: pick sane defaults if the client asked for the
    // "let the server decide" sentinel (0xFFFFFFFF) or 0.
    const uint32_t cf = frame_size_bytes(st->spec);
    auto def_or = [](uint32_t v, uint32_t d) {
        return (v == 0 || v == 0xFFFFFFFFu) ? d : v;
    };
    st->tlength = def_or(st->tlength, cf * (st->spec.rate / 4));   // ~250ms
    st->minreq = def_or(st->minreq, cf * (st->spec.rate / 20));    // ~50ms
    st->prebuf = def_or(st->prebuf, st->minreq);
    st->maxlength = def_or(st->maxlength, st->tlength * 4);
    if (st->minreq == 0) st->minreq = cf * 256;
    if (st->tlength < st->minreq) st->tlength = st->minreq * 2;

    // Device-format ring: ~1s plus the client's tlength worth, in device frames.
    size_t ring_frames = kRingFramesDefault;
    {
        // tlength bytes -> client frames -> device frames (rate-scaled, +slack).
        uint64_t client_frames = (cf ? st->tlength / cf : 0);
        uint64_t dev_frames = client_frames;
        if (st->spec.rate) {
            dev_frames = client_frames * g_server.device_rate / st->spec.rate;
        }
        if (dev_frames + kRingFramesDefault > ring_frames) {
            ring_frames = static_cast<size_t>(dev_frames) + kRingFramesDefault;
        }
    }
    st->ring_frames = ring_frames;
    st->ring.assign(ring_frames * kSinkChannels, 0);
    st->head = st->tail = st->fill = 0;

    uint32_t idx;
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        idx = g_server.next_stream_index++;
        st->index = idx;
        st->channel = idx;
        c.streams[idx] = st;
        g_server.mix_streams[idx] = st;
        g_server.active_streams.store(static_cast<int>(g_server.mix_streams.size()),
                                      std::memory_order_relaxed);
        // Open the AAudio device on first stream.
        if (!g_server.aaudio) open_aaudio_locked();
    }

    // Reply: stream index, sink-input index, missing bytes (==prebuf credit), and
    // the negotiated buffer attrs (>= v9 returns tlength/prebuf/minreq).
    TagWriter tw = make_reply(tag);
    tw.put_u32(idx);                 // stream index
    tw.put_u32(idx);                 // sink-input index
    tw.put_u32(st->tlength);         // missing bytes (initial credit ~ a full buffer)
    // >= v9 buffer attr block:
    tw.put_u32(st->maxlength);
    tw.put_u32(st->tlength);
    tw.put_u32(st->prebuf);
    tw.put_u32(st->minreq);
    // >= v12 returns the negotiated sample spec + channel map + ...
    tw.put_sample_spec(st->spec);
    tw.put_channel_map_stereo();
    tw.put_u32(0);                   // sink index
    tw.put_string("alr-android");    // sink name
    tw.put_boolean(false);          // sink suspended
    queue_command(c, tw);

    // Grant initial credit: ask for ~tlength bytes so the client fills the buffer.
    st->requested_bytes = st->tlength;
    queue_request(c, idx, st->tlength);

    ALR_AUD_LOGI("client %u CREATE_PLAYBACK_STREAM idx=%u fmt=%u ch=%u rate=%u "
                 "tlength=%u minreq=%u ring=%zu",
                 c.index, idx, st->spec.format, st->spec.channels, st->spec.rate,
                 st->tlength, st->minreq, st->ring_frames);
}

void erase_stream(Client& c, uint32_t index) {
    std::lock_guard<std::mutex> lk(g_server.mutex);
    c.streams.erase(index);
    g_server.mix_streams.erase(index);
    g_server.active_streams.store(static_cast<int>(g_server.mix_streams.size()),
                                  std::memory_order_relaxed);
}

void handle_delete_playback_stream(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    rd.get_u32(&index);
    erase_stream(c, index);
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
    ALR_AUD_LOGI("client %u DELETE_PLAYBACK_STREAM idx=%u", c.index, index);
}

void handle_cork(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    bool cork = false;
    rd.get_u32(&index);
    rd.get_boolean(&cork);
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        auto it = c.streams.find(index);
        if (it != c.streams.end()) it->second->corked = cork;
    }
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
}

void handle_flush(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    rd.get_u32(&index);
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        auto it = c.streams.find(index);
        if (it != c.streams.end()) {
            PlaybackStream& st = *it->second;
            st.head = st.tail = st.fill = 0;
            st.resample_pos = 0.0;
        }
    }
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
}

void handle_trigger(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    rd.get_u32(&index);
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
}

void handle_drain(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    rd.get_u32(&index);
    // Simplest correct behavior: ack immediately once the ring is empty. For v1 we
    // ack right away (clients tolerate it; the ring drains on its own).
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
}

void handle_get_playback_latency(Client& c, uint32_t tag, TagReader& rd) {
    uint32_t index = 0;
    rd.get_u32(&index);
    // client-supplied timeval (we echo it back).
    uint64_t client_tv = 0;
    rd.skip_value();  // PA_TAG_TIMEVAL

    uint64_t sink_usec = 0, source_usec = 0;
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        auto it = c.streams.find(index);
        if (it != c.streams.end() && g_server.device_rate > 0) {
            // Pending device frames in the ring + AAudio buffer estimate.
            uint64_t pending = it->second->fill;
            sink_usec = pending * 1000000ull / static_cast<uint64_t>(g_server.device_rate);
            // add the AAudio presentation backlog if available.
            if (g_server.aaudio) {
                int64_t fpos = 0, t_ns = 0;
                if (AAudioStream_getTimestamp(g_server.aaudio, CLOCK_MONOTONIC,
                                              &fpos, &t_ns) == AAUDIO_OK) {
                    uint64_t written = g_server.frames_written.load();
                    if (written > static_cast<uint64_t>(fpos)) {
                        uint64_t hw_pending = written - static_cast<uint64_t>(fpos);
                        sink_usec += hw_pending * 1000000ull /
                                     static_cast<uint64_t>(g_server.device_rate);
                    }
                }
            }
        }
    }

    // pa_timing_info-ish reply for GET_PLAYBACK_LATENCY (the fields libpulse reads).
    TagWriter tw = make_reply(tag);
    tw.put_usec(sink_usec);     // sink latency
    tw.put_usec(source_usec);   // source latency (0)
    tw.put_boolean(true);       // playing
    tw.put_timeval(0, 0);       // local timeval (echo)
    tw.put_timeval(0, 0);       // remote timeval
    tw.put_s64(0);              // write index
    tw.put_s64(0);              // read index
    // >= v13 extras:
    tw.put_u64(0);              // underrun_for
    tw.put_u64(0);              // playing_for
    (void)client_tv;
    queue_command(c, tw);
}

// Generic "introspection we don't model" -> empty REPLY (keeps libpulse happy).
void handle_empty_reply(Client& c, uint32_t tag) {
    TagWriter tw = make_reply(tag);
    queue_command(c, tw);
}

// Dispatch a parsed control command.
void dispatch_command(Client& c, uint32_t command, uint32_t tag, TagReader& rd) {
    switch (command) {
        case PA_COMMAND_AUTH: handle_auth(c, tag, rd); break;
        case PA_COMMAND_SET_CLIENT_NAME: handle_set_client_name(c, tag, rd); break;
        case PA_COMMAND_GET_SERVER_INFO:
            handle_get_server_info(c, tag, g_server.device_rate); break;
        case PA_COMMAND_GET_SINK_INFO: {
            SampleSpec ss; ss.format = PA_SAMPLE_S16LE; ss.channels = 2;
            handle_get_sink_info(c, tag, ss, g_server.device_rate, false); break;
        }
        case PA_COMMAND_GET_SINK_INFO_LIST: {
            SampleSpec ss; ss.format = PA_SAMPLE_S16LE; ss.channels = 2;
            handle_get_sink_info(c, tag, ss, g_server.device_rate, true); break;
        }
        case PA_COMMAND_GET_SINK_INPUT_INFO: handle_get_sink_input_info(c, tag, false); break;
        case PA_COMMAND_GET_SINK_INPUT_INFO_LIST: handle_get_sink_input_info(c, tag, true); break;
        case PA_COMMAND_CREATE_PLAYBACK_STREAM:
            handle_create_playback_stream(c, tag, rd); break;
        case PA_COMMAND_DELETE_PLAYBACK_STREAM:
            handle_delete_playback_stream(c, tag, rd); break;
        case PA_COMMAND_CORK_PLAYBACK_STREAM: handle_cork(c, tag, rd); break;
        case PA_COMMAND_FLUSH_PLAYBACK_STREAM: handle_flush(c, tag, rd); break;
        case PA_COMMAND_TRIGGER_PLAYBACK_STREAM: handle_trigger(c, tag, rd); break;
        case PA_COMMAND_DRAIN_PLAYBACK_STREAM: handle_drain(c, tag, rd); break;
        case PA_COMMAND_GET_PLAYBACK_LATENCY: handle_get_playback_latency(c, tag, rd); break;
        // Stubbed introspection / control: ack with an empty REPLY so the client
        // proceeds (design §1b: unknown/optional commands NAK gracefully — but an
        // empty positive REPLY is friendlier for the ones libpulse expects to work).
        case PA_COMMAND_GET_CLIENT_INFO:
        case PA_COMMAND_GET_CLIENT_INFO_LIST:
        case PA_COMMAND_GET_MODULE_INFO_LIST:
        case PA_COMMAND_GET_SOURCE_INFO:
        case PA_COMMAND_GET_SOURCE_INFO_LIST:
        case PA_COMMAND_SUBSCRIBE:
        case PA_COMMAND_SET_SINK_INPUT_VOLUME:
        case PA_COMMAND_SET_PLAYBACK_STREAM_NAME:
        case PA_COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST:
            handle_empty_reply(c, tag); break;
        default:
            // NAK with NOTSUPPORTED — libpulse downgrades / proceeds.
            queue_error(c, tag, PA_ERR_NOTSUPPORTED);
            break;
    }
}

// A complete control frame payload has arrived: parse command+tag, dispatch.
void on_control_frame(Client& c, const uint8_t* payload, uint32_t len) {
    TagReader rd(payload, len);
    uint32_t command = 0, tag = 0;
    if (!rd.get_u32(&command) || !rd.get_u32(&tag)) {
        ALR_AUD_LOGW("client %u malformed control frame (len=%u)", c.index, len);
        return;
    }
    dispatch_command(c, command, tag, rd);
}

// A data (memblock) frame for a playback stream: append PCM, re-grant credit.
void on_data_frame(Client& c, uint32_t channel, const uint8_t* payload, uint32_t len) {
    std::shared_ptr<PlaybackStream> st;
    bool just_started = false;
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        auto it = c.streams.find(channel);
        if (it == c.streams.end()) return;  // stray frame
        st = it->second;
        st->received_bytes += len;
        push_pcm_to_ring(*st, payload, len, g_server.device_rate);
        if (!st->started) { st->started = true; just_started = true; }
    }
    // Notify STARTED on the first data (stream left prebuffering) so the client's
    // pa_stream_set_started_callback fires (best-effort; design §5b).
    if (just_started) queue_started(c, channel);
    // Re-grant credit: request another minreq worth so the client keeps writing
    // (design §5b.5 / §5d — this IS the back-pressure).
    if (st) {
        queue_request(c, channel, st->minreq ? st->minreq : len);
        flush_wbuf(c);
    }
}

// --------------------------------------------------------------------------- //
// Socket I/O — reassemble frames out of the byte stream, flush wbuf.
// --------------------------------------------------------------------------- //

void flush_wbuf(Client& c) {
    while (!c.wbuf.empty()) {
        ssize_t n = ::send(c.fd, c.wbuf.data(), c.wbuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.wbuf.erase(c.wbuf.begin(), c.wbuf.begin() + n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // socket full; try again on next EPOLLOUT/wakeup
        } else {
            c.wbuf.clear();  // hard error; client teardown handled by caller
            break;
        }
    }
}

// Parse as many complete frames as rbuf holds. Returns false if the client should
// be dropped (protocol error / oversize).
bool drain_frames(Client& c) {
    size_t off = 0;
    const size_t total = c.rbuf.size();
    while (total - off >= 20) {
        const uint8_t* h = c.rbuf.data() + off;
        auto be = [&](int i) {
            return (static_cast<uint32_t>(h[i]) << 24) |
                   (static_cast<uint32_t>(h[i + 1]) << 16) |
                   (static_cast<uint32_t>(h[i + 2]) << 8) |
                   static_cast<uint32_t>(h[i + 3]);
        };
        uint32_t flen = be(0);
        uint32_t channel = be(4);
        if (flen > kFrameSizeMax) {
            ALR_AUD_LOGW("client %u frame too large (%u)", c.index, flen);
            return false;
        }
        if (total - off < 20u + flen) break;  // wait for the rest
        const uint8_t* payload = h + 20;
        if (channel == kChannelControl) {
            on_control_frame(c, payload, flen);
        } else {
            on_data_frame(c, channel, payload, flen);
        }
        off += 20u + flen;
    }
    if (off > 0) c.rbuf.erase(c.rbuf.begin(), c.rbuf.begin() + off);
    return true;
}

void close_client(int fd) {
    auto it = g_server.clients.find(fd);
    if (it == g_server.clients.end()) return;
    Client& c = *it->second;
    // Drop all of this client's streams from the mixer.
    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        for (auto& kv : c.streams) g_server.mix_streams.erase(kv.first);
        g_server.active_streams.store(static_cast<int>(g_server.mix_streams.size()),
                                      std::memory_order_relaxed);
    }
    if (g_server.epoll_fd >= 0) epoll_ctl(g_server.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    g_server.clients.erase(it);
    ALR_AUD_LOGI("client fd=%d closed", fd);
}

void accept_clients() {
    for (;;) {
        int fd = ::accept4(g_server.listen_fd, nullptr, nullptr,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        auto c = std::make_shared<Client>();
        c->fd = fd;
        c->index = g_server.next_client_index++;
        g_server.clients[fd] = c;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(g_server.epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            ALR_AUD_LOGW("epoll_ctl ADD fd=%d failed: %s", fd, std::strerror(errno));
            close_client(fd);
            continue;
        }
        ALR_AUD_LOGI("client fd=%d accepted (index=%u)", fd, c->index);
    }
}

void handle_client_readable(int fd) {
    auto it = g_server.clients.find(fd);
    if (it == g_server.clients.end()) return;
    Client& c = *it->second;
    uint8_t buf[8192];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.rbuf.insert(c.rbuf.end(), buf, buf + n);
            if (c.rbuf.size() > kFrameSizeMax * 4) {
                // Runaway buffer w/o a complete frame -> drop.
                ALR_AUD_LOGW("client %u rbuf overrun", c.index);
                close_client(fd);
                return;
            }
            continue;
        }
        if (n == 0) {  // peer closed
            close_client(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        close_client(fd);
        return;
    }
    if (!drain_frames(c)) {
        close_client(fd);
        return;
    }
    flush_wbuf(c);
}

void epoll_loop() {
    g_server.running.store(true);
    ALR_AUD_LOGI("audio epoll loop started on %s", g_server.socket_path.c_str());
    constexpr int kMaxEvents = 16;
    epoll_event events[kMaxEvents];
    while (!g_server.stop_requested.load()) {
        int n = epoll_wait(g_server.epoll_fd, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            ALR_AUD_LOGE("epoll_wait failed: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == g_server.wakeup_fd) {
                uint64_t v;
                ssize_t r = ::read(g_server.wakeup_fd, &v, sizeof(v));
                (void)r;
                continue;
            }
            if (fd == g_server.listen_fd) {
                accept_clients();
                continue;
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                close_client(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                handle_client_readable(fd);
            }
        }
        // Opportunistically flush any client whose wbuf still has bytes.
        for (auto it = g_server.clients.begin(); it != g_server.clients.end();) {
            int fd = it->first;
            Client& c = *it->second;
            ++it;
            if (!c.wbuf.empty()) flush_wbuf(c);
            (void)fd;
        }
    }
    g_server.running.store(false);
    ALR_AUD_LOGI("audio epoll loop exiting");
}

bool bind_listen(const std::string& xdg_runtime_dir, std::string* err) {
    // Socket lives at ${XDG_RUNTIME_DIR}/pulse/native (design §4).
    g_server.socket_dir = xdg_runtime_dir + "/pulse";
    g_server.socket_path = g_server.socket_dir + "/native";

    // mkdir the XDG dir (it may already exist via the compositor) then pulse/.
    ::mkdir(xdg_runtime_dir.c_str(), 0700);
    if (::mkdir(g_server.socket_dir.c_str(), 0700) != 0 && errno != EEXIST) {
        *err = std::string("mkdir ") + g_server.socket_dir + ": " + std::strerror(errno);
        return false;
    }

    if (g_server.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        *err = "socket path too long: " + g_server.socket_path;
        return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *err = std::string("socket(): ") + std::strerror(errno);
        return false;
    }
    ::unlink(g_server.socket_path.c_str());  // stale socket from a prior run
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, g_server.socket_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *err = std::string("bind(") + g_server.socket_path + "): " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        *err = std::string("listen(): ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    g_server.listen_fd = fd;
    return true;
}

}  // namespace

// --------------------------------------------------------------------------- //
// Public API
// --------------------------------------------------------------------------- //

std::string alr_audio_sink_start(const std::string& xdg_runtime_dir, int device_rate) {
    if (g_server.running.load()) {
        return alr_audio_sink_status();
    }
    if (device_rate <= 0) device_rate = 48000;
    g_server.device_rate = device_rate;
    g_server.stop_requested.store(false);

    std::string err;
    if (!bind_listen(xdg_runtime_dir, &err)) {
        ALR_AUD_LOGE("audio sink start FAIL: %s", err.c_str());
        return "ALR AUDIO SINK: FAIL " + err;
    }

    g_server.epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (g_server.epoll_fd < 0) {
        std::string e = std::strerror(errno);
        ::close(g_server.listen_fd);
        g_server.listen_fd = -1;
        return "ALR AUDIO SINK: FAIL epoll_create1: " + e;
    }
    g_server.wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_server.wakeup_fd < 0) {
        std::string e = std::strerror(errno);
        ::close(g_server.epoll_fd);
        ::close(g_server.listen_fd);
        g_server.epoll_fd = g_server.listen_fd = -1;
        return "ALR AUDIO SINK: FAIL eventfd: " + e;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = g_server.listen_fd;
    epoll_ctl(g_server.epoll_fd, EPOLL_CTL_ADD, g_server.listen_fd, &ev);
    ev.data.fd = g_server.wakeup_fd;
    epoll_ctl(g_server.epoll_fd, EPOLL_CTL_ADD, g_server.wakeup_fd, &ev);

    g_server.thread = std::thread(epoll_loop);

    std::ostringstream out;
    out << "ALR AUDIO SINK: started socket=" << g_server.socket_path
        << " rate=" << g_server.device_rate;
    ALR_AUD_LOGI("%s", out.str().c_str());
    return out.str();
}

std::string alr_audio_sink_status() {
    std::ostringstream out;
    out << "ALR AUDIO SINK: " << (g_server.running.load() ? "running" : "stopped")
        << " streams=" << g_server.active_streams.load(std::memory_order_relaxed)
        << " rate=" << g_server.device_rate
        << " clients=" << g_server.clients.size()
        << " frames=" << g_server.frames_written.load(std::memory_order_relaxed)
        << " socket=" << g_server.socket_path;
    return out.str();
}

std::string alr_audio_sink_stop() {
    if (!g_server.running.load() && !g_server.thread.joinable()) {
        return "ALR AUDIO SINK: stopped (was not running)";
    }
    g_server.stop_requested.store(true);
    if (g_server.wakeup_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = ::write(g_server.wakeup_fd, &one, sizeof(one));
        (void)w;
    }
    if (g_server.thread.joinable()) g_server.thread.join();

    {
        std::lock_guard<std::mutex> lk(g_server.mutex);
        close_aaudio_locked();
        for (auto& kv : g_server.clients) ::close(kv.first);
        g_server.clients.clear();
        g_server.mix_streams.clear();
        g_server.active_streams.store(0, std::memory_order_relaxed);
    }
    if (g_server.epoll_fd >= 0) { ::close(g_server.epoll_fd); g_server.epoll_fd = -1; }
    if (g_server.wakeup_fd >= 0) { ::close(g_server.wakeup_fd); g_server.wakeup_fd = -1; }
    if (g_server.listen_fd >= 0) { ::close(g_server.listen_fd); g_server.listen_fd = -1; }
    if (!g_server.socket_path.empty()) ::unlink(g_server.socket_path.c_str());

    ALR_AUD_LOGI("audio sink stopped");
    return "ALR AUDIO SINK: stopped";
}

bool alr_audio_sink_running() { return g_server.running.load(); }

}  // namespace audio
}  // namespace alr
