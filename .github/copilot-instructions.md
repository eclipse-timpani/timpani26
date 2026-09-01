<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# GitHub Copilot Instructions

These instructions apply to Copilot-assisted work in the
`eclipse-timpani/timpani26` repository. Keep this file focused on how Copilot
should work with the repository. Detailed project information belongs in the
referenced documentation.

## Repository orientation

- `timpani-n/`: node execution component and real-time scheduling enforcement.
- `timpani-o/`: scheduling and orchestration component.
- `sample-apps/`: workloads and scenarios used for validation.
- `timpani_rust/`: Rust implementations and Rust-specific tooling; check its
  documentation and the current task before changing it.
- `doc/architecture/`: system and component architecture.
- `doc/design/`: design decision records (DDRs).
- `doc/contribution/`: coding and contribution guidance.
- `.github/workflows/`: repository CI and automation.

Do not copy detailed architecture, workload models, roadmaps, or API
descriptions into this file. Use the document that owns the information.

## Documentation and context

- Use `README.md` and the relevant component README for project scope, setup,
  and usage.
- Read the relevant document in `doc/architecture/` for architecture changes.
- Read the relevant DDR in `doc/design/` before changing scheduling behavior,
  workload semantics, resource allocation, protocols, BPF behavior, runtime
  updates, or fault handling.
- Use `doc/contribution/` for coding, testing, commit, and pull request rules.
- Read documentation on demand. If two documents conflict, identify the
  conflict and ask for clarification before making a policy-sensitive change.
- Treat the current checkout and the current GitHub repository state as the
  working baseline. Do not assume that a historical or unadvertised branch
  exists.

## Engineering constraints

- Keep changes within the component and behavior requested by the user.
- Do not modify protobuf interface files without explicit approval.
- Do not put gRPC or asynchronous logic in the real-time critical path,
  including the Timer Master path in `timpani-n`.
- Preserve deterministic ordering wherever scheduling behavior depends on
  ordering; do not use unordered containers for such data.
- Do not add external dependencies or new public APIs without explicit
  approval from the project maintainer or Project Leader.
- Do not delete or rename existing public methods in `TimerMaster`,
  `BpfLoader`, `TaskRegistry`, `FaultMonitor`, or `GlobalScheduler`.
- Check return values and handle cleanup in C and BPF code. Follow the local
  kernel-style conventions for those files.
- Use RAII, smart pointers, and explicit error handling in C++ code. Do not
  introduce exceptions into hot paths.
- Add or update focused tests for new behavior and update documentation when
  behavior or usage changes.
- Preserve unrelated user changes in the working tree.

## Validation

- Start by checking the working tree and inspecting the relevant implementation
  and tests.
- Run the narrowest relevant build, test, lint, or static-analysis command
  after making a change.
- Use the component README and repository scripts for exact build commands.
- C, C++, BPF, and target-hardware validation may require Linux, WSL, a
  container, or the target device. Do not claim that a check passed when it
  was not run in the required environment.
- Report unavailable environments, skipped checks, and remaining risks.

## GitHub workflow

- The current default branch is `main`. Verify the current remote state before
  creating a branch.
- For non-trivial work, create or update a GitHub Issue and link the change to
  it. Follow the existing issue titles `[FEATURE]`, `[TASK]`, and `[BUG]`.
- Use a branch name in the form `<type>/<issue-number>-<short-description>`.
  Common types are `feat`, `fix`, `refactor`, `docs`, `test`, and `chore`.
- Use the repository's conventional commit format and include the Issue
  number, for example `docs(workflow): update Copilot guidance [#36]`.
- Open a Pull Request targeting `main`, include `Closes #<issue-number>` when
  the PR completes the Issue, and describe the validation performed.
- Do not push directly to `main`, force-push, or merge a Pull Request without
  the user's explicit request and the required project review.