# Proposal 0009: compatibility policy and what 1.0 freezes

- Status: proposed
- Date: 2026-11-20

## Motivation

The 0.x releases have changed meaning more than once in a patch release: `for (k, v in map)`, `==` on a struct that implements `Equatable` (0.4.3), the default optimisation level (0.4.4), what `Async.Spawn` runs on (0.4.3), the layout of the standard library, and the name of `build/zevpm`. That was fine while there were no users. Before 1.0 we need to say what will not change, so that a program written for 1.0 keeps building with every 1.x.

The list of what is likely to break (kept in the project as `v1-breaking-changes.md`) is the input of this proposal.

## Proposal

### 1. What 1.x keeps (frozen)

- **The language**: the grammar (Appendix A of the specification), the type rules, the meaning of every construct, the reserved words, the visibility rule (an upper-case first letter is public), and the run-time behaviour that the specification states (`Panic` on overflow and on an index out of range, `defer` per block, and so on).
- **The public API of the standard library**: the names, the types and the documented behaviour of the modules that the specification lists under "stable" (section 2). Adding a function, a method or a module is allowed. Removing or changing one is not.
- **`zevic.toml` and `zevic.lock`**: the keys and their meaning. A new key may be added. A program that does not use it keeps working, and an old `zevpm` that meets an unknown key reports it as an error.
- **The C boundary**: `extern "C"`, `export "C"`, the C types of the standard library (`CInt`, `CString`, ...), and the format of the header that `--emit-header` writes.
- **The commands of `zevpm`** (`new`, `build`, `run`, `test`, `get`, ...) and their meaning, and the exit codes.

### 2. What 1.x does not promise

- The LLVM IR that `--emit-ll` writes. It is an internal format.
- The text of the error messages and the notes (they keep getting better). Tools should use the exit code.
- The details of `zevfmt` output: see 4 below.
- The options of the compiler that are marked "experimental" in `zevic --help`, and the modules and functions marked `Experimental` in the specification.
- The speed, the size of the executable, and the layout of the memory.
- Anything that the specification does not describe.

### 3. The standard library has two parts

- **Core** (frozen): `Core` (`Option`, `Result`, `Error`, `ErrorKind`, ...), `Text`, `Console`, `Env`, `Fs`, `Path`, `Process`, `Time`, `Math`, collections, `Json`, `Thread`, `Async`, and the conversions.
- **Outer** (not frozen until it is declared stable one by one): `Http`, `Regex`, `Zip`, `Random`, `DateTime`, ... Each one is listed as `Stable` or `Experimental` in the specification. An `Experimental` module may change in a minor release, and the change is written in the CHANGELOG under "Experimental".

A module that is better kept outside (`Http` is the first candidate, because of `zevic-lang/curl`) can move to its own package before 1.0. A package has its own version and is not bound by this policy.

### 4. `zevfmt`

Frozen at 1.0: a file that `zevfmt -check` accepts at 1.0 is accepted by every 1.x. A change that would reformat existing code is made only in a new major version, or behind an option that is off by default.

### 5. Changes that make an existing program fail to compile

In 1.x these are allowed only for a bug fix (the compiler accepted something the specification says is an error), and the CHANGELOG says so under "Fixes that can break". Adding a new reserved word, a new variant to a public `enum` of the standard library, or a new required method to an `interface` of the standard library is **not** a bug fix.

To make that safe, before 1.0:

- **`ErrorKind`** (and every other public `enum` of the standard library that is expected to grow) is documented as "open": a `match` on it must have `_`. The compiler reports a missing `_` for an enum marked open (the marker is a small attribute-like form that is itself a proposal of its own; until then it is a rule of the specification for named enums).
- **Reserved words** that the "future considerations" need are reserved now: `unsafe`, `where`, `type`, `union` (to be settled). Reserving a word we never use costs nothing; adding one later breaks code.

### 6. Deprecation

- A function or a module that will go away is marked `Deprecated` in the specification and the CHANGELOG, and the compiler prints a warning that names the replacement.
- It stays for the rest of the major version. It can be removed only in the next major version.

### 7. The road to 1.0

1. **0.5 to 0.8**: the breaking changes that this proposal asks for are made one at a time, each with a CHANGELOG line under "Breaking".
2. **0.9.x**: release candidates. Only fixes, and changes that the candidates show to be necessary. Every breaking change from here must be argued in an issue.
3. **1.0.0**: the specification, the standard library lists (Core and Outer, with `Stable` or `Experimental`) and this policy are tagged together.
4. **After 1.0**: new features go in a minor release. A breaking change means 2.0.

## Alternatives

- **Freeze everything**: simple, but the standard library is the part that most needs a second look, and the packages (`Http`, `Regex`) are still moving.
- **Never reach 1.0**: users cannot tell what is safe. This is the situation that this proposal ends.
- **Semantic versions per module**: more precise, but the language, the compiler and the standard library are released together today. A package split can be done later for the Outer modules.

## Impact

- The specification gets a chapter "Compatibility", a `Stable` / `Experimental` mark on each module of the standard library, and the list of reserved words (extended).
- `Core.zev`: `ErrorKind` becomes an open enum. Existing `match` expressions on it in the repository need a `_`.
- The compiler: a warning for `Deprecated`, a check for open enums, new reserved words.
- `zevfmt`: no change now; its behaviour is frozen at 1.0.
- CHANGELOG: new headings "Breaking", "Experimental" and "Fixes that can break".
- The site: a page for the policy, and a note in the release notes of each 0.x about the breaking changes.

## Open questions

- Which modules are Core? (the list above is a first guess)
- The marker for an open enum: a rule of the specification only, or syntax?
- Do we want a `1.0` that already contains `unsafe` and `NoGC`, or only reserve the words?
- `Http`: keep in the standard library as Experimental, or move to a package?
