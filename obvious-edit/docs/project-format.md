# Project format

**Status: reserved.** Phase 0 does not read or write project files.

This document reserves the design space for the Obvious Edit project
format so early structural decisions are not made implicitly by file
plumbing that sneaks in early.

## Phase 0 decisions that already constrain the format

- **Serialization library:** json-glib is a hard build dependency as of
  Phase 0. The project format will be JSON. There is no parser and no
  schema yet — the dependency is locked so later phases do not fork the
  dependency list.
- **Time model floor:** later phases specify integer timestamps with a
  rational time base. Any temporary time bookkeeping added before then
  must be trivially replaceable (no `double` seconds in APIs that would
  leak into serialized state).

## Phase 2 note: imported-asset records are session-transient

Phase 2 added asset records (paths, probe metadata, thumbnails,
waveforms) and a derived-media cache — none of it persists into any
project file. The asset library lives and dies with the process; the
`$XDG_CACHE_HOME` cache is a performance artifact, regenerable at any
time and never treated as document state. No persistence hardening
happens here: when the project phase defines save/load it will define
which of this (if any) becomes document state, and the current
session-transient behavior deliberately does not constrain that choice.

## What the format must support (from the product goals)

- Multi-track sequence definitions with per-clip trims and positions.
- Non-destructive references to source media (never embedded copies).
- Undo/redo history or an operation log sufficient to rebuild it.
- A version field with explicit migration semantics.

## Planned evolution

The format document gains its actual schema, versioning rules, and
migration policy in the phase that introduces save/load. Until then this
file is the contract that Phase 0 does NOT define one — preventing
accidental ad-hoc persistence from hardening into the format.
