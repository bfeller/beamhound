# Releasing Beamhound

Releases are built by GitHub Actions from a version tag. Submitting to the CardputerZero
Application Store is a short manual step (the store's developer portal signs in with your
GitHub account in the browser, so no store credentials live in this repo).

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

## Cutting a release

```sh
tools/bump_version.sh 0.1.1            # updates app-builder.json (the only place the version lives)
make package && make deploy            # optional: build locally and try it on the Cardputer
git commit -am "Release v0.1.1"
git tag v0.1.1
git push && git push origin v0.1.1     # CI builds, tests and creates the GitHub Release
```

Then publish to the store:

1. Wait for the **build** workflow on the tag to go green; open the new release on GitHub and
   download `beamhound_0.1.1_arm64.deb`.
2. Upload it at <https://dev.cardputer.cc/#/upload> (signed in as `bfeller`), source repository
   `https://github.com/bfeller/beamhound`, keep "hide email" on.
3. The portal opens a PR on `CardputerZero/packages`. After the first release (v0.1.0) was merged,
   updates merge automatically once the store's checks pass.
4. Record the result (PR link, SHA-256) in `PUBLISH.md` and commit it.

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
