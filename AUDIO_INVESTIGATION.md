# Anland Android Audio Investigation

Last updated: 2026-07-28

## Repositories and Git State

- Original clean source: `C:\software\anland-main`
- Working tracked source: `C:\software\anland-5.15`
- `C:\software\anland-5.15` was not a git repository initially. It is now tracked locally.

Important commits in `C:\software\anland-5.15`:

- `c182f06` - `Baseline original anland-main`
  - Created from the untouched `C:\software\anland-main` tree.
- `ff5b3a8` - `Snapshot current anland-5.15 investigation state`
  - Preserved the then-current mixed state, including older APK/rootfs/producer experiments.
- `48dc2e6` - `Limit investigation state to APK audio bridge changes`
  - Native APK audio bridge rewrite only.
  - Relative to `c182f06`, this commit only changes:
    - `consumers/anland_v5/android_consumer/app/src/main/jni/native_audio.c`
    - `consumers/anland_v5/android_consumer/app/src/main/jni/native_audio.h`
- `9f0db4e` - `Declare media audio focus for Android consumer`
  - Adds Android-side media stream/audio-focus declaration in `MainActivity`.
- `edc283e` - `Record APK audio investigation notes`
  - Adds this investigation file so the evidence survives context compaction.
- `135d1ee` - `Harden APK audio callback recovery`
  - Keeps all follow-up work APK-only.
  - Aligns playback ring reads/writes/drops to whole PCM frames.
  - Adds an audio-output-only stall recovery path that reopens AAudio without
    touching the display reconnect path.
  - Preserves queued PCM across audio-only reopen when the playback format did
    not change, so the first post-idle sound is not discarded by recovery.
  - Makes the capture buffer match the actual input channel count.

Relative to `c182f06`, the current code changes are limited to:

- `consumers/anland_v5/android_consumer/app/src/main/java/com/anland/consumer/MainActivity.java`
- `consumers/anland_v5/android_consumer/app/src/main/jni/native_audio.c`
- `consumers/anland_v5/android_consumer/app/src/main/jni/native_audio.h`

The current direction is APK-only. Producer/rootfs/KDE backend changes were removed from the current tree after being preserved in git history.

## Observed Symptoms

- Pressing the Linux/Ubuntu volume key only plays the first sound.
- After that, changing volume again is silent until the app is backgrounded/reopened or the screen orientation changes.
- Opening Ubuntu's bottom-right volume menu makes repeated volume changes audible.
- Closing that menu brings the one-shot/no-sound behavior back.
- Web video playback can have sound the first time, then become silent after a pause.
- A previous reconnect-based APK attempt made sound recover sometimes, but caused screen flashes because it rebuilt the display path.
- Repeated volume-key presses after that reconnect attempt sometimes kept sound going briefly, then produced loud burst/noise.
- Releasing the key after that reconnect attempt could trigger one more visible flash.
- Opening the Ubuntu volume menu and then holding the volume key did not produce the same burst/noise.

## First-Principles Hypothesis

The audio transport itself can carry repeated audio, because opening Ubuntu's volume menu keeps sound working. That points away from a Linux distribution problem and toward Android-side playback lifecycle/timing.

Original APK-side playback did this:

- Open one AAudio output stream.
- Poll the audio socket.
- When a PCM datagram arrives, call `AAudioStream_write()` with a short timeout.
- When Linux is idle, no audio is written.

Short UI sounds such as volume ticks are very bursty. If Android's output stream or route has gone idle/cold, the first burst can wake the path but later bursts may be dropped or fail to restart cleanly. Ubuntu's volume menu likely keeps the Linux/PipeWire side producing enough continuous activity that the Android stream stays hot.

Display reconnect is the wrong recovery tool:

- It recreates/renegotiates display buffers and native window state.
- That explains the visible flash.
- Repeated reconnect or write/reopen cycles can accumulate stale PCM or timing discontinuities, explaining burst/noise.

The APK-side fix should keep Android playback clocked without touching the display connection.

The latest long-press result narrows the APK-side failure mode further:

- The audio path is capable of sustained playback while the Ubuntu volume menu is open.
- The burst/noise only appears in the reconnect-based recovery path, so display
  reconnect is not just visually bad; it also perturbs audio timing.
