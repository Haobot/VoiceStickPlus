# VoiceStick Release Process

VoiceStick releases have three moving parts:

- macOS app: built, signed, notarized, and uploaded by GitHub Actions. (The macOS job is currently disabled in the CI pipeline; only the Windows package and firmware are published for v2.3.6.)
- StickS3 firmware: built on the Windows signing machine (`python scripts/idf_cli.py -c --merge`), so the binary that was verified on a real device is the binary that gets published. The one-command script uploads it to the GitHub Release and COS.
- Windows app: built and signed manually on the Windows signing machine, then uploaded to the matching GitHub Release.

Domestic distribution: every release additionally publishes assets to Tencent COS (`dl.davenger.cloud`) — firmware at `firmware/v<tag>/`, MSIs at `software/windows/v<tag>/`, plus the whole website at the bucket root. GitHub stays the source of truth; GitHub Pages remains the international/backup site. See `Doc/Ref/cos-distribution.md` (channel conventions) and `Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md`. COS writes happen on the Windows signing machine only (`scripts/publish_cos.py`; credentials from `TENCENT_COS_*` env vars, `TENCENTCLOUD_*` accepted as fallback): GitHub Actions runners are overseas and the cross-border uplink to the Shanghai bucket stalls at ~8 KB/s with ~130 s connection cuts, which kills every multipart upload (>=1 MB body) — diagnosed 2026-09-26, see `Doc/Ref/cos-distribution.md` item 3. The CI-side COS steps were removed accordingly.

The Windows package is the special case because the signing certificate is local hardware or local machine state. The release process supports either order:

- Build and sign Windows first, then let GitHub Actions publish macOS and firmware.
- Publish macOS and firmware first, then build/sign Windows and upload it afterward.

In both cases, finish by redeploying the website and verifying all update URLs.

## macOS-only Release Round (unattended CI rail, from the Mac)

For rounds that ship macOS changes only (Windows MSI and firmware unchanged functionally; firmware is still rebuilt because `firmware/version.txt` is version-inlined). Execution machine = a Mac with SSH push access but **no gh PAT / no COS credentials** — the CI acts as the publishing proxy (`.github/workflows/publish-mac.yml`):

1. Package on the Mac: `SPARKLE_PRIVATE_ED_KEY="$(cat ~/.voicestick-sign/sparkle/ed-private-key)" VOICESTICK_ARCHS=arm64 scripts/build-macos.sh --release` (needs proxy env — separate scratch path re-fetches SwiftPM deps) + `scripts/make-dmg.sh` + `shasum -a 256` for zip/dmg. Verify `codesign --verify --deep --strict` passed and the `.signature` file is non-empty. **Internal-test gate (v2.4.8+)**: the build log must show `Injecting built-in credentials into VoiceStickApp` (source: `~/Library/Application Support/VoiceStick/config.toml`; `VOICESTICK_EMBED_BUILTIN_KEYS=0` for a public build); after the build, `BuiltinSecrets.swift` must be restored to placeholders (trap) — check `git status` is clean for it; the packaged zip/dmg then carry the builtin credentials (scan with `--allow-builtin`).
2. Commit + push `feat/stick-gateway`, then push tag `v<version>` (triggers `release.yml` firmware verification gate; CI `ci.yml` runs mac/win/firmware builds + tests on the branch push). Poll both to **completed success** (anonymous `api.github.com` works from the Mac with the proxy).
3. Prepare branch `ci/publish-stage-v<version>` from the release branch containing `dist/` (mac zip + `.signature` + `.sha256` + dmg + dmg sha256) and `RELEASE_NOTES.md`; push it. The stage job creates/updates the GitHub Release (mac assets + firmware assets pulled from the release.yml artifact, P0-4 scan with `--allow-builtin`) and commits `cos-urls.json` (presigned PUT URLs, 2h TTL) back to the branch.
4. On the Mac: fetch the branch, PUT each listed local file to its presigned URL (Mac is domestic → COS ap-shanghai uplink is fast), verify with `HEAD` + sha256.
5. Push marker branch `ci/publish-finalize-v<version>` (contains file `FINALIZE`). The finalize job refreshes appcast (macOS item added; Windows item keeps the last MSI release), regenerates `downloads.json`, syncs firmware to Pages, deploys Pages, then builds the site with `--base=/` and commits `cos-site-urls.json` (whole-site PUT list + stale `firmware/latest/` bin DELETE list).
6. On the Mac: PUT all site files, DELETE the stale keys, run the final verification checklist (appcast version + mac enclosure on COS, `software/macos/v<tag>/` HEAD 200, `[^"']*assets/` grep without `/VoiceStickPlus/` prefix, `firmware/latest/manifest.json` version), then delete both `ci/publish-*` branches (SSH `git push origin --delete`).

Notes: presigned URLs cover fixed keys only; branches are deleted after use. The Sparkle EdDSA keypair and self-signed code-signing identity live only in `~/.voicestick-sign/` on the Mac (identity also in the login keychain); the matching public key is pinned in `Info.plist` `SUPublicEDKey` (rotated 2026-10-02 — no installed 2.x Mac base, so rotation was free). First open of a self-signed, non-notarized build requires right-click → Open.

