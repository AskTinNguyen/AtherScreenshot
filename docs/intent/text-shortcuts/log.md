# QuickPrompts: Prompt Log

Append-only. Each user prompt verbatim, with credentials and machine paths redacted.

## L-1 (2026-11-14) | class: question | -> rev 1

> https://github.com/AskTinNguyen/AtherScreenshot
>
> Clone this repo to C.
>
> Review and propose Building an additional feature for this app that allows the users to save their frequently used phrases or agent prompts and use some sequence of hotkeys to trigger them, similar to what Apple has, for example: just by typing "doitforme" the app will recognize its a shortcut to be replaced with a stored Longer Prompt by the user.

## L-2 (2026-11-14) | class: intent | -> rev 2

> 1. expand after a Space
> 2. always paste
> 3. always-on-hook once turned on at the first time app open, changable via settings
> 4. a QuickPrompts page (with a list of pre-made prompts for team members to use)

## L-3 (2026-11-14) | class: intent | -> rev 3

> Agree with your suggestion. build everything, test, verify and release.

("Your suggestion" was: release to the team first, before a public update, because an always-on listener in an
unsigned app may draw antivirus attention. Read as: ship as the staging pre-release, which the in-app updater never
offers; a public release stays the owner's `package.bat publish`.)

## L-4 (2026-11-14) | class: intent | -> rev 3 (content)

> Now let's add the QuickPrompts starter content, i have a few to share with you ill send them in this thread

## L-5 (2026-11-14) | class: intent | -> rev 3 (content)

> Help me fill in the rest

Followed by seven prompt texts (agent away 8 hours, level building, thermo-nuclear code review gate, Working
Backwards, overnight run with a decision log, interior design rendering album, architecture exploration). They are in
`BuiltinQuickPrompts()` in `src/quickprompts.cpp` as `awaypls`, `levelpls`, `nukepls`, `workbackpls`, `bedtimepls`,
`interiorpls` and `archpls`, with spelling fixed (see progress.md, S6); the owner's originals are in the conversation.
