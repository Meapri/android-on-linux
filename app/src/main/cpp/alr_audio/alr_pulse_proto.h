// PulseAudio native-protocol constants + tagstruct codec (the slice ALR needs).
//
// Names/values mirror PulseAudio's src/pulsecore/native-common.h, tagstruct.h and
// pulse/sample.h. We deliberately implement only the playback-sink subset (design
// §1b). Values are STABLE across PulseAudio versions (the wire format is a public
// ABI); we re-declare them here rather than vendor PulseAudio headers so the module
// stays self-contained and host-buildable under the NDK.
//
// The tagstruct is PulseAudio's self-describing TLV: each value is a 1-byte type
// tag followed by its payload (big-endian for integers). A command packet is a
// tagstruct beginning with U32(command) U32(tag); the reply is U32(command=REPLY)
// U32(tag) ... . Data (memblock) frames use a separate fixed 5-u32 descriptor
// header on the same socket (see alr_pulse_server.cpp).
#ifndef ALR_AUDIO_ALR_PULSE_PROTO_H
#define ALR_AUDIO_ALR_PULSE_PROTO_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace alr {
namespace audio {
namespace proto {

// We announce this protocol version in the AUTH reply. <=14 keeps the simple
// memblock framing (no memfd/shm/srbchannel negotiation) — design §1b.
inline constexpr uint32_t kProtocolVersion = 13;

// Frame descriptor: a control packet rides as channel 0xFFFFFFFF; a data
// (memblock) frame carries the playback-stream index in the 'channel' field.
inline constexpr uint32_t kFrameSizeMax = 64u * 1024u;  // sanity cap per frame
inline constexpr uint32_t kChannelControl = 0xFFFFFFFFu;

// --- command opcodes (subset; src/pulsecore/native-common.h) ----------------
enum Command : uint32_t {
    PA_COMMAND_ERROR = 0,
    PA_COMMAND_TIMEOUT = 1,
    PA_COMMAND_REPLY = 2,

    PA_COMMAND_CREATE_PLAYBACK_STREAM = 3,
    PA_COMMAND_DELETE_PLAYBACK_STREAM = 4,
    PA_COMMAND_CREATE_RECORD_STREAM = 5,
    PA_COMMAND_DELETE_RECORD_STREAM = 6,
    PA_COMMAND_EXIT = 7,
    PA_COMMAND_AUTH = 8,
    PA_COMMAND_SET_CLIENT_NAME = 9,
    PA_COMMAND_LOOKUP_SINK = 10,
    PA_COMMAND_LOOKUP_SOURCE = 11,
    PA_COMMAND_DRAIN_PLAYBACK_STREAM = 12,
    PA_COMMAND_STAT = 13,
    PA_COMMAND_GET_PLAYBACK_LATENCY = 14,
    PA_COMMAND_CREATE_UPLOAD_STREAM = 15,
    PA_COMMAND_DELETE_UPLOAD_STREAM = 16,
    PA_COMMAND_FINISH_UPLOAD_STREAM = 17,
    PA_COMMAND_PLAY_SAMPLE = 18,
    PA_COMMAND_REMOVE_SAMPLE = 19,

    PA_COMMAND_GET_SERVER_INFO = 20,
    PA_COMMAND_GET_SINK_INFO = 21,
    PA_COMMAND_GET_SINK_INFO_LIST = 22,
    PA_COMMAND_GET_SOURCE_INFO = 23,
    PA_COMMAND_GET_SOURCE_INFO_LIST = 24,
    PA_COMMAND_GET_MODULE_INFO = 25,
    PA_COMMAND_GET_MODULE_INFO_LIST = 26,
    PA_COMMAND_GET_CLIENT_INFO = 27,
    PA_COMMAND_GET_CLIENT_INFO_LIST = 28,
    PA_COMMAND_GET_SINK_INPUT_INFO = 29,
    PA_COMMAND_GET_SINK_INPUT_INFO_LIST = 30,
    PA_COMMAND_GET_SOURCE_OUTPUT_INFO = 31,
    PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST = 32,
    PA_COMMAND_GET_SAMPLE_INFO = 33,
    PA_COMMAND_GET_SAMPLE_INFO_LIST = 34,
    PA_COMMAND_SUBSCRIBE = 35,

    PA_COMMAND_SET_SINK_VOLUME = 36,
    PA_COMMAND_SET_SINK_INPUT_VOLUME = 37,
    PA_COMMAND_SET_SOURCE_VOLUME = 38,
    PA_COMMAND_SET_SINK_MUTE = 39,
    PA_COMMAND_SET_SOURCE_MUTE = 40,

