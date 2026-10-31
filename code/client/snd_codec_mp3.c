/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2005 Stuart Dalton (badcdev@gmail.com)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#if defined(USE_CODEC_MINIMP3)

#include "client.h"
#include "snd_codec.h"

#include <limits.h>
#include <string.h>

#if defined(__PS3__)
#include "../sys/ps3_log.h"
#define MP3_TRACE(...) PS3_BootTracef(__VA_ARGS__)
#else
#define MP3_TRACE(...) ((void)0)
#endif

/*
 * minimp3 is intentionally compiled as a single translation unit here.
 * MINIMP3_ONLY_MP3 removes Layer I/II code. PPU has no SSE/NEON path, so
 * the library naturally selects its portable scalar implementation.
 */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "../thirdparty/minimp3/minimp3.h"

#define MP3_SAMPLE_WIDTH       2
#define MP3_DATA_BUFSIZ        (128 * 1024)
/* Keep >=10 frames while re-filling: minimp3 needs look-ahead to sync safely. */
#define MP3_SYNC_LOOKAHEAD_BYTES (16 * 1024)
#define MP3_PCM_BUFFER_BYTES   (MINIMP3_MAX_SAMPLES_PER_FRAME * MP3_SAMPLE_WIDTH)

snd_codec_t mp3_codec =
{
    "mp3",
    S_MP3_CodecLoad,
    S_MP3_CodecOpenStream,
    S_MP3_CodecReadStream,
    S_MP3_CodecCloseStream,
    NULL
};

struct snd_codec_mp3_info
{
    mp3dec_t decoder;

    byte encbuf[MP3_DATA_BUFSIZ];
    int encbuflen;
    int eof;

    /* One decoded frame can contain at most 1152 stereo sample frames. */
    byte pcmbuf[MP3_PCM_BUFFER_BYTES];
    int buflen;

    char filename[MAX_QPATH];
    unsigned int refillCount;
    unsigned int frameCount;
    unsigned int skippedBytes;
    unsigned int decodeErrors;
    unsigned int pcmProbeFrames;
    int scanning;
};

static void S_MP3_ConsumeInput(struct snd_codec_mp3_info *mp3info, int bytes)
{
    if (bytes <= 0)
        return;

    mp3info->encbuflen -= bytes;
    if (mp3info->encbuflen > 0)
        memmove(mp3info->encbuf, mp3info->encbuf + bytes, (size_t)mp3info->encbuflen);
}

/*
 * Preserve every unconsumed byte and append more compressed input. minimp3
 * expects a reasonably large look-ahead window for robust sync detection;
 * the 128 KiB buffer is deliberately retained across frame calls.
 */
static int S_MP3_ReadMore(snd_stream_t *stream,
                          struct snd_codec_mp3_info *mp3info,
                          int consumed)
{
    int leftover;
    int bytesRead;

    if (!stream || !mp3info || consumed < 0 || consumed > mp3info->encbuflen)
        return -1;

    leftover = mp3info->encbuflen - consumed;
    if (leftover > 0 && consumed > 0)
        memmove(mp3info->encbuf, mp3info->encbuf + consumed, (size_t)leftover);
    mp3info->encbuflen = leftover;

    if (mp3info->eof)
        return 0;

    if (leftover >= (int)sizeof(mp3info->encbuf)) {
        MP3_TRACE("PS3AUDIO: minimp3 input buffer full file=%s phase=%s kept=%d capacity=%u",
                  mp3info->filename, mp3info->scanning ? "scan" : "play",
                  leftover, (unsigned int)sizeof(mp3info->encbuf));
        return -1;
    }

    bytesRead = FS_Read(mp3info->encbuf + leftover,
                        (int)sizeof(mp3info->encbuf) - leftover,
                        stream->file);
    if (bytesRead <= 0) {
        mp3info->eof = 1;
        return 0;
    }

    if (bytesRead > (int)sizeof(mp3info->encbuf) - leftover)
        return -1;

    mp3info->encbuflen = leftover + bytesRead;
    mp3info->refillCount++;

    if (mp3info->refillCount <= 12) {
        MP3_TRACE("PS3AUDIO: minimp3 refill file=%s phase=%s count=%u kept=%d read=%d capacity=%u",
                  mp3info->filename, mp3info->scanning ? "scan" : "play",
                  mp3info->refillCount, leftover, bytesRead,
                  (unsigned int)sizeof(mp3info->encbuf));
    }

    return bytesRead;
}

