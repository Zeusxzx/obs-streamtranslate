# obs-streamtranslate

An OBS Studio audio filter that sends your audio to
[StreamTranslate](https://streamtranslate.live) for live translated captions —
built for IRL streamers and cloud OBS setups (IRLToolkit and similar) where
there's no browser tab available to capture a microphone.

**Full setup guide: https://streamtranslate.live/obs-plugin.html**

## How it works

Two pieces, and they do different jobs:

- **This plugin** is an audio filter. You attach it to your mic or stream audio
  source and it quietly sends a copy of your audio to StreamTranslate. It does
  not alter your audio and nothing appears on your stream.
- **Your captions overlay** is a normal Browser Source using the overlay URL from
  your StreamTranslate account. That's what actually displays the captions.

Filter = the microphone. Browser source = the screen.

## Install

Grab the latest build from [Releases](https://github.com/Zeusxzx/obs-streamtranslate/releases/latest).

### macOS

Open the `.pkg`. macOS will block it the first time because the plugin isn't
signed with an Apple Developer certificate yet.

**Without Terminal:** double-click the .pkg, let it get blocked, then go to
**System Settings → Privacy & Security**, scroll to the bottom, and click
**Open Anyway**. Run the installer again.

On recent macOS versions right-click → Open does *not* work for .pkg files —
use the steps above.

**With Terminal:**
```bash
xattr -d com.apple.quarantine ~/Downloads/StreamTranslate-OBS-Plugin.pkg
```
then open the .pkg normally.

Restart OBS after installing.

### Windows

Extract the zip into your OBS folder — the plugin lands in `obs-plugins\64bit`.
Restart OBS.

### Linux / cloud OBS

Use the `.so` from Releases, or build from source (below).

Cloud OBS providers don't let you put files on their servers, so open a support
ticket asking them to install this plugin and link this repository. It's open
source and Linux-compilable, which is what providers such as IRLToolkit require.
Everything after that you do yourself from your OBS remote.

## Setup

1. Right-click your audio source → **Filters** → **+** → **StreamTranslate**
2. Paste your **Plugin Key** (generate it on your StreamTranslate control page
   under *Cloud OBS / IRL Mode*)
3. Add your overlay URL as a **Browser Source** to display the captions

The filter shows a live status line — it counts audio as you speak when
connected, and names the problem when it isn't.

## Using it

- **Change languages:** from your control page, in any browser including your
  phone. Applies live, never asks for microphone access.
- **Pause:** mute the audio source in OBS, or disable the filter (the eye icon).
  Captions stop immediately.
- **Stop:** the control page shows a *Stop stream* button while the plugin is
  connected.

## Building from source (Linux)

```bash
sudo apt install build-essential cmake pkg-config libobs-dev libwebsockets-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The module is `build/obs-streamtranslate.so`.

## Privacy

Audio is only sent while the filter is enabled and the source is unmuted.
Muting in OBS stops transmission immediately.