    PA_COMMAND_CORK_PLAYBACK_STREAM = 41,
    PA_COMMAND_FLUSH_PLAYBACK_STREAM = 42,
    PA_COMMAND_TRIGGER_PLAYBACK_STREAM = 43,

    PA_COMMAND_SET_DEFAULT_SINK = 44,
    PA_COMMAND_SET_DEFAULT_SOURCE = 45,
    PA_COMMAND_SET_PLAYBACK_STREAM_NAME = 46,
    PA_COMMAND_SET_RECORD_STREAM_NAME = 47,
    PA_COMMAND_KILL_CLIENT = 48,
    PA_COMMAND_KILL_SINK_INPUT = 49,
    PA_COMMAND_KILL_SOURCE_OUTPUT = 50,
    PA_COMMAND_LOAD_MODULE = 51,
    PA_COMMAND_UNLOAD_MODULE = 52,

    // server -> client notifications
    PA_COMMAND_REQUEST = 61,
    PA_COMMAND_OVERFLOW = 62,
    PA_COMMAND_UNDERFLOW = 63,
    PA_COMMAND_PLAYBACK_STREAM_KILLED = 64,
    PA_COMMAND_RECORD_STREAM_KILLED = 65,
    PA_COMMAND_STARTED = 66,

    PA_COMMAND_EXTENSION = 67,
    PA_COMMAND_SET_STREAM_BUFFER_ATTR = 72,
    PA_COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST = 74,
    PA_COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED = 76,
};

// --- error codes (pulse/def.h pa_error_code) --------------------------------
enum Error : uint32_t {
    PA_OK = 0,
    PA_ERR_ACCESS = 1,
    PA_ERR_COMMAND = 2,
    PA_ERR_INVALID = 3,
    PA_ERR_NOTSUPPORTED = 19,
};

// --- tagstruct value tags (src/pulsecore/tagstruct.h) -----------------------
enum Tag : uint8_t {
    PA_TAG_INVALID = 0,
    PA_TAG_STRING = 't',
    PA_TAG_STRING_NULL = 'N',
    PA_TAG_U32 = 'L',
    PA_TAG_U8 = 'B',
    PA_TAG_U64 = 'R',
    PA_TAG_S64 = 'r',
    PA_TAG_SAMPLE_SPEC = 'a',
    PA_TAG_ARBITRARY = 'x',
    PA_TAG_BOOLEAN_TRUE = '1',
    PA_TAG_BOOLEAN_FALSE = '0',
    PA_TAG_TIMEVAL = 'T',
    PA_TAG_USEC = 'U',
    PA_TAG_CHANNEL_MAP = 'm',
    PA_TAG_CVOLUME = 'v',
    PA_TAG_PROPLIST = 'P',
    PA_TAG_VOLUME = 'V',
    PA_TAG_FORMAT_INFO = 'f',
};

// --- sample formats (pulse/sample.h pa_sample_format_t) ----------------------
enum SampleFormat : uint8_t {
    PA_SAMPLE_U8 = 0,
    PA_SAMPLE_ALAW = 1,
    PA_SAMPLE_ULAW = 2,
    PA_SAMPLE_S16LE = 3,
    PA_SAMPLE_S16BE = 4,
    PA_SAMPLE_FLOAT32LE = 5,
    PA_SAMPLE_FLOAT32BE = 6,
    PA_SAMPLE_S32LE = 7,
    PA_SAMPLE_S32BE = 8,
    PA_SAMPLE_S24LE = 9,
    PA_SAMPLE_S24BE = 10,
    PA_SAMPLE_S24_32LE = 11,
    PA_SAMPLE_S24_32BE = 12,
    PA_SAMPLE_INVALID = 0xFF,
};

inline constexpr uint32_t PA_VOLUME_NORM = 0x10000u;  // pulse/volume.h
inline constexpr uint32_t PA_INVALID_INDEX = 0xFFFFFFFFu;

// pa_sample_spec wire form: U8 format, U8 channels, U32 rate (design §5b.4).
struct SampleSpec {
    uint8_t format = PA_SAMPLE_S16LE;
    uint8_t channels = 2;
    uint32_t rate = 48000;
};

inline uint32_t sample_size_bytes(uint8_t fmt) {
    switch (fmt) {
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_ULAW:
            return 1;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
            return 2;
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
            return 3;
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            return 4;
        default:
            return 2;
    }
}

inline uint32_t frame_size_bytes(const SampleSpec& s) {
    uint32_t ch = s.channels ? s.channels : 1;
    return sample_size_bytes(s.format) * ch;
}

// --------------------------------------------------------------------------- //
// Tagstruct writer — big-endian integers, type-tagged values.
// --------------------------------------------------------------------------- //
class TagWriter {
public:
    void put_u32(uint32_t v) {
        buf_.push_back(PA_TAG_U32);
        put_be32(v);
    }
    void put_u8(uint8_t v) {
        buf_.push_back(PA_TAG_U8);
        buf_.push_back(v);
    }
    void put_u64(uint64_t v) {
        buf_.push_back(PA_TAG_U64);
        put_be64(v);
    }
    void put_s64(int64_t v) {
        buf_.push_back(PA_TAG_S64);
        put_be64(static_cast<uint64_t>(v));
    }
    void put_usec(uint64_t v) {  // microseconds
        buf_.push_back(PA_TAG_USEC);
        put_be64(v);
    }
    void put_boolean(bool v) { buf_.push_back(v ? PA_TAG_BOOLEAN_TRUE : PA_TAG_BOOLEAN_FALSE); }
    void put_string(const char* s) {
        if (!s) {
            buf_.push_back(PA_TAG_STRING_NULL);
            return;
        }
        buf_.push_back(PA_TAG_STRING);
        const size_t n = std::strlen(s);
        buf_.insert(buf_.end(), s, s + n);
        buf_.push_back('\0');
    }
    void put_string(const std::string& s) { put_string(s.c_str()); }
    void put_sample_spec(const SampleSpec& s) {
        buf_.push_back(PA_TAG_SAMPLE_SPEC);
        buf_.push_back(s.format);
        buf_.push_back(s.channels);
        put_be32(s.rate);
    }
    // pa_channel_map: U8 count, then count * U8 channel positions (1=left,2=right).
    void put_channel_map_stereo() {
        buf_.push_back(PA_TAG_CHANNEL_MAP);
        buf_.push_back(2);
        buf_.push_back(1);  // PA_CHANNEL_POSITION_FRONT_LEFT
        buf_.push_back(2);  // PA_CHANNEL_POSITION_FRONT_RIGHT
    }
    void put_channel_map_mono() {
        buf_.push_back(PA_TAG_CHANNEL_MAP);
        buf_.push_back(1);
        buf_.push_back(0);  // PA_CHANNEL_POSITION_MONO
    }
    // pa_cvolume: U8 count, then count * U32 volumes.
    void put_cvolume(uint8_t channels, uint32_t vol) {
        buf_.push_back(PA_TAG_CVOLUME);
        buf_.push_back(channels);
        for (uint8_t i = 0; i < channels; ++i) put_be32(vol);
    }
    void put_volume(uint32_t vol) {
        buf_.push_back(PA_TAG_VOLUME);
        put_be32(vol);
    }
    // empty proplist: tag then a terminating PA_TAG_STRING_NULL.
    void put_empty_proplist() {
        buf_.push_back(PA_TAG_PROPLIST);
        buf_.push_back(PA_TAG_STRING_NULL);
    }
    void put_timeval(uint32_t sec, uint32_t usec) {
        buf_.push_back(PA_TAG_TIMEVAL);
        put_be32(sec);
        put_be32(usec);
    }
    // raw bytes (no tag) — for the frame descriptor header.
    void put_be32_raw(uint32_t v) { put_be32(v); }

    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    void put_be32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>(v >> 24));
        buf_.push_back(static_cast<uint8_t>(v >> 16));
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v));
    }
    void put_be64(uint64_t v) {
        for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    std::vector<uint8_t> buf_;
};

