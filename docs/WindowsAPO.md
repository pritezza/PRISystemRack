# How Windows APOs Work

Windows lets you plug components into the playback and recording paths to apply effects to the sound. These components are called APOs (Audio Processing Objects). PRI System Rack uses this mechanism to pass all system playback audio through VST3 plugins.

This document explains where APOs run, how they are registered, and what rules they must follow. It then describes how the PRI System Rack APO works.

## What an APO is

- An APO is a COM object packaged in a DLL. APOs have been used since Windows Vista.
- The Windows audio engine (`audiodg.exe`) loads APO DLLs and runs them inside its own process.
- APOs were originally intended for sound device manufacturers to add device-specific effects such as speaker correction, equalization, or loudness. Depending on the driver and the Windows version, an "Enhancements" tab may appear in the device properties of the Sound control panel, where these effects can be turned on and off. (Current Microsoft documentation recommends a Hardware Support App for manufacturer-specific settings.)

## How playback audio flows

```
App A ─┐
App B ─┼─ Windows audio engine (audiodg.exe) ─→ driver ─→ sound device
App C ─┘        │
                └ APOs process the audio here
```

- Ordinary apps play sound in shared mode (WASAPI shared mode). The audio engine mixes the sound from all apps into one stream and sends it to the device.
- APOs sit in this path. Some slots process each app's sound separately, and others process the sound after it has been mixed (see the next section).
- Sound played in exclusive mode (WASAPI exclusive mode) or through ASIO reaches the driver without going through the audio engine's mix, so it does not pass through APOs.

## Slots for APOs

An APO is placed in a "slot" that determines where in the audio path it processes. Which APO goes in which slot is set in the registry for each playback device (see "Registering an APO with a device").

Current Microsoft documentation describes three slots:

| Slot | Full name | Where it processes |
| --- | --- | --- |
| SFX | Stream effect | Before the mix. One instance per app stream |
| MFX | Mode effect | After the streams of the same mode (see below) are mixed, but before all modes are mixed |
| EFX | Endpoint effect | After all modes are mixed. Applies to all sound sent to the device |

- Modes: each stream has a mode that describes its purpose (default, communications, movie, music, RAW, and so on). A device can apply different effects for each mode.
- RAW mode asks for sound with no processing at all. According to Microsoft documentation, SFX are not applied to RAW streams (Windows 10 does load RAW MFX), while EFX are always applied, even to RAW streams.
- Except for the Composite slots (see the next table), a device can hold only one APO per slot. Putting a different APO in a slot replaces the previous one.

There are also these slots:

| Slot | Description |
| --- | --- |
| LFX | Older-style slot. Before the mix (close to the SFX position) |
| GFX | Older-style slot. After the mix (PRI System Rack goes here) |
| Composite SFX / MFX / EFX | Lets one slot hold several APOs in a chain (since Windows 10 version 1803) |
| UI | Not an APO. Specifies the component that shows the settings page on the "Enhancements" tab |

The older-style slots (LFX / GFX) are not used when the same device has any SFX, MFX, or EFX. (This is not in Microsoft documentation; it comes from material published by developers of system-wide APOs.)

## Registering an APO with a device

Two registrations are needed before an APO can be used:

1. **Registering the APO itself**: register the DLL as a COM object (`HKLM\SOFTWARE\Classes\CLSID\{APO CLSID}`). Also add it to the list of APOs (`HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects`).
2. **Assigning it to a playback device**: under the per-device key `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{device GUID}\FxProperties`, write the APO's CLSID as the value for a slot.

The main values that hold APO CLSIDs are named `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},<number>`. The numbers are as follows. (FxProperties also contains other values, such as the effect name and the list of supported modes.)

| Number | Slot |
| --- | --- |
| 1 | LFX |
| 2 | GFX |
| 3 | UI |
| 5 | SFX |
| 6 | MFX |
| 7 | EFX |
| 13–15 | Composite SFX / MFX / EFX (a list of CLSIDs) |

- Normally, the driver's setup file (INF) performs this registration when the sound device driver is installed. Starting with Windows 10 version 1809, APOs installed with a driver must be registered by that driver itself (the componentized model).
- Tools that apply effects system-wide work independently of the driver. They register their APO in the location in step 1 and assign it by writing FxProperties directly. PRI System Rack works this way, and we have confirmed that its APO loads on Windows 10 22H2.
- FxProperties is under `HKLM`, so administrator rights are required to change it.
- After changing it, restarting the Windows Audio service (`Audiosrv`) loads the new APO (no PC restart is needed). Windows sound stops for a few seconds during the restart.

## Signing and the protected environment

