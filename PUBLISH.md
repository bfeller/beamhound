# Application Store Publish Record

Portal: <https://dev.cardputer.cc/#/upload>

## Package

- Debian package: `beamhound_<version>_arm64.deb` from the GitHub Release of the version tag (built by CI)
- Package: `beamhound`
- Current version: `0.2.2`
- Architecture: `arm64`
- Maintainer: `Bryan Feller <6282600+bfeller@users.noreply.github.com>`
- APPLaunch desktop path: `/usr/share/APPLaunch/applications/beamhound.desktop`
- Executable path: `/usr/share/APPLaunch/bin/beamhound`
- Depends: `libc6 (>= 2.34), curl, tar`

## Source and Ownership

- Public source repository: https://github.com/bfeller/beamhound
- Expected GitHub uploader/owner: `bfeller`
- Hide uploader email: `true`

## Listing

- Application name: Beamhound
- One-line summary (maximum 80 characters): Sniffs out your IR remote from one button press and becomes it.
- Description: see `store.description` in `app-builder.json`
- Categories (maximum 6): Hardware & IoT, System Tools
- Share code: `BEAM`
- License: MIT
- Permissions: network (first-run database download); everything else false

## Assets

- 256 x 256 source icon: `assets/images/beamhound_256.png` (from `assets/images/beamhound.svg`)
- Icon packaged in `.deb`: `usr/share/APPLaunch/share/images/beamhound.png` (100 x 100)
- Screenshot 1 (320 x 170): `store/screenshots/1-sniff.png`
- Screenshot 2 (320 x 170): `store/screenshots/2-questions.png`
- Screenshot 3 (320 x 170): `store/screenshots/3-found.png`
- Screenshot 4 (320 x 170): `store/screenshots/4-remote.png`
- Screenshot 5 (320 x 170): `store/screenshots/5-code-search.png`

## Preflight

- [x] `.deb` exists and can be parsed.
- [x] Control fields contain Package, Version, Architecture, and Maintainer.
- [x] Architecture is appropriate for the package (`arm64` for native binaries).
- [x] APPLaunch `.desktop`, executable, and PNG icon exist inside the package.
- [x] Package name ownership and update version are valid (portal accepted the upload).
- [x] Listing copy and categories have been reviewed.
- [x] Four clean 320 x 170 baseline screenshots exist.
- [x] No credentials, tokens, or private server details are embedded.
- [x] `prepublish_check.py` passed (2026-11-22).
- [x] The user explicitly approved external submission (submitted by the user via the portal).

## Notes

- IR codes are not shipped in the package. The app downloads Flipper-IRDB commit
  `d126fb1b6f1e114c52b4a8c19839ea65e3a9c24d` on first run (about 17 MB; about 1 minute
  including indexing on the device).

## Submission Results

### 0.1.0

- Status: `submitted, awaiting maintainer review` (first release requires a demo video in the PR)
- Submitted at: 2026-11-23T01:36:08Z
- Portal message: All automated checks passed. First release of a new app: post a short video of the app
  running on a real device in the PR; a maintainer reviews and merges. Later updates merge automatically.
- Package SHA-256: `a436b61801e113347bf00cf0acf218bf39bc2909cda11b57e5e440c2a14ce993` (matches local build)
- Actions URL: https://github.com/CardputerZero/packages/actions/runs/45095456623
- Tracking or pull-request URL: https://github.com/CardputerZero/packages/pull/685

### 0.2.0 (brand code search for users without the original remote)

- Status: `merged` (updates merge automatically once the store checks pass)
- Submitted and merged at: 2026-11-27T03:11Z
- Package SHA-256: `d5bccfec8e20edfe5ca454200d364806e51f15cea82394103c415816f9931f16` (the GitHub Release asset)
- Actions URL: https://github.com/CardputerZero/packages/actions/runs/45932666829
- Pull-request URL: https://github.com/CardputerZero/packages/pull/936

### 0.2.1 (brand aliases folded; installed apps re-index on update) - first CI submission

- Status: `merged` (submitted by the `store` CI job with `czdev publish`, no portal upload)
- Submitted at: 2026-11-27T03:42Z, merged at 2026-11-27T03:43:09Z
- Package SHA-256: `8da2c500a9624ce766beb003651dd482f41560667d4e08b30bd754daa58b99e6` (the GitHub Release asset)
- Uploaded via: `bfeller/packages` release `czdev-buffer`; uploader email recorded as the noreply address
- CI run: https://github.com/bfeller/beamhound/actions/runs/45935569644
- Pull-request URL: https://github.com/CardputerZero/packages/pull/937

### 0.2.2 (placeholder brands folded into Unknown; builder rev 3)

- Status: `merged` (submitted by the `store` CI job)
- Merged at: 2026-11-27T04:00:25Z
- Package SHA-256: `51a81765fc0d1f12fe9aa972a75dadc9b99de36f05be407bbebde6ecb54d4824` (the GitHub Release asset)
- CI run: https://github.com/bfeller/beamhound/actions/runs/45937111913
- Pull-request URL: https://github.com/CardputerZero/packages/pull/939
