# RFC 0012: The Build Viewport

- RFC Number: 0012
- Title: The Build Viewport
- Status: Draft
- Created: 2026-08-17

## Summary

This RFC defines how a build shows its progress on a terminal: the names of the
files being compiled live in a **region** at the bottom of the screen, redrawn
as the work moves and taken away when the work stops, and the inventory above
it **counts** the project's own sources rather than listing them.

It changes nothing about a build whose stderr is not a terminal. A pipe, a file
and a CI job keep exactly what they had — one line per source, no bar, no
region, and not one escape sequence — which is the same split RFC-0005 made for
`molto lint`: frames at a terminal, lines everywhere else.

It extends RFC-0011 rather than replacing any part of it. A diagnostic is still
composed into one block and handed to the report in one call; what changes is
that the thing it is atomic against is now several rows rather than one.

## Motivation

A build printed one line per source before compiling any of them. Molto's own
build is sixty-three sources, so touching a header cost sixty-three lines of
scrollback that nobody reads twice, plus a bar underneath them:

```
 ● project    build/compile_db.c
 ● project    build/compile_flags.c
 ...sixty-one more...
 ○ cached     20 files

 ████████████████████████████████ 100%

 ✓ Finished `debug` build in 0.30s
```

Those lines are worth something for the seconds in which the file is being
compiled and worth nothing afterwards, which is the definition of something
that belongs in a region rather than in a history. The same build now leaves
three lines behind.

The lines are not the only thing that went missing in them. The list said what
the build **would** do; nothing said what it **was** doing. A build stalled on
one slow translation unit and a build racing through two hundred looked
identical: a bar, moving or not.

## The region

A region is a block of rows anchored to the bottom of the screen. It holds, in
order:

- up to eight of the units being compiled at this instant, oldest first
- a line counting the ones that did not fit, when there are any
- the bar

```
 ● project    services/build_service.c
 ● project    build/report.c
 ◆ sqlite3    btree.c
 … and 5 more
 ████████░░░░░░░░░░░░░░░░  22%  47/210
```

A row has the shape of an inventory line, and deliberately so: the same glyph,
the same colour, the same field. What changes is what the field holds — a
package names itself there, and the project's own code keeps the origin word.
The two are composed by one function, so they cannot drift apart.

Ordered by arrival and not by the slot the unit was given, so a row does not
jump to a different file because the slot beside it was reused.

### Truncation is correctness

Every row is cut to the terminal's width before it is written, and the cut
counts columns rather than bytes.

This is not tidiness. Redrawing the region means moving the cursor up by a
number of **rows of the terminal**. A row 120 columns long on an 80-column
terminal is not one row: it is two. From that point on the region's count of
what it drew is wrong, every later move lands somewhere else, the erasures
blank rows that were not the region's, and the wreckage scrolls upward twenty
times a second.

It is the argument `progress_erase_line` already made — a run of spaces assumes
the terminal is at least as wide as the run is long, and it is not — carried one
step further. There the cost of being wrong was a scroll per frame; here it is a
region that no longer knows where it is.

Counting columns means an escape sequence costs none however many bytes it
takes, a UTF-8 character costs one however many bytes it takes, and neither is
ever cut through the middle. A row cut in the middle of colour closes it, or
the colour runs on into the row below and out onto the prompt. A row that
opened no colour gains no escape, so `NO_COLOR` still means what it says.

The count is a character count and not a display width, inherited from
`text_columns` along with its limitation: CJK and emoji measure short. Doing
better means `wcwidth`, which means a locale, which Molto does not set.

### Height

`min(in flight, 8, rows / 3)`.

The third is what guarantees the rows, the overflow line and the bar all fit on
the screen at once. A region taller than the screen scrolls its own anchor away
and can never find it again. On a screen too short to spare three rows the
answer is no file rows at all: **what shrinks is the list, and the bar always
survives.**

Eight rather than `-j`: the region is a window onto the build, not a mirror of
the worker pool. `-j 32` on a forty-row terminal would otherwise fill the
screen.

### Measured every frame

The terminal is measured on every redraw — `TIOCGWINSZ`, then `COLUMNS` and
`LINES`, then eighty by twenty-four — rather than once at the start.

A window dragged narrower halfway through a build is ordinary, and a region
drawn against the width of a minute ago wraps, which is the one failure
everything above is arranged to avoid. The cost is one `ioctl` per frame; what
it buys is not owning `SIGWINCH`.

### The cursor is not hidden

Hiding the cursor means restoring it from a signal handler, because Ctrl-C
during a long build is the common case and not the exotic one. Doing that
properly is a static file descriptor, a handler restricted to async-signal-safe
calls, the default disposition reinstated so the exit status stays 130, and the
same again for SIGTERM, SIGHUP and SIGQUIT — none of which exists in Molto
today, all of it in the service of a visual detail, in a process that forks
compilers.

The cost of not hiding it is a cursor parked at the end of the last row, which
is where the bar already left it. This is revisited if a signal handler ever
arrives for a reason that justifies itself.

## The inventory

On a terminal, the origins whose lines are sources rather than packages —
`project` and `tests` — are counted in one line each:

```
 ◆ registry   sqlite3 v3.50.3
 ◇ modules    network
 ● project    62 files
 ○ cached     20 files
```

A package is unchanged: its line was already one piece of work however many
sources it has.

The reason is not brevity for its own sake. The region below is about to name
each of those sources as it compiles it, one at a time, and saying them twice
is filling the scrollback with what the region says better and then throwing
the region away. The counted line has the shape the `cached` line already had,
which is the sign that this report was counting rather than listing before this
RFC touched it.

Off a terminal there is no region to defer to, so the line per source stays. It
is the whole of the record there, and it is what a CI job should keep.

## What a finished build leaves

The region goes, whether the build worked or not. What survives is the
inventory and one verdict.

A list of files being compiled stops being true the instant the build stops. A
bar left behind makes a claim about the run: a full one over a failure claims it
worked, which is the reason a failed build already erased it, and a full one
over `✓ Finished` says a second time what the tick already said. The argument
that applied to the failing case applies to both.

## What this does not do

- **No alternate screen.** A build is not an application you enter and leave;
  it prints things you keep.
- **No SIGWINCH handler.** Measuring per frame answers the same question
  without owning a signal.
- **No `wcwidth`.** It means a locale, and Molto sets none.
- **No hidden cursor**, for the reasons above.
- **The region is not the diagnostics.** A compiler's words go through
  `build_report_message` and are never cut: a truncated error message hides the
  error. Only the region's own rows are cut, because only the region's own rows
  are counted in terminal rows.
- **The link step is not named.** Linking happens after every compilation, when
  the region has nothing left to list and the bar is at its end.

## Related RFCs

- **RFC-0011** (Build Diagnostics) defines the frame a diagnostic is drawn in
  and the single-call contract that makes it atomic against the region. This
  RFC widens what it is atomic against.
- **RFC-0005** (Style and Analysis) established the terminal/non-terminal split
  for `molto lint`. This applies it to a build.
- **RFC-0007** (Build) defines the plan that counts the units, which is what
  gives the bar an honest denominator and lets the region exist at all.
- **RFC-0015** (Build Pipeline and Transforms) makes `generate` a barrier so
  that denominator is still final, and gives a plugin's step an origin to be
  named by.
