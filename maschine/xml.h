/* A small XML reader for Controller Editor templates: elements, attributes,
 * text, the predefined and numeric entities. No DTDs, no namespaces. */

#ifndef XML_H
#define XML_H

struct xml
{
    char *name;
    char **attrs;           /* name, value, ..., NULL */
    char *text;             /* trimmed, "" if none */
    struct xml *child, *next;
};

struct xml *xml_parse(const char *data);
void xml_free(struct xml *node);
const char *xml_attr(const struct xml *node, const char *name);
struct xml *xml_child(const struct xml *node, const char *name);
/* text of the first child called name, or NULL */
const char *xml_text(const struct xml *node, const char *name);

#endif
