# Fuzzing the parsers

Every target here drives a reader that is handed text Molto did not write: a
`Project.toml` from a dependency's tree, a recipe the registry served, what a
compiler printed, what a plugin answered. Those are the inputs no test can be
written against, because the point of them is that nobody thought of them.

| Target | Reads | Where it comes from |
|---|---|---|
| `fuzz_toml` | `toml_parse` and every accessor | manifests, recipes, `Molto.lock`, pickup's answer |
| `fuzz_json` | `json_parse` and the whole walk | `format.json`, `linter.json`, the registry |
| `fuzz_semver` | `semver_parse`, `_compare`, `_highest`, `_sort_desc` | version strings the registry serves and never interprets |
| `fuzz_diagnostic` | both diagnostic grammars, plus the cache round trip | whatever a compiler and a linker wrote |
| `fuzz_depfile` | `depfile_parse` | what the compiler wrote for `-MF` |
| `fuzz_ir` | `ir_read_json` **and `ir_validate`** | a frontend plugin's document |

`fuzz_ir` is the one that matters most. `ir_validate` is what stands between a
plugin's document and Molto doing what it says, so a document that slips past it
is third-party code choosing what runs on the user's machine.

`fuzz_semver` checks a property rather than only looking for a crash: an
ordering that is not antisymmetric sorts differently depending on how the list
arrived, which is a resolution that is not reproducible.

## Running them

```sh
make fuzz-corpus                        # replay what is committed
make fuzz-run                           # search. FUZZ_TIME=<seconds> per target
make fuzz-run FUZZ_TARGETS=toml FUZZ_TIME=1800
make fuzz FUZZ_CC=clang-19
```

`fuzz-corpus` is a test — fast, deterministic, and the gate every pull request
runs. `fuzz-run` is a search, and the two are not interchangeable: a search
finds something or it does not, and a minute of it finds nothing an hour has not
already found. It writes what it generates under `build/`, never into
`fuzz/corpus`, so the corpus in the repository stays what a person put there.

## Where each half runs

| | Where | Budget |
|---|---|---|
| `fuzz-corpus` | `ci.yml`, every pull request | seconds |
| `fuzz-run` | `fuzz.yml`, nightly and on demand | 5 minutes a target, one job each |

The search is not on a pull request on purpose. It would fail whichever pull
request happened to be open when it found something — never the one that caused
it — and the budget a pull request can afford is the budget at which fuzzing
does not work. Nightly it also **keeps its corpus** between runs, so each night
starts where the last one stopped instead of re-walking the shallow end of the
same parser.

Touched a parser and want it searched now? `workflow_dispatch` on the Fuzz
workflow takes a `seconds` input.

## When it finds something

libFuzzer writes the input that crashed to `crash-<sha1>` in the working
directory, and CI uploads it as an artifact. That file is the bug report:

```sh
./build/fuzz/bin/fuzz_toml crash-abc123     # reproduce it
```

Fix the parser, then **commit the crashing input into `fuzz/corpus/<target>/`**.
That is what turns one finding into a regression test: `fuzz-corpus` replays it
on every run from then on, and no search has to rediscover it.

## A local trap that is not a bug in Molto

On a recent kernel with an older clang (14, say), these binaries segfault at
startup perhaps one run in eight — before libFuzzer prints its first line, and
with no crash file written. It is AddressSanitizer's shadow mapping losing a
race with address-space randomisation, and it is fixed in LLVM 18 and later.

Two ways past it, and the first is better:

```sh
make fuzz FUZZ_CC=clang-19    # the version CI pins
setarch -R make fuzz-run      # or take randomisation out of it
```

A segfault with an empty log and no `crash-` file is this, not a finding.
