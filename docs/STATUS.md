# Current status and limitations

Modified 2026-10-23.

This experimental edition supports startup, profile loading, race completion,
saving, achievements and driver-level progression. Career progression is verified
through Amateur and Clubman into Sportsman; later tiers are unverified.
The XMediaFacade and SpeechFacade modules load, and the four extracted Disc 2
packages can be installed into local user data.

## Graphics

The Enhanced edition supports native 720p, 1440p and 4K rendering, supersampling and optional texture packs. DLSS, FSR and Neural Rendering have reached live driving but remain experimental; see [graphics settings](ENHANCEMENTS.md).

Keep the supplied edition settings for occlusion fallback and sun visibility.
Fallback changes can affect both road visibility and exposure; these workarounds
do not establish correctness on every track.
Automatic readback and guest-vblank overrides can cause flicker or texture
corruption. Full readback has a hardware-dependent synchronization cost.
Experimental batching needs broader scene and memory-coherency validation.

## Input

Wheel/controller switching, paddle shifting, analog clutch operation and virtual
device disconnect/reconnect are covered by integration checks. Fanatec input
ranges have been measured. Other wheel models, physical force direction/feel,
physical device handoff and DualSense Bluetooth/adaptive-trigger feel require
further hardware validation. See [Wheel support](WHEEL_SUPPORT.md).

## Known limitations

- Car thumbnails can show the showroom without the car; blank results can remain cached.
- Career-garage menu layers can disappear while the car continues rendering.
- Smooth frame pacing and whole-race performance are not established across hardware and tracks.
- Online services and marketplace acquisition are outside the tested scope.
- Full car/track coverage and long-term career compatibility are unverified.

See [Build notes](../BUILD-NOTES.md) for the required toolchain and
[Forza Plus 4](FORZA_PLUS.md) for the optional mod’s separate limitations.
