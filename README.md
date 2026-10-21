# ViTube Music

A YouTube Music client for the PS Vita. Search for a song, hit play, and autoplay radio keeps the music going. It runs entirely on the console, no companion server or PC needed.

## Building

You need [VitaSDK](https://vitasdk.org) with these packages installed:

```
vdpm curl faad2 libvita2d freetype libpng libjpeg-turbo zlib zstd bzip2
```

Then:

```
mkdir build && cd build
cmake ..
make
```

## Files on the Vita

Everything lives in `ux0:data/vitube-music/`:

- `config.json` is created on first run:
  ```json
  { "hl": "en", "gl": "US", "insecure_tls": false }
  ```
  `hl` and `gl` are the language and region sent to YouTube. `insecure_tls` turns off certificate checking and is only meant for diagnosing certificate problems.
- `favorites.json` and `history.json` are your lists.
- `cacert.pem` is optional. If it's there, it replaces the bundled CA list.
- `log.txt` records what happened (HTTP status codes, decode errors and so on). Attach it when opening an issue.

YouTube changes things often and this can break without warning. The client version and request shape live in `src/ytm.c`.

## How it works

Search and radio use the YouTube Music web API. For audio it asks for an HLS stream, downloads the audio-only MPEG-TS segments, pulls out the AAC frames and decodes them in software with FAAD2.

## Known limitations

- No sign-in, so no personal library or recommendations. Favorites are stored locally.
- Not gapless, there's a short pause between tracks.
- Seeking lands within one segment of the target, then skips ahead to the exact spot.
- Autoplay stops after about 1000 queued tracks.

## Credits and license

- [ViTube](https://github.com/shorelight82/vitube-vpk) documents the HLS approach on the Vita. No code was copied from it.
- Request shapes follow [yt-dlp](https://github.com/yt-dlp/yt-dlp) and [ytmusicapi](https://github.com/sigma67/ytmusicapi).
- jansson (MIT) is bundled. libcurl, vita2d and FreeType are linked from VitaSDK.
- FAAD2 is GPL-2.0-or-later, so the combined work is distributed under **GPL-2.0-or-later**.

Unofficial and for personal use. Not affiliated with Google or YouTube.