#if defined(__PS3__)
static void S_MP3_ProbePCM(struct snd_codec_mp3_info *mp3info,
                           const mp3d_sample_t *pcm,
                           int samples,
                           const mp3dec_frame_info_t *frameInfo)
{
    int i;
    int totalSamples;
    int minSample = 32767;
    int maxSample = -32768;
    int peak = 0;
    unsigned int nearFullScale = 0;
    int firstLeft[4] = { 0, 0, 0, 0 };
    int firstRight[4] = { 0, 0, 0, 0 };

    if (mp3info->scanning || mp3info->pcmProbeFrames >= 2)
        return;

    totalSamples = samples * frameInfo->channels;
    for (i = 0; i < totalSamples; ++i) {
        const int value = (int)pcm[i];
        const int magnitude = value < 0 ? -value : value;

        if (value < minSample) minSample = value;
        if (value > maxSample) maxSample = value;
        if (magnitude > peak) peak = magnitude;
        if (magnitude >= 32760)
            nearFullScale++;

        if ((i / frameInfo->channels) < 4) {
            const int sampleIndex = i / frameInfo->channels;
            if ((i % frameInfo->channels) == 0)
                firstLeft[sampleIndex] = value;
            else
                firstRight[sampleIndex] = value;
        }
    }

    MP3_TRACE("PS3AUDIO: minimp3 PCM file=%s frame=%u rate=%d channels=%d samples=%d min=%d max=%d peak=%d near_full_scale=%u firstL=%d,%d,%d,%d firstR=%d,%d,%d,%d",
              mp3info->filename, mp3info->pcmProbeFrames + 1,
              frameInfo->hz, frameInfo->channels, samples,
              minSample, maxSample, peak, nearFullScale,
              firstLeft[0], firstLeft[1], firstLeft[2], firstLeft[3],
              firstRight[0], firstRight[1], firstRight[2], firstRight[3]);
    mp3info->pcmProbeFrames++;
}
#endif

/*
 * Decode the next complete frame, consuming ID3/junk bytes as minimp3
 * reports them. Returns sample frames per channel, zero at EOF, or -1 on an
 * invalid/stalled stream. The decoder state is preserved between calls.
 */
