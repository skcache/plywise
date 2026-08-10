#!/bin/sh
set -eu

port=${PCT_PORT:-8787}
exec curl --fail --silent "http://127.0.0.1:${port}/api/ready"
