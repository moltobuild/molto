#include <molto/build/sbom_cyclonedx.h>

#include <molto/util/json_write.h>

#include <stdio.h>
#include <string.h>

#define SPEC_VERSION "1.6"
#define BOM_FORMAT "CycloneDX"

/* The revision of this document. Not the spec's version and not molto's: a BOM
   describing the same build twice is the same revision, which is why it is a
   constant here and not a clock. */
#define BOM_REVISION "1"

/* CycloneDX component types. The package under description is what gets built
   — an application — and everything it links is a library. */
#define TYPE_APPLICATION "application"
#define TYPE_LIBRARY "library"

/* Scopes. `excluded` is CycloneDX for "present in the graph, not in the
   artifact", which is exactly a development dependency. */
#define SCOPE_REQUIRED "required"
#define SCOPE_EXCLUDED "excluded"

/* Room for `pkg:generic/<name>@<version>`. */
#define PURL_MAX (DEP_NAME_MAX + DEP_VERSION_MAX + 32)

/* Properties are the escape hatch for what CycloneDX has no field for, and
   they are namespaced so a consumer can tell whose they are. */
#define PROPERTY_SOURCE "molto:source"
#define PROPERTY_SCOPE "molto:scope"
#define PROPERTY_UNVERIFIED "molto:unverified"

static bool stated(const char *value) { return value != NULL && value[0] != '\0'; }

/* One `"key": "value"`, skipped entirely when there is nothing to say. An
   empty description is not a description of nothing; it is silence. */
static void write_optional(json_writer *writer, const char *key, const char *value) {
    if(stated(value))
        json_write_field(writer, key, value);
}

/* `licenses` is an array of objects, and the object carries `expression`
   rather than `id` because what molto validates is an SPDX *expression*:
   `MIT OR Apache-2.0` is a legal value and is not an identifier. */
static void write_licenses(json_writer *writer, const char *license) {
    if(!stated(license))
        return;
    json_array_open(writer, "licenses");
    json_object_open(writer, NULL);
    json_write_field(writer, "expression", license);
    json_object_close(writer);
    json_array_close(writer);
}

static void write_reference(json_writer *writer, const char *type, const char *url) {
    json_object_open(writer, NULL);
    json_write_field(writer, "type", type);
    json_write_field(writer, "url", url);
    json_object_close(writer);
}

static void write_references(json_writer *writer, const char *homepage, const char *repository) {
    if(!stated(homepage) && !stated(repository))
        return;
    json_array_open(writer, "externalReferences");
    if(stated(homepage))
        write_reference(writer, "website", homepage);
    if(stated(repository))
        write_reference(writer, "vcs", repository);
    json_array_close(writer);
}

static void write_property(json_writer *writer, const char *name, const char *value) {
    json_object_open(writer, NULL);
    json_write_field(writer, "name", name);
    json_write_field(writer, "value", value);
    json_object_close(writer);
}

/* What CycloneDX has no field for: where molto fetched it from, with the
   revision already resolved, and the fact that a path dependency's bytes could
   not be verified against anything. */
static void write_properties(json_writer *writer, const sbom_component *component) {
    json_array_open(writer, "properties");
    if(stated(component->source))
        write_property(writer, PROPERTY_SOURCE, component->source);
    write_property(writer, PROPERTY_SCOPE, component->ships ? "runtime" : "dev");
    if(!component->verified)
        write_property(writer, PROPERTY_UNVERIFIED, "true");
    json_array_close(writer);
}

static void write_hashes(json_writer *writer, const sbom_component *component) {
    if(!component->verified)
        return;
    json_array_open(writer, "hashes");
    json_object_open(writer, NULL);
    json_write_field(writer, "alg", "SHA-256");
    json_write_field(writer, "content", component->checksum);
    json_object_close(writer);
    json_array_close(writer);
}

/* `pkg:generic/<name>@<version>`.
 *
 * generic, because C has no package ecosystem purl knows about, and no
 * vulnerability database is indexed by one that it invented here. The download
 * URL and the digest belong in a purl's qualifiers, and they are deliberately
 * left out: they would need percent-encoding to be legal, and both are already
 * in the document — the digest under `hashes`, the origin under
 * `molto:source`. */
static void write_purl(json_writer *writer, const sbom_component *component) {
    char purl[PURL_MAX];
    if(stated(component->version))
        snprintf(purl, sizeof purl, "pkg:generic/%s@%s", component->name, component->version);
    else
        snprintf(purl, sizeof purl, "pkg:generic/%s", component->name);
    json_write_field(writer, "purl", purl);
}