static int S_MP3_NextFrame(snd_stream_t *stream,
                           struct snd_codec_mp3_info *mp3info,
                           mp3d_sample_t *pcm,
                           mp3dec_frame_info_t *frameInfo)
{
    for (;;) {
        int samples;

        if (mp3info->encbuflen == 0 && !mp3info->eof) {
            if (S_MP3_ReadMore(stream, mp3info, 0) < 0)
                return -1;
        }

        if (mp3info->encbuflen == 0 && mp3info->eof)
            return 0;

        memset(frameInfo, 0, sizeof(*frameInfo));
        samples = mp3dec_decode_frame(&mp3info->decoder,
                                      mp3info->encbuf,
                                      mp3info->encbuflen,
                                      pcm,
                                      frameInfo);

        if (frameInfo->frame_bytes < 0 ||
            frameInfo->frame_bytes > mp3info->encbuflen) {
            mp3info->decodeErrors++;
            MP3_TRACE("PS3AUDIO: minimp3 invalid consumption file=%s bytes=%d buffered=%d",
                      mp3info->filename, frameInfo->frame_bytes, mp3info->encbuflen);
            return -1;
        }

        if (frameInfo->frame_bytes > 0) {
            if (samples <= 0) {
                int discardBytes = frameInfo->frame_bytes;

                /*
                 * minimp3 reports bytes skipped while looking for a sync word.
                 * A sync/header/frame may straddle an input refill boundary.
                 * Until EOF, retain a 16 KiB suffix so the next call can inspect
                 * that boundary with the recommended 10-frame look-ahead.
                 * This also lets arbitrarily large ID3 tags be skipped in chunks.
                 */
                if (!mp3info->eof) {
                    if (discardBytes > MP3_SYNC_LOOKAHEAD_BYTES)
                        discardBytes -= MP3_SYNC_LOOKAHEAD_BYTES;
                    else
                        discardBytes = 0;
                }

                if (discardBytes > 0) {
                    S_MP3_ConsumeInput(mp3info, discardBytes);
                    mp3info->skippedBytes += (unsigned int)discardBytes;
                    continue;
                }

                if (!mp3info->eof) {
                    if (S_MP3_ReadMore(stream, mp3info, 0) < 0)
                        return -1;
                    continue;
                }

                /* At EOF there is no later refill; discard the final junk. */
                S_MP3_ConsumeInput(mp3info, frameInfo->frame_bytes);
                mp3info->skippedBytes += (unsigned int)frameInfo->frame_bytes;
                continue;
            }

            S_MP3_ConsumeInput(mp3info, frameInfo->frame_bytes);

            if (frameInfo->layer != 3 ||
                frameInfo->channels < 1 || frameInfo->channels > 2 ||
                frameInfo->hz <= 0 || samples > 1152) {
                mp3info->decodeErrors++;
                MP3_TRACE("PS3AUDIO: minimp3 invalid frame file=%s layer=%d channels=%d rate=%d samples=%d",
                          mp3info->filename, frameInfo->layer,
                          frameInfo->channels, frameInfo->hz, samples);
                return -1;
            }

            mp3info->frameCount++;
#if defined(__PS3__)
            if (pcm)
                S_MP3_ProbePCM(mp3info, pcm, samples, frameInfo);
#endif
            return samples;
        }

        /* frame_bytes == 0 means minimp3 needs more compressed bytes. */
        if (mp3info->eof)
            return 0;

        if (mp3info->encbuflen >= (int)sizeof(mp3info->encbuf)) {
            mp3info->decodeErrors++;
            MP3_TRACE("PS3AUDIO: minimp3 made no progress file=%s buffered=%d capacity=%u",
                      mp3info->filename, mp3info->encbuflen,
                      (unsigned int)sizeof(mp3info->encbuf));
            return -1;
        }

        if (S_MP3_ReadMore(stream, mp3info, 0) < 0)
            return -1;
    }
}

/*
 * Scan once to populate the sound-system metadata, then rewind. This mirrors
 * the former codec's contract while using minimp3 for both scan and playback.
 */
static int S_MP3_Scanfile(snd_stream_t *stream,
                          struct snd_codec_mp3_info *mp3info)
{
    mp3dec_frame_info_t frameInfo;
    int decoded;
    int totalSamples = 0;
    int channels = 0;
    int rate = 0;

    mp3dec_init(&mp3info->decoder);
    mp3info->scanning = 1;
    mp3info->encbuflen = 0;
    mp3info->eof = 0;
    mp3info->refillCount = 0;
    mp3info->frameCount = 0;
    mp3info->skippedBytes = 0;
    mp3info->decodeErrors = 0;

    for (;;) {
        /* pcm == NULL asks minimp3 for frame/header metadata only. */
        decoded = S_MP3_NextFrame(stream, mp3info, NULL, &frameInfo);
        if (decoded <= 0)
            break;

        if (channels == 0) {
            channels = frameInfo.channels;
            rate = frameInfo.hz;
        } else if (channels != frameInfo.channels || rate != frameInfo.hz) {
            MP3_TRACE("PS3AUDIO: minimp3 format changed file=%s old_rate=%d old_channels=%d new_rate=%d new_channels=%d",
                      mp3info->filename, rate, channels,
                      frameInfo.hz, frameInfo.channels);
            decoded = -1;
            break;
        }

        if (decoded > INT_MAX - totalSamples) {
            decoded = -1;
            break;
        }
        totalSamples += decoded;
    }

    mp3info->scanning = 0;

