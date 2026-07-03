# PostgreSQL patch: dynamic psql prompts (PROMPT_COMMAND)

## Summary

Add bash-like `PROMPT_COMMAND` support to psql so interactive prompts can be
regenerated before each `readline()` call, with optional session export for
external prompt renderers.  A companion patch for the
[powerline](https://github.com/powerline/powerline) project provides a
reference implementation; see `src/bin/psql/powerline-integration.md`.

## Backward compatibility

- **Default behavior unchanged** for users who do not set new variables.
- `PROMPT_COMMAND` and `%D` are inert until configured (same model as bash).
- `PROMPT_SESSION_EXPORT` defaults to **off**; session environment is only
  exported when explicitly enabled, so existing `PROMPT_COMMAND` uses are not
  surprised by extra `setenv()` calls.
- New psql variables (`SHELL_EXIT`, etc.) do not alter query execution.

We are happy to adjust naming, defaults, or add further gating if reviewers
prefer a different compatibility story.

## Features

1. **`PROMPT_COMMAND`** — run a shell command before each interactive prompt;
   capture first line of stdout.
2. **`%D` prompt escape** — insert `PROMPT_COMMAND` output into `PROMPT1`/`PROMPT2`.
3. **`SHELL_EXIT`** — last command exit status (SQL failure → 1, `\!` → real
   shell code); not updated by `PROMPT_COMMAND` itself.
4. **`PROMPT_SESSION_EXPORT`** — when on, export `PG*` and `PSQL_*` env vars
   for the `PROMPT_COMMAND` subprocess (connection info, txn state, row count,
   superuser flag, optional `txid`).
5. **Prompt refresh** — `run_prompt_command()` before each `readline()` (fixes
   stale prompts without `rl_pre_input_hook` hacks).
6. **Connect reset** — clear `ROW_COUNT`/`SHELL_EXIT` and refresh optional
   `txid` after successful `\connect`.

## Files touched

- `src/bin/psql/prompt.c`, `prompt.h` — core prompt/PROMPT_COMMAND logic
- `src/bin/psql/input.c`, `input.h` — regenerate prompt before readline
- `src/bin/psql/common.c` — `SHELL_EXIT` tracking
- `src/bin/psql/command.c` — connect hook
- `src/bin/psql/mainloop.c`, `startup.c`, `help.c`
- `src/bin/psql/powerline-integration.md` — integration notes (not installed)

## Testing

Manual: interactive psql with `PROMPT_COMMAND`, `%D`, `\c`, failed SQL,
`\!`, and readline editing (e.g. `\c postgres` must retain spaces).

Suggested follow-up: TAP test for `%D` and `SHELL_EXIT` in `src/bin/psql/t/`.

## Companion patch

powerline psql extension (separate PR): segments, theme, readline renderer.
Cross-reference: `src/bin/psql/powerline-integration.md`.