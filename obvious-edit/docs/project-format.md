# Project format

Obvious Edit project files are JSON documents, version 1, written and
read by `src/core/oe_project_format.[ch]` (Phase 3). The file extension
is `.oe`. The format carries the document — name, frame rate, media
references, tracks, clips — and deliberately nothing else: asset probe
metadata, thumbnails, and waveforms remain session state (regenerable,
never document state).

## Version 1 schema

The document root has exactly one member. Its value is the project:

```json
{
  "obvious-edit-project": {
    "format-version": 1,
    "name": "My Edit",
    "frame-rate": { "num": 30000, "den": 1001 },
    "width": 1920,
    "height": 1080,
    "media": [
      { "ref": 1, "path": "/media/interview.mp4" },
      { "ref": 2, "path": "/media/theme.png" }
    ],
    "tracks": [
      {
        "kind": "video",
        "clips": [
          {
            "media-ref": 1,
            "position-us": 2002000,
            "source-in-us": 0,
            "source-out-us": 5000000
          }
        ]
      },
      { "kind": "audio", "clips": [] }
    ]
  }
}
```

Member-by-member:

| Member | Type | Rules |
|---|---|---|
| `obvious-edit-project` | object | the only root member |
| `format-version` | integer | first member; must be exactly `1` (see versions) |
| `name` | string | project name, may be empty |
| `frame-rate` | object | `num` and `den` integers, reduced, `den > 0`, `num > 0` |
| `width` | integer | sequence width in pixels; positive and even. Always written; absent on read means 1920 (Phase 7-and-older files) |
| `height` | integer | sequence height in pixels; positive and even. Always written; absent on read means 1080 (Phase 7-and-older files) |
| `media` | array | `ref` (unique positive integer), `path` (string); document order is load order |
| `tracks` | array | `kind` is `"video"` or `"audio"`; array order is compositing order for video, mixing order for audio |
| `clips` | array | `media-ref` names a `media.ref`; positions integer microseconds; `source-out-us` must exceed `source-in-us`; clips are stored sorted by `position-us` |

A clip's timeline duration is always `source-out-us − source-in-us`,
including still images: a still's "source range" encodes its screen
duration (the model inserts stills at 5 s by default). Gaps are the
absence of clips, never placeholder elements.

`width` and `height` (Phase 8) are additive and backward-compatible:
newer writers always emit them, current readers backfill 1920×1080
when they are absent — a Phase 7 file (written before this field
existed) loads unchanged. When present they follow
the strict rules like any member (wrong JSON type is `TYPE`; zero,
negative, or odd values are `VALUE`). The evenness rule exists because
the export path emits yuv420p, whose chroma planes need even
dimensions. No version bump: the additive fields cannot orphan an
older file (it has none to reject) and a current reader never loses
data it read (absence is normalized to defaults before the model is
built).

Each clip also carries a `visual` member (Phase 9 Wave A) following
the same recipe: every writer emits it on every clip, and a reader
backfills the identity visual when it is absent — a Phase 8 file
loads unchanged and a Phase 9 file loads in a Phase 8 reader only by
dropping the member, which is exactly what the version does NOT do
(see below for why that is safe). The member is a closed object with
integer tokens only:

| `visual` member | Meaning | Identity |
|---|---|---|
| `pos-x`, `pos-y` | frame-pixel offsets from the centered anchor | 0 |
| `scale-permille` | uniform scale, 1000 = 1.0× (never a float) | 1000 |
| `rotation-cdeg` | rotation in 1/100 degree | 0 |
| `opacity` | 0–255 straight-alpha layer opacity | 255 |
| `crop-l/t/r/b` | crop insets in source pixels | 0 |
| `fade-in-us`, `fade-out-us` | reserved for Wave B fades (0 in Wave A) | 0 |

Wrong JSON types are `TYPE` (including float tokens where integers
are required), out-of-range values are `VALUE`, and unknown or
missing members inside `visual` are `UNKNOWN_MEMBER` / `MISSING` —
the member list is closed like the rest of the schema. No version
bump: a reader that predates `visual` never sees it, and a current
reader normalizes absence to the identity before the model is built.
Save-load-save is byte-identical because the writer's emission is
canonical.

## Strictness: no silent partial loads

Loading is strict and closed-schema. Any defect fails the whole load
with a typed error from `OE_PROJECT_FORMAT_ERROR` naming what is wrong:

- **`SYNTAX`** — the file is not JSON at all (corrupt, truncated).
- **`MISSING`** — a required member is absent, at any depth.
- **`UNKNOWN_MEMBER`** — a member outside the v1 schema, at any depth.
  Unknown members are never tolerated: a reader that skips them would
  silently drop the data on the next save.
- **`TYPE`** — a member has the wrong JSON type (including float
  tokens where integers are required: `"position-us": 1.5` is an error).
- **`VALUE`** — an integer/string is out of domain (zero denominator,
  unknown `kind`, non-positive rate, empty source range, unknown
  `media-ref`).
- **`VERSION`** — `format-version` is not `1` (see below).
- **`IO`** — the file cannot be read or written.

Integer-only serialization: every number in a saved file is an integer
token — rates travel as `num`/`den` pairs exactly, never as floats
(NTSC 30000/1001 is never 29.97). This and the strict member checking
keep `save → load → save` byte-identical for an unchanged project.

## Versions and migration

The version field exists so it can be checked. v1 readers reject any
`format-version` other than `1` with a typed error — including future
versions (2 and up) and anything older. There is no migration yet: the
first versioned change to this schema will define one. What will not
change: reading a newer file is always an explicit, reported failure,
never a guessed best-effort import.

## Atomic saves

Saving writes a temporary file in the target file's directory, fsyncs
it, and renames it over the target only after the full write succeeded.
A failed save leaves any pre-existing file byte-identical (the same
contract as the versioned GKeyFile layout persistence). No partial
project file can ever exist at the target path.

## Constraints inherited from earlier phases

- **Serialization library:** json-glib is a hard build dependency
  (locked since Phase 0). The format is JSON.
- **Time model floor:** integer microseconds and num/den rational
  rates only — no `double` anywhere in the model or in serialized
  state (architecture.md time-model floor).
- **Media is referenced, never embedded.** Project files store paths;
  probe metadata, thumbnails, and waveforms are session state and are
  re-derived after Open by re-importing each referenced path through
  the import worker (probe results re-mark assets OK / MISSING /
  UNSUPPORTED).
- **Undo/redo history** is not serialized in v1. If a later phase adds
  an operation log, it becomes a new schema version.
