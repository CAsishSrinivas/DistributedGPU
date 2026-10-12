# Antigravity Agent Guidelines & Rules

## 1. Git Workflow & PR Merge Constraint

- **Direct Pushes Forbidden**: NEVER push directly to `develop` or `main`.
- **Issue-First**: Always create a GitHub issue first before starting work (`gh issue create`).
- **Feature Branches**: Always create a feature branch off `develop`: `git checkout -b feature/issue-<ID>-<description>`.
- **Local Verification**: Always compile with MSVC Release and pass all CTest tests before opening a PR.

### CRITICAL RULE: USER-ONLY PR MERGE & CLOSURE
> **The AI assistant is STRICTLY FORBIDDEN from merging or closing Pull Requests.**
> - The AI must NEVER execute `gh pr merge`, `gh pr close`, or invoke GitHub merge/close APIs.
> - Once the AI creates a Pull Request, it must **STOP** and provide the PR URL to the user.
> - **ONLY the human user has the authority to review, approve, merge, or close Pull Requests.**

---

## 2. Cross-Device Release Policy

- Releases flow exclusively through GitHub Releases:
  - `develop` branch $\to$ `develop-latest` pre-release (`DistributedGPU-windows-x64-develop.zip`)
  - `main` branch $\to$ `main-latest` stable release (`DistributedGPU-windows-x64-main.zip`)
- Secondary devices (e.g. Laptop B) update automatically using `.\scripts\update_laptop_b.ps1 -StartWorker` matching their active branch.
