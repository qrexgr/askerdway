"""Check tracked source files without printing any discovered credential values."""
from pathlib import Path
import re
import subprocess
import sys

FORBIDDEN_SUFFIXES = {
    ".dll", ".exe", ".lib", ".pdb", ".obj", ".ilk", ".pfx", ".p12", ".pem", ".key", ".pyc",
}
CREDENTIAL = re.compile(
    rb"gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}"
    rb"|-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"
    rb"|AKIA[0-9A-Z]{16}"
)


def check_file(path: Path) -> list[str]:
    issues = []
    name = path.name.lower()
    example = name in {".env.example", ".env.sample", ".env.template"}
    if path.suffix.lower() in FORBIDDEN_SUFFIXES or (
        (name == ".env" or name.startswith(".env.")) and not example
    ):
        issues.append("credential container or build output must not be tracked")
    if path.is_symlink():
        issues.append("tracked symlinks need explicit review; do not scan their targets")
        return issues
    data = path.read_bytes()
    if CREDENTIAL.search(data):
        issues.append("possible credential or private key; remove and rotate if real")
    return issues


def main() -> int:
    tracked = subprocess.check_output(["git", "ls-files", "-z"]).split(b"\0")
    failures = 0
    for raw in tracked:
        if not raw:
            continue
        path = Path(raw.decode("utf-8"))
        for issue in check_file(path):
            print(f"{path.as_posix()}: {issue}", file=sys.stderr)
            failures += 1
    if failures:
        return 1
    print("Tracked-file checks passed (limited patterns, not a complete secret audit).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