    if (decoded < 0 || totalSamples <= 0 || channels < 1 || rate <= 0 ||
        totalSamples > INT_MAX / (channels * MP3_SAMPLE_WIDTH)) {
        MP3_TRACE("PS3AUDIO: minimp3 scan failed file=%s decoded=%d samples=%d rate=%d channels=%d errors=%u",
                  mp3info->filename, decoded, totalSamples,
                  rate, channels, mp3info->decodeErrors);
        return -1;
    }

    stream->info.rate = rate;
    stream->info.width = (float)MP3_SAMPLE_WIDTH;
    stream->info.channels = channels;
    stream->info.samples = totalSamples;
    stream->info.size = totalSamples * channels * MP3_SAMPLE_WIDTH;
    stream->info.dataofs = 0;
    stream->info.dataalign = 1;

    MP3_TRACE("PS3AUDIO: minimp3 scan file=%s rate=%d channels=%d samples=%d bytes=%d frames=%u skipped_bytes=%u",
              mp3info->filename, rate, channels, totalSamples,
              stream->info.size, mp3info->frameCount, mp3info->skippedBytes);

    FS_Seek(stream->file, 0, FS_SEEK_SET);
    stream->pos = 0;

    /* Start playback with a clean decoder and an empty input/PCM queue. */
    mp3dec_init(&mp3info->decoder);
    mp3info->encbuflen = 0;
    mp3info->eof = 0;
    mp3info->buflen = 0;
    mp3info->refillCount = 0;
    mp3info->frameCount = 0;
    mp3info->skippedBytes = 0;
    mp3info->decodeErrors = 0;
    mp3info->pcmProbeFrames = 0;

    return 0;
}

/*
 * Convert interleaved native int16 samples to the engine's host-native PCM
 * byte order. Use explicit stores so the PS3 big-endian path is unambiguous.
 */
static int S_MP3_WritePCM(byte *dest,
                          const mp3d_sample_t *pcm,
                          int samples,
                          int channels)
{
    int i;
    int sampleValues = samples * channels;
    int offset = 0;

    for (i = 0; i < sampleValues; ++i) {
        const int sample = (int)pcm[i];

#ifdef Q3_BIG_ENDIAN
        dest[offset++] = (byte)((sample >> 8) & 0xff);
        dest[offset++] = (byte)(sample & 0xff);
#else
        dest[offset++] = (byte)(sample & 0xff);
        dest[offset++] = (byte)((sample >> 8) & 0xff);
#endif
    }

    return offset;
}

/*
=================
S_MP3_CodecOpenStream
=================
*/
snd_stream_t *S_MP3_CodecOpenStream(const char *filename)
{
    snd_stream_t *stream;
    struct snd_codec_mp3_info *mp3info;

    if (!filename || !filename[0])
        return NULL;

    stream = S_CodecUtilOpen(filename, &mp3_codec);
    if (!stream)
        return NULL;

    if (stream->length <= 0) {
        S_CodecUtilClose(&stream);
        return NULL;
    }

    mp3info = (struct snd_codec_mp3_info *)Z_Malloc(sizeof(*mp3info));
    if (!mp3info) {
        S_CodecUtilClose(&stream);
        return NULL;
    }

    memset(mp3info, 0, sizeof(*mp3info));
    Q_strncpyz(mp3info->filename, filename, sizeof(mp3info->filename));

    if (S_MP3_Scanfile(stream, mp3info) < 0) {
        Z_Free(mp3info);
        S_CodecUtilClose(&stream);
        return NULL;
    }

    stream->ptr = mp3info;

    MP3_TRACE("PS3AUDIO: minimp3 stream open file=%s rate=%d channels=%d samples=%d bytes=%d",
              mp3info->filename, stream->info.rate, stream->info.channels,
              stream->info.samples, stream->info.size);
    return stream;
}

/*
=================
S_MP3_CodecCloseStream
=================
*/
void S_MP3_CodecCloseStream(snd_stream_t *stream)
{
    if (!stream)
        return;

    if (stream->ptr) {
        Z_Free(stream->ptr);
        stream->ptr = NULL;
    }

    S_CodecUtilClose(&stream);
}