## One-Command Release (Windows signing machine)

The whole flow above is scripted for the Windows signing machine:

```bat
powershell -File scripts\release.ps1 -Version 2.3.9
```

The script syncs `VERSION` / `firmware/version.txt`, builds and signs the MSI, builds the firmware locally (`idf_cli.py -c --merge`), commits and pushes the `v<version>` tag, waits for the `release.yml` build-verification gate, creates the GitHub Release and uploads all assets (firmware bins + checksums + `manifest.json` + both MSIs, all gated by the P0-4 credential scan), triggers the website deploy (Pages), then pushes everything to COS from the local machine (`scripts/publish_cos.py`: firmware, MSIs, appcast/downloads.json mirrored from Pages, whole website) and verifies every update URL — GitHub and COS alike (appcast, `manifest.json` incl. `min_version`, `downloads.json`, firmware and MSI assets on both `dl.davenger.cloud` and GitHub). Use `-SkipMsi` / `-SkipFirmware` for partial releases, `-SkipCos` when the machine has no COS credentials (GitHub channel only), and `-DryRun` to print the steps without executing anything. It refuses to run off `main` or with a dirty tree.

Note: `release.ps1` creates the Release with `--generate-notes`, which only yields a compare link. After releasing, hand-edit the release notes to user-facing specifics (Chinese, categorized bullets — see the v2.4.2 / v2.4.4 releases for style), then re-run `deploy-website.yml` and `publish_cos.py pages-mirror`: the appcast `release-notes` field is generated from the Release body, so WinSparkle's update dialog shows whatever the notes say at deploy time.

Note: with `-SkipFirmware` (software-only release) the new Release carries no `manifest.json`, so `releases/latest/download/manifest.json` resolves to the newest Release without it; the COS stable address `dl.davenger.cloud/firmware/latest/manifest.json` keeps serving the previous firmware's manifest, which remains correct.

`FIRMWARE_MIN_VERSION` (repo root) feeds the manifest `min_version` field: devices below it get a mandatory upgrade prompt. Bump it manually when the desktop protocol drops compatibility with older firmware; it must never exceed `VERSION`.

The deploy workflow also regenerates `website/public/downloads.json` (via `scripts/update-downloads.py`) from the latest GitHub Releases; the website download page reads it at runtime.

The manual flows below remain the reference for each individual step.

## Version Sources

Update both version files before creating the release tag:

```text
VERSION
firmware/version.txt
```

`VERSION` is used by the desktop packaging scripts and the GitHub release workflow. `firmware/version.txt` is the firmware version reported by the device, so it must match the release version for OTA update detection to work correctly.

For release `2.3.6`, the tag must be:

```text
v2.3.6
```

The GitHub Actions release workflow validates that `v<VERSION>` matches the pushed tag.

## Standard Flow

1. Update `VERSION` and `firmware/version.txt` to the new version.
2. Commit the version change and any release workflow changes.
3. Push `main`.
4. Push the release tag:

```sh
git tag -a v2.3.6 -m "VoiceStick 2.3.6"
git push origin main
git push origin v2.3.6
```

Pushing the tag runs `.github/workflows/release.yml`, which is now a build-verification gate only: it validates the tag against `VERSION` and cross-compiles the firmware (Espressif CI action; bins, checksums, and `manifest.json` are retained as run artifacts). It does **not** publish the Release or write to COS — `scripts/release.ps1` owns both.

## Windows First

Use this flow when the Windows package has already been built and signed before the macOS/firmware release.

1. Set the new version in `VERSION`.
2. On the Windows signing machine, build and sign the MSI:

```bat
scripts\build-msi.bat
```

The output is (two language versions; WiX 4.0 builds one culture per pass, so the script loops over `zh-CN` and `en-US`):

```text
desktop\windows\build-msi-x64\VoiceStick_<version>_zh-CN.msi
desktop\windows\build-msi-x64\VoiceStick_<version>_en-US.msi
```