static void write_component(json_writer *writer, const sbom_component *component) {
    json_object_open(writer, NULL);
    json_write_field(writer, "type", TYPE_LIBRARY);
    /* The name alone is a unique reference: one name is one package across the
       whole graph (RFC-0008), which is the uniqueness CycloneDX asks for. */
    json_write_field(writer, "bom-ref", component->name);
    json_write_field(writer, "name", component->name);
    write_optional(writer, "version", component->version);
    json_write_field(writer, "scope", component->ships ? SCOPE_REQUIRED : SCOPE_EXCLUDED);
    write_purl(writer, component);
    write_optional(writer, "description", component->description);
    write_licenses(writer, component->license);
    write_hashes(writer, component);
    write_references(writer, component->homepage, component->repository);
    write_properties(writer, component);
    json_object_close(writer);
}

static void write_authors(json_writer *writer, const manifest_about *about) {
    if(about->author_count == 0)
        return;
    json_array_open(writer, "authors");
    for(size_t i = 0; i < about->author_count; i++) {
        json_object_open(writer, NULL);
        json_write_field(writer, "name", about->authors[i]);
        json_object_close(writer);
    }
    json_array_close(writer);
}

/* The package the document is about, which CycloneDX keeps apart from the
   things it depends on. */
static void write_root_component(json_writer *writer, const sbom_document *document) {
    json_object_open(writer, "component");
    json_write_field(writer, "type", TYPE_APPLICATION);
    json_write_field(writer, "bom-ref", document->name);
    json_write_field(writer, "name", document->name);
    write_optional(writer, "version", document->version);
    write_optional(writer, "description", document->about->description);
    write_licenses(writer, document->about->license);
    write_references(writer, document->about->homepage, document->about->repository);
    write_authors(writer, document->about);
    json_object_close(writer);
}

static void write_metadata(json_writer *writer, const sbom_document *document,
                           const char *tool_version) {
    json_object_open(writer, "metadata");
    json_object_open(writer, "tools");
    json_array_open(writer, "components");
    json_object_open(writer, NULL);
    json_write_field(writer, "type", TYPE_APPLICATION);
    json_write_field(writer, "name", "molto");
    json_write_field(writer, "version", tool_version);
    json_object_close(writer);
    json_array_close(writer);
    json_object_close(writer);
    write_root_component(writer, document);
    json_object_close(writer);
}

/* True when `name` has a component in the document.
 *
 * A `dependsOn` naming something that is not there is a dangling reference,
 * and a consumer is entitled to reject the whole document over one. It cannot
 * happen as the graph is built today — a package required at runtime carries
 * that scope down to everything below it — but a document that would be
 * invalid if that ever changed is not worth the two lines saved. */
static bool present(const sbom_document *document, const char *name) {
    for(size_t i = 0; i < document->count; i++) {
        if(strcmp(document->components[i].name, name) == 0)
            return true;
    }
    return false;
}

static void write_edges(json_writer *writer, const sbom_document *document, const char *ref,
                        const str_list *edges) {
    json_object_open(writer, NULL);
    json_write_field(writer, "ref", ref);
    json_array_open(writer, "dependsOn");
    for(size_t i = 0; i < str_list_count(edges); i++) {
        const char *name = str_list_get(edges, i);
        if(present(document, name))
            json_write_element(writer, name);
    }
    json_array_close(writer);
    json_object_close(writer);
}

/* The graph itself. The root comes first and the components follow in their
   own order, which is sorted — so two runs over one graph write one file. */
static void write_dependencies(json_writer *writer, const sbom_document *document) {
    json_array_open(writer, "dependencies");
    write_edges(writer, document, document->name, &document->root_dependencies);
    for(size_t i = 0; i < document->count; i++) {
        const sbom_component *component = &document->components[i];
        write_edges(writer, document, component->name, component->dependencies);
    }
    json_array_close(writer);
}

void sbom_write_cyclonedx(FILE *stream, const sbom_document *document, const char *tool_version) {
    json_writer writer;
    json_writer_init(&writer, stream);

    json_object_open(&writer, NULL);
    json_write_field(&writer, "bomFormat", BOM_FORMAT);
    json_write_field(&writer, "specVersion", SPEC_VERSION);
    json_write_raw(&writer, "version", BOM_REVISION);
    write_metadata(&writer, document, tool_version);

    json_array_open(&writer, "components");
    for(size_t i = 0; i < document->count; i++)
        write_component(&writer, &document->components[i]);
    json_array_close(&writer);

    write_dependencies(&writer, document);
    json_object_close(&writer);
    json_writer_finish(&writer);
}
