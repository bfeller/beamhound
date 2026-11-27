# Releasing Beamhound

Releases are built by GitHub Actions from a version tag, which also submits them to the
CardputerZero Application Store with the store's own `czdev publish` CLI. The only store
credential is the `CZDEV_TOKEN` secret in the repository's protected `store` environment.

## What CI does

`.github/workflows/build.yml` runs on every push to `master`, every pull request, and every
`v*` tag:

1. Fetches LVGL v9.5.0 and the pinned Flipper-IRDB snapshot (both cached).
2. Checks that the tag matches the version in `app-builder.json`, and that the workflow's
   `IRDB_COMMIT` matches `kIrdbCommit` in `src/app/setup.hpp`.
3. Runs the tests: decoder round trips plus the database consistency check (`test_core`),
   builds the index, and runs the matcher simulation (fails unless it is 100% correct).
4. Cross-compiles with Zig and builds `build/pkg/beamhound_<version>_arm64.deb`.
5. Runs the store's own `prepublish_check.py` against the package.
6. Uploads the `.deb` as a workflow artifact.
7. **On a tag only:** creates a GitHub Release named after the tag with the `.deb` attached.
8. **On a tag only** (job `store`, environment `store`): runs `czdev publish` (CardputerZero/AppBuilder
   at the pinned `CZDEV_COMMIT`) on that same `.deb`. czdev uploads it to a release on
   `bfeller/packages` (the owner's fork of `CardputerZero/packages`) and opens the publish PR;
   updates merge automatically once the store's checks pass. The job summary shows the PR link.
   CI patches czdev to record the noreply address as the uploader email (the profile email is
   public, and czdev would otherwise copy it into the public manifest); the job stops if a
   newer czdev no longer matches that patch.

## Cutting a release

```sh
tools/bump_version.sh 0.1.1            # updates app-builder.json (the only place the version lives)
make package && make deploy            # optional: build locally and try it on the Cardputer
git commit -am "Release v0.1.1"
git tag v0.1.1
git push && git push origin v0.1.1     # CI builds, tests and creates the GitHub Release
```

CI then publishes to the store by itself. When the **build** workflow on the tag is green,
take the PR link from the `store` job summary and record it (with the SHA-256) in `PUBLISH.md`.

Manual fallback (if the `store` job fails, e.g. an expired token): download the `.deb` from the
GitHub Release and upload it at <https://dev.cardputer.cc/#/upload> (signed in as `bfeller`),
source repository `https://github.com/bfeller/beamhound`, keep "hide email" on.

### Store token (one-time setup, and on expiry)

1. Create a **classic** personal access token as `bfeller` at
   <https://github.com/settings/tokens/new> with only the `public_repo` scope and an expiry
   (fine-grained tokens can't open PRs on another organisation's repository).
2. Add it as secret `CZDEV_TOKEN` in Settings → Environments → `store` (not as a repository
   secret). The environment only admits `v*` tags, so branch and pull-request builds can't
   read it.

The store rejects a version that is not newer than the published one, so always bump before
tagging. If CI fails on a tag, fix the problem, delete the tag
(`git tag -d v0.1.1 && git push origin :refs/tags/v0.1.1`), and tag again.

## Changing store listing details

The listing (summary, description, categories, screenshots, share code, permissions) comes from
the `store` section of `app-builder.json`; the portal imports it from the source repository.
Screenshots live in `store/screenshots/` and must be exactly 320 x 170; regenerate them with the
desktop preview build (`make desktop`, then `./build/desktop/beamhound --demo <dir>`).

## Updating the IR database snapshot

The app downloads a pinned Flipper-IRDB commit on first run. To move to a newer one, change
`kIrdbCommit` in `src/app/setup.hpp` **and** `IRDB_COMMIT` in the workflow, let CI confirm the
tests still pass, and release. Existing installs keep their current database (setup only runs
when no index exists).

## Changing the index builder

Installed apps build `index.bin` on the device, so a builder change (brand folding, categories,
signal handling) would otherwise only reach new installs. Bump `kBuilderRev` in
`src/core/builder.hpp` with any such change: on the next launch the app sees the older revision
and rebuilds its index from the database already on the device (about a minute, no download;
if that fails it keeps the old index and retries next launch). Check it with
`BEAMHOUND_DATA=<copy of a data dir with an older index.bin> ./build/desktop/beamhound --update-test <dir>`.
