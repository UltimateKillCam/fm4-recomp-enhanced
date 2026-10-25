# Owner-supplied DLC

For original Xbox 360 packages, choose **Original Xbox content folder** on the
launcher's Content tab, or set `dlc_packages_folder` to the FM4 title folder
containing `00000002` (and optionally `00009000`). Leave it empty to retain the
current setup. Original-container support requires an SDK built with
`-DFM4_DLC_CONTAINERS=ON`; this build option defaults to OFF pending integration.

The launcher copies validated FM4 containers into the selected private profile.
It keeps the originals untouched and archives replaced copies or extracted
package links under `.local/archive/dlc-packages-*`. The runtime
mounts containers read-only and reads each package's enabled universal or
matching-profile license records. Console-bound records need a console identity
and are not granted by this implementation. Extracted copies of the same
publisher are skipped, as are duplicate original publisher packages with equal
grants. Avatar items stay in their own content type and do not become car packs.

Preview without installing or launching:

```powershell
./tools/Run-Enhanced.ps1 -ValidateOnly
python tools/Dlc-Packages.py --source "D:/My FM4/4D530910" --output originals.json
```

The original inventory includes content IDs, titles, all nonempty license
records and header hashes. Add `--hash-payloads` to hash complete containers.
Marketplace catalog offer types and account balances are not encoded in these
headers. No content or license record is created for an absent package.

Header format checks do not authenticate a Marketplace purchase or verify package signatures.

Set `dlc_folder` to your extracted FM4 content folder. The launcher links its
packages into the selected profile. Keep the source files in place.

A mounted package is not necessarily unlocked. Extracted assets can include a
whole car pack while its original license covered a sampler or individual car.
Blank `.puboffer` files and empty `LicenseMasks` files cannot recover ownership.
Do not use the global `license_mask` setting to grant every pack.

Create a read-only inventory with:

```powershell
python tools/Dlc-Inventory.py "D:/My FM4 DLC" --output inventory.json
```

The report separates package folders, publisher-offer IDs, required license bits,
car/event data and unknown entitlements. It never edits the source or unlocks cars.

If you retained exported runtime `.header` files from your original installation,
preview their import while the game is closed:

```powershell
./tools/Import-DlcHeaders.ps1 -SourceHeaders "D:/My FM4 Headers" -UserDataRoot ".local/user"
```

Add `-Apply` to import. Only headers for installed FM4 marketplace packages are
accepted. A header must contain its explicit package license mask; the importer
preserves those bytes and backs up any replaced header in
`.local/archive/dlc-headers-*`. Headers are metadata exports, not a
cryptographic proof of purchase; supply your own original records.

For automatic restoration, optionally set `dlc_headers_folder` in your settings
to that export folder. Without this setting, existing launch behavior is unchanged.
`-ValidateOnly` previews the header import without applying it.

This does not create VIP membership, tokens, online Rivals events or Marketplace
purchases. Original console title updates also require compatible executable
support; copying update data into the DLC folder does not update the executable.

With a game build containing local VIP support, Content > **I own the VIP pass** enables your owned membership for local VIP checks (off by default); car packs still require their own installed packages and licenses.
For verification, the default-off `fm4_content_availability_log` diagnostic logs installed, entitled and visible states through the game's showroom predicates after the profile loads; these states do not add cars to your garage or restore online services.
