#!/usr/bin/env bash
set -euo pipefail
test "$(gh api "repos/$GITHUB_REPOSITORY" --jq .private)" = true
test "$(gh api "repos/$GITHUB_REPOSITORY/commits/$RELEASE_TAG" --jq .sha)" = "$SOURCE_COMMIT"
comparison=$(gh api "repos/$GITHUB_REPOSITORY/compare/$WORKFLOW_COMMIT...main" --jq .status)
case "$comparison" in identical|ahead) ;; *) exit 1 ;; esac
cd release
sha256sum --check release-sha256.txt
sha256sum --check askerdway-windows-x64.zip.sha256
jq -e --arg commit "$SOURCE_COMMIT" --arg workflow "$WORKFLOW_COMMIT" --arg run "$RUN_URL" \
  '.status=="clean" and .scan_exit_code==0 and .definition_update_exit_code==0 and
   .source_commit==$commit and .workflow_commit==$workflow and .run_url==$run' clamav-report.json
unzip -q askerdway-windows-x64.zip -d checked
(
  cd checked
  sha256sum --check SHA256SUMS.txt
)
test "$(sha256sum checked/askerdway.dll | cut -d ' ' -f 1)" = "$(jq -r .sha256 clamav-report.json)"
cmp clamav-report.json checked/clamav-report.json
cmp clamav.log checked/clamav.log

gh release upload "$RELEASE_TAG" --repo "$GITHUB_REPOSITORY" --clobber \
  askerdway-windows-x64.zip askerdway-windows-x64.zip.sha256 clamav-report.json clamav.log
expected_archive=$(sha256sum askerdway-windows-x64.zip | cut -d ' ' -f 1)
uploaded_archive=$(gh api "repos/$GITHUB_REPOSITORY/releases/tags/$RELEASE_TAG" \
  --jq '.assets[] | select(.name=="askerdway-windows-x64.zip") | .digest')
test "$uploaded_archive" = "sha256:$expected_archive"
{
  printf 'Windows x64 Release build of askerdway. A loader is not included.\n\n'
  printf 'Built from [%s](https://github.com/%s/commit/%s).\n\n' "$SOURCE_COMMIT" "$GITHUB_REPOSITORY" "$SOURCE_COMMIT"
  printf '[Build and scan report](%s) | [ClamAV report](https://github.com/%s/releases/download/%s/clamav-report.json)\n\n' "$RUN_URL" "$GITHUB_REPOSITORY" "$RELEASE_TAG"
  printf 'Scanner: %s\n\n' "$(jq -r .scanner clamav-report.json)"
  printf 'Scan finished (UTC): %s\n\n' "$(jq -r .finished_at_utc clamav-report.json)"
  printf 'DLL SHA256: `%s`\n\nArchive SHA256: `%s`\n\n' "$(jq -r .sha256 clamav-report.json)" "$expected_archive"
  printf 'ClamAV reported no detections with the recorded definitions. This is not a guarantee of safety.\n\n'
  printf 'Requires the Microsoft Visual C++ x64 runtime and a compatible Dota 2 client using Direct3D 11. License notices and scan evidence are included in the archive.\n'
} > "$RUNNER_TEMP/release-notes.txt"
gh release edit "$RELEASE_TAG" --repo "$GITHUB_REPOSITORY" --notes-file "$RUNNER_TEMP/release-notes.txt"
printf 'Published the checked archive: %s\n' "$expected_archive" >> "$GITHUB_STEP_SUMMARY"
