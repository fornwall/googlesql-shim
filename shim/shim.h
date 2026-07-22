// C ABI over GoogleSqlLocalServiceImpl, restricted to the front end
// (parser, analyzer, catalog, formatter). The evaluator RPCs -- Prepare*,
// Evaluate*, Unprepare* -- are deliberately not exposed; see README.
//
// Every call is proto-bytes in, proto-bytes out: `gsql_call` is the one
// dispatch entry point, and the other four exports below only create,
// destroy and deallocate. No C++ type crosses the boundary, so Rust never
// needs a binding shaped like a GoogleSQL class -- which is what keeps the
// unsafe surface there to service lifetime plus this one call.

#ifndef GOOGLESQL_RS_SHIM_H_
#define GOOGLESQL_RS_SHIM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gsql_service gsql_service;

// Method codes. Must stay in sync with `Method` in src/lib.rs.
enum {
  GSQL_PARSE = 0,
  GSQL_ANALYZE = 1,
  GSQL_BUILD_SQL = 2,
  GSQL_EXTRACT_TABLE_NAMES_FROM_STATEMENT = 3,
  GSQL_EXTRACT_TABLE_NAMES_FROM_NEXT_STATEMENT = 4,
  GSQL_FORMAT_SQL = 5,
  GSQL_LENIENT_FORMAT_SQL = 6,
  GSQL_REGISTER_CATALOG = 7,
  GSQL_UNREGISTER_CATALOG = 8,
  GSQL_GET_TABLE_FROM_PROTO = 9,
  GSQL_GET_BUILTIN_FUNCTIONS = 10,
  GSQL_GET_LANGUAGE_OPTIONS = 11,
  GSQL_GET_ANALYZER_OPTIONS = 12,
};

// The service owns registered catalog and descriptor-pool state, so a single
// instance must outlive the ids it hands out.
gsql_service* gsql_service_new(void);
void gsql_service_free(gsql_service* svc);

// Returns an absl::StatusCode (0 == OK).
//
// On success `*out`/`*out_len` receive the serialized response; free with
// `gsql_buf_free`. On failure `*err` receives a NUL-terminated message; free
// with `gsql_str_free`. Exactly one of the two is populated.
int gsql_call(gsql_service* svc, int method, const uint8_t* req, size_t req_len,
              uint8_t** out, size_t* out_len, char** err);

void gsql_buf_free(uint8_t* buf);
void gsql_str_free(char* s);

// The callback catalog (experimental)
// ===================================
//
// `gsql_analyze_with_catalog` is a deliberate departure from the invariant
// above: a second, typed entry point, through which C++ calls *back into
// Rust* while the analyzer runs. It exists as the architectural counterpoint
// to the catalog-as-data design -- instead of shipping a whole
// `SimpleCatalogProto` ahead of time (inline or registered by id), the
// analyzer resolves each table name by invoking the caller's `find_table`
// callback mid-analysis.
//
// Everything user-visible is per call. The function constructs a `Catalog`
// subclass whose `FindTable` calls Rust and a `TypeFactory`, both destroyed
// before the function returns. **No mutable user catalog state lives on the
// C++ side** -- no tables that could drift from the caller's model, so there
// is nothing to register, re-register after DDL, or free. The one deliberate
// piece of retained state is the builtin function library: a pure function of
// the language options, no user data, memoized process-wide per distinct
// options and immutable after construction (`CachedBuiltins` in shim.cc), so
// repeated analyses do not rebuild it.
//
// Callback contract:
//
// * `find_table` receives the identifier path the analyzer is resolving
//   (`["t"]`, `["ds", "t"]`, ...) as `num_segments` byte strings. It returns
//   an absl::StatusCode: 0 (OK) with `*out`/`*out_len` set to a serialized
//   `SimpleTableProto` (including `serialization_id`, which the resolved AST
//   round-trips); 5 (kNotFound) when there is no such table, which the
//   analyzer turns into its own "Table not found: ..." error; any other code
//   fails the analysis, with `*out`/`*out_len` optionally carrying a UTF-8
//   message.
// * Every buffer the callback returns through `out` is released with exactly
//   one `free_buf(ctx, buf, len)` call before `gsql_analyze_with_catalog`
//   returns.
// * Callbacks run synchronously, on the calling thread, possibly several
//   times per statement. They must not unwind (no Rust panics across the
//   boundary) and must not call back into this library.
typedef struct gsql_catalog_callbacks {
  int (*find_table)(void* ctx, const uint8_t* const* segments,
                    const size_t* segment_lens, size_t num_segments,
                    uint8_t** out, size_t* out_len);
  void (*free_buf)(void* ctx, uint8_t* buf, size_t len);
} gsql_catalog_callbacks;

// Analyzes one statement against the callback catalog.
//
// `options`/`options_len` is a serialized `AnalyzerOptionsProto` (the same
// message `AnalyzeRequest.options` carries); `sql`/`sql_len` is the statement
// text. On success `*out`/`*out_len` receive a serialized `AnalyzeResponse`,
// freed with `gsql_buf_free`; on failure `*err` receives a NUL-terminated
// message, freed with `gsql_str_free`. Returns an absl::StatusCode.
//
// Takes no `gsql_service`: the point of the exercise is that analysis needs
// no service-owned state at all.
int gsql_analyze_with_catalog(const uint8_t* options, size_t options_len,
                              const uint8_t* sql, size_t sql_len,
                              const gsql_catalog_callbacks* callbacks,
                              void* ctx, uint8_t** out, size_t* out_len,
                              char** err);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GOOGLESQL_RS_SHIM_H_
