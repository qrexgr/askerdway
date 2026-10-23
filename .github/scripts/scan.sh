#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence release
expected=$(jq -er '.dll_sha256' candidate/build-info.json)
actual=$(sha256sum candidate/askerdway.dll | cut -d ' ' -f 1)
test "$actual" = "$expected"
test "$(jq -r '.source_commit' candidate/build-info.json)" = "$SOURCE_COMMIT"
test "$(jq -r '.workflow_commit' candidate/build-info.json)" = "$WORKFLOW_COMMIT"
test "$(jq -r '.run_url' candidate/build-info.json)" = "$RUN_URL"

database=/var/lib/clamav
if systemctl is-active --quiet clamav-freshclam; then
  sudo systemctl stop clamav-freshclam
fi
started=$(date -u +%FT%TZ)
set +e
sudo freshclam --stdout 2>&1 | tee evidence/freshclam.log
update_exit=${PIPESTATUS[0]}
scan_exit=2
if test "$update_exit" -eq 0; then
  clamscan --database="$database" --scan-pe=yes --scan-archive=yes \
    --max-filesize=512M --max-scansize=1G --alert-exceeds-max=yes \
    --log=evidence/clamav.log candidate/askerdway.dll
  scan_exit=$?
fi
set -e
finished=$(date -u +%FT%TZ)
version=$(clamscan --database="$database" --version 2>&1 || true)
status=error
if test "$update_exit" -eq 0 && test "$scan_exit" -eq 0 &&
   grep -Eq '^Scanned files:[[:space:]]+1$' evidence/clamav.log &&
   grep -Eq '^Infected files:[[:space:]]+0$' evidence/clamav.log; then status=clean; fi
if test "$scan_exit" -eq 1; then status=detected; fi
test -f evidence/clamav.log || printf 'Scan was not completed. Definition update exit: %s\n' "$update_exit" > evidence/clamav.log
cp candidate/build-info.json evidence/build-info.json
for file in "$database"/*.cvd "$database"/*.cld; do
  test -f "$file" || continue
  sigtool --info "$file" >> evidence/signatures.txt
done
jq -n --arg status "$status" --arg sha "$actual" --arg source "$SOURCE_COMMIT" \
  --arg workflow "$WORKFLOW_COMMIT" --arg run "$RUN_URL" --arg tag "$RELEASE_TAG" \
  --arg started "$started" --arg finished "$finished" --arg version "$version" \
  --argjson update_exit "$update_exit" --argjson scan_exit "$scan_exit" \
  '{status:$status,file:"askerdway.dll",sha256:$sha,source_commit:$source,workflow_commit:$workflow,
    release_tag:$tag,run_url:$run,started_at_utc:$started,finished_at_utc:$finished,
    scanner:$version,definition_update_exit_code:$update_exit,scan_exit_code:$scan_exit,
    scope:"ClamAV scan of this exact DLL. No detections is not a guarantee of safety."}' \
  > evidence/clamav-report.json
{
  printf '### ClamAV: %s\n\n' "$status"
  printf 'DLL SHA256: `%s`\n\nSource: `%s`\n\n[Workflow run](%s)\n' "$actual" "$SOURCE_COMMIT" "$RUN_URL"
} >> "$GITHUB_STEP_SUMMARY"
test "$status" = clean
test "$(sha256sum candidate/askerdway.dll | cut -d ' ' -f 1)" = "$actual"
cp evidence/clamav-report.json evidence/clamav.log evidence/freshclam.log evidence/signatures.txt candidate/
(
  cd candidate
  sha256sum askerdway.dll LICENSE THIRD_PARTY_NOTICES.txt LICENSE-FONT.txt build-info.json \
    clamav-report.json clamav.log freshclam.log signatures.txt > SHA256SUMS.txt
  zip -q -9 ../release/askerdway-windows-x64.zip askerdway.dll LICENSE THIRD_PARTY_NOTICES.txt \
    LICENSE-FONT.txt build-info.json clamav-report.json clamav.log freshclam.log signatures.txt SHA256SUMS.txt
)
cp evidence/clamav-report.json evidence/clamav.log release/
(
  cd release
  sha256sum askerdway-windows-x64.zip > askerdway-windows-x64.zip.sha256
  sha256sum askerdway-windows-x64.zip clamav-report.json clamav.log > release-sha256.txt
)
