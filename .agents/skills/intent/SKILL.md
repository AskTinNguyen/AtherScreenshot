---
name: intent
description: Run a feature as a living intent in docs/intent/<feature>/, carried out by a background worker against an acceptance list with proof from real tool output. Use when the user types /intent (or /intent init to set a project up), and also when the user invokes any other skill with a prompt that asks to build, change, or fix a feature (auto-capture). Not for recurring or scheduled jobs, one-off utilities, or questions.
---

# Intent-Driven Sessions

One feature, one living intent. The user's prompt becomes `docs/intent/<feature>/prompt.md`. A background worker executes against it until the acceptance list is met. Every later prompt is logged verbatim and either amends the intent, answers a finding, or is answered from progress. The folder is committed, so any later session resumes from the files alone.

## The Project Contract

This skill is the same in every repository. What differs per project lives in **`docs/intent/README.md`**, the project contract: areas, statuses, the proof kinds, what workers read first, what they must never do, which calls go to the user, what `main` means and who may merge, and what closing an intent updates. Read it before anything else. Where this skill and the contract differ, the contract wins; neither ever weakens a gate in `AGENTS.md` or `CLAUDE.md`.

If `docs/intent/README.md` does not exist, set the project up first (`/intent init`, below), then continue with what the user asked.

## Invocation

| The user | You do |
| --- | --- |
| `/intent init` | Set the project up: follow [references/setup.md](references/setup.md). |
| `/intent <prompt>` | Start a new intent from the prompt. |
| Invokes another skill with a feature prompt | Auto-capture: start a new intent, with that skill as the worker's skill. |
| `/intent` alone | List open intents (status, rev, blocking findings). If exactly one is active or parked, resume it; otherwise ask which. |
| Says "park it" / "we're done" | Set `Status: parked` (with a reason) or `completed`, stop the worker, commit. |
| Anything else while an intent is active | Route it through the orchestrator loop below. |

### Starting

Infer everything the user did not say:

- **Feature name:** a short kebab-case slug. If `docs/intent/<slug>/` exists for another feature, add a distinguishing word.
- **Skill:** the invoked skill for auto-capture; otherwise the best match among the skills this session has, or `none`.
- **Area:** exactly one from the contract's Areas table. Never invent one; if none fits, say so and propose adding one to the contract.
- **Owner:** `git config user.name` of the person who asked.
- **Issue:** `#<number>` for a GitHub issue, or the tracker ID the contract names; leave the line out otherwise.
- **Acceptance:** items with ids drafted from the prompt, each naming one proof kind from the contract's Proofs table; no checkboxes.

Copy the templates from `assets/templates/` into `docs/intent/<feature>/`, fill them, and reply with one short block, starting the worker in the same turn:

> Started **<slug>** (rev 1), area <Area>, skill `<skill>`.
> Done when: A1 … A2 … A3 …
> Rename it, change the area or skill, or change the list by just saying so.

## Proof

A proof names a check whose result appears in tool output: an exit code, a pass count, a deployment state, a URL that answered. Prose claims ("checked in the browser", "19 states look right") are not proof.

The proof kinds are the contract's Proofs table. Two kinds exist everywhere:

| Proof | What counts |
| --- | --- |
| `gate: <command>` | The command ran in the intent's worktree and exited 0; for test runners, a pass count with zero failures. Quote its result line. |
| `review` | A versioned review record: who or what process, timestamp, content revision (a `progress.md` step, or the owner's decision in `log.md`). |

## Files And Ownership

One writer per file:

| File | Writer | Content |
| --- | --- | --- |
| `prompt.md` | Orchestrator only | Goal, non-goals, decisions, acceptance (ids and proofs, no checkboxes), constraints, changelog. Carries `Rev` and `Status`. |
| `log.md` | Orchestrator only | Append-only. Each user prompt verbatim, with its class and the rev it produced. |
| `progress.md` | Worker only | `- PR:` line; Acceptance table (`met` / `open` with evidence); current and next step; steps with evidence; reconciliations. |
| `findings.md` | Worker adds, orchestrator resolves | Discoveries that may change the intent, each with a proposed amendment. |

Whether an item is met lives only in `progress.md`. The worker never edits `prompt.md`; it writes a finding instead.

Findings are for changes to the intent and for the calls the contract reserves for the user (product, design, content, licensing, privacy, and whatever else it lists), which go to the user as options with a recommendation. Engineering decisions inside the intent's scope are the worker's; it records them in `progress.md`.

## Orchestrator Loop

1. **Capture.** Append the user's message verbatim to `log.md`. Replace credentials, tokens, personal identifiers, and machine paths with `<redacted>` or `<local path>`.
2. **Classify** it: `intent` (amend `prompt.md`, bump `Rev`, changelog line, tell the user the new rev), `decision` (resolve a finding, fold accepted amendments, bump `Rev`, add a Decisions entry), `question` (answer from progress and findings), or `aside`.
3. **Dispatch.** If the worker runs, send it the new rev on its thread (Claude Code: `SendMessage`). Otherwise fill `assets/worker-brief.md` and start one in the background on the most capable model (Claude Code: an `Agent` with `run_in_background`; other agents: their background or sub-agent mechanism, or run the brief yourself step by step). Record its name in `progress.md` → `Worker`.
4. **Review** each report against the acceptance list and the evidence. No evidence row, no acceptance.
5. **Merge and close.** When every Acceptance row is `met` from real tool output, follow the contract's Merge section. If it grants merge authority, merge the PR (squash; never override a failed check), confirm what it says `main` triggers (a deployment, CI on the merge commit), then close. If it does not, tell the user the PR is ready, with the evidence, and close once they merge. Closing: set `Status: completed`, add a changelog line, do the contract's On close steps, and report scope, checks, residual risk, and follow-ups. Never merge on a failed or unrun gate.

## Worker Rules

The full contract is `assets/worker-brief.md`. Essentials: read the project contract, `AGENTS.md` / `CLAUDE.md`, and the intent files; re-read `prompt.md` at each step and reconcile on a rev change; keep steps small; update `progress.md` after each step with evidence; stop on all met, a blocking finding, a destructive / production / cost / security / credential action, or a rate limit; commit exact paths on the feature branch; never force-push; never merge.

## Git

- Each intent works in its own worktree and branch (`intent/<feature>`, worktree where the contract says, default `../<repo>-wt/<feature>`); never switch the main checkout.
- The intent folder lands with the feature's PR.
- Only the orchestrator merges, and only after proof and only with the contract's authority.

## Resuming In A New Session

Read the contract and all four files. If every Acceptance row is `met` and every PR merged, close it. Otherwise summarize in five lines or fewer, list blocking findings, and start a fresh worker. The files are the whole state.