- The audio engine runs in a protected environment and does not load APOs that do not meet its requirements.
- Setting `DisableProtectedAudioDG` to 1 under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio` weakens this protection, so unsigned APOs are also loaded. Microsoft documentation treats this as a setting for development and testing. Users have reported that playback of protected content (such as copy-protected video) can fail when it is set to 1.
- According to Microsoft documentation, if creating or preparing an APO (format checks, `LockForProcess`) fails 10 times in a row, Windows disables effects ("Enhancements") for that device. A success in between resets the count. The documentation describes this for SFX, MFX, and EFX.

## Rules an APO must follow

The audio engine calls an APO's methods roughly in this order:

```
Initialize                          receive which device and slot the APO is used for
IsInputFormatSupported, etc.        agree on the audio format (channels, sample rate, etc.)
LockForProcess                      prepare before processing starts
APOProcess (repeated each period)   process the audio
UnlockForProcess                    end of processing
```

- `APOProcess` is called at a fixed period from the audio engine's real-time thread. The period depends on the device. In our development environment (Windows 10 22H2, 48 kHz, 2 channels), each call was 480 frames (10 ms).
- Microsoft documentation requires that methods called in real time "must not block, use paged memory, or call any blocking system routines." In practice, this means no memory allocation, locks, file access, or logging inside `APOProcess`.
- If one call does not finish within the period, all sound on that device drops out. Because the APO runs inside `audiodg.exe`, an APO crash takes down `audiodg.exe` and the device's sound drops out.
- `audiodg.exe` runs as a service in a session separate from the user's desktop (session 0). In our development environment it ran under the LOCAL SERVICE account. An APO cannot show any UI.
- The audio engine creates APOs using COM aggregation. An APO that does not support aggregation does not work.

## Why plugins don't run inside the APO

Running VST3 plugins directly inside the APO (inside `audiodg.exe`) causes these problems:

- Plugin editors cannot be shown, because the process runs in a session separate from the desktop.
- If a plugin crashes, the whole audio engine goes down and all sound on the device drops out.
- Plugins often rely on the user's folders and on licensing systems, which are not available to a service account.
- If a heavy plugin does not finish within the period, all sound drops out.

So PRI System Rack keeps the APO's job to "pass the audio to the main app and receive the result," and runs the plugins in the main app, an ordinary process on the user's desktop.

## The PRI System Rack APO

```
App audio → audio engine (audiodg.exe) mix → GFX slot → sound device
                                               │ ↑
                                    shared memory (audio exchange)
                                               ↓ │
                          main app (runs VST3 plugins in order, shows their editors)
```

### Where it goes

- It goes in the playback device's GFX slot (after the mix). It processes the mixed sound of all apps once, so there is a single processing path no matter how many apps are playing.
- The helper program `PRISystemRackSetup.exe`, which runs with administrator rights, adds and removes it. The main app calls it from its Enable and Disable buttons.
- When enabling, Setup saves all APO values in the device's FxProperties (the LFX, GFX, UI, SFX, MFX, EFX, and Composite slots) and the effect name to `HKLM\SOFTWARE\PRISystemRack\EndpointBackup\{device GUID}`, then removes them. It creates this backup even if no other APO is installed. Because the GFX slot is not used when any SFX, MFX, or EFX is present, effects installed by the manufacturer's driver (such as speaker correction) are also removed while PRI System Rack is enabled.
- When disabling, Setup writes the saved values back and deletes the backup. However, if another tool changed a slot after enabling and its current value differs from the backup, Setup keeps the current value.

### Exchanging audio with the main app

- In `Initialize`, the APO reads the device GUID from the device properties it is given. In `LockForProcess`, it creates a shared memory block and two events whose names include that GUID (for example, `Global\PRISystemRack.v1.Memory.{device GUID}`). The main app opens them by the same names.
- `audiodg.exe` and the main app run under different accounts in different sessions. The names are therefore placed in `Global\`, with access rights that let the logged-on user read and write them.
- The flow for one period:
  1. The APO writes the input audio to shared memory, increments a sequence number, and signals the main app (Request event).
  2. The main app processes the audio with the plugins, writes the result to shared memory, and signals the APO (Response event).
  3. The APO copies the result to its output.
- Inside `APOProcess`, the APO waits for the main app's result for up to 60% of the period (6 ms for a 10 ms period). This deliberately departs from the "must not block" rule in the previous section. The wait has an upper limit. If the result does not arrive in time, the APO passes the original sound through unprocessed for that period, so the sound does not drop out.
- The main app writes an "alive" marker (heartbeat) each time it waits for the APO's signal; each wait lasts at most 0.1 seconds. If the heartbeat is older than 0.5 seconds, the APO treats the main app as gone and passes the sound through without waiting. When the main app exits normally, it tells the APO before exiting, so pass-through starts immediately. Even if the main app crashes, the sound does not stop.
- Silent periods are also passed to the main app, so reverb tails and analyzer displays are not cut off.
- Inside `APOProcess`, the APO only reads and writes shared memory and waits on events. It does not allocate memory or write logs.

### Latency

- The result is returned within the same period, so passing through the APO adds no latency. The only added latency is the plugins' own latency, plus the main app's processing buffer when it is used.
- In measurements during development (2026-11-03, Windows 10 22H2), the round trip averaged 23 µs, with a maximum of 162 µs. These figures were measured with a test host and do not include plugin processing.

## Things to keep in mind

- Sound played in exclusive mode or through ASIO does not pass through APOs, so PRI System Rack's plugins are not applied to it either.
- While PRI System Rack is enabled, all other APOs on that device (including effects installed by the manufacturer's driver) are removed. It cannot be used together with other APOs on the same device.
- Windows Audio is restarted every time the APO is added or removed, so sound stops for a few seconds.
- Right after the APO is added, the first playback may fail in the app with a "device invalidated" error. Playing again works.

## References

- [Windows Audio Processing Objects](https://learn.microsoft.com/windows-hardware/drivers/audio/windows-audio-processing-objects) (Microsoft Learn)
- [Audio Processing Object Architecture](https://learn.microsoft.com/windows-hardware/drivers/audio/audio-processing-object-architecture) (positions of SFX / MFX / EFX)
- [Implementing Audio Processing Objects](https://learn.microsoft.com/windows-hardware/drivers/audio/implementing-audio-processing-objects) (interfaces to implement, real-time rules, registration)
- [Audio Signal Processing Modes](https://learn.microsoft.com/windows-hardware/drivers/audio/audio-signal-processing-modes) (modes and RAW)
