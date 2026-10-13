# Pre-build: compile the git commit into the firmware (GIT_COMMIT, e.g. "df405f1"
# or "df405f1-dirty"), shown in /api/netstatus and on the page, so two builds
# with the same version can still be told apart.
import subprocess

Import("env")  # noqa: F821

try:
    commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
    dirty = subprocess.run(["git", "diff", "--quiet", "HEAD"]).returncode != 0
    commit += "-dirty" if dirty else ""
except (OSError, subprocess.CalledProcessError):
    commit = "unknown"
env.Append(CPPDEFINES=[("GIT_COMMIT", env.StringifyMacro(commit))])  # noqa: F821
