#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "xml.h"

#define MAX_DEPTH 64

struct buf
{
    char *data;
    size_t len, size;
};

static int put(struct buf *b, const char *s, size_t n)
{
    char *data;

    if (b->len + n + 1 > b->size)
    {
        size_t size = b->size ? b->size * 2 : 64;
        while (size < b->len + n + 1) size *= 2;
        if (!(data = realloc(b->data, size))) return -1;
        b->data = data;
        b->size = size;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

static int put_utf8(struct buf *b, unsigned long c)
{
    char s[4];

    if (c < 0x80) s[0] = c;
    else if (c < 0x800)
    {
        s[0] = 0xc0 | c >> 6;
        s[1] = 0x80 | (c & 0x3f);
        return put(b, s, 2);
    }
    else if (c < 0x10000)
    {
        s[0] = 0xe0 | c >> 12;
        s[1] = 0x80 | (c >> 6 & 0x3f);
        s[2] = 0x80 | (c & 0x3f);
        return put(b, s, 3);
    }
    else if (c < 0x110000)
    {
        s[0] = 0xf0 | c >> 18;
        s[1] = 0x80 | (c >> 12 & 0x3f);
        s[2] = 0x80 | (c >> 6 & 0x3f);
        s[3] = 0x80 | (c & 0x3f);
        return put(b, s, 4);
    }
    else s[0] = '?';
    return put(b, s, 1);
}

/* the character of a predefined entity "&name;" (name = s[0..n)), 0 if unknown */
static char entity(const char *s, size_t n)
{
    static const struct
    {
        const char *name;
        char c;
    }
    entities[] = {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"quot", '"'}, {"apos", '\''}};
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(entities); i++)
        if (n == strlen(entities[i].name) && !strncmp(s, entities[i].name, n)) return entities[i].c;
    return 0;
}

/* appends s[0..n) with entities decoded, unknown ones as they are */
static int put_decoded(struct buf *b, const char *s, size_t n)
{
    const char *end = s + n, *semi;
    unsigned long code;
    char c;
    int ret;

    while (s < end)
    {
        if (*s != '&' || !(semi = memchr(s, ';', end - s)))
        {
            if (put(b, s, 1)) return -1;
            s++;
            continue;
        }
        if (s[1] == '#')
        {
            code = s[2] == 'x' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            ret = put_utf8(b, code);
        }
        else if ((c = entity(s + 1, semi - s - 1))) ret = put(b, &c, 1);
        else ret = put(b, s, semi - s + 1);
        if (ret) return -1;
        s = semi + 1;
    }
    return 0;
}

static char *decoded(const char *s, size_t n)
{
    struct buf b = {0};

    if (put(&b, "", 0) || put_decoded(&b, s, n))
    {
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* skips <?...?>, <!--...--> and <!...>; false if none of them is at *p */
static bool skip_markup(const char **p)
{
    const char *end;
    size_t close_len;

    if (!strncmp(*p, "<?", 2))
    {
        end = strstr(*p, "?>");
        close_len = 2;
    }
    else if (!strncmp(*p, "<!--", 4))
    {
        end = strstr(*p, "-->");
        close_len = 3;
    }
    else if (!strncmp(*p, "<!", 2) && strncmp(*p, "<![CDATA[", 9))
    {
        end = strchr(*p, '>');
        close_len = 1;
    }
    else return false;
    *p = end ? end + close_len : *p + strlen(*p);
    return true;
}

static bool is_name_char(char c)
{
    return c && !isspace((unsigned char)c) && !strchr("/>=<\"'", c);
}

static struct xml *element(const char **p, int depth);

static int content(struct xml *node, const char **p, int depth)
{
    struct buf text = {0};
    struct xml **tail = &node->child;
    const char *s = *p, *start;
    char *t;
    int ret = -1;

    if (put(&text, "", 0)) return -1;
    while (*s)
    {
        if (*s != '<')
        {
            for (start = s; *s && *s != '<'; s++);
            if (put_decoded(&text, start, s - start)) goto done;
        }
        else if (s[1] == '/')
        {
            if (!(s = strchr(s, '>'))) goto done;
            s++;
            ret = 0;
            break;
        }
        else if (!strncmp(s, "<![CDATA[", 9))
        {
            if (!(start = strstr(s, "]]>"))) goto done;
            if (put(&text, s + 9, start - s - 9)) goto done;
            s = start + 3;
        }
        else if (!skip_markup(&s))
        {
            if (!(*tail = element(&s, depth + 1))) goto done;
            tail = &(*tail)->next;
        }
    }
done:
    *p = s;
    /* trim */
    for (t = text.data + text.len; t > text.data && isspace((unsigned char)t[-1]); t--);
    *t = 0;
    for (t = text.data; isspace((unsigned char)*t); t++);
    memmove(text.data, t, strlen(t) + 1);
    node->text = text.data;
    return ret;
}

static struct xml *element(const char **p, int depth)
{
    const char *s = *p, *start;
    struct xml *node;
    size_t count = 0;
    char quote, **attrs;

    if (*s != '<' || depth > MAX_DEPTH || !(node = calloc(1, sizeof(*node)))) return NULL;
    if (!(node->attrs = calloc(1, sizeof(char *)))) goto fail;
    for (start = ++s; is_name_char(*s); s++);
    if (s == start || !(node->name = decoded(start, s - start))) goto fail;

    for (;;)
    {
        while (isspace((unsigned char)*s)) s++;
        if (*s == '/' || *s == '>' || !*s) break;
        if (!(attrs = realloc(node->attrs, (count + 3) * sizeof(char *)))) goto fail;
        node->attrs = attrs;
        attrs[count] = attrs[count + 1] = attrs[count + 2] = NULL;
        for (start = s; is_name_char(*s); s++);
        if (s == start || !(attrs[count] = decoded(start, s - start))) goto fail;
        while (isspace((unsigned char)*s)) s++;
        if (*s++ != '=') goto fail;
        while (isspace((unsigned char)*s)) s++;
        if ((quote = *s++) != '"' && quote != '\'') goto fail;
        for (start = s; *s && *s != quote; s++);
        if (!*s || !(attrs[count + 1] = decoded(start, s - start))) goto fail;
        s++;
        count += 2;
    }
    if (*s == '/')
    {
        if (s[1] != '>' || !(node->text = calloc(1, 1))) goto fail;
        *p = s + 2;
        return node;
    }
    if (*s++ != '>') goto fail;
    if (content(node, &s, depth)) goto fail;
    *p = s;
    return node;

fail:
    xml_free(node);
    return NULL;
}

struct xml *xml_parse(const char *data)
{
    const char *p = data;

    if (!strncmp(p, "\xef\xbb\xbf", 3)) p += 3;
    for (;;)
    {
        while (isspace((unsigned char)*p)) p++;
        if (!skip_markup(&p)) break;
    }
    return element(&p, 0);
}

void xml_free(struct xml *node)
{
    struct xml *next;
    char **a;

    for (; node; node = next)
    {
        next = node->next;
        xml_free(node->child);
        for (a = node->attrs; a && *a; a++) free(*a);
        free(node->attrs);
        free(node->name);
        free(node->text);
        free(node);
    }
}

const char *xml_attr(const struct xml *node, const char *name)
{
    char **a;

    for (a = node->attrs; a && *a; a += 2)
        if (!strcmp(a[0], name)) return a[1];
    return NULL;
}

struct xml *xml_child(const struct xml *node, const char *name)
{
    struct xml *c;

    for (c = node->child; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

const char *xml_text(const struct xml *node, const char *name)
{
    struct xml *c = xml_child(node, name);
    return c ? c->text : NULL;
}
