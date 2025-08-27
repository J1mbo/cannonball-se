/*****************************************************************************
  wav123.h, Copyright (c) 2025 James Pearce

  Exposes the same interface as mpg123 for the functions used by audio.cpp
  in CannonBall-SE, to enable background loading of .wav files.

  This avoids a notable pause when using custom music in .wav format on
  slow micro-SD based platforms like the Pi2.
*****************************************************************************/

#include "wav123.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <limits>

struct wav123_handle
{
    // File
    std::FILE* fp = nullptr;
    std::string path;

    // Source WAV format (from "fmt " chunk)
    uint16_t src_format      = 0;     // 1 = PCM, 3 = IEEE float
    uint16_t src_channels    = 0;     // 1 or 2 supported
    uint32_t src_rate        = 0;
    uint16_t src_bits        = 0;     // 8/16/24/32 or 32f
    uint16_t src_block_align = 0;

    // Data chunk
    uint64_t data_offset     = 0;     // file pos of data start
    uint64_t data_bytes      = 0;     // total bytes in data chunk
    uint64_t data_left       = 0;     // bytes remaining to read

    // Desired output format (from wav123_format)
    long     out_rate        = 0;
    int      out_channels    = 2;
    int      out_enc         = WAV123_ENC_SIGNED_16;

    // Streaming/resample state
    bool     opened          = false;
    bool     formatted       = false;
    int      last_error      = 0;

    // For resampling
    bool     rates_equal     = true;
    double   step_src_per_out = 1.0;  // src_rate / out_rate
    double   phase           = 0.0;   // fractional position within [prev,curr)

    // Current/previous source frames (interleaved, 2 samples even for mono)
    int32_t  prevL = 0, prevR = 0;
    int32_t  currL = 0, currR = 0;
    bool     have_prev = false;
    bool     have_curr = false;

    // Small raw read buffer (untranslated bytes)
    std::vector<unsigned char> raw;   // multiple of block_align
};

// ---------- helpers ----------
static inline int16_t clamp16_from_i32(int32_t x)
{
    if (x > 32767)  return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static inline int32_t sample_from_u8(uint8_t v)      { return (int32_t(v) - 128) * 256; }
static inline int32_t sample_from_s16(int16_t v)     { return (int32_t)v; }
static inline int32_t sample_from_s24(const unsigned char* p)
{
    // Little-endian 24-bit to signed 32
    int32_t v =  (int32_t(p[0])      ) |
                 (int32_t(p[1]) <<  8) |
                 (int32_t(p[2]) << 16);
    // Sign-extend
    if (v & 0x00800000) v |= 0xFF000000;
    // Scale to ~16-bit domain to keep headroom
    return v >> 8;
}
static inline int32_t sample_from_s32(int32_t v)     { return v >> 16; } // reduce to ~16-bit scale
static inline int32_t sample_from_f32(float f)
{
    if (f > 1.0f)  f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int32_t)std::lrintf(f * 32767.0f);
}

// Read 1 source frame -> interleaved L/R in int32 domain.
// Returns false on EOF or error (last_error set on error).
static bool read_one_src_frame(wav123_handle* h, int32_t& L, int32_t& R)
{
    if (!h->fp || h->data_left < h->src_block_align) return false;

    const uint16_t ch = h->src_channels;
    const uint16_t bps = h->src_bits;

    // Refill temporary raw buffer if needed
    if (h->raw.size() < h->src_block_align) h->raw.resize(h->src_block_align);
    size_t got = std::fread(h->raw.data(), 1, h->src_block_align, h->fp);
    if (got != h->src_block_align)
    {
        h->last_error = WAV123_ERR;
        return false;
    }
    h->data_left -= got;

    const unsigned char* p = h->raw.data();

    auto decode_one = [&](const unsigned char* at)->int32_t
    {
        if (h->src_format == 3 /*IEEE float*/ && bps == 32)
        {
            float fv;
            std::memcpy(&fv, at, 4);
            return sample_from_f32(fv);
        }
        switch (bps)
        {
            case 8:  return sample_from_u8(*(const uint8_t*)at);
            case 16: return sample_from_s16(*(const int16_t*)at);
            case 24: return sample_from_s24(at);
            case 32: return sample_from_s32(*(const int32_t*)at);
            default: return 0;
        }
    };

    if (ch == 1)
    {
        int32_t s = decode_one(p);
        L = s; R = s;
    }
    else if (ch == 2)
    {
        if (bps == 24)
        {
            L = decode_one(p);
            R = decode_one(p+3);
        }
        else
        {
            L = decode_one(p);
            R = decode_one(p + (bps/8));
        }
    }
    else
    {
        // Not supported channel count
        h->last_error = WAV123_ERR;
        return false;
    }

    return true;
}

