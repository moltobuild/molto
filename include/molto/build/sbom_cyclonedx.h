#ifndef MOLTO_SBOM_CYCLONEDX_H
#define MOLTO_SBOM_CYCLONEDX_H

#include <stdio.h>

#include <molto/services/sbom_service.h>

/*
 * A bill of materials, as CycloneDX 1.6 JSON.
 *
 * The format was chosen because it models a dependency *graph* rather than a
 * list, which is what molto has: `components` are the packages and
 * `dependencies` are the edges, and both come straight out of a resolved
 * graph. Dependency-Track, Grype and Trivy read it without translation.
 *
 * Presentation only. Which packages belong in the document, what a missing
 * checksum means and what order they come in are decided in sbom_service; this
 * file knows the spelling of the keys and nothing else.
 *
 * Two fields the specification allows and this writer does not emit:
 * `metadata.timestamp` and `serialNumber`. Both would change on every run, and
 * a document that cannot be diffed against the last one is a document nobody
 * compares — the same reason Molto.lock is written sorted (RFC-0008). Both are
 * optional, so what comes out is still a conforming 1.6 document.
 */

/* Write `document` to `stream`. `tool_version` is what the `molto` entry under
   `metadata.tools` reports, passed in rather than read from the build so the
   output of a test is a function of its input alone. */
void sbom_write_cyclonedx(FILE *stream, const sbom_document *document, const char *tool_version);

#endif /* MOLTO_SBOM_CYCLONEDX_H */
