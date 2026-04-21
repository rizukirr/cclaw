# CClaw

> ## Warning
> 
> This repository is an early-stage work in progress.
> 
> It is not ready for production use yet, and there is nothing broadly useful for end users at this time.
> 
> Expect frequent breaking changes while the architecture is still being built.

## What this repository is

CClaw is an LLM-agnostic agentic AI framework written in C11.

Long-term direction: a portable AI agent runtime that can run across desktop, server, edge, and embedded environments (with target-appropriate capability profiles).

Current status:

- Core initialization and provider selection are in place.
- OpenAI Responses API integration exists for chat (non-streaming and streaming paths).
- Terminal UI (TUI) is available for interactive testing.

## Project direction

- Build a general-purpose AI agent foundation that can integrate with different LLM providers.
- Keep core logic provider-agnostic through the provider vtable boundary.
- Support both non-streaming and streaming interactions as first-class paths.
- Evolve toward broad portability, including constrained targets, once the architecture is stable.

## Current limitations

- Architecture is still evolving.
- Public API and internal structure may change without notice.
- Features described in `TODO.md` are not fully implemented yet.
- Embedded-focused runtime profiles are not implemented yet.

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/bin/cclaw
```

## Run tests

```bash
cd build && ctest --output-on-failure
```

## Configuration

Create a `.env` file with provider credentials before running `cclaw`.

At minimum, current OpenAI setup expects:

- `OPENAI_API_KEY`
- `OPENAI_MODEL`

## Notes for contributors

- Follow `AGENTS.md` for architecture boundaries and agent workflow guidance.
- Keep changes focused and reversible.
- Verify build and relevant tests before considering work complete.
- Prefer minimal, clean, and optimized algorithms that fit current constraints.
