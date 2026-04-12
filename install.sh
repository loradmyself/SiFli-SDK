#!/usr/bin/env bash

set -e

basedir=$(dirname "$0")
SIFLI_SDK_PATH=$(cd "${basedir}"; pwd -P)
export SIFLI_SDK_PATH

if ! command -v uv >/dev/null 2>&1; then
    echo "ERROR: uv was not found in PATH. Please install uv before running install.sh." >&2
    exit 1
fi

uv run --with rich --with tomli_w --python 3.13.0 --no-project "${SIFLI_SDK_PATH}/tools/sdk_env.py" install "$@"
