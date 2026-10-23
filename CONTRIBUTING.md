# Contributing

Open a change request or bug report before substantial work. The owner decides priority and scope.
Work in a topic branch and submit a pull request to main. Do not push directly to main,
rewrite shared history, publish a release, or change repository visibility without an explicit owner request.

Pull requests run lightweight repository and Rust checks. They have a read-only token, no secrets,
and run on GitHub-hosted machines. The published source currently has zero Rust unit/doc tests;
the Rust check verifies formatting, linting, and compilation, not behavioral regression coverage.
A green result does not prove Windows DLL compatibility or behavior in Dota.
Full builds, scans, and release publication use the existing manually dispatched release workflow;
only the repository owner may start or rerun that workflow. It still requires an existing release tag.

Keep changes focused, explain the result and risks, and record actual validation. Runtime behavior must be
checked in a demo or private lobby before claiming live acceptance. Never add credentials, private keys,
personal data, build outputs, or copies of DLLs.

The owner reviews all paths through CODEOWNERS. While the repository remains private on the current plan,
GitHub does not enforce CODEOWNERS approval or block direct pushes. Only the owner currently has write access.
After making the repository public, enable the prepared rulesets as described in
[repository administration](.github/admin/README.md).

Automation does not merge PRs, modify source code, or schedule releases. Do not grant collaborators
admin access. Public contributors can submit fork PRs without write access to this repository.
