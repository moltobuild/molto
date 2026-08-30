#!/bin/sh
#
# Turn a compile log into the number the Windows jobs exist to produce.
#
# `make -k` keeps going after a failure, so the log holds every error the tree
# has rather than the first one. What is wanted from it is not the scroll but
# three things: how many errors, across how many files, and — the one that
# actually directs the work — how many *distinct* problems, because eleven
# `realpath` sites are one afternoon and one `fork` is a week.
#
# Written to the job summary as well as to stdout, so the tally is on the run's
# front page and nobody has to open a log to see whether the port moved.
set -u

log=$1
what=$2

errors=$(grep -c 'error:' "$log")
files=$(grep 'error:' "$log" | cut -d: -f1 | sort -u | wc -l | tr -d ' ')

report() {
    echo "### $what"
    echo
    echo "$errors errors across $files files, of these kinds:"
    echo
    echo '```'
    grep 'error:' "$log" | sed 's/.*error: //' | sed 's/[0-9][0-9]*/N/g' \
        | sort | uniq -c | sort -rn | head -40
    echo '```'
}

report
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    report >> "$GITHUB_STEP_SUMMARY"
fi
