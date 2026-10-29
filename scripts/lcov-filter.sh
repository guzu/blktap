#!/bin/sh
# lcov-filter.sh [-f RULES]... [-e RULE]... IN.info OUT.info: removes files, functions, lines (ranges or
# source text) from an lcov tracefile. See lcov-filter.py for the rules; scripts/cov-filters.conf is an example.
exec python3 "$(dirname "$0")/lcov-filter.py" "$@"