- A plausible APK-only root cause remains: after idle, Android's AAudio callback
  or output route can be cold/stalled while Linux sends short PCM bursts. Recovery
  must be local to the AAudio stream and must keep PCM frame boundaries intact.

## Current APK-Only Implementation

`native_audio.c` now uses a pull model for playback:

- AAudio output is opened with a data callback.
- The callback continuously feeds Android:
  - queued Linux PCM when available;
  - zeroed silence when Linux is idle.
- The socket thread only receives PCM datagrams and appends them to a small ring buffer.
- If the ring overflows, old PCM is dropped so stale audio cannot burst later.
- The ring is reset when the audio fd changes.
- An AAudio error callback marks the output stream for reopen by the playback thread.
- Ring capacity, reads, writes, and overflow drops are aligned to full PCM frames.
  This prevents stale-buffer trimming from splitting a 16-bit sample or stereo
  frame, which can sound like a loud burst.
- If PCM is queued but the AAudio callback has not run for about 600 ms, the
  playback thread reopens only the AAudio output stream. It does not set
  `need_reconnect` and does not rebuild the display/native-window path.
- Audio-only reopen preserves queued PCM when the negotiated playback format is
  unchanged. Full resets still happen on fd changes, stop/destroy, or format
  changes.
- The capture thread now allocates its mic buffer using the actual input channel
  count returned by AAudio instead of assuming mono storage forever.

This models the useful part of "Ubuntu volume menu open": Android sees a continuously running media output stream, but the display pipeline is not reconnected.

`native_audio.h` was updated to document the callback/ring-buffer behavior.

`MainActivity.java` now also declares the Android window as a media-audio client:

- `setVolumeControlStream(AudioManager.STREAM_MUSIC)` keeps hardware/system volume
  handling tied to the music/media stream while Anland is focused.
- The activity requests `AUDIOFOCUS_GAIN` with `USAGE_MEDIA`/`CONTENT_TYPE_MUSIC`
  on resume and abandons it on pause/destroy.
- This is APK-only and does not reconnect display surfaces.

## Local Verification Limits

This VM cannot fully build or run the APK locally:

- No Java found.
- No Android SDK/`ANDROID_HOME` found.
- No usable WSL Linux distribution.
- No C compiler found (`clang`, `gcc`, and `cl` are unavailable).

Git/source checks completed:

- `git diff --name-status c182f06..HEAD` shows only APK-side code files plus
  this investigation note:
  - `AUDIO_INVESTIGATION.md`
  - `MainActivity.java`
  - `native_audio.c`
  - `native_audio.h`
- Old audio-triggered display reconnect code is not part of the current APK-only tree.

Expected build path:

- Upload/export current `C:\software\anland-5.15` or the generated upload directory to the `FengY233/anland` fork.
- Let GitHub Actions build the APK.

## Test Expectations

If this APK-side hypothesis is correct:

- Pressing Ubuntu volume keys repeatedly should keep producing ticks without opening Ubuntu's volume menu.
- No screen flash should occur after stopping volume-key presses.
- Web video should resume audio after pause more reliably.
- If PCM arrives faster than Android can play it, old PCM should be dropped instead of delayed into a loud burst.

If it still fails:

- Check `logcat` for tags `AnlandAudio` and `Anland`.
- Important messages:
  - `output stream ready`
  - `output stream error`
  - `output stream disconnected, reopening`
  - `output callback stalled with ... queued bytes; reopening audio output only`
  - `device formats`
- Next APK-only ideas to investigate:
  - Try an `AudioTrack` playback backend as an alternative to AAudio.
  - Add a user-tunable playback ring size/latency if burst underrun or overrun remains.
  - Add temporary log counters for callback cadence, queued bytes, and AAudio
    `xrun` count around the first silent post-idle volume tick.

## Do Not Reintroduce

- Do not use display reconnect as audio recovery.
- Do not modify rootfs/KDE producer as the next step unless the user explicitly reverses the APK-only constraint.
- Do not upload the old mixed `ff5b3a8` state as the current solution; it is preserved only for history.
