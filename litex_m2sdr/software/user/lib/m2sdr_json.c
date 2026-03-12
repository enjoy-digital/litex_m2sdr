/* SPDX-License-Identifier: BSD-2-Clause */

#include "m2sdr_json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static struct m2sdr_json_token *json_alloc_token(struct m2sdr_json_parser *parser,
                                                 struct m2sdr_json_token *tokens,
                                                 unsigned num_tokens)
{
    struct m2sdr_json_token *tok;

    if (parser->toknext >= num_tokens)
        return NULL;
    tok = &tokens[parser->toknext++];
    tok->type = M2SDR_JSON_UNDEFINED;
    tok->start = -1;
    tok->end = -1;
    tok->parent = -1;
    return tok;
}

static void json_fill_token(struct m2sdr_json_token *tok, enum m2sdr_json_type type, int start, int end)
{
    tok->type = type;
    tok->start = start;
    tok->end = end;
}

static int json_parse_string(struct m2sdr_json_parser *parser, const char *js, size_t len,
                             struct m2sdr_json_token *tokens, unsigned num_tokens)
{
    int start = (int)parser->pos + 1;
    struct m2sdr_json_token *tok;

    parser->pos++;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];

        if (c == '\"') {
            tok = json_alloc_token(parser, tokens, num_tokens);
            if (!tok)
                return -1;
            json_fill_token(tok, M2SDR_JSON_STRING, start, (int)parser->pos);
            tok->parent = parser->toksuper;
            return 0;
        }
        if (c == '\\') {
            parser->pos++;
            if (parser->pos >= len)
                return -1;
        }
    }
    return -1;
}

static int json_is_primitive_end(char c)
{
    return c == '\0' || c == '\t' || c == '\r' || c == '\n' || c == ' ' ||
           c == ',' || c == ']' || c == '}';
}

static int json_parse_primitive(struct m2sdr_json_parser *parser, const char *js, size_t len,
                                struct m2sdr_json_token *tokens, unsigned num_tokens)
{
    int start = (int)parser->pos;
    struct m2sdr_json_token *tok;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];

        if (json_is_primitive_end(c)) {
            tok = json_alloc_token(parser, tokens, num_tokens);
            if (!tok)
                return -1;
            json_fill_token(tok, M2SDR_JSON_PRIMITIVE, start, (int)parser->pos);
            tok->parent = parser->toksuper;
            parser->pos--;
            return 0;
        }
        if ((unsigned char)c < 0x20)
            return -1;
    }
    tok = json_alloc_token(parser, tokens, num_tokens);
    if (!tok)
        return -1;
    json_fill_token(tok, M2SDR_JSON_PRIMITIVE, start, (int)parser->pos);
    tok->parent = parser->toksuper;
    parser->pos--;
    return 0;
}

void m2sdr_json_parser_init(struct m2sdr_json_parser *parser)
{
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

int m2sdr_json_parse(struct m2sdr_json_parser *parser, const char *js, size_t len,
                     struct m2sdr_json_token *tokens, unsigned num_tokens)
{
    unsigned i;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        struct m2sdr_json_token *tok;

        switch (c) {
        case '{':
        case '[':
            tok = json_alloc_token(parser, tokens, num_tokens);
            if (!tok)
                return -1;
            json_fill_token(tok, c == '{' ? M2SDR_JSON_OBJECT : M2SDR_JSON_ARRAY,
                            (int)parser->pos, -1);
            tok->parent = parser->toksuper;
            parser->toksuper = (int)(parser->toknext - 1);
            break;
        case '}':
        case ']':
            for (i = parser->toknext; i > 0; i--) {
                tok = &tokens[i - 1];
                if (tok->start != -1 && tok->end == -1) {
                    if ((tok->type == M2SDR_JSON_OBJECT && c == '}') ||
                        (tok->type == M2SDR_JSON_ARRAY && c == ']')) {
                        tok->end = (int)parser->pos + 1;
                        parser->toksuper = tok->parent;
                        break;
                    }
                    return -1;
                }
            }
            if (i == 0)
                return -1;
            break;
        case '\"':
            if (json_parse_string(parser, js, len, tokens, num_tokens) != 0)
                return -1;
            break;
        case '\t':
        case '\r':
        case '\n':
        case ' ':
        case ':':
        case ',':
            break;
        default:
            if (json_parse_primitive(parser, js, len, tokens, num_tokens) != 0)
                return -1;
            break;
        }
    }

    for (i = parser->toknext; i > 0; i--) {
        if (tokens[i - 1].start != -1 && tokens[i - 1].end == -1)
            return -1;
    }
    return (int)parser->toknext;
}

