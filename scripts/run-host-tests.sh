#!/usr/bin/env sh
# Shared local+CI entrypoint for the WS-5 host-runnable pytest suite.
set -eu

# Ensure uv's user-install bin dir is on PATH (matches local + CI invocation).
PATH="$HOME/.local/bin:$PATH"
export PATH

exec uvx pytest tests/ "$@"
