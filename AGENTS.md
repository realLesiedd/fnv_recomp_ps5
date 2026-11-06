# Working on this repository

Rules for the AI assistants that work here (Claude, and FNV HELPER), and for anyone else who sends changes. The
owner decides anything this file doesn't cover. Read this, [STATUS.md](STATUS.md) and
[docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md) before starting something new.

## Never commit

- **Game files of any kind.** `FalloutNV.exe`, `Data/` (`.esm`, `.bsa`, loose assets), the game's `.ini` files,
  textures or sounds taken from the game, or anything rebuilt from them.
- **Anything generated from the executable.** `gen/`, its import table, and dumps or listings of its code. A
  few addresses or a short instruction sequence quoted in a note are fine.
- **Build outputs.** `eboot.bin`, `build/`, `build-ps5/`, `.elf` files and zips.
- **Players' files.** Saves, `fnv.log`, `_d3d9.log` and screenshots. Quote the few lines that matter.
- **Personal information.** No names, emails, IP addresses, console IDs, account or PSN names, anywhere: code,
  docs, commit messages, issues or pull requests.
- **Anything copied from fnv_decomp.** It grants no licence. Its function list is read from a local checkout at
  build time only.
- **Secrets.** Keys, tokens and passwords never go in the repo or in a comment.

## Who does what

| | Owns | Does |
|---|---|---|
| **Owner** | Every decision, and merging | Tests on the PS5 and sends logs |
| **Claude** | The runtime (`rt/`, except the files below), `CMakeLists.txt`, `ps5/` (build script, DXVK patch, packaging), `tools/recomp.py`, `tools/hot_gen_files.txt`, and the per-version notes in STATUS.md | Stability and performance work; all builds; reading the owner's logs; reviewing FNV HELPER's pull requests |
| **FNV HELPER** | "Future plans" in STATUS.md, `docs/OPEN_QUESTIONS.md`, `docs/research/`, `CHANGELOG.md`, `.github/` | Research, docs, release paperwork, and small code changes that Claude builds |

To change a file the other assistant owns, ask in the issue first. Never edit the other assistant's branch.
`README.md` is shared: keep changes small, and say in the pull request what changed.

## How work moves

1. **Handoff.** An issue titled `[Handoff] <task>`: what, why, which files, and how we'll know it's done. Work for
   FNV HELPER also carries the label `fnv-helper`.
2. **Pick-up.** A comment that it's been picked up, then the plan or the findings. Questions go in that thread.
3. **Work.** On its own branch, in a pull request that links the issue.
4. **Report on the pull request.** Post "done", "ready for review" and questions as comments on the pull request,
   not only on the issue. Claude is told about pull-request comments automatically, but not about issue comments.
5. **Sign comments.** Start with "Claude here" or "FNV HELPER here".
6. **Batch research.** Put several findings in one docs pull request, or in one comment, not one pull request each.

## Checking for work

Everything both assistants post appears under the owner's GitHub account, so GitHub never marks it as new. Handoff
issues carry the label `fnv-helper` (Claude adds it when opening one). At each check, FNV HELPER:

1. Reads the comments on its open pull requests and answers them.
2. Lists the open issues labelled `fnv-helper`. One without an "FNV HELPER here" pick-up comment is new work:
   comment that it's picked up, then start.
3. Re-reads every thread it has commented in for replies, rather than relying on an earlier check.
4. Works in this order: a bug the owner reported, then work that's been approved, then the order given in the
   thread.

Waiting for Claude's answer on one issue doesn't stop work on the others.

## Merging

- **Docs only:** the owner merges after Claude comments "ready".
- **Code:** Claude builds it into a test build, the owner tests it on the PS5, then the owner merges.
- Nobody merges their own pull request without the owner's go-ahead.
- Commits on `main` appear under the owner's GitHub account (noreply address). Assistants don't add their own
  names or sign-off lines to commits.

## Builds and testing

- Only Claude builds: it needs the owner's own `FalloutNV.exe` and the PS5 toolchain. FNV HELPER doesn't need
  the game, and must never ask for the executable, game files or any login.
- Builds cost compute. Changes are batched so the owner tests one build at a time, and builds happen only when
  there's something to test.
- The console: PS5, system software 13.42. The app is `PPSA97701`. Its logs are `fnv.log` and `_d3d9.log` in
  `/data/homebrew/PPSA97701/`.

## Technical rules learned the hard way

- **PS5 system functions are direct imports.** Looking them up at run time (`sceKernelDlsym`, `dladdr`) fails on
  the console. A direct import that the system can't resolve may stop the app from starting, so test it first.
- **The main thread is the bottleneck.** Anything that runs every frame, or every half second, on it must be
  cheap. Work that only feeds an overlay row runs only while that row is shown.
- **The game's optional threading stays off.** Threaded FaceGen deadlocked save loads (v36). Threaded AI,
  morpher, particles, blood and audio gave no speed-up.
- **The frame cap never goes above 60.** The engine's physics break above 60 fps.
- **Keep the hook list short.** `HOOKS` in `tools/recomp.py` replaces game functions; each one is a maintenance
  cost.
- **Test before claiming.** Say what was measured, on which build and machine (Linux desktop, or the PS5), and
  mark anything unconfirmed.

## Writing

Plain English and short sentences. Name files and functions, and mark speculation. Player-facing text (README,
CHANGELOG) avoids internal detail.

## Licence

The project's source is GPL-3.0-or-later ([LICENSE](LICENSE)). Third-party code keeps its own licence
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). New third-party code or assets need an entry there and in
[CREDITS.md](CREDITS.md).
