/*****************************************************************************
  wav123.h, Copyright (c) 2025 James Pearce

  Exposes the same interface as mpg123 for the functions used by audio.cpp
  in CannonBall-SE, to enable background loading of .wav files.

  This avoids a notable pause when using custom music in .wav format on
  slow micro-SD based platforms like the Pi2.
*****************************************************************************/



#ifndef WAV123_H
#define WAV123_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#ifdef _WIN32
// Some Windows toolchains don't have sys/types off_t by default
#include <basetsd.h>
#ifndef off_t
#define off_t int64_t
#endif
#endif

// ---- Return codes compatible with how mpg123 is used in audio.cpp ----
#define WAV123_OK       0
#define WAV123_ERR     -1
#define WAV123_DONE   -12

// ---- Encoding flag (only SIGNED_16 is supported for output) ----
#define WAV123_ENC_SIGNED_16 0x040

struct wav123_handle;

// Parity with the subset of mpg123 used in audio.cpp
int          wav123_init();
void         wav123_exit();

wav123_handle* wav123_new(const char* /*decoder*/, int* err_out);
int          wav123_open(wav123_handle* h, const char* filename);

// No-op like mpg123_format_none(); resets accepted formats
int          wav123_format_none(wav123_handle* h);

// Select desired output format (rate/channels/encoding)
int          wav123_format(wav123_handle* h, long rate, int channels, int enc);

// Total samples PER CHANNEL at the *output* rate
off_t        wav123_length(wav123_handle* h);

// Fill caller buffer with decoded interleaved S16 stereo samples
// - out      : destination buffer
// - out_size : size in BYTES of out
// - done     : bytes actually written
// Returns: WAV123_OK (more data may follow), WAV123_DONE (finished), WAV123_ERR (error)
int          wav123_read(wav123_handle* h, unsigned char* out, size_t out_size, size_t* done);

// Close and free
int          wav123_close(wav123_handle* h);
void         wav123_delete(wav123_handle* h);

// Error helpers (compatible usage with mpg123_* variants)
const char*  wav123_strerror(wav123_handle* h);
const char*  wav123_plain_strerror(int errcode);

#endif // WAV123_H