static bool parse_wav_header(wav123_handle* h)
{
    // Minimal RIFF/WAVE parser with chunk walking
    struct RIFFHeader { char riff[4]; uint32_t size; char wave[4]; };
    RIFFHeader rh{};
    if (std::fread(&rh, 1, sizeof(rh), h->fp) != sizeof(rh)) return false;
    if (std::memcmp(rh.riff, "RIFF", 4) != 0 || std::memcmp(rh.wave, "WAVE", 4) != 0) return false;

    bool have_fmt = false, have_data = false;
    uint16_t audioFormat=0, channels=0, bitsPerSample=0, blockAlign=0;
    uint32_t sampleRate=0;
    uint64_t dataSize=0, dataOffset=0;

    for (;;)
    {
        char id[4];
        uint32_t size=0;
        if (std::fread(id, 1, 4, h->fp) != 4) break;
        if (std::fread(&size, 1, 4, h->fp) != 4) break;

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            unsigned char fmtbuf[64] = {0};
            if (size > sizeof(fmtbuf)) size = sizeof(fmtbuf);
            if (std::fread(fmtbuf, 1, size, h->fp) != size) return false;

            audioFormat    = *(uint16_t*)&fmtbuf[0];
            channels       = *(uint16_t*)&fmtbuf[2];
            sampleRate     = *(uint32_t*)&fmtbuf[4];
            blockAlign     = *(uint16_t*)&fmtbuf[12];
            bitsPerSample  = *(uint16_t*)&fmtbuf[14];

            have_fmt = true;

            // Skip possible padding if chunk bigger than we read (unlikely here)
            long skip = long(((size + 1) & ~1u) - size);
            if (skip>0) std::fseek(h->fp, skip, SEEK_CUR);
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            dataOffset = (uint64_t)std::ftell(h->fp);
            dataSize   = size;
            std::fseek(h->fp, long((size + 1) & ~1u), SEEK_CUR);
            have_data = true;
        }
        else
        {
            // skip other chunks (pad to even)
            std::fseek(h->fp, long((size + 1) & ~1u), SEEK_CUR);
        }

        if (have_fmt && have_data) break;
    }

    if (!have_fmt || !have_data) return false;

    // Basic validation
    if (!(channels == 1 || channels == 2)) return false;
    if (!(bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32)) return false;
    if (!(audioFormat == 1 || (audioFormat == 3 && bitsPerSample == 32))) return false; // PCM or float32

    h->src_format      = audioFormat;
    h->src_channels    = channels;
    h->src_rate        = sampleRate;
    h->src_bits        = bitsPerSample;
    h->src_block_align = blockAlign ? blockAlign : (uint16_t)((channels * bitsPerSample) / 8);

    h->data_offset     = dataOffset;
    h->data_bytes      = dataSize;
    h->data_left       = dataSize;

    // Seek to data start
    std::fseek(h->fp, (long)h->data_offset, SEEK_SET);
    return true;
}

// ---------- public API ----------

int wav123_init() { return WAV123_OK; }
void wav123_exit() {}

wav123_handle* wav123_new(const char* /*decoder*/, int* err_out)
{
    auto* h = new (std::nothrow) wav123_handle();
    if (!h)
    {
        if (err_out) *err_out = WAV123_ERR;
        return nullptr;
    }
    if (err_out) *err_out = WAV123_OK;
    return h;
}

int wav123_open(wav123_handle* h, const char* filename)
{
    if (!h) return WAV123_ERR;
    h->fp = std::fopen(filename, "rb");
    if (!h->fp)
    {
        h->last_error = WAV123_ERR;
        return WAV123_ERR;
    }
    if (!parse_wav_header(h))
    {
        std::fclose(h->fp);
        h->fp = nullptr;
        h->last_error = WAV123_ERR;
        return WAV123_ERR;
    }
    h->path = filename;
    h->opened = true;
    h->have_prev = h->have_curr = false;
    h->phase = 0.0;
    h->raw.clear();
    return WAV123_OK;
}

int wav123_format_none(wav123_handle* h)
{
    if (!h) return WAV123_ERR;
    h->formatted = false;
    h->out_rate = 0;
    h->out_channels = 2;
    h->out_enc = WAV123_ENC_SIGNED_16;
    return WAV123_OK;
}

int wav123_format(wav123_handle* h, long rate, int channels, int enc)
{
    if (!h || !h->opened) return WAV123_ERR;
    if (enc != WAV123_ENC_SIGNED_16) return WAV123_ERR;
    if (!(channels == 2 || channels == 1)) return WAV123_ERR;

    h->out_rate = (rate > 0) ? rate : (long)h->src_rate;
    h->out_channels = channels;
    h->out_enc = enc;
    h->formatted = true;

    h->rates_equal = (uint32_t)h->out_rate == h->src_rate;
    h->step_src_per_out = h->rates_equal ? 1.0 : (double)h->src_rate / (double)h->out_rate;

    // Reset stream state for a fresh read
    std::fseek(h->fp, (long)h->data_offset, SEEK_SET);
    h->data_left = h->data_bytes;
    h->have_prev = h->have_curr = false;
    h->phase = 0.0;

    return WAV123_OK;
}

