# Contributing to Daneel's RiTron

Thanks for your interest in improving Daneel's RiTron. This is an experimental
operating system, so focused changes, clear testing notes, and accurate
documentation are especially helpful.

Participation in project spaces is covered by the
[Community Code of Conduct](CODE_OF_CONDUCT.md).

## Before making a change

1. Check existing issues and pull requests for related work.
2. Keep changes scoped to one bug fix or feature where practical.
3. Preserve the project's freestanding x86_64 UEFI and kernel constraints.
   Kernel code cannot depend on hosted operating-system services or a standard
   C++ runtime.
4. Update the relevant guide under `docs/` when behavior, commands, limitations,
   or build requirements change.

## Build and validate

Use the host toolchain described in
[Building and running Daneel's RiTron](docs/BUILD.md), or use the Docker
Compose build:

```sh
docker compose run --build --rm builder
```

The Compose command builds the EFI applications and ISO, creates or updates
the persistent FAT16 disk, and verifies that disk. For changes that affect
runtime behavior, also run the project in QEMU when available:

```sh
make qemu
```

If you cannot run a test, say which test was skipped and why. Do not include
generated output from `build/` in a pull request.

## Code and pull request expectations

- Follow `.editorconfig` and the existing naming, formatting, and error-handling
  patterns.
- Keep comments focused on contracts, invariants, hardware behavior, or other
  details not obvious from the code.
- Avoid unrelated refactoring in feature or bug-fix changes.
- Add or update tests when practical, especially for host-side tooling and
  filesystem behavior.
- Describe the problem, the approach, compatibility or behavior changes, and
  validation performed in the pull request.
- Never commit credentials, private keys, personal data, or other secrets.

## Reporting issues

For general bugs and feature requests, open a GitHub issue. Include the
environment, reproduction steps, expected and actual behavior, and relevant
serial output. Remove secrets and unrelated personal information from logs
before posting.
