# Portable package builder

Turns a finished Dolphin build plus a Remix runtime drop into a release zip.

```powershell
.\Tools\PortablePackage\Package-Portable.ps1 -Version 0.0.7
```

That is the whole thing. It stages, scrubs, versions, verifies, compresses, and
writes a provenance record — about 30 seconds end to end, most of it zipping.

This script **does not build anything**. Build Dolphin and the runtime first by
whatever means you normally use.

---

## 1. What goes in

| Input | Default location | Override |
|---|---|---|
| Dolphin build output | `build\release\x64\Binaries` | `-BinariesDir` |
| Remix runtime drop | `..\dolphin-remix-runtime` | `-RuntimeDir` |
| Runtime source repo | `..\dxvk-remix-dolphin-atmos` | `-RuntimeRepo` |
| Docs | `Tools\PortablePackage\docs` | — |
| Skeleton | `Tools\PortablePackage\skeleton` | — |
| Output | `..\_releases` | `-OutDir` |

The runtime's `d3d9.dll` is copied in as **`d3d9-remix.dll`**. That rename is
load-bearing; see §2 of the shipped README.

## 2. What comes out

In `_releases\`:

- `Dolphin-RTX-Remix-portable-vX.Y.Z.zip` — the release. Flat layout, no wrapper
  folder, matching v0.0.3 through v0.0.6. Pass `-RootFolder` to wrap it.
- `Dolphin-RTX-Remix-portable-vX.Y.Z\` — the staged tree the zip was made from,
  kept so you can look at what shipped.
- `...release.txt` — both repos' commits, the runtime's identity and hash, the
  zip's hash, and the input paths.
- `...zip.sha256` — checksum in the usual format.

## 3. Releasing a new version

1. Build Dolphin. Build or refresh the runtime drop.
2. Add a `## vX.Y.Z — YYYY-MM-DD` section at the top of `docs\CHANGELOG.md`.
3. Run the script. With no `-Version` it takes the newest changelog heading and
   that entry's date, so step 2 is the only place a version number is typed.
4. Read the warnings. They are the point of the tool.

### Useful flags

| Flag | Why |
|---|---|
| `-NoZip` | Stage and verify without spending time compressing. Use while iterating. |
| `-Force` | Replace an existing staged folder or zip for this version. |
| `-RuntimeRepo <path>` | Override the manifest's runtime checkout. Cross-checked against the DLL — see §5. |
| `-RootFolder` | Wrap zip contents in one top-level folder. |
| `-AllowLargeFiles` | Permit a file over 260 MB besides the runtime. You almost never want this. |

## 4. Changing what ships

Edit **`package.manifest.psd1`**, not the script. It holds the required-input
lists, the exclusion patterns, the empty directories, and the post-staging
assertions. The script reads it and contains no file names of its own.

The emulator and runtime trees are copied wholesale minus an exclusion list, so
a new Qt DLL or `Sys\` subfolder ships automatically. A new kind of dev
droppings does not — it gets caught by the `Verify` block and stops the release,
which is the intended direction: new build outputs are routine, new junk next to
`Dolphin.exe` is something a human should look at once.

### Docs are templated

`docs\` holds the four shipped documents. `{{VERSION}}`, `{{DATE}}`,
`{{DOLPHIN_COMMIT}}`, `{{RUNTIME_SHA256}}` and friends are substituted at package
time; `VERSION.txt` is entirely generated this way. A placeholder the script
cannot fill stops the release rather than shipping literal braces.

Only the *current-version banner* is templated. Prose like "new in v0.0.4" is
history and stays literal.

Sources must be UTF-8. The rendered output is scanned for mis-decoded text,
because these documents are full of em dashes and a mangled one is invisible to
whoever shipped it and obvious to everyone else.

## 5. The checks that stop a release

**Missing inputs.** Every name in the manifest's `Required` lists must exist
before anything is copied.

**A runtime that is not this fork.** The DLL is scanned for its embedded
`remix-...+<commit>` string. A stock RTX Remix release has no such string and
would fail at load with `INCOMPATIBLE_VERSION`; the ABI number proves nothing,
since different forks have shipped `0.1000.0` from different lineages.

**A `-RuntimeRepo` that did not build this DLL.** The commit in the embedded
string is compared against that checkout's `HEAD`. On a mismatch the git details
are dropped rather than recorded, because a plausible-looking wrong commit is
worse than no commit.

**Forbidden files in the staged tree.** Checked after staging, independently of
the exclusion rules that were supposed to prevent them. v0.0.6 shipped a 242 MB
`d3d9-remix.dll.before-ignore-wins.bak` because a blocklist only knows about
mistakes that have already been made once.

**Implausible size.** Under 600 MB means something did not get copied; over
1200 MB means something got copied twice.

### Warnings that do not stop a release

- **Dirty working tree** — the recorded commit will not fully describe the
  binaries.
- **Stale binaries** — `Dolphin.exe` is older than `HEAD`, *or* older than an
  uncommitted source edit. Both are checked, and the second matters more: a
  binary built after the last commit still misses everything edited since, and
  on this project the working tree is usually where the newest code lives. The
  offending files are listed by name and timestamp.
- **No dated changelog entry** — today's date is used instead.

These are judgement calls, so they are yours to make.
