# VoiceStick Website

Vue + Vite source for the VoiceStick homepage and Sparkle/WinSparkle appcast. The site uses
`vue-i18n` for Simplified Chinese and English, and picks Chinese automatically
when the browser language starts with `zh`.

Suggested GitHub Pages URL for this repository:

```text
https://haobot.github.io/VoiceStickPlus/
```

The macOS and Windows apps check the generated root-level appcast:

```text
https://haobot.github.io/VoiceStickPlus/appcast.xml
```

## Release Flow

1. Generate Sparkle keys once and keep the private key out of git:

   ```bash
   cd desktop/macos
   swift package resolve
   find .build -name generate_keys -type f -perm -111 -print -quit
   .build/artifacts/sparkle/Sparkle/bin/generate_keys --account voicestick
   ```

2. Put the public key into the app before a release build:

   ```bash
   SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
   ```

3. Create the DMG:

   ```bash
   scripts/make-dmg.sh
   ```

4. Upload these files to GitHub Release `v<version>`:

   ```text
   build/VoiceStick-<version>.dmg
   build/VoiceStick-<version>.zip
   ```

5. Update `website/public/appcast.xml`:

   - `url`: GitHub Release URL for the ZIP, not the DMG
   - `sparkle:edSignature`: content from `build/VoiceStick-<version>.signature`
   - `length`: byte size from `wc -c build/VoiceStick-<version>.zip`

The homepage download section links directly to the current versioned macOS DMG and Windows MSI. The version number is read at build time from the repository root `VERSION` file (single source of truth), not `website/package.json`. The browser flasher reads the firmware manifest from `VITE_FIRMWARE_MANIFEST_URL` and uses `merged_url`; if the manifest cannot be loaded, it falls back to the versioned merged firmware URL derived from the same `VERSION` file.

## Develop

```bash
cd website
npm install
npm run dev
```

## Build

```bash
cd website
npm run build
```

The generated `dist/` directory is deployed to GitHub Pages. `public/appcast.xml` is copied to `dist/appcast.xml`. The firmware manifest itself is hosted on the GitHub Release (`releases/latest/download/manifest.json`), not GitHub Pages or OSS.

## GitHub Actions

- `.github/workflows/release.yml` runs on `v*` tags or manual dispatch. It builds versioned OTA and merged firmware images, uploads the firmware assets and `manifest.json` to the matching GitHub Release, then triggers `deploy-website.yml`. (macOS is not built in the current CI pipeline; the macOS job is disabled for the v2.3.6 release.)
- `scripts/build-msi.bat` runs on the local Windows signing machine with the USB signing key inserted. Upload the generated `VoiceStick_<version>.msi` to the matching GitHub Release.
- `.github/workflows/deploy-website.yml` runs when `website/**` changes on `main` or when manually dispatched after uploading a Windows MSI. Before deploying, it reads the current live appcast and latest GitHub Release, then regenerates `public/appcast.xml` from the latest ZIP/signature and optional MSI assets. If the latest Release has no Windows MSI yet, the previous Windows item is preserved.

Required repository secrets for release builds:

```text
SPARKLE_PUBLIC_ED_KEY
SPARKLE_PRIVATE_ED_KEY
MACOS_CERTIFICATE_P12
MACOS_CERTIFICATE_PASSWORD
APPLE_ID
APPLE_TEAM_ID
APPLE_APP_SPECIFIC_PASSWORD
```
