# Repository administration

Current setup keeps the repository private. No command here changes visibility.
The private repository's current GitHub plan returns HTTP 403 for rulesets and branch protection.
CODEOWNERS, templates, and passing CI alone do not block a direct push.

## Available now

- Only qrexgr has repository access; no collaborators, invitations, deploy keys, or webhooks were present at setup.
- Auto-merge is disabled; squash is the only enabled merge method. Merged topic branches are deleted.
- Actions have a read-only default token and cannot approve PRs.
- Only GitHub-owned actions are allowed; external action references must use full commit SHAs.
- Private fork PR execution, write tokens, and secret sharing are disabled.
- PRs run Repository hygiene and Rust checks. There are no scheduled jobs or push-triggered builds.
- The existing build / scan / release workflow is manually started by the owner on main.
  Publication additionally requires selecting the publish input; the default only builds and scans.
- Dependabot vulnerability alerts are enabled. No automatic dependency-update merges are configured.

## After making the repository public

First audit all Git history, releases, logs, and images for private data and credentials.
The lightweight pattern check in this PR is not a comprehensive history audit.
Changing visibility requires a separate explicit owner request.

The existing release scripts currently require a private repository. Review those privacy guards
before expecting releases to work after publication; they deliberately fail closed today.

Run Pull request checks from Actions on the current main commit, then run from a trusted local checkout:

```powershell
# Preview (no changes).
pwsh -File .github/admin/Configure-Repository.ps1

# Apply baseline permissions and the four active rulesets.
pwsh -File .github/admin/Configure-Repository.ps1 -Apply -EnableBranchProtection
```

On a public repository the command also requires approval for every external contributor's workflow
and enables private vulnerability reporting, secret scanning, and secret push protection.
Review untrusted changes before approving a fork workflow.
Never use pull_request_target to execute PR code, pass secrets to PR code, or run untrusted PRs on a self-hosted runner.

The rulesets apply independently:

| Ruleset | Required behavior | Bypass |
| --- | --- | --- |
| main safety | PR, successful current GitHub Actions checks, resolved threads, squash; no deletion or force push | None, including admin |
| owner review | One review from CODEOWNERS; new commits dismiss approval | Admin may bypass this review rule through a PR only |
| immutable release tags | v* tags cannot be deleted or rewritten | None |
| owner creates release tags | Only the sole admin can create v* tags | Admin, for creation only |

The owner-review exception lets the sole owner merge their own PR, which GitHub cannot let them approve.
It does not bypass the separate main safety ruleset. Never grant another person or app admin access without
revisiting this exception. Normal collaborators still need owner review.
A repository administrator can change repository settings; these rules cannot protect against a stolen admin account.
Use GitHub 2FA/passkeys and narrowly scoped app/token access.

After applying, read back all rulesets in Settings > Rules > Rulesets and check that they are Active.
Confirm a test PR waits for both check jobs and that a contributor PR requests qrexgr's review.
Do not test force push or deletion against the real main branch or release tags.

The script updates only these four named rulesets. Other protections are preserved.
If GitHub rejects an operation, it stops with an error; earlier successfully applied settings remain in place.

## Routine work

1. Describe the task in an issue or request it in your coding agent.
2. Get a focused branch and pull request with checks and evidence.
3. Review the diff and merge explicitly.
4. Dispatch the release workflow on main only when a build / scan is needed.
5. Select publish only when you intend to replace assets of the existing release tag.

Do not store a personal access token in Actions secrets for ordinary builds. A read-only GITHUB_TOKEN
is sufficient for checks; only the existing publish job gets contents: write.