The MSI also installs the COM flash tool `VoiceStickFlash.exe` (BLE OTA fallback path) and its self-contained esptool runtime under `INSTALLFOLDER\FlashTool\` (embedded Python + esptool, prepared by `scripts/prepare_flash_payload.ps1`, which `build-msi.bat` invokes automatically; override the embeddable-Python download with `VOICESTICK_PYTHON_EMBED_URL`). See `Doc/Plan/windows-com-flash-tool.md`.

After signing, `build-msi.bat` runs a content gate (Step 4b, `scripts/verify_msi_contents.ps1`): it enumerates each MSI's `File` table and fails the build if any required top-level file is missing (`VoiceStick.exe`, `WinSparkle.dll`, `config.template.toml`, `VoiceStickFlash.exe`, `VoiceStickHidTap.dll`, `VoiceStickTapInject.exe`, `sherpa-onnx-c-api.dll`, `onnxruntime.dll`). When you add a new top-level packaged file to `installer/VoiceStick.wxs`, update `-RequiredFiles` in that script too — v2.4.1 shipped without the sherpa-onnx DLLs because the hand-maintained packaging list had no gate. See `Doc/Expe/msi-missing-sherpa-dll-defender-sign-flake-2026-09-27.md`.

Installer localization lives in `desktop/windows/installer/` (`zh-CN.wxl` / `en-US.wxl` plus `license-zh-CN.rtf` / `license-en.rtf`). The install-time UI (wizard dialogs, uninstall confirm, start-menu shortcut name for the flash tool) follows the MSI's culture. **WinSparkle 0.9.2 does not support per-language enclosure selection** (`sparkle:language` is not implemented), so the appcast only lists the `en-US` MSI; the `zh-CN` MSI is for manual download only.

3. Confirm `firmware/version.txt` also matches the new version.
4. Commit, push `main`, and push the matching `v<version>` tag.
5. Wait for the release workflow to finish successfully.
6. Upload both signed MSIs to the same GitHub Release:

```sh
gh release upload v2.3.6 desktop/windows/build-msi-x64/VoiceStick_2.3.6_zh-CN.msi --repo Haobot/VoiceStickPlus
gh release upload v2.3.6 desktop/windows/build-msi-x64/VoiceStick_2.3.6_en-US.msi --repo Haobot/VoiceStickPlus
```

7. Re-run the website deploy workflow so the appcast includes the Windows MSI (`en-US` only):

```sh
gh workflow run deploy-website.yml --repo Haobot/VoiceStickPlus --ref main
```

## macOS and Firmware First

Use this flow when macOS and firmware should be published before the Windows package is ready.

1. Update `VERSION` and `firmware/version.txt`.
2. Commit, push `main`, and push the matching `v<version>` tag.
3. Wait for the release workflow to publish macOS and firmware.
4. Later, on the Windows signing machine, build and sign the MSI:

```bat
scripts\build-msi.bat
```

5. Upload both signed MSIs to the already published GitHub Release:

```sh
gh release upload v2.3.6 desktop/windows/build-msi-x64/VoiceStick_2.3.6_zh-CN.msi --repo Haobot/VoiceStickPlus
gh release upload v2.3.6 desktop/windows/build-msi-x64/VoiceStick_2.3.6_en-US.msi --repo Haobot/VoiceStickPlus
```

6. Re-run the website deploy workflow:

```sh
gh workflow run deploy-website.yml --repo Haobot/VoiceStickPlus --ref main
```

Until the MSI is uploaded and the website deploy has run, Windows clients will not see the new Windows update in the appcast.

## Verification

After every release, verify the appcast, firmware manifest, and actual package URLs.

Stable update endpoints:

```text
https://dl.davenger.cloud/appcast.xml
https://dl.davenger.cloud/downloads.json
https://dl.davenger.cloud/firmware/latest/manifest.json
https://haobot.github.io/VoiceStickPlus/appcast.xml            # Pages backup site
https://github.com/Haobot/VoiceStickPlus/releases/latest/download/manifest.json   # client fallback
```

For version `2.3.10`, the appcast should contain (Windows enclosure points to the `en-US` MSI only; enclosure URLs point to the COS mirror):

```text
https://dl.davenger.cloud/software/windows/v2.3.10/VoiceStick_2.3.10_en-US.msi
https://dl.davenger.cloud/software/macos/v2.3.10/VoiceStick-2.3.10.zip
```

The `zh-CN` MSI (`VoiceStick_2.3.10_zh-CN.msi`) is mirrored for manual download but is not in the appcast.

The firmware manifest should contain (primary URLs on COS, `*_fallback` fields on the GitHub Release):

```text
https://dl.davenger.cloud/firmware/v2.3.10/voicestick-firmware-sticks3-ota-2.3.10.bin
https://dl.davenger.cloud/firmware/v2.3.10/voicestick-firmware-sticks3-merged-2.3.10.bin
```

Use `HEAD` requests or a browser to confirm every URL returns `200`.

```powershell
Invoke-WebRequest -UseBasicParsing https://haobot.github.io/VoiceStickPlus/appcast.xml
Invoke-WebRequest -UseBasicParsing https://github.com/Haobot/VoiceStickPlus/releases/latest/download/manifest.json

Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.6/VoiceStick_2.3.6_en-US.msi
Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.6/VoiceStick-2.3.6.zip
Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.6/voicestick-firmware-sticks3-ota-2.3.6.bin
Invoke-WebRequest -UseBasicParsing -Method Head https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.6/voicestick-firmware-sticks3-merged-2.3.6.bin
```

Also confirm these workflow runs are successful:

- `Release Build`
- `Deploy Website to GitHub Pages`

The release is complete when:

- macOS appcast entry points to the new Sparkle ZIP.
- Windows appcast entry points to the new signed MSI.
- firmware `latest/manifest.json` reports the new version.
- OTA and merged firmware URLs are reachable.
- the GitHub Release contains all macOS, Windows, and firmware assets.
