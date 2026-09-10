# Repository automation

## Purpose

Defines contribution checks, release preparation, publishing, issue maintenance and repository ownership.

## Contents

- `workflows/` contains GitHub Actions workflows.
- `CODEOWNERS` assigns review ownership.
- `PULL_REQUEST_TEMPLATE.md` and `SECURITY.md` describe contribution and vulnerability-reporting routes.
- `dependabot.yml` configures dependency update proposals.

## Working here

Read [contributing](../docs/contributing.md) before changing a workflow. Keep actions pinned to commit SHAs and give each job only the permissions it needs. `ci.yml` checks contributions; `prepare-release.yml` drafts reviewed notes; `release.yml` publishes tagged builds; `pages.yml` publishes the site. The two `issues-*.yml` workflows manage retest requests and unanswered reports.