int m2sdr_json_skip(const struct m2sdr_json_token *tokens, int count, int index)
{
    int end;

    if (!tokens || index < 0 || index >= count)
        return index + 1;
    end = tokens[index].end;
    index++;
    while (index < count && tokens[index].start < end)
        index++;
    return index;
}

bool m2sdr_json_token_streq(const char *js, const struct m2sdr_json_token *tok, const char *s)
{
    size_t len;

    if (!js || !tok || !s || tok->start < 0 || tok->end < tok->start)
        return false;
    len = (size_t)(tok->end - tok->start);
    return strlen(s) == len && strncmp(js + tok->start, s, len) == 0;
}

int m2sdr_json_object_get(const char *js, const struct m2sdr_json_token *tokens, int count,
                          int object_index, const char *key)
{
    int i;

    if (!js || !tokens || object_index < 0 || object_index >= count ||
        tokens[object_index].type != M2SDR_JSON_OBJECT)
        return -1;

    i = object_index + 1;
    while (i < count && tokens[i].start < tokens[object_index].end) {
        if (tokens[i].parent == object_index &&
            tokens[i].type == M2SDR_JSON_STRING &&
            m2sdr_json_token_streq(js, &tokens[i], key)) {
            if (i + 1 >= count)
                return -1;
            return i + 1;
        }
        i = m2sdr_json_skip(tokens, count, i);
    }
    return -1;
}

int m2sdr_json_token_tostr(const char *js, const struct m2sdr_json_token *tok, char *out, size_t out_len)
{
    size_t i = 0;
    int pos;

    if (!js || !tok || tok->type != M2SDR_JSON_STRING || !out || out_len == 0)
        return -1;

    for (pos = tok->start; pos < tok->end && i + 1 < out_len; pos++) {
        char c = js[pos];

        if (c == '\\' && pos + 1 < tok->end) {
            pos++;
            switch (js[pos]) {
            case 'n': out[i++] = '\n'; break;
            case 'r': out[i++] = '\r'; break;
            case 't': out[i++] = '\t'; break;
            case '\\': out[i++] = '\\'; break;
            case '"': out[i++] = '"'; break;
            default: out[i++] = js[pos]; break;
            }
            continue;
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return 0;
}

static int json_token_toprimitive(const char *js, const struct m2sdr_json_token *tok, char *buf, size_t buf_len)
{
    size_t len;

    if (!js || !tok || tok->type != M2SDR_JSON_PRIMITIVE || !buf || buf_len == 0)
        return -1;
    len = (size_t)(tok->end - tok->start);
    if (len + 1 > buf_len)
        return -1;
    memcpy(buf, js + tok->start, len);
    buf[len] = '\0';
    return 0;
}

int m2sdr_json_token_todouble(const char *js, const struct m2sdr_json_token *tok, double *value)
{
    char buf[128];
    char *end = NULL;

    if (!value || json_token_toprimitive(js, tok, buf, sizeof(buf)) != 0)
        return -1;
    *value = strtod(buf, &end);
    return (end && *end == '\0') ? 0 : -1;
}

int m2sdr_json_token_touint(const char *js, const struct m2sdr_json_token *tok, unsigned *value)
{
    char buf[64];
    char *end = NULL;
    unsigned long v;

    if (!value || json_token_toprimitive(js, tok, buf, sizeof(buf)) != 0)
        return -1;
    v = strtoul(buf, &end, 10);
    if (!end || *end != '\0')
        return -1;
    *value = (unsigned)v;
    return 0;
}

int m2sdr_json_token_tou64(const char *js, const struct m2sdr_json_token *tok, uint64_t *value)
{
    char buf[64];
    char *end = NULL;
    unsigned long long v;

    if (!value || json_token_toprimitive(js, tok, buf, sizeof(buf)) != 0)
        return -1;
    v = strtoull(buf, &end, 10);
    if (!end || *end != '\0')
        return -1;
    *value = (uint64_t)v;
    return 0;
}
