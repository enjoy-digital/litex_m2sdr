/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_JSON_H
#define M2SDR_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum m2sdr_json_type {
    M2SDR_JSON_UNDEFINED = 0,
    M2SDR_JSON_OBJECT,
    M2SDR_JSON_ARRAY,
    M2SDR_JSON_STRING,
    M2SDR_JSON_PRIMITIVE
};

struct m2sdr_json_token {
    enum m2sdr_json_type type;
    int start;
    int end;
    int parent;
};

struct m2sdr_json_parser {
    unsigned pos;
    unsigned toknext;
    int toksuper;
};

void m2sdr_json_parser_init(struct m2sdr_json_parser *parser);
int m2sdr_json_parse(struct m2sdr_json_parser *parser, const char *js, size_t len,
                     struct m2sdr_json_token *tokens, unsigned num_tokens);

int m2sdr_json_skip(const struct m2sdr_json_token *tokens, int count, int index);
int m2sdr_json_object_get(const char *js, const struct m2sdr_json_token *tokens, int count,
                          int object_index, const char *key);
bool m2sdr_json_token_streq(const char *js, const struct m2sdr_json_token *tok, const char *s);
int m2sdr_json_token_tostr(const char *js, const struct m2sdr_json_token *tok, char *out, size_t out_len);
int m2sdr_json_token_todouble(const char *js, const struct m2sdr_json_token *tok, double *value);
int m2sdr_json_token_touint(const char *js, const struct m2sdr_json_token *tok, unsigned *value);
int m2sdr_json_token_tou64(const char *js, const struct m2sdr_json_token *tok, uint64_t *value);

#endif /* M2SDR_JSON_H */
