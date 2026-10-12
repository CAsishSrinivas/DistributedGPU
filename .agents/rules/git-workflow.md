# Git Workflow & Branch Protection Rules

## Mandatory Development Lifecycle

Direct pushes to `develop` and `main` branches are **strictly forbidden**. All changes must follow this exact sequence:

1. **Create an Issue First**:
   - Before writing or modifying any code, an issue must be created on GitHub describing the feature, bug, or task.
   - Note the Issue ID (e.g., `#1`).

2. **Create a Dedicated Feature Branch**:
   - Branch off `develop`:
     ```bash
     git checkout develop
     git pull origin develop
     git checkout -b feature/issue-<ID>-<short-description>
     ```

3. **Implement, Test & Verify**:
   - Implement the changes adhering to all design and performance constraints.
   - Run local build and CTest regression suites (`ctest --test-dir build -C Release`).
   - Commit changes to the feature branch.

4. **Create a Pull Request (PR)**:
   - Push the feature branch to `origin`:
     ```bash
     git push -u origin feature/issue-<ID>-<short-description>
     ```
   - Open a PR targeting the `develop` branch using GitHub CLI:
     ```bash
     gh pr create --base develop --title "..." --body "..."
     ```
   - Reference the issue in the PR description (`Closes #<ID>`).

5. **User-Only PR Review & Merge (CRITICAL RULE)**:
   > **STRICT CONSTRAINT**: **ONLY THE HUMAN USER CAN REVIEW, MERGE, OR CLOSE A PULL REQUEST.**  
   > The AI assistant is **STRICTLY FORBIDDEN** from running `gh pr merge`, `gh pr close`, or using the GitHub API to merge/close PRs.  
   > Once the AI opens a PR, it must **STOP** and provide the PR link to the user.
   - The user reviews the PR diff on GitHub.
   - The user merges the PR (Squash and Merge) or requests changes.
   - Once the user merges into `develop`, the release pipeline updates `develop-latest`.
   - Merging `develop` into `main` updates the production `main-latest` release.

6. **Detailed Documentation Standards**:
   - **Issues**: Must contain `Problem Statement & Context`, `Objective & Requirements`, `Technical Specifications`, and `Acceptance Criteria`.
   - **PRs**: Must contain `Summary of Changes`, `Motivation & Linked Issues`, `Detailed Code Modifications`, `Verification & Test Results`, and `Impact on Cross-Device Releases`.
   - Brief or one-line descriptions are strictly unacceptable.

7. **Detailed Commit Message Standards**:
   - Every commit message must be multi-line and structured:
     - Header: `<type>(<scope>): <summary (<= 72 chars)>`
     - Body: `## What` (itemized list of modifications), `## Why / Motivation` (technical context & rationale), `## Impact & Subsystems`.
     - Footer: `Refs: #<ID>` or `Closes: #<ID>`.
   - Terse or single-line commits without technical explanations are strictly forbidden.
