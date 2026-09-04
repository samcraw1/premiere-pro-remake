/* oe_project_format.h — versioned JSON v1 project files (Phase 3).
 *
 * The document schema (integer-only; no float tokens anywhere):
 *
 *   {
 *     "obvious-edit-project": {
 *       "format-version": 1,
 *       "name": <string>,
 *       "frame-rate": { "num": <int>, "den": <int> },
 *       "media": [ { "ref": <int>, "path": <string> } ],
 *       "tracks": [
 *         {
 *           "kind": "video" | "audio",
 *           "clips": [
 *             {
 *               "media-ref": <int>,
 *               "position-us": <int>,
 *               "source-in-us": <int>,
 *               "source-out-us": <int>
 *             }
 *           ]
 *         }
 *       ]
 *     }
 *   }
 *
 * Strict v1: missing required members, unknown members, malformed
 * JSON, non-integer numbers, out-of-domain values, and
 * format-version > 1 all fail with a typed OE_PROJECT_FORMAT_ERROR
 * naming the defect. A failed load never hands back a half-built
 * project (rationale: tolerated unknown members would be silently
 * dropped on re-save, corrupting the user's document).
 *
 * Saves are atomic: the payload is written to a temp file in the
 * target directory, fsynced, then renamed over the target only on
 * success — a failed save leaves any pre-existing file byte-identical
 * (versioned-GKeyFile precedent, architecture.md).
 */

#pragma once

#include <glib.h>

#include "oe_project.h"

G_BEGIN_DECLS

#define OE_PROJECT_FORMAT_ERROR (oe_project_format_error_quark ())

GQuark oe_project_format_error_quark (void);

/**
 * OeProjectFormatError:
 * @OE_PROJECT_FORMAT_ERROR_SYNTAX: the file is not parseable JSON.
 * @OE_PROJECT_FORMAT_ERROR_MISSING: a required member is absent.
 * @OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER: a member the schema does
 *     not define is present (v1 is closed — never silently dropped).
 * @OE_PROJECT_FORMAT_ERROR_TYPE: right name, wrong JSON type (or a
 *     float where an integer is required).
 * @OE_PROJECT_FORMAT_ERROR_VALUE: a value outside its domain (bad
 *     rate, non-positive us, unknown kind, duplicate or unknown
 *     media reference, ...).
 * @OE_PROJECT_FORMAT_ERROR_VERSION: format-version newer than this
 *     build supports.
 * @OE_PROJECT_FORMAT_ERROR_IO: save-side file failure; any
 *     pre-existing file is untouched.
 */
typedef enum
{
  OE_PROJECT_FORMAT_ERROR_SYNTAX,
  OE_PROJECT_FORMAT_ERROR_MISSING,
  OE_PROJECT_FORMAT_ERROR_UNKNOWN_MEMBER,
  OE_PROJECT_FORMAT_ERROR_TYPE,
  OE_PROJECT_FORMAT_ERROR_VALUE,
  OE_PROJECT_FORMAT_ERROR_VERSION,
  OE_PROJECT_FORMAT_ERROR_IO,
} OeProjectFormatError;

/** The only format version this build reads and writes. */
#define OE_PROJECT_FORMAT_VERSION 1

/**
 * oe_project_format_save:
 * @project: the project to serialize.
 * @path: destination file path.
 *
 * Serializes the model to the v1 schema and writes it atomically:
 * temp file in @path's directory, fsync, rename over @path only on
 * success. On any failure the target directory is left without temp
 * residue and any pre-existing @path is byte-identical.
 *
 * Returns: TRUE on success, FALSE with @error set otherwise.
 */
gboolean oe_project_format_save (OeProject *project, const gchar *path, GError **error);

/**
 * oe_project_format_load:
 * @path: source file path.
 *
 * Parses and validates the document against the v1 schema, then builds
 * a fresh project. Strict: any deviation from the schema (including
 * unknown members and format-version > 1) fails with a typed error
 * naming the defect and its location (e.g. "tracks[0].clips[2]").
 *
 * Returns: (transfer full): a new project, or NULL with @error set.
 */
OeProject *oe_project_format_load (const gchar *path, GError **error);

G_END_DECLS
