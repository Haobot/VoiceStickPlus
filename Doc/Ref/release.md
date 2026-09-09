# VoiceStick Release Process

VoiceStick releases have three moving parts:

- macOS app: built, signed, notarized, and uploaded by GitHub Actions. (The macOS job is currently disabled in the CI pipeline; only the Windows package and firmware are published for v2.3.6.)
- StickS3 firmware: built by GitHub Actions and uploaded to GitHub Releases (as release assets; the firmware manifest is served from COS `dl.davenger.cloud/firmware/latest/manifest.json` with the GitHub `releases/latest/download/manifest.json` kept as client fallback).
- Windows app: built and signed manually on the Windows signing machine, then uploaded to the matching GitHub Release.

Domestic distribution: every release additionally mirrors assets to Tencent COS (`dl.davenger.cloud`) — firmware at `firmware/v<tag>/`, MSIs at `windows/v<tag>/`, plus the whole website at the bucket root. GitHub stays the source of truth; GitHub Pages remains the international/backup site. See `Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md`. COS writes happen only in GitHub Actions with repo secrets `TENCENT_COS_SECRET_ID` / `TENCENT_COS_SECRET_KEY`; the signing machine keeps talking to GitHub only.

The Windows package is the special case because the signing certificate is local hardware or local machine state. The release process supports either order:

- Build and sign Windows first, then let GitHub Actions publish macOS and firmware.
- Publish macOS and firmware first, then build/sign Windows and upload it afterward.

In both cases, finish by redeploying the website and verifying all update URLs.

## One-Command Release (Windows signing machine)

The whole flow above is scripted for the Windows signing machine:

```bat
powershell -File scripts\release.ps1 -Version 2.3.9
```

The script syncs `VERSION` / `firmware/version.txt`, builds and signs the MSI, commits and pushes the `v<version>` tag, waits for the release workflow, uploads both MSIs with `.sha256` checksums, triggers the website deploy, and verifies every update URL — GitHub and COS alike (appcast, `manifest.json` incl. `min_version`, `downloads.json`, firmware and MSI assets on both `dl.davenger.cloud` and GitHub). Use `-SkipMsi` for firmware/website-only releases and `-DryRun` to print the steps without executing anything. It refuses to run off `main` or with a dirty tree.

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

Pushing the tag runs `.github/workflows/release.yml`. That workflow builds:

- `voicestick-firmware-sticks3-ota-<version>.bin`
- `voicestick-firmware-sticks3-merged-<version>.bin`
- firmware checksums and `manifest.json`

The firmware assets and `manifest.json` are uploaded to the GitHub Release (no Aliyun OSS upload; the CI no longer carries OSS credentials).

After publishing the GitHub Release, the workflow requests a website deploy so the appcast is refreshed.

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
https://dl.davenger.cloud/windows/v2.3.10/VoiceStick_2.3.10_en-US.msi
https://dl.davenger.cloud/macos/v2.3.10/VoiceStick-2.3.10.zip
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
