# PS3 XMB presentation assets

This directory is the input location for the optional XMB presentation media.

- `PIC1.PNG`: the PS3 Game-column background image (1920x1080).
- `Medal_of_Honor_Allied_Assault_30s.mp3`: the opening 30 seconds of the XMB hover music source. The Linux workflow trims it to exactly 30 seconds, encodes plain ATRAC3, wraps it as RIFF/WAVE with the required sample/loop chunks, verifies it, and packages it as `SND0.AT3`.

The package builder puts `PIC1.PNG` and `SND0.AT3` at the package content root, beside `PARAM.SFO` and `ICON0.PNG`; these are XMB content files, not game data under `USRDIR`.

If either media input is absent, the build emits a warning and continues without that custom media. This keeps unrelated build/test jobs working while the assets are being staged.
