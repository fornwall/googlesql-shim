/* Smoke test for the wasm32 shim archive: drives the real GoogleSQL parser
 * through the shim's C ABI, with the request proto hand-encoded so the test
 * needs no protobuf library of its own.
 *
 * FORMAT_SQL parses the statement with GoogleSQL's parser and pretty-prints
 * it back, so one call proves the whole front-end closure -- shim, GoogleSQL,
 * absl, protobuf, ICU -- is alive inside the wasm module. A malformed
 * statement is also sent to prove the error path (status + message) works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim/shim.h"

/* Serializes FormatSqlRequest { optional string sql = 1; }: tag 0x0A,
 * varint length (statements here are < 128 bytes), then the bytes. */
static size_t encode_format_sql_request(const char* sql, uint8_t* buf) {
  size_t n = strlen(sql);
  if (n > 127) {
    fprintf(stderr, "statement too long for this toy encoder\n");
    exit(1);
  }
  buf[0] = 0x0A;
  buf[1] = (uint8_t)n;
  memcpy(buf + 2, sql, n);
  return n + 2;
}

int main(void) {
  gsql_service* svc = gsql_service_new();
  if (!svc) {
    fprintf(stderr, "gsql_service_new failed\n");
    return 1;
  }

  uint8_t req[192];
  size_t req_len = encode_format_sql_request(
      "select a,sum(b) from t where c=1 group by a", req);

  uint8_t* out = NULL;
  size_t out_len = 0;
  char* err = NULL;
  int code = gsql_call(svc, GSQL_FORMAT_SQL, req, req_len, &out, &out_len,
                       &err);
  if (code != 0) {
    fprintf(stderr, "FORMAT_SQL failed: code=%d err=%s\n", code,
            err ? err : "(none)");
    return 1;
  }
  /* FormatSqlResponse { optional string sql = 1; } -- same toy shape. */
  if (out_len < 2 || out[0] != 0x0A) {
    fprintf(stderr, "unexpected response encoding\n");
    return 1;
  }
  printf("formatted: %.*s\n", (int)out[1], (const char*)out + 2);
  gsql_buf_free(out);

  /* The error path: a parse error must come back as a status + message. */
  req_len = encode_format_sql_request("select from from", req);
  out = NULL;
  err = NULL;
  code = gsql_call(svc, GSQL_FORMAT_SQL, req, req_len, &out, &out_len, &err);
  if (code == 0) {
    fprintf(stderr, "malformed SQL unexpectedly accepted\n");
    return 1;
  }
  printf("parse error surfaced: code=%d msg=%s\n", code,
         err ? err : "(none)");
  gsql_str_free(err);

  gsql_service_free(svc);
  printf("wasm smoke test passed\n");
  return 0;
}
