# ViTube Music

A YouTube Music client for the PS Vita. Search for a song, hit play, and autoplay radio keeps the music going. It runs entirely on the console, no companion server or PC needed.

<img width="960" height="544" alt="2026-10-24-152555" src="https://github.com/user-attachments/assets/d1bf986e-641c-42b5-932a-e05114950ca5" />

> [!CAUTION]
> This project was made with AI assistance.

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
- `settings.json` stores the switches from the Options tab.
- `playlists.json` holds your playlists and `downloads.json` is the index of downloaded songs. The audio itself is in `downloads/` (one `.aac` file and one cover `.jpg` per song).
- `cacert.pem` is optional. If it's there, it replaces the bundled CA list.
- `log.txt` records what happened (HTTP status codes, decode errors and so on). Attach it when opening an issue.

YouTube changes things often and this can break without warning. The client version and request shape live in `src/ytm.c`.

## Playlists and offline

- Hold TRIANGLE on a song (or press and hold it on the touch screen) to open its menu: add it to a playlist, download it, add it to the queue, or remove it from the playlist you're in. A quick TRIANGLE press still toggles the favorite.
- The Playlists tab lists your playlists, and the first row creates a new one. Hold TRIANGLE on a playlist to play, download, rename or delete it.
- Downloads run one at a time in the background, with progress shown in the top bar. Downloaded songs show a green arrow, appear in the Offline tab, and play from the Vita's storage wherever they come up, with no network needed.
- Downloaded songs are raw AAC streams meant for this app, not files for the Vita's Music app.

## How it works

Search and radio use the YouTube Music web API. For audio it asks for an HLS stream, downloads the audio-only MPEG-TS segments, pulls out the AAC frames and decodes them in software with FAAD2.

## Known limitations

- No sign-in, so no personal library or recommendations. Favorites are stored locally.
- Not gapless, there's a short pause between tracks.
- Seeking lands within one segment of the target, then skips ahead to the exact spot.
- Autoplay stops after about 1000 queued tracks.
- Playlists are stored locally. Playlists from your YouTube Music account can't be imported.
- Playback from the Offline tab doesn't auto-fill the queue with similar songs.

## Credits and license

- [ViTube](https://github.com/shorelight82/vitube-vpk) documents the HLS approach on the Vita. No code was copied from it.
- Request shapes follow [yt-dlp](https://github.com/yt-dlp/yt-dlp) and [ytmusicapi](https://github.com/sigma67/ytmusicapi).
- jansson (MIT) is bundled. libcurl, vita2d and FreeType are linked from VitaSDK.
- FAAD2 is GPL-2.0-or-later, so the combined work is distributed under **GPL-2.0-or-later**.

Unofficial and for personal use. Not affiliated with Google or YouTube.
