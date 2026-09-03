# Security Policy

Snapmap+ loads into DOOM 2016 as a set of DLLs, so a vulnerability here is a supply-chain risk for everyone who
installs a release. We take reports seriously and appreciate responsible disclosure.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.** Instead use GitHub's private vulnerability
reporting: open the repository's **Security** tab and choose **Report a vulnerability**. That starts a private
channel with the maintainer so the issue can be fixed before it is disclosed.

Please include:

- what the problem is and where in the code it lives,
- how to reproduce it (a minimal example if you can), and
- the impact you believe it has.

We will acknowledge your report, keep you posted on the fix, and credit you when it ships (unless you prefer to
remain anonymous).

## Supported versions

Security fixes are made against the latest release; older releases are not back-patched. Run `snapmap-plus update` to
stay current.

## How releases are protected

- Pull requests run in a secretless sandbox: fork PRs get a read-only token and no repository secrets.
- The build and release path holds no long-lived third-party credential. The one such credential the
  project stores -- the changelog drafter's API key -- lives in a branch-restricted environment used by a
  single job that has no write permission, and is inventoried in [`docs/services.md`](../docs/services.md).
- Any change to a supply-chain-critical path (the CI/release workflows, the build scripts, the installer)
  requires maintainer review before it can merge.
- Every merge to the default branch requires a passing security + build gate.
- Third-party GitHub Actions are pinned to full commit SHAs.
- Release artifacts ship a `MANIFEST.sha256` and a build-provenance attestation; verify a download with
  `gh attestation verify <file> --repo doom-snapmap/snapmap-plus`.

Thank you for helping keep Snapmap+ and its users safe.
