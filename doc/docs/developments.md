
<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# TIMPANI Development Guide

This document describes the development workflow, testing, static analysis, and best practices for contributing to the TIMPANI project.

---

## Table of Contents

1. [Development Workflow](#development-workflow)
2. [Branching Strategy](#branching-strategy)
3. [Pull Requests & Code Review](#pull-requests--code-review)
4. [Continuous Integration](#continuous-integration)
5. [Static Analysis](#static-analysis)
6. [Testing](#testing)
7. [Documentation](#documentation)

---

## Development Workflow

1. Fork the repository and clone your fork.
2. Create a new branch for your feature or bugfix.
3. Make your changes, following the [coding rules](../contribution/coding-rule.md).
4. Run static analysis and tests locally.
5. Push your branch and open a Pull Request (PR) against the `development_0.5` branch (not main).
6. Request a review and address any feedback.
7. Once the milestone is reached, changes from `development_0.5` will be merged to `main`.

---

## Branching Strategy

- **main**: Stable, production-ready code.
- **development_0.5**: All ongoing development until June 2026. All developers should use this branch for features and bugfixes.
- **feature/xxx**: New features (branch from development_0.5).
- **bugfix/xxx**: Bug fixes (branch from development_0.5).
- **hotfix/xxx**: Urgent fixes (branch from main).

---

## Pull Requests & Code Review

- All changes must go through a Pull Request (PR).
- PRs must be made against the `development_0.5` branch, not `main`.
- PRs should be linked to an issue or feature request when possible.
- Ensure your code passes all checks (formatting, lint, tests).
- At least one reviewer must approve before merging.
- Use clear commit messages and PR descriptions.

---

## Continuous Integration

The project uses CI (e.g., GitHub Actions) to automatically run static analysis, tests, and build checks on every PR and push to main branches.

Typical CI checks include:
- rustfmt (formatting)
- clippy (lint)
- cargo test (unit/integration tests)
- cargo audit (security)
- cargo deny (license/dependency checks)
- cargo tarpaulin (coverage, optional)

### CI pipeline layout

`.github/workflows/ci-dispatcher.yml` is the single entry point. It detects
which paths changed and calls only the matching reusable workflow. A manual
run (`workflow_dispatch`) runs everything.

| Changed path | Workflow | What it does |
|:--|:--|:--|
| `timpani_rust/` | `run-rust-ci.yml` | build, test, clippy, fmt, deny, coverage |
| `timpani-o/` | `run-timpani-o-ci.yml` | build + `ctest` (amd64, arm64), ASan/UBSan, gcovr coverage, image build check |
| `libtrpc/`, `sample-apps/` | `run-native-libs-ci.yml` | CMake build (amd64, arm64) |
| any C/C++ | `run-cpp-lint.yml` | clang-format (changed lines only), cppcheck |
| `**/*.md` | `run-doc.yml` | markdownlint (report only, uploaded as an artifact) |
| `.github/workflows/` | `run-validate.yml` | actionlint (syntax, job graph, expressions, action inputs, shellcheck) |

The final `CI Complete Summary` job fails if any triggered job failed or was
cancelled, or if change detection itself failed. Configure it as the required
status check in the branch protection rules of `develop_1.0` and `main` so that
a failing pipeline blocks merging.

Workflow conventions: every job sets `timeout-minutes`, workflows use the least
permissions they need (`contents: read` by default), and action versions are
kept on current majors that run on Node 24. Run `actionlint` locally before
pushing workflow changes:

```bash
docker run --rm -v "$PWD":/repo -w /repo rhysd/actionlint:1.7.12
```

### Scripts layout

| Folder | Content |
|:--|:--|
| `scripts/rust/` | Rust CI scripts used by `run-rust-ci.yml` (`installdeps.sh`, `buildNparse.sh`, `testNparse.sh`, `clippy_check.sh`, `fmt_check.sh`, `deny_check.sh`, `test_coverage.sh`, `license_check.sh`). Run them from the repository root. |
| `scripts/cpp/` | C/C++ CI scripts used by the timpani-o, native libs, lint and package workflows |
| `scripts/` | Development helpers that are not part of CI (`mock_pullpiri*`, `verify_pipeline.sh`, ...) |

### Running the C/C++ checks locally

The workflows call scripts in `scripts/cpp/`, so the same commands work
locally (Ubuntu 22.04 or newer):

```bash
scripts/cpp/installdeps.sh timpani-o lint   # groups: timpani-o, libs, lint, all
scripts/cpp/build.sh timpani-o              # also: libtrpc, sample-apps
scripts/cpp/test.sh timpani-o               # ctest + JUnit XML in dist/tests
scripts/cpp/coverage.sh                     # gcovr reports in dist/coverage/timpani-o
BASE_REF=origin/develop_1.0 scripts/cpp/fmt_check.sh
scripts/cpp/static_check.sh                 # cppcheck
```

Useful environment variables: `BUILD_TYPE`, `SANITIZE=address,undefined`,
`COVERAGE_MIN_LINES`, `FMT_STRICT=1`, `CPPCHECK_STRICT=1`.

The existing sources are not fully clang-format clean, so the format gate is
incremental: it only fails on lines changed relative to the base branch.

### Release and delivery (CD)

Delivery is tag driven, as in pullpiri. The version comes from the `VERSION`
file (CalVer, see `doc/VERSIONING.md`).

| Tag | Workflow | Result |
|:--|:--|:--|
| `v<VERSION>` (e.g. `v2026.08.0`) | `release.yml` | full CI gates, release packages, GitHub Release, container image |
| `dev*` (e.g. `dev-2026.08.0-1`) | `dev-release.yml` | fast pre-release, no CI gates |

```bash
# release (VERSION must be 2026.08.0 and CHANGELOG.md must have "## [2026.08.0]")
git tag v2026.08.0
git push origin v2026.08.0
```

`release.yml` steps:

1. `verify-version`: the tag must equal `v` plus the content of `VERSION`, and
   `CHANGELOG.md` must contain a `## [<VERSION>]` section.
2. CI gates: Rust CI, timpani-o CI, native libs CI, C/C++ lint,
   docs lint and workflow YAML validation.
3. `package` (`run-package.yml`, `scripts/cpp/package.sh`): release build of
   timpani-o for amd64 and arm64 as
   `timpani-o-<VERSION>-linux-<arch>.tar.gz` plus SHA-256 files.
4. `publish-release`: only after every gate passed, uploads the packages,
   `SHA256SUMS-timpani`, CI reports, timpani-o coverage report,
   `CHANGELOG.md`, documentation archive and the Eclipse QuEVee quality
   manifest to the GitHub Release.
5. `build-and-push-container` (`build-container.yml`): multi-arch
   (`linux/amd64`, `linux/arm64`) timpani-o image from `timpani-o/Dockerfile`,
   pushed to `ghcr.io/<owner>/timpani26/timpani-o` with the tag name and
   `latest`.

`dev-release.yml` only runs `package`, publishes the packages and checksums as
a GitHub pre-release and pushes the container image tagged with the tag name
and `dev-latest`.

---

## Static analysis

### [rustfmt](https://github.com/rust-lang/rustfmt)

Rustfmt formats Rust code according to the official rust style guide.
It enforces consistency across codebases and making code easier to read and review.

#### Using rustfmt

Step 1: Install Rustfmt.

```bash
rustup component add rustfmt
```

Step 2: Format Your Code: Recursively formats all `.rs` files in the project.

```bash
# in src directory
make fmt
```

### [clippy](https://doc.rust-lang.org/nightly/clippy/)

Clippy is a collection of lints to catch common mistakes and improve Rust code.
It analyzes source code and provides suggestions for idiomatic Rust practices
to performance improvements and potential bugs.

#### Using clippy

Step 1: Install Clippy.

```bash
rustup component add clippy
```

Step 2: Run Clippy: Runs the linter on your project.
It reports warnings and suggestions for your code.

```bash
# in src directory
make clippy
```

*Optional*: Automatically fix warnings: Applies safe automatic fixes to clippy warnings.

```bash
# directory located `Cargo.toml` like `src/server/apiserver`
cargo clippy --fix
```

### [cargo audit](https://crates.io/crates/cargo-audit) - Security vulnerability scanner

Cargo Audit scans your `Cargo.lock` file for crates with known security
vulnerabilities using the RustSec Advisory Database.
It helps you ensure your dependencies are secure.

#### Using cargo-audit

Step 1: Install Cargo Audit.

```bash
cargo install cargo-audit
```

Step 2: Run: Scans the dependency tree for known vulnerabilities and outdated crates.

```bash
# directory located `Cargo.lock` like `src/server`, `src/player`, `src/agent`
cargo audit
```

### [cargo deny](https://crates.io/crates/cargo-deny) - Dependency & License checker

Cargo Deny checks for issues including:It’s used to enforce policies for project dependencies.

- Duplicate crates
- Insecure or unwanted licenses
- Vulnerabilities
- Unmaintained crates

#### Using cargo-deny

Step 1: Install Cargo Deny

```bash
cargo install cargo-deny
```

Step 2: Initialize Configuration (for New components only) -
Creates a default deny.toml file where you can configure license policies, bans and exceptions.

```bash
# directory located `Cargo.toml` like `src/server/apiserver`
# almost all crates already done.
cargo deny init
```

Step 3: Run Check -
Analyzes dependency metadata and reports issues related to licenses, advisories and duplicates.

```bash
# directory located `Cargo.toml` like `src/server/apiserver`
cargo deny check
```

### [cargo udeps](https://crates.io/crates/cargo-udeps) - Unused Dependency Detector

Cargo Udeps identifies unused dependencies in your `Cargo.toml` Keeping unused dependencies can bloat the project and expose unnecessary vulnerabilities."For this requires Nightly Rust to run".

*Caution* : rustc 1.86.0 or above is required. (on July 18th, 2025)

#### Using cargo-udeps

Step 1: Install Cargo Udeps

```bash
cargo install cargo-udeps
rustup install nightly
```

Step 2: Nightly run -
This runs the unused dependency checker and lists packages not being used.

```bash
cargo +nightly udeps
```


---

## Testing

### Unit Tests

Unit tests can be executed using the following command:

```bash
# In any directory containing `Cargo.toml`
cargo test
```

### Integration Tests

Integration tests should be placed in the `tests/` directory of each crate. For more information, refer to the [Rust documentation](https://doc.rust-lang.org/rust-by-example/testing/integration_testing.html).


### Code Coverage

Use [cargo-tarpaulin](https://crates.io/crates/cargo-tarpaulin) to measure test coverage (see above for usage).

**Required:** Code coverage must be at least 80% for a PR to be accepted.

---

## Documentation

- All public functions, structs, and modules should have Rust doc comments (`///`).
- Each crate should have a `README.md` describing its purpose, usage, and examples.
- Update the main project documentation as needed when adding new features or modules.
- Use `cargo doc --open` to build and preview documentation locally.

---

## [cargo tarpaulin](https://crates.io/crates/cargo-tarpaulin) - Code coverage

cargo-tarpaulin is a code coverage tool specifically designed for Rust projects.
It works by instrumenting the Rust code, running the tests,and
generating a detailed report that shows which lines of code were covered during testing.
It supports a variety of Rust test strategies, including unit tests and integration tests.

### Install and run

To use cargo-tarpaulin you need to install it on your system.
This can be done easily via cargo:

```bash
cargo install cargo-tarpaulin

#Once installed, you can run the tool in your project by using the following command:
# in any directories include `Cargo.toml`
cargo tarpaulin
```