/*
=================
S_MP3_CodecReadStream
=================
*/
int S_MP3_CodecReadStream(snd_stream_t *stream, int bytes, void *buffer)
{
    struct snd_codec_mp3_info *mp3info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t frameInfo;
    int remaining;
    int frameBytes;
    int total = 0;

    if (!stream || !buffer || bytes <= 0 || !stream->ptr)
        return 0;

    mp3info = (struct snd_codec_mp3_info *)stream->ptr;
    frameBytes = stream->info.channels * MP3_SAMPLE_WIDTH;
    if (frameBytes <= 0)
        return 0;

    remaining = stream->info.size - stream->pos;
    if (remaining <= 0)
        return 0;
    if (bytes > remaining)
        bytes = remaining;
    bytes -= bytes % frameBytes;
    if (bytes <= 0)
        return 0;

    while (total < bytes) {
        if (mp3info->buflen > 0) {
            int copyBytes = bytes - total;
            if (copyBytes > mp3info->buflen)
                copyBytes = mp3info->buflen;

            memcpy((byte *)buffer + total, mp3info->pcmbuf, (size_t)copyBytes);
            total += copyBytes;
            mp3info->buflen -= copyBytes;

            if (mp3info->buflen > 0) {
                memmove(mp3info->pcmbuf, mp3info->pcmbuf + copyBytes,
                        (size_t)mp3info->buflen);
            }
            continue;
        }

        {
            int decoded = S_MP3_NextFrame(stream, mp3info, pcm, &frameInfo);
            int pcmBytes;

            if (decoded <= 0) {
                if (decoded < 0) {
                    mp3info->decodeErrors++;
                    if (mp3info->decodeErrors <= 8) {
                        MP3_TRACE("PS3AUDIO: minimp3 playback decode failed file=%s errors=%u",
                                  mp3info->filename, mp3info->decodeErrors);
                    }
                }
                break;
            }

            if (frameInfo.hz != stream->info.rate ||
                frameInfo.channels != stream->info.channels) {
                mp3info->decodeErrors++;
                MP3_TRACE("PS3AUDIO: minimp3 playback format mismatch file=%s expected=%d/%d got=%d/%d",
                          mp3info->filename, stream->info.rate, stream->info.channels,
                          frameInfo.hz, frameInfo.channels);
                break;
            }

            pcmBytes = S_MP3_WritePCM(mp3info->pcmbuf, pcm, decoded,
                                      frameInfo.channels);
            if (pcmBytes <= 0 || pcmBytes > (int)sizeof(mp3info->pcmbuf)) {
                mp3info->decodeErrors++;
                break;
            }
            mp3info->buflen = pcmBytes;
        }
    }

    stream->pos += total;
    return total;
}

/*
=====================================================================
S_MP3_CodecLoad

Load the complete stream using the same incremental decoder path.
======================================================================
*/
void *S_MP3_CodecLoad(const char *filename, snd_info_t *info)
{
    snd_stream_t *stream;
    byte *pcmBuffer;
    int decodedBytes;

    if (!filename || !info)
        return NULL;

    stream = S_MP3_CodecOpenStream(filename);
    if (!stream)
        return NULL;

    *info = stream->info;

    pcmBuffer = (byte *)Z_Malloc(stream->info.size);
    if (!pcmBuffer) {
        S_MP3_CodecCloseStream(stream);
        return NULL;
    }

    decodedBytes = S_MP3_CodecReadStream(stream, stream->info.size, pcmBuffer);
    if (decodedBytes <= 0) {
        Z_Free(pcmBuffer);
        pcmBuffer = NULL;
        info->size = 0;
        info->samples = 0;
    } else if (decodedBytes != stream->info.size) {
        info->size = decodedBytes;
        info->samples = decodedBytes / (stream->info.channels * MP3_SAMPLE_WIDTH);
    }

    S_MP3_CodecCloseStream(stream);
    return pcmBuffer;
}

#endif /* USE_CODEC_MINIMP3 */
