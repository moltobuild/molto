#!/bin/sh
#
# Turn what the instrumented suite left behind into the number, and the list,
# that direct the next test.
#
# The percentage is the headline and is the least useful half. What a person
# does something with is the other half: which files under src/ the suite
# enters least, worst first. A total that moves says whether the tree is going
# the right way; a file at the top of that list says where to go next.
#
# The floor is why this can fail. A measurement nobody is accountable to is a
# number in a log, and `coverage.floor` turns it into a commitment — not "reach
# 90%", which is a target somebody picked, but "do not go below where we already
# are", which is a fact the tree earned. Raising it is a deliberate edit, the
# same shape as the Windows tally: the file records where the work stands and
# the job ends the way the tree really is.
#
# Written to the job summary as well as to stdout, so the number is on the run's
# front page and nobody has to open a log to see it move.
set -u

objects=$1
# The gcov that matches the compiler. A gcov older than the gcc that wrote the
# notes refuses them with "version 'B23', prefer 'B14'", which reads like a
# corrupt file and is a mismatched pair — and on a machine with three gccs
# installed, /usr/bin/gcov is whichever one the distribution picked.
gcov=${2:-gcov}
floor_file=coverage.floor

# gcov reports per object, and only for what the run actually touched. `-n`
# keeps it from writing .gcov files nobody reads; the numbers all arrive on
# stdout, in pairs of lines: the file, then how much of it ran.
#
# Everything outside src/ is dropped. The suite's own lines and moltest's are
# not what is being measured, and leaving them in would let a well-tested test
# file raise the figure for the code that ships.
report=$(mktemp)
trap 'rm -f "$report"' EXIT

find "$objects" -name '*.gcda' -exec "$gcov" -n -p {} + 2>/dev/null \
    | awk '
        /^File / {
            path = $2
            gsub(/'"'"'/, "", path)
            named = 1
            next
        }
        /^Lines executed:/ {
            # gcov ends a multi-file run with a total of its own, on a line
            # that no "File" precedes. Taken as a file it would overwrite the
            # last one measured with the figure for all of them.
            if (!named)
                next
            named = 0
            if (path !~ /^src\//)
                next
            # "Lines executed:83.33% of 12"
            split($0, part, ":")
            split(part[2], figure, "%")
            percent = figure[1] + 0
            total = $NF + 0
            # gcov emits a file once per object that pulled it in; the last
            # one wins, which is the same number every time.
            lines[path] = total
            ran[path] = int(percent * total / 100 + 0.5)
        }
        END {
            for (file in lines)
                printf "%s %d %d\n", file, ran[file], lines[file]
        }
    ' > "$report"

if [ ! -s "$report" ]; then
    echo "no coverage data under $objects, which is its own kind of red"
    exit 1
fi

covered=$(awk '{ sum += $2 } END { print sum + 0 }' "$report")
total=$(awk '{ sum += $3 } END { print sum + 0 }' "$report")
percent=$(awk -v c="$covered" -v t="$total" 'BEGIN { printf "%.1f", t ? c * 100 / t : 0 }')

summary() {
    echo "### Coverage"
    echo
    echo "**$percent%** of lines in src/ — $covered of $total."
    echo
    echo "Least covered, which is where a test is worth writing:"
    echo
    echo '```'
    awk '$3 > 0 { printf "%5.1f%%  %5d lines  %s\n", $2 * 100 / $3, $3, $1 }' "$report" \
        | sort -n | head -15
    echo '```'
}

summary
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    summary >> "$GITHUB_STEP_SUMMARY"
fi

if [ ! -f "$floor_file" ]; then
    echo "no $floor_file, so nothing to hold to; write $percent into it to start"
    exit 0
fi

# Comments and blank lines, so the file can say what it is and when it moves.
floor=$(grep -v '^[[:space:]]*#' "$floor_file" | grep -v '^[[:space:]]*$' | head -1)
echo "floor: $floor%"
awk -v now="$percent" -v was="$floor" 'BEGIN { exit (now + 0 < was + 0) ? 1 : 0 }' || {
    echo "coverage fell from $floor% to $percent%"
    echo "Either the new code needs a test, or $floor_file is being lowered on purpose."
    exit 1
}
