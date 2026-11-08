# Security policy

Daneel's RiTron is experimental software intended for development and emulation.
It is not designed or audited for production use, and its kernel, drivers,
filesystem, and network stack may contain serious defects.

## Reporting a vulnerability

Please do not report an exploitable vulnerability in a public issue. Use
GitHub's **Report a vulnerability** option on the repository's Security tab
to submit a private vulnerability report. Include:

- The affected revision, files, or component.
- The conditions required to trigger the issue.
- A concise impact assessment and reproduction details.
- Any suggested mitigation, if known.

Avoid including real credentials, private user data, or destructive proof of
concepts in the report. If private vulnerability reporting is unavailable,
contact the repository maintainers privately through a verified channel listed
on the repository profile rather than publishing exploit details.

## Response expectations

The maintainers will acknowledge a report as soon as reasonably possible,
investigate its impact, and coordinate any fix or disclosure with the reporter.
There is no guaranteed response or remediation timeline.

## Scope and usage

Security reports are welcome for defects in this project's source, build
scripts, container configuration, and GitHub Actions workflow. Do not test
against systems or services you do not own or have explicit permission to
assess. The network client supports unencrypted HTTP only; do not send
credentials or sensitive information through it.
