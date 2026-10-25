# In-game tools (experimental)

These tools require a matching private SDK/game build. Configure the SDK with
`-DFM4_LIVE_SETTINGS=ON -DFM4_INGAME_TOOLS=ON`; both options default OFF.
Enable `runtime_overrides.fm4_live_settings=true` and only the tools you want:

| Setting | Default | Purpose |
| --- | --- | --- |
| `fm4_photo_export` | false | Export FM4 saved photo image payloads |
| `fm4_photo_directory` | empty | Empty selects Pictures/FM4 Enhanced |
| `fm4_save_backups` | false | Launch, timer and after-write save snapshots |
| `fm4_backup_count` | 8 | Active generations, 2–32 |
| `fm4_backup_minutes` | 10 | Idle timer, 1–120 minutes |
| `fm4_trainer` | false | Show the separately armed Trainer page |
| `fm4_local_achievements` | false | Per-profile guest achievement tracking |

Open F6 and select Photos, Save backups, Trainer or Achievements. Disabled tools
have no page. Existing launcher settings and deployed builds are unchanged.

Saving a photo in FM4's own photo mode queues an export after five seconds
without save writes. The exporter uses photo content packages and complete
JPEG/PNG image streams, retains their framing/effects and encoded dimensions,
and excludes thumbnails smaller than 640×360. It never substitutes a desktop or
normal gameplay screenshot. Export stored photos scans known photo packages and
Photo-prefixed save packages; open an unfamiliar older photo in FM4 once to
register its package. Files are deduplicated by SHA256 and grouped by XUID.
Unsupported encodings remain in the save. A 720p saved image remains 720p; this
version does not capture a separate higher-resolution photo resolve. Already
encoded high-resolution images retain their resolution.

Automatic photo export is experimental.

Backups include the signed-in XUID's entire FM4 title directory, including save
content and headers. They are stored below the selected user-data root in
`.fm4-tools/<XUID>/backups`. Each generation has file sizes and SHA256 hashes.
The worker waits for five quiet seconds and acquires read leases that refuse an
active writer. A shared guest I/O gate lets active operations finish and makes
new guest file opens/writes wait until a snapshot finishes, so backup read leases
cannot reject a new game save. Busy snapshots are deferred until the next write/timer/manual
request. Restore requires confirmation in F6 and a normal restart with backups
enabled. It verifies all files and applies before the guest main thread starts.
The previous title directory is retained as `before-restore-*` beside the title
directory, and a journal recovers interrupted directory moves. A failed pending
restore blocks guest startup instead of silently creating a new profile.

Only the configured number of active backups appears in F6. Superseded snapshots
are moved to `.fm4-tools-archive/rotated/<XUID>` rather than deleted. Archives and
pre-restore copies consume disk space until the owner removes them. Separate
launcher seed copies and Xbox profile settings outside the title save directory
are outside this backup's scope.

Trainer requires both its setting and the Arm trainer checkbox. Game speed
multiplies the existing measured delta from 0.25× to 2×; FM4 retains its own
physics substeps and safety clamp. Closing the page, collapsing/closing F6 or
disarming resets normal speed. A queued credits change is one explicit action:
close F6 and return to a menu that reads credits to apply it through FM4's own
setter, including dirty tracking and UI notifications. Cancel pending credits
change withdraws it; exiting the session also cancels unapplied requests.
Applied credits persist with the game save. Live time-of-day changes are
not verified; select the track's supported time of day in FM4. FM4 photo mode
provides the available free camera.

Achievements use FM4's own XGI write-achievements messages and title metadata.
Local unlocks and original unlock times persist in
`.fm4-tools/<XUID>/achievements.json`, with a toast and F6 list. The currently
supported runtime has one signed-in local user (index 0). Other user indices
are ignored, and existing title-wide unlock files are not imported into this
per-profile list. Guest unlock IDs missing from the loaded metadata are retained
with an ID label, including DLC unlocks whose labels are unavailable. Xbox Live
synchronization is not provided.
