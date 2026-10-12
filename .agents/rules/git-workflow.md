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
   - Open a PR targeting the `develop` branch.
   - Reference the issue in the PR description (`Closes #<ID>`).

5. **Merge & Automated Release**:
   - Squash and merge the PR into `develop`.
   - Once merged, the release pipeline produces/updates the `develop-latest` release (`DistributedGPU-windows-x64-develop.zip`).
   - Merging `develop` into `main` produces the production `main-latest` release (`DistributedGPU-windows-x64-main.zip`).
   - Any device / laptop will automatically pull the build corresponding to its active branch.
