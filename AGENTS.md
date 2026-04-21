# AGENTS.md

This repository allows autonomous coding agents to operate with these defaults:

- Make focused, reversible changes.
- Verify changes with relevant build/tests before claiming completion.
- Do not introduce dependencies unless explicitly requested.
- Keep responses concise and include changed files.
- suggest the fastest, cleanest and optimized algorithm

## What this is

CClaw — LLM-agnostic agentic AI framework in C11. Early phase: single-provider (OpenAI Responses API) synchronous `chat()` works; the architecture is being built toward multi-provider vtable + ReAct agent loop and will scale to Agent Memory.

## Build, run, test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug    # Debug enables ASan+UBSan (see CCLAW_ASAN option)
cmake --build build -j$(nproc)
./build/bin/cclaw                           # runs the REPL (requires .env with provider creds)
cd build && ctest --output-on-failure       # all tests
./build/bin/test_error                      # run a single test binary directly
```

Top-level options: `CCLAW_BUILD_EXAMPLES`, `CCLAW_BUILD_TESTS` (on when top-level), `CCLAW_ASAN` (on by default, Debug only).

`.env` must define provider credentials before `cclaw_init()` succeeds — the init path calls `libenv_load(".env")` then `cclaw_provider_from_env()`. No `.env` = no startup.

## Architecture — the boundary that matters

Three layers with a strict direction of dependency:

```
app (main, REPL, future agent/chain/graph)
  ↓
core  ── src/core/         provider-neutral types + lifecycle
  ↓ (via vtable only)
provider ── src/provider/  one file per provider, owns all wire-format translation
  ↓
net ── src/net/            dumb HTTP/SSE wrapper over libcurl
```

**The agnostic boundary is the provider vtable** (`include/cclaw/provider.h`). `CClawProvider` holds function pointers (`chat`, `free`) and a void* impl. Core never sees provider JSON; providers never touch core internals. Adding a new provider = one new `src/provider/<name>.c` implementing the vtable + one registration line in `registry.c`.

Current reality: only `openai` is registered. `cclaw_provider_from_env()` reads `CCLAW_PROVIDER` / `CCLAW_API_KEY` / `CCLAW_MODEL` / `CCLAW_BASE_URL` from the loaded env.

## Memory model — arena everything

All non-trivial allocations flow through `Arena *` (`src/lib/arena.h`). Pattern:

- `CClaw` struct owns `ctx->arena` created in `cclaw_init()`, freed in `cclaw_destroy()`.
- Subsystems that outlive a single call (REPL, future agent runs) create their own arena and free it on teardown — don't reuse `ctx->arena` for short-lived session data.
- Response buffers returned from `cclaw_chat()` currently `malloc` — caller `free()`s. This is transitional; long-term target is arena-backed slices (see `CclawSlice` plan in `TODO.md`).
- `arena_checkpoint()` / `arena_restore()` exist for scoped allocations (use them for per-iteration agent state later).

Do **not** introduce new ad-hoc `malloc` sites in core/provider without a clear reason. Arena is the default.

## Single-header vendored libs

`src/lib/` holds STB-style headers: `arena.h`, `libenv.h`, `libjson.h`, `termbox2.h`. **Exactly one TU per library** defines the IMPL macro:

- `src/core/cclaw.c` → `ARENA_IMPLEMENTATION`, `LIBENV_IMPLEMENTATION`
- `src/tui/tui.c`    → `TB_IMPL` (+ `TB_OPT_ATTR_W 32` for truecolor)
- libjson IMPL lives elsewhere in core — check before adding

Vendored headers trip `-Wconversion -Wsign-conversion -Wdouble-promotion -Wshadow`. Suppress per-file via `set_source_files_properties(<file> PROPERTIES COMPILE_FLAGS "...")`. Keep the property value on ONE physical line — embedded newlines in the quoted string propagate into `flags.make` and break Make/Ninja parsers.

## JSON parsing convention

Responses use `JsonSlice { const char *data; size_t len; }` — zero-copy views into the raw response buffer. The Response struct in `cclaw/response.h` is a fixed-shape mirror of the OpenAI Responses API (`MAX_OUTPUT`, `MAX_CONTENT` caps). Walk it by terminating on `.type.data == NULL`. Do not assume null-termination — always carry `.len`.

## Public vs internal headers

- `include/cclaw/*.h` — public API. Only these go in installed artifacts.
- `src/internal.h` — private struct definitions (`CClaw` body, etc.). Anything here stays out of `include/`.
- Provider .c files must include only public headers + `internal.h` if needed — they must not reach into other providers.

## TODO.md is the spec

`TODO.md` contains the target architecture, API sketch, and phased roadmap. When adding features that sound architectural ("add anthropic", "add streaming", "add agent loop"), check `TODO.md` first — the shape is already designed and names/signatures are pre-decided. Don't invent parallel APIs.

## REPL

`src/tui/` is the terminal UI. `src/tui/tui.c` owns termbox2 lifecycle (`tui_init`/`tui_shutdown`) + shared draw helpers as they grow. `src/tui/repl.c` owns REPL state and event loop. Keep termbox flags out of `repl.c` — go through `tui.h`.

## General AI-agent reference

Use this checklist regardless of agent vendor/tooling:

1. Understand scope first: restate the requested outcome and touch only files needed for that outcome.
2. Respect boundaries: keep provider-specific JSON/parsing in `src/provider/`, keep `src/core/` provider-agnostic, keep HTTP details in `src/net/`.
3. Prefer minimal, reversible diffs: avoid opportunistic refactors during feature/bugfix work.
4. Follow memory policy: arena-first in core/provider; introduce `malloc` only with explicit justification.
5. Preserve API compatibility when possible: add new entry points before changing existing call sites.
6. Verify before claiming done: run build + relevant tests and report exactly what was run.
7. Report crisply: list changed files and why each changed.

For streaming/non-streaming changes specifically:

- Keep both code paths functional (`chat` and `chat_stream`) and avoid regressions in non-stream mode.
- Parse/handle provider error payloads explicitly (stream and non-stream), don't treat error JSON as success.
- Keep streaming callbacks low-latency: minimal per-chunk work in hot paths.