// --------------------------------------------------------------------------- //
// Tagstruct reader — fails closed (ok()==false) on a short/garbled read.
// --------------------------------------------------------------------------- //
class TagReader {
public:
    TagReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }

    bool get_u32(uint32_t* out) {
        if (!expect(PA_TAG_U32) || !avail(4)) return fail();
        *out = be32();
        return true;
    }
    bool get_u8(uint8_t* out) {
        if (!expect(PA_TAG_U8) || !avail(1)) return fail();
        *out = data_[pos_++];
        return true;
    }
    bool get_u64(uint64_t* out) {
        if (!expect(PA_TAG_U64) || !avail(8)) return fail();
        *out = be64();
        return true;
    }
    bool get_s64(int64_t* out) {
        if (!expect(PA_TAG_S64) || !avail(8)) return fail();
        *out = static_cast<int64_t>(be64());
        return true;
    }
    bool get_boolean(bool* out) {
        if (!avail(1)) return fail();
        uint8_t t = data_[pos_++];
        if (t == PA_TAG_BOOLEAN_TRUE) { *out = true; return true; }
        if (t == PA_TAG_BOOLEAN_FALSE) { *out = false; return true; }
        return fail();
    }
    // string into out (may be empty); a NULL string yields empty + null_out=true.
    bool get_string(std::string* out, bool* null_out = nullptr) {
        if (!avail(1)) return fail();
        uint8_t t = data_[pos_++];
        if (t == PA_TAG_STRING_NULL) {
            if (out) out->clear();
            if (null_out) *null_out = true;
            return true;
        }
        if (t != PA_TAG_STRING) return fail();
        size_t start = pos_;
        while (pos_ < len_ && data_[pos_] != '\0') ++pos_;
        if (pos_ >= len_) return fail();  // unterminated
        if (out) out->assign(reinterpret_cast<const char*>(data_ + start), pos_ - start);
        ++pos_;  // consume the NUL
        if (null_out) *null_out = false;
        return true;
    }
    bool get_sample_spec(SampleSpec* out) {
        if (!expect(PA_TAG_SAMPLE_SPEC) || !avail(6)) return fail();
        out->format = data_[pos_++];
        out->channels = data_[pos_++];
        out->rate = be32();
        return true;
    }
    // Skip an arbitrary value by tag (best-effort) — used to walk past fields we
    // don't consume (channel map, cvolume, proplist, format-info) so we can reach
    // the ones we do. Returns false on a tag we can't size.
    bool skip_value() {
        if (!avail(1)) return fail();
        uint8_t t = data_[pos_++];
        switch (t) {
            case PA_TAG_U32: return avail(4) ? (pos_ += 4, true) : fail();
            case PA_TAG_U8: return avail(1) ? (pos_ += 1, true) : fail();
            case PA_TAG_U64:
            case PA_TAG_S64:
            case PA_TAG_USEC: return avail(8) ? (pos_ += 8, true) : fail();
            case PA_TAG_VOLUME: return avail(4) ? (pos_ += 4, true) : fail();
            case PA_TAG_BOOLEAN_TRUE:
            case PA_TAG_BOOLEAN_FALSE: return true;
            case PA_TAG_STRING_NULL: return true;
            case PA_TAG_STRING: {
                while (pos_ < len_ && data_[pos_] != '\0') ++pos_;
                return (pos_ < len_) ? (++pos_, true) : fail();
            }
            case PA_TAG_SAMPLE_SPEC: return avail(6) ? (pos_ += 6, true) : fail();
            case PA_TAG_TIMEVAL: return avail(8) ? (pos_ += 8, true) : fail();
            case PA_TAG_CHANNEL_MAP: {
                if (!avail(1)) return fail();
                uint8_t n = data_[pos_++];
                return avail(n) ? (pos_ += n, true) : fail();
            }
            case PA_TAG_CVOLUME: {
                if (!avail(1)) return fail();
                uint8_t n = data_[pos_++];
                return avail(static_cast<size_t>(n) * 4) ? (pos_ += static_cast<size_t>(n) * 4, true) : fail();
            }
            case PA_TAG_ARBITRARY: {
                if (!avail(4)) return fail();
                uint32_t n = be32();
                return avail(n) ? (pos_ += n, true) : fail();
            }
            case PA_TAG_PROPLIST: {
                // proplist = repeated (STRING key, U32 len, ARBITRARY value) until
                // a terminating STRING_NULL. Walk it field-by-field.
                while (true) {
                    if (!avail(1)) return fail();
                    if (data_[pos_] == PA_TAG_STRING_NULL) { ++pos_; return true; }
                    std::string key;
                    if (!get_string(&key)) return fail();
                    uint32_t vlen = 0;
                    if (!get_u32(&vlen)) return fail();
                    // value is PA_TAG_ARBITRARY length vlen
                    if (!avail(1) || data_[pos_++] != PA_TAG_ARBITRARY) return fail();
                    if (!avail(4)) return fail();
                    uint32_t alen = be32();
                    if (alen != vlen || !avail(alen)) return fail();
                    pos_ += alen;
                }
            }
            case PA_TAG_FORMAT_INFO: {
                // FORMAT_INFO = U8 encoding + a proplist. Recurse via the cases.
                if (!avail(1)) return fail();
                if (data_[pos_++] != PA_TAG_U8) return fail();
                if (!avail(1)) return fail();
                ++pos_;  // encoding byte
                return skip_value();  // the proplist
            }
            default:
                return fail();
        }
    }

private:
    bool avail(size_t n) const { return ok_ && pos_ + n <= len_; }
    bool expect(uint8_t tag) {
        if (!avail(1)) return false;
        if (data_[pos_] != tag) { fail(); return false; }
        ++pos_;
        return true;
    }
    bool fail() { ok_ = false; return false; }
    uint32_t be32() {
        uint32_t v = (static_cast<uint32_t>(data_[pos_]) << 24) |
                     (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                     (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
                     static_cast<uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }
    uint64_t be64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data_[pos_++];
        return v;
    }
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace proto
}  // namespace audio
}  // namespace alr

#endif  // ALR_AUDIO_ALR_PULSE_PROTO_H
