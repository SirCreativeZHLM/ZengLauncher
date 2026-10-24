# ZengLauncher security review

Review date: 2026-10-24
Source base: FreesmTeam/FreesmLauncher `develop`, commit `163424fc202e451f05ca360ef431209664691137`
Scope: focused source review of authentication, archive extraction, network behavior, updater launch, process arguments, and CI configuration. This is not a formal audit or malware-free certification.

## Identity and endpoint changes

- Product identity changed to `ZengLauncher`, binary `zenglauncher`, environment prefix `ZENGLAUNCHER`, and bundle ID `com.zengbot.zenglauncher`.
- Product help points to `https://zengbot.com/mc/`.
- Freesm RSS and Sparkle update feeds, Freesm updater repository, Freesm Discord client ID, and Freesm Ely.by client ID are disabled/cleared. A Zeng release repository is not configured; updates remain unavailable until a trusted Zeng release feed and signing key exist.
- Minecraft metadata uses Prism Launcher's upstream metadata service and translation files. This is an upstream service dependency, not a Zeng-owned endpoint.
- Copyright headers, GPL notices, and upstream Freesm/Prism/PolyMC/MultiMC provenance are retained.

## Findings

### High — Archive extraction could traverse symlink chains

**Evidence:** `launcher/archive/ArchiveReader.cpp`, shared `willEscapeRoot()` check. The prior check assessed an archive symlink's immediate target; a link chain or a pre-existing symlink in the destination could still redirect later writes outside the intended directory.

**Disposition:** Updated the rooted disk-extraction path to reject archive symlinks/hardlinks and any output path containing an existing symlink component. It also requires the cleaned output path to remain below the target root (allowing the root directory entry itself). This intentionally means archives that depend on symlinks/hardlinks cannot be extracted through these rooted extraction APIs.

**Verification limitation:** The changed code compiled in the application build; no archive exploit regression test was run, per the agreed no-test-suite constraint. Review this change with targeted malicious-archive tests before release.

### Medium — Account tokens are serialized in account JSON

**Evidence:** `launcher/minecraft/auth/AccountData.cpp` serializes persistent access and refresh tokens as JSON fields; `launcher/minecraft/auth/AccountList.cpp` writes account JSON to its configured file path.

**Disposition:** Existing upstream-compatible behavior retained to avoid breaking stored sessions. Protect the account data directory with OS account permissions and disk encryption. Keychain-backed token storage is not implemented by this change.

### Low — CI contains mutable third-party action references

**Evidence:** `.github/workflows/container.yml` references `redhat-actions/podman-install@main`; multiple build workflows use third-party actions by version tags instead of immutable commit digests.

**Disposition:** Recorded for follow-up. The Freesm Cachix and Winget integrations are gated to the upstream repository. Other release/signing workflows still need Zeng repository identities and secrets configured before use. Do not enable publishing until those identities and secrets are reviewed for ZengLauncher.

### Reviewed — Account ownership and offline launch

`launcher/ui/pages/global/AccountListPage.cpp` now requires `anyAccountIsValid()` before adding an offline account. `launcher/LaunchController.cpp` retains the existing account-state validation and offline fallback path. No authentication or entitlement bypass was introduced. Online account tokens remain subject to the storage limitation above.

### Reviewed — Network and subprocess behavior

`launcher/net/NetRequest.cpp` handles TLS errors by logging them and leaving the request failed; the reviewed code does not call `ignoreSslErrors()`. `launcher/updater/PrismExternalUpdater.cpp` constructs a fixed updater path under the application directory and passes arguments as a `QStringList`, rather than interpolating them into a shell command. Product updater endpoints are disabled in this build until Zeng signing/update infrastructure is configured.

## Verification and scan evidence

Artifact: `/tmp/FreesmLauncher-zenglauncher-rebrand/build/Debug/zenglauncher.app`; main executable is arm64 Mach-O. Bundle metadata is `com.zengbot.zenglauncher`, `ZengLauncher`, executable `zenglauncher`. `codesign --verify --deep --strict` succeeded after applying an ad-hoc signature. `spctl --assess --type execute --verbose=4` rejected the ad-hoc build (no Developer ID signature/notarization). Distribution requires signing and notarization.

- Build: `JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home VCPKG_ROOT=/Users/zhlm/vcpkg cmake --fresh --preset macos`, then `cmake --build --preset macos --parallel` — completed successfully. The first link attempt caught a stale Qt resource initializer; it was corrected and the incremental build passed. No unit tests were run.
- ClamAV: version 1.5.4; final scan at 2026-10-24 21:04:14–21:04:20 Asia/Singapore; scanned the `.app` recursively; 3 files, 102.49 MiB, 0 infected. Definitions: daily 28163 (built 2026-10-24 06:26 UTC), main 63, bytecode 339.
- Malwarebytes: official notarized macOS package `5.28.4.4244` was downloaded from its official vendor endpoint and its Apple developer signature/notarization verified. A custom scan report at 21:03 showed 6 items scanned, 0 threats, 0 PUPs, no items detected. That report preceded the final incremental rebuild. I initiated a custom scan again after rebuilding at 21:04, but Malwarebytes stopped responding through the UI and I could not retrieve its final report; the final Malwarebytes result is therefore unverified. The app reports that whole-device scans require Full Disk Access; that permission was not granted.
- Gatekeeper: rejected the local ad-hoc build, as expected for a build without Developer ID signing and notarization. This is not a malware detection result.

These scan results apply only to this exact build and scanner definitions at the recorded time. They do not prove the source or future builds are malware-free. No public scanning service received the app or source.
