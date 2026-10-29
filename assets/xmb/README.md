# PS3 XMB presentation assets

This directory is the source location for optional PS3 XMB presentation assets.

- `PIC1.PNG`: Game-column background image. The workflow scales it to 1920x1080 RGBA PNG.
- `SND0.AT3`: pre-converted PS3 XMB hover audio. Use the canonical Sony filename exactly; do not check in an MP3 and do not generate aliases such as `SND01.AT3` or `SND0_01.AT3`.

## Audio requirements

The workflow copies `assets/xmb/SND0.AT3` to the package content root as `SND0.AT3` and validates it before packaging. It must be a RIFF/WAVE SND0 file containing plain ATRAC3 audio, 44.1 kHz stereo, with the expected Sony XMB chunks, and be no larger than 500 KiB. An MP3 is not a PS3-compatible XMB hover file and is not converted during the build.

To prepare the file locally from a legally obtained source MP3, first decode/trim it to a 30-second 44.1 kHz stereo PCM WAV, then use the checked-in media tool and an ATRAC3 encoder:

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

Commit the resulting binary at `assets/xmb/SND0.AT3`. The build no longer clones or compiles an ATRAC3 encoder and therefore does not need the source MP3 at CI time.

The package builder places `PIC1.PNG` and `SND0.AT3` at the package content root, beside `PARAM.SFO` and `ICON0.PNG`, not under `USRDIR`. If either asset is absent, the build warns and continues without that custom XMB media.