off_t wav123_length(wav123_handle* h)
{
    if (!h || !h->opened) return 0;
    // Duration in seconds from source, then scale to out rate, and return per-channel count
    double seconds = (h->src_rate > 0 && h->src_block_align > 0)
                     ? double(h->data_bytes) / double(h->src_block_align) / double(h->src_rate)
                     : 0.0;
    return (off_t)std::llround(seconds * (double)(h->out_rate > 0 ? h->out_rate : h->src_rate));
}

int wav123_read(wav123_handle* h, unsigned char* out, size_t out_size, size_t* done)
{
    if (done) *done = 0;
    if (!h || !h->opened || !h->formatted || !out || out_size == 0) return WAV123_ERR;
    if (h->out_enc != WAV123_ENC_SIGNED_16) return WAV123_ERR;

    const int out_ch = (h->out_channels == 2) ? 2 : 1; // we still produce interleaved frames
    const size_t bytes_per_frame = sizeof(int16_t) * out_ch;
    size_t frames_wanted = out_size / bytes_per_frame;
    if (frames_wanted == 0) return WAV123_OK;

    int16_t* dst = (int16_t*)out;
    size_t frames_done = 0;

    auto emit = [&](int32_t L, int32_t R)
    {
        if (h->out_channels == 1)
        {
            int32_t mono = ( (int64_t)L + (int64_t)R ) / 2;
            *dst++ = clamp16_from_i32(mono);
        }
        else
        {
            *dst++ = clamp16_from_i32(L);
            *dst++ = clamp16_from_i32(R);
        }
        frames_done++;
    };

    if (h->rates_equal)
    {
        // Fast path: 1:1 frames; decode frames_wanted source frames (or until EOF)
        while (frames_done < frames_wanted && h->data_left >= h->src_block_align)
        {
            int32_t L, R;
            if (!read_one_src_frame(h, L, R)) break;
            emit(L, R);
        }
    }
    else
    {
        // Simple linear interpolation resampler
        // Ensure we have initial prev/curr
        if (!h->have_prev)
        {
            if (!read_one_src_frame(h, h->prevL, h->prevR)) {
                // No data at all
                return WAV123_DONE;
            }
            h->have_prev = true;
        }
        if (!h->have_curr)
        {
            if (!read_one_src_frame(h, h->currL, h->currR)) {
                // Only one frame in file; just emit that until done
                while (frames_done < frames_wanted)
                    emit(h->prevL, h->prevR);
                if (done) *done = frames_done * bytes_per_frame;
                return WAV123_DONE;
            }
            h->have_curr = true;
        }

        while (frames_done < frames_wanted)
        {
            // Interpolate
            double t = h->phase;
            double omt = 1.0 - t;
            int32_t L = (int32_t)std::lrint(omt * h->prevL + t * h->currL);
            int32_t R = (int32_t)std::lrint(omt * h->prevR + t * h->currR);
            emit(L, R);

            h->phase += h->step_src_per_out;
            while (h->phase >= 1.0)
            {
                // Advance to next source frame
                h->phase -= 1.0;
                h->prevL = h->currL; h->prevR = h->currR;
                if (!read_one_src_frame(h, h->currL, h->currR))
                {
                    // No more source data; finish by holding the last frame and drain requested output
                    h->have_curr = false;
                    // Emit remaining frames with last available sample if caller asked for more
                    while (frames_done < frames_wanted)
                        emit(h->prevL, h->prevR);
                    if (done) *done = frames_done * bytes_per_frame;
                    return WAV123_DONE;
                }
            }
        }
    }

    if (done) *done = frames_done * bytes_per_frame;

    if (frames_done == 0)
    {
        // No more data to read
        return WAV123_DONE;
    }
    return (h->data_left >= h->src_block_align) ? WAV123_OK : WAV123_DONE;
}

int wav123_close(wav123_handle* h)
{
    if (!h) return WAV123_ERR;
    if (h->fp)
    {
        std::fclose(h->fp);
        h->fp = nullptr;
    }
    h->opened = false;
    return WAV123_OK;
}

void wav123_delete(wav123_handle* h)
{
    if (!h) return;
    wav123_close(h);
    delete h;
}

const char* wav123_strerror(wav123_handle* h)
{
    (void)h;
    return "wav123 error";
}

const char* wav123_plain_strerror(int errcode)
{
    switch (errcode)
    {
        case WAV123_OK:   return "ok";
        case WAV123_DONE: return "done";
        default:          return "error";
    }
}
