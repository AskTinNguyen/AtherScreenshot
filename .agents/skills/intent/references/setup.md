# Setting A Project Up (`/intent init`)

Goal: a `docs/intent/README.md` (the project contract) that is true for this repository, drafted from evidence, confirmed by the user. Nothing else changes.

1. **Check.** If `docs/intent/README.md` exists, show its sections and ask what to change; edit only that. Otherwise continue.
2. **Read the repository**, read-only: `AGENTS.md`, `CLAUDE.md`, `README.md`, the package or build manifests (`package.json` scripts, `Makefile`, `Cargo.toml`, `pyproject.toml`, `*.xcodeproj`, `*.uproject`, …), CI workflows (`.github/workflows/`), deploy config (`vercel.json`, `netlify.toml`, `fly.toml`, …), the top-level source folders, and any tracker or roadmap in `docs/`.
3. **Draft** from `assets/contract.md`:
   - **Areas:** 4 to 9, cut by what users or the team would call the product's parts, each naming its source folders, plus `Platform`.
   - **Proofs:** `gate:` lists the commands that really exist (test, typecheck, lint, golden or snapshot runs); add `build`, a UI check, a CI kind, a `prod` kind only when the repository has them. Keep `review` and `owner`.
   - **Git And Merge:** what `main` triggers, from CI and deploy config. Merge authority defaults to `none`; write `after-proof` only when the user grants it in so many words, and quote them with the date.
   - **Read First, Decisions, Never, On Close:** from `AGENTS.md` / `CLAUDE.md` and the docs; leave a section's placeholder line out rather than guess.
4. **Confirm.** Show the draft as a short summary (areas, proofs, merge authority, anything you could not tell) and ask the user to correct it. Ask at most one question at a time, and only about what the repository cannot answer (merge authority always).
5. **Write** `docs/intent/README.md`. If `AGENTS.md` or `CLAUDE.md` exists, offer one line pointing to it ("Features run as intents: see `docs/intent/README.md`."); add it only on a yes.
6. **Commit** on a branch (`intent/setup`) with exact paths, or leave it uncommitted if the user prefers. Then carry on with what the user originally asked, if anything.
