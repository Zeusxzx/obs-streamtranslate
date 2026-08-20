# obs-streamtranslate

An OBS Studio audio filter that streams your audio source to
[StreamTranslate](https://streamtranslate.live) for real-time translated
captions — designed for cloud-hosted OBS setups (IRLToolkit and similar)
where no local browser exists to capture your microphone.

## How it works

Attach the **StreamTranslate** filter to the audio source you stream with.
The filter passes your audio through untouched, and in parallel sends a
mono copy to StreamTranslate's servers over a secure websocket. Captions
render through the StreamTranslate overlay browser source like normal.

No browser tab, no PC, nothing running on your phone.

## Setup

1. Generate a **Plugin Key** from your StreamTranslate account.
2. In OBS: right-click your stream audio source → Filters → + →
   **StreamTranslate (live translated captions)**.
3. Paste your Plugin Key. Leave Server as `streamtranslate.live`.
4. Add the StreamTranslate overlay as a Browser Source (same as any setup).
5. Pick your languages from your StreamTranslate control page (works from
   any phone browser — no mic access needed).

Going live is automatic: when this filter is receiving audio and connected,
your room is live.

## IRLToolkit / cloud OBS

This plugin is Linux-compilable with open source code, meeting
[IRLToolkit's third-party plugin requirements](https://account.irltoolkit.com/plugin/support_manager/knowledgebase/view/17/third-party-obs-plugins/6/).
Open a support ticket with them referencing this repository to have it
installed on your cloud OBS server.

## Building (Linux)

```
sudo apt install build-essential cmake pkg-config libobs-dev libwebsockets-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The built module is `build/obs-streamtranslate.so` — install to your OBS
plugins directory.
