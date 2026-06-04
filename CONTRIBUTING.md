# File 1: `CONTRIBUTING.md`

````markdown
# Contributing Guide

This repository develops dynamic connectivity indexes for graph database systems, with an initial focus on integrating STree and DTree-style data structures into the Kuzu/LadybugDB ecosystem.

The project is currently research-oriented, but the codebase should be maintained with upstream-quality engineering standards.

## Development workflow

Please use the following workflow:

1. Create or select an issue.
2. Create a focused feature branch from `master`.
3. Keep the pull request small and reviewable.
4. Add or update tests for code changes.
5. Document benchmark commands and results when relevant.
6. Open a pull request into `master`.
7. Keep `master` buildable and testable.

Do not develop directly on `master`.

## Branch naming

Use descriptive branch names:

```text
feature/dynamic-connectivity-interface
feature/stree-index-wrapper
feature/dtree-index-wrapper
bench/randomized-runner
docs/benchmark-trace-format
```

Avoid generic branch names such as:

```text
dev
update
test
new-code
```

## Pull request expectations

Each pull request should have:

- A clear title.
- A focused scope.
- A short motivation.
- A summary of changes.
- Test commands and results.
- Known limitations.
- Suggested reviewer guide.

Large pull requests should be split into smaller ones whenever possible.

## Testing

For code changes, include the exact commands used to build and test the project.

Example:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

For benchmark-related changes, include:

- Dataset or trace used.
- Number of repetitions.
- Validation mode.
- Summary statistics generated.
- Whether benchmark result files are committed or intentionally excluded.

## Benchmark results

Generated benchmark results should not be committed by default.

Commit only:

- Benchmark scripts.
- Trace format documentation.
- Small sanity traces.
- Result summarization tools.

Large generated result directories should be ignored unless there is a clear reason to include them.

## Code review principles

A good pull request should allow a reviewer to understand:

1. What problem this PR solves.
2. What files changed.
3. How correctness was tested.
4. What is intentionally left for future work.
5. Whether the change is safe to merge.

The goal is to make this repository easy to review, maintain, and eventually upstream into Kuzu/LadybugDB-style graph database systems.
