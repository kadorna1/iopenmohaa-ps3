# PS3 XMB assets

This directory is optional. The PKG builder copies every file and subdirectory
under `assets/xmb/` to the package content root, excluding this README. No
particular XMB file is required for CI or packaging to succeed.

Recognized Sony filenames receive their normal handling when present:

- `ICON0.PNG`: fitted to 320x176 RGBA while preserving transparency.
- `PIC1.PNG`: fitted to 1920x1080 RGBA.
- `SND0.AT3`: checked for a RIFF/WAVE ATRAC stream and copied as the canonical
  XMB hover sound.

Other files and subdirectories retain their names and contents. The builder
does not fetch missing assets, generate filename aliases, or require a source
MP3. An absent icon, background, or hover sound simply means that custom item
is omitted.

## Preparing hover audio

For a PS3-compatible XMB hover sound, use a legally obtained source recording
and prepare a 30-second, 44.1 kHz, stereo PCM WAV before ATRAC3 encoding:

```sh
ffmpeg -y -v error -i source.mp3 -t 30 -ar 44100 -ac 2 \
  -sample_fmt s16 -c:a pcm_s16le /tmp/xmb-hover-30s.wav

python3 tools/pspmedia.py snd0 /tmp/xmb-hover-30s.wav \
  -o assets/xmb/SND0.AT3 --encoder atracdenc --codec at3

python3 tools/pspmedia.py analyze assets/xmb/SND0.AT3
ffprobe -v error -select_streams a:0 \
  -show_entries stream=codec_name,sample_rate,channels \
  -of default=nw=1 assets/xmb/SND0.AT3
```

The package builder validates the ATRAC container structure when the
canonical `SND0.AT3` is supplied. It does not download or compile an encoder
during CI.
