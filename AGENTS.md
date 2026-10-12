# Antigravity Agent Guidelines & Rules

## 1. Git Workflow & PR Merge Constraint

- **Direct Pushes Forbidden**: NEVER push directly to `develop` or `main`.
- **Permanent Base Branches**: Base branches `main` and `develop` are permanent and CAN NEVER BE DELETED.
- **Issue-First**: Always create a GitHub issue first before starting work (`gh issue create`).
- **Feature Branches**: Always create a feature branch off `develop`: `git checkout -b feature/issue-<ID>-<description>`.
- **Auto-Deletion on Merge**: Feature branches are automatically deleted upon merge (`delete_branch_on_merge` is enabled), keeping the repository clean while preserving `develop` and `main`.
- **Local Verification**: Always compile with MSVC Release and pass all CTest tests before opening a PR.

### CRITICAL RULE: USER-ONLY PR MERGE & CLOSURE
> **The AI assistant is STRICTLY FORBIDDEN from merging or closing Pull Requests.**
> - The AI must NEVER execute `gh pr merge`, `gh pr close`, or invoke GitHub merge/close APIs.
> - Once the AI creates a Pull Request, it must **STOP** and provide the PR URL to the user.
> - **ONLY the human user has the authority to review, approve, merge, or close Pull Requests.**

### MANDATORY DETAILED DESCRIPTIONS FOR ISSUES & PRS
> **Every Issue and PR must contain thorough, structured technical descriptions.** Brief or one-line summaries are unacceptable.

1. **Issue Template Requirements**:
   - `## Problem Statement & Context`: Background, system state, and why this work is needed.
   - `## Objective & Requirements`: Concrete functional and architectural deliverables.
   - `## Technical Specifications`: Proposed API, data structures, algorithms, or protocols.
   - `## Acceptance Criteria`: Verifiable checklist for completion.

2. **Pull Request Template Requirements**:
   - `## Summary of Changes`: Overview of what was implemented.
   - `## Motivation & Linked Issues`: Rationale and `Closes #<ID>`.
   - `## Detailed Code Modifications`: List of files modified and key architectural decisions.
   - `## Verification & Test Results`: Compilation evidence, CTest output, and benchmark numbers.
   - `## Impact on Cross-Device Releases`: Effect on `develop` or `main` release artifacts.

### MANDATORY DETAILED COMMIT MESSAGES
> **Every Git commit must contain a structured, multi-line commit message.** One-liner, vague, or terse commits (e.g. "fix", "update", "wip") are strictly forbidden.

**Commit Message Format**:
```text
<type>(<scope>): <concise imperative summary (<= 72 chars)>

## What
- Detailed bullet points of every file and component modified or created.

## Why / Motivation
- Detailed technical reason, architectural context, or bug root-cause explanation.

## Impact & Subsystems
- Affected modules, latency/throughput considerations, or runtime behaviors.

Refs: #<ID> (or Closes: #<ID>)
```
Allowed Types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `ci`, `chore`.

---

## 2. Cross-Device Release Policy

- Releases flow exclusively through GitHub Releases:
  - `develop` branch $\to$ `develop-latest` pre-release (`DistributedGPU-windows-x64-develop.zip`)
  - `main` branch $\to$ `main-latest` stable release (`DistributedGPU-windows-x64-main.zip`)
- Secondary devices (e.g. Laptop B) update automatically using `.\scripts\update_laptop_b.ps1 -StartWorker` matching their active branch.
