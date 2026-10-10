# Intents

Each feature built by agents runs as a living intent in `docs/intent/<feature>/`: `prompt.md` (the spec, by the orchestrator), `log.md` (every prompt, verbatim), `progress.md` (the worker's record and evidence), and `findings.md` (discoveries that may change the spec). The procedure is the `intent` skill ([AskTinNguyen/intent](https://github.com/AskTinNguyen/intent)); this file is the project's contract with it, and wins where they differ.

<!-- Optional: what is NOT an intent here (recurring jobs, tracker-only tasks), and where the backlog lives. -->

## Statuses

| Status | Meaning |
| --- | --- |
| `planned` | Written ahead; no worker yet. A `Waits for` line names what must happen first. |
| `active` | A worker runs against it. |
| `parked` | Stopped on purpose, with a reason; resumes from the files. |
| `completed` | Every Acceptance row met and the PR merged<!-- , and production deployed -->. |

## Areas

Every intent names exactly one area.

| Area | Covers |
| --- | --- |
| <Area> | <what it covers, with its source folders> |
| Platform | Build, deploy, CI, test harnesses, the agent workflow |

## Proofs

Every acceptance item names one of these. A proof is a result in tool output, never a prose claim.

| Proof | What counts |
| --- | --- |
| `gate: <command>` | The command ran in the intent's worktree and exited 0; for test runners, a pass count with zero failures. This project's gates: <`npm test`, `npm run typecheck`, …>. |
| `build` | <the build command(s)> succeeded. |
| `review` | A versioned review record: who or what process, timestamp, content revision. |
| `owner` | The owner checked it themselves and said so (recorded in `log.md`). |
<!-- Add project kinds, e.g. `ui:verify` (Playwright + axe), `prod` (the production deployment for the merge commit is READY and a probe answered 200), `ci:<workflow>` (a named CI run, cite its URL). -->

## Read First

Workers read these besides the intent files and `AGENTS.md` / `CLAUDE.md`:

- <`docs/architecture.md`, `docs/design.md` for UI, …>

## Decisions

Engineering calls inside an intent's scope are the worker's. These go to the user as findings with options and a recommendation:

- Product and scope, design, content, licensing, privacy<!-- , plus project-specific: pricing rules, accuracy tolerances, … -->.

## Never

Besides credentials, `.env` files, full chat transcripts, pushing to `main`, force-pushing and weakening a gate:

- <project data that must not be committed, sites not to sign in to, invariants not to break>.

## Git And Merge

- Worktrees: `../<repo>-wt/<feature>`, branch `intent/<feature>`.
- `main`: <what a push to main triggers, e.g. "deploys production on Vercel" / "runs CI only">.
- Merge authority: <`none` (the orchestrator says the PR is ready; the owner merges) / `after-proof` (the orchestrator squash-merges when every applicable gate passed in tool output; never `--admin` over a failed check)>.
- After merge, confirm: <the production deployment for the merge commit / CI green on the merge commit / nothing>.

## On Close

- <e.g. update `docs/product-status.md` when a capability changed; update the tracker's task status>.
