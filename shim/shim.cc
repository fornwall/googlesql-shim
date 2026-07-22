#include "shim.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "googlesql/local_service/local_service.pb.h"
#include "googlesql/local_service/local_service_analysis.h"
#include "googlesql/proto/options.pb.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/simple_table.pb.h"
#include "googlesql/public/types/type_deserializer.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"

// The analysis-only service: the 13 front-end RPCs without the
// Prepare*/Evaluate*/Unprepare* families, so the shim's build closure does
// not compile (or link) the reference evaluator. Requires the local-service
// split of fornwall/googlesql#1.
using ::googlesql::local_service::GoogleSqlLocalServiceAnalysisImpl;

namespace ls = ::googlesql::local_service;
namespace gs = ::googlesql;

struct gsql_service {
  GoogleSqlLocalServiceAnalysisImpl impl;
};

namespace {

char* dup_cstr(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p == nullptr) return nullptr;
  std::memcpy(p, s.data(), s.size());
  p[s.size()] = '\0';
  return p;
}

// Serializes `resp` into the out params. Returns an absl status code.
int emit(const google::protobuf::Message& resp, uint8_t** out, size_t* out_len,
         char** err) {
  const size_t n = resp.ByteSizeLong();
  // malloc(0) may return null legitimately; use a 1-byte allocation so a null
  // return is unambiguously OOM. Empty responses are common (e.g. Unregister).
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(n == 0 ? 1 : n));
  if (buf == nullptr) {
    *err = dup_cstr("out of memory serializing response");
    return static_cast<int>(absl::StatusCode::kResourceExhausted);
  }
  if (n > 0 && !resp.SerializeToArray(buf, static_cast<int>(n))) {
    std::free(buf);
    *err = dup_cstr("failed to serialize response");
    return static_cast<int>(absl::StatusCode::kInternal);
  }
  *out = buf;
  *out_len = n;
  return 0;
}

int fail(const absl::Status& st, char** err) {
  *err = dup_cstr(std::string(st.message()));
  return static_cast<int>(st.code());
}

// ---- the callback catalog --------------------------------------------------

// The builtin function library for one set of options, built once and shared
// read-only by every analysis with those options.
//
// The state the callback design removes is *mutable user catalog state* --
// registered tables that could drift from the Rust side. The builtin library
// is a pure function of the language options, carries no user data, and never
// changes after construction, so memoizing it process-wide does not
// reintroduce what the design removed; it removes the one per-call cost that
// was not per-call in nature (~22 ms of `GetBuiltinFunctionsAndTypes` per
// analysis, measured). Upstream memoizes only the `AllReleasedFunctions()`
// default set (`GetDefaultBuiltinFunctionsAndTypes`, builtin_function.cc),
// which is PRODUCT_INTERNAL and not keyed by options, so it cannot serve
// here.
//
// Concurrent lookups on the shared `SimpleCatalog` are safe: the class
// documents itself as thread-safe (simple_catalog.h:75, "This class is
// thread-safe"), with every mutable member `ABSL_GUARDED_BY(mutex_)`
// (simple_catalog.h:705-766) -- and unlike `local_service_analysis.cc`'s
// `ResolvePropertyDefinitionsForSimpleCatalog` path, nothing here mutates the
// catalog after construction.
//
// The `TypeFactory` member owns every builtin type the catalog's functions
// reference; declared before the catalog so it is destroyed after it, and
// kept alive for the process by the cache entry -- which also covers resolved
// ASTs whose serialization reads those types during the call.
struct CachedBuiltins {
  CachedBuiltins() : catalog("bigquery", &type_factory) {}
  gs::TypeFactory type_factory;
  gs::SimpleCatalog catalog;
};

// Returns the cached builtin library for `proto`, building it on first use.
//
// Keyed by the serialized options: within one process the same message
// content serializes to the same bytes, and a non-canonical duplicate would
// only miss the cache, never alias a wrong entry. The key covers everything
// `BuiltinFunctionOptions(proto)` reads -- the whole `LanguageOptionsProto`
// (product mode, name resolution mode, enabled features) plus the
// include/exclude signature lists.
//
// The mutex guards only lookup and insert; construction runs outside it, so
// two threads racing on a cold key may both build and the first insert wins
// (the loser's copy is dropped -- wasted work once, never a wrong answer).
// (The pointee is logically const -- nothing mutates it after construction --
// but `Catalog::Find*` are non-const virtuals, so it cannot be spelled
// `shared_ptr<const CachedBuiltins>` without a const_cast at every
// delegation.)
absl::StatusOr<std::shared_ptr<CachedBuiltins>> BuiltinsForOptions(
    const gs::GoogleSQLBuiltinFunctionOptionsProto& proto) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::NoDestructor<
      absl::flat_hash_map<std::string, std::shared_ptr<CachedBuiltins>>>
      cache;

  std::string key = proto.SerializeAsString();
  {
    absl::MutexLock lock(&*mutex);
    auto it = cache->find(key);
    if (it != cache->end()) return it->second;
  }

  auto built = std::make_shared<CachedBuiltins>();
  absl::Status status = built->catalog.AddBuiltinFunctionsAndTypes(
      gs::BuiltinFunctionOptions(proto));
  if (!status.ok()) return status;

  absl::MutexLock lock(&*mutex);
  auto [it, inserted] = cache->emplace(std::move(key), std::move(built));
  return it->second;
}

// A `Catalog` that resolves table names by calling back into Rust, per
// analysis. Everything else -- functions, types, TVFs -- delegates to the
// shared, per-options `CachedBuiltins` above, holding only the builtin
// library for the request's language options: the same library the
// catalog-as-data path's root `SimpleCatalogProto{builtin_function_options}`
// produced, built through the same `BuiltinFunctionOptions` construction
// `SimpleCatalog::Deserialize` applies (simple_catalog.cc).
//
// `FindTable` receives the full identifier path (`["t"]`, `["ds", "t"]`), so
// no nested catalogs are needed for table resolution; the not-found *message*
// does not matter for analysis errors, because the resolver replaces a
// kNotFound status with its own "Table not found: <path>" text
// (analyzer/resolver_query.cc). What is lost against `SimpleCatalog` is the
// "Did you mean ...?" suffix, since `SuggestTable` has nothing to enumerate.
//
// Deserialized tables are owned here and die with the catalog -- which must
// outlive the analysis but not the serialized response, per catalog.h's
// lifetime contract ("all objects returned from Catalog lookups must stay
// valid for the lifetime of the Catalog").
class RustCallbackCatalog : public gs::Catalog {
 public:
  RustCallbackCatalog(const gsql_catalog_callbacks* callbacks, void* ctx,
                      gs::TypeFactory* type_factory,
                      std::shared_ptr<CachedBuiltins> builtins)
      : callbacks_(callbacks),
        ctx_(ctx),
        type_factory_(type_factory),
        builtins_(std::move(builtins)) {}

  std::string FullName() const override { return "bigquery"; }

  absl::Status FindTable(const absl::Span<const std::string>& path,
                         const gs::Table** table,
                         const FindOptions& options) override {
    *table = nullptr;
    if (path.empty()) return EmptyNamePathInternalError<gs::Table>();

    std::vector<const uint8_t*> segments;
    std::vector<size_t> lengths;
    segments.reserve(path.size());
    lengths.reserve(path.size());
    for (const std::string& segment : path) {
      segments.push_back(reinterpret_cast<const uint8_t*>(segment.data()));
      lengths.push_back(segment.size());
    }

    uint8_t* buf = nullptr;
    size_t buf_len = 0;
    const int code = callbacks_->find_table(
        ctx_, segments.data(), lengths.data(), path.size(), &buf, &buf_len);

    if (code == static_cast<int>(absl::StatusCode::kNotFound)) {
      return TableNotFoundError(path);
    }
    if (code != 0) {
      // A failing callback (a Rust panic, converted) may carry its message in
      // the buffer; without one, say at least whose fault it was.
      std::string message = "catalog callback failed";
      if (buf != nullptr) {
        message.assign(reinterpret_cast<const char*>(buf), buf_len);
        callbacks_->free_buf(ctx_, buf, buf_len);
      }
      return absl::Status(static_cast<absl::StatusCode>(code), message);
    }

    gs::SimpleTableProto proto;
    const bool parsed =
        buf != nullptr && proto.ParseFromArray(buf, static_cast<int>(buf_len));
    if (buf != nullptr) callbacks_->free_buf(ctx_, buf, buf_len);
    if (!parsed) {
      return absl::InternalError(
          "catalog callback returned a malformed SimpleTableProto");
    }

    // Preserves `serialization_id`, which the resolved AST round-trips and
    // the Rust side routes DML by. Types intern into the per-call factory.
    absl::StatusOr<std::unique_ptr<gs::SimpleTable>> deserialized =
        gs::SimpleTable::Deserialize(
            proto, gs::TypeDeserializer(type_factory_,
                                        /*descriptor_pools=*/{}));
    if (!deserialized.ok()) return deserialized.status();
    tables_.push_back(*std::move(deserialized));
    *table = tables_.back().get();
    return absl::OkStatus();
  }

  absl::Status FindFunction(const absl::Span<const std::string>& path,
                            const gs::Function** function,
                            const FindOptions& options) override {
    return builtins_->catalog.FindFunction(path, function, options);
  }

  absl::Status FindTableValuedFunction(
      const absl::Span<const std::string>& path,
      const gs::TableValuedFunction** function,
      const FindOptions& options) override {
    return builtins_->catalog.FindTableValuedFunction(path, function, options);
  }

  absl::Status FindType(const absl::Span<const std::string>& path,
                        const gs::Type** type,
                        const FindOptions& options) override {
    return builtins_->catalog.FindType(path, type, options);
  }

 private:
  const gsql_catalog_callbacks* callbacks_;
  void* ctx_;
  gs::TypeFactory* type_factory_;
  // Shared, immutable-after-construction; see `CachedBuiltins`.
  std::shared_ptr<CachedBuiltins> builtins_;
  std::vector<std::unique_ptr<const gs::SimpleTable>> tables_;
};

// The body of `gsql_analyze_with_catalog`, in status-returning form so error
// paths read straight. Everything it builds is destroyed when it returns.
absl::Status AnalyzeWithCatalogImpl(const uint8_t* options_bytes,
                                    size_t options_len, const uint8_t* sql,
                                    size_t sql_len,
                                    const gsql_catalog_callbacks* callbacks,
                                    void* ctx, ls::AnalyzeResponse* response) {
  gs::AnalyzerOptionsProto options_proto;
  if (!options_proto.ParseFromArray(options_bytes,
                                    static_cast<int>(options_len))) {
    return absl::InvalidArgumentError("failed to parse AnalyzerOptionsProto");
  }

  // A per-call TypeFactory. It must outlive both the analysis and the
  // serialization below, since the resolved AST points into it; it does, by
  // being a local of this whole function.
  gs::TypeFactory factory;
  gs::AnalyzerOptions options;
  const std::vector<const google::protobuf::DescriptorPool*> pools;
  absl::Status status =
      gs::AnalyzerOptions::Deserialize(options_proto, pools, &factory,
                                       &options);
  if (!status.ok()) return status;

  // The builtin function library is keyed off the request's language options,
  // the same way `Catalog::to_proto` on the Rust side set
  // `builtin_function_options` under the proto-registration design -- shared
  // and immutable across calls, per `CachedBuiltins`.
  gs::GoogleSQLBuiltinFunctionOptionsProto builtin_options;
  *builtin_options.mutable_language_options() =
      options_proto.language_options();
  absl::StatusOr<std::shared_ptr<CachedBuiltins>> builtins =
      BuiltinsForOptions(builtin_options);
  if (!builtins.ok()) return builtins.status();
  RustCallbackCatalog catalog(callbacks, ctx, &factory, *std::move(builtins));

  std::unique_ptr<const gs::AnalyzerOutput> output;
  status = gs::AnalyzeStatement(
      absl::string_view(reinterpret_cast<const char*>(sql), sql_len), options,
      &catalog, &factory, &output);
  if (!status.ok()) return status;

  // The same response shape `SerializeResolvedOutput` produces for the
  // Analyze RPC, with no descriptor pools in play (the map picks up the
  // generated pool on its own if a builtin drags one in).
  if (output->resolved_statement() == nullptr) {
    return absl::InternalError("analysis produced no resolved statement");
  }
  gs::FileDescriptorSetMap file_descriptor_set_map;
  return output->resolved_statement()->SaveTo(
      &file_descriptor_set_map, response->mutable_resolved_statement());
}

}  // namespace

// Uniform case: parse ReqT, call impl.Method, serialize RespT.
//
// Both type arguments must be fully qualified: most live in `ls`, but the
// option and catalog protos live in `gs`.
#define GSQL_DISPATCH(CODE, METHOD, ReqT, RespT)                     \
  case CODE: {                                                       \
    ReqT parsed;                                                     \
    if (!parsed.ParseFromArray(req, static_cast<int>(req_len))) {    \
      *err = dup_cstr("failed to parse " #ReqT);                     \
      return static_cast<int>(absl::StatusCode::kInvalidArgument);   \
    }                                                                \
    RespT resp;                                                      \
    absl::Status st = svc->impl.METHOD(parsed, &resp);               \
    if (!st.ok()) return fail(st, err);                              \
    return emit(resp, out, out_len, err);                            \
  }

extern "C" {

gsql_service* gsql_service_new(void) { return new (std::nothrow) gsql_service(); }

void gsql_service_free(gsql_service* svc) { delete svc; }

void gsql_buf_free(uint8_t* buf) { std::free(buf); }

void gsql_str_free(char* s) { std::free(s); }

int gsql_call(gsql_service* svc, int method, const uint8_t* req, size_t req_len,
              uint8_t** out, size_t* out_len, char** err) {
  *out = nullptr;
  *out_len = 0;
  *err = nullptr;

  // GoogleSQL is exception-free internally, but protobuf and the allocator are
  // not. Unwinding across the C ABI is UB, so it stops here.
  try {
    switch (method) {
      GSQL_DISPATCH(GSQL_PARSE, Parse, ls::ParseRequest, ls::ParseResponse)
      GSQL_DISPATCH(GSQL_ANALYZE, Analyze, ls::AnalyzeRequest,
                    ls::AnalyzeResponse)
      GSQL_DISPATCH(GSQL_BUILD_SQL, BuildSql, ls::BuildSqlRequest,
                    ls::BuildSqlResponse)
      GSQL_DISPATCH(GSQL_EXTRACT_TABLE_NAMES_FROM_STATEMENT,
                    ExtractTableNamesFromStatement,
                    ls::ExtractTableNamesFromStatementRequest,
                    ls::ExtractTableNamesFromStatementResponse)
      GSQL_DISPATCH(GSQL_EXTRACT_TABLE_NAMES_FROM_NEXT_STATEMENT,
                    ExtractTableNamesFromNextStatement,
                    ls::ExtractTableNamesFromNextStatementRequest,
                    ls::ExtractTableNamesFromNextStatementResponse)
      GSQL_DISPATCH(GSQL_FORMAT_SQL, FormatSql, ls::FormatSqlRequest,
                    ls::FormatSqlResponse)
      GSQL_DISPATCH(GSQL_LENIENT_FORMAT_SQL, LenientFormatSql,
                    ls::FormatSqlRequest, ls::FormatSqlResponse)
      GSQL_DISPATCH(GSQL_REGISTER_CATALOG, RegisterCatalog,
                    ls::RegisterCatalogRequest, ls::RegisterResponse)
      GSQL_DISPATCH(GSQL_GET_TABLE_FROM_PROTO, GetTableFromProto,
                    ls::TableFromProtoRequest, gs::SimpleTableProto)
      GSQL_DISPATCH(GSQL_GET_BUILTIN_FUNCTIONS, GetBuiltinFunctions,
                    gs::GoogleSQLBuiltinFunctionOptionsProto,
                    ls::GetBuiltinFunctionsResponse)
      GSQL_DISPATCH(GSQL_GET_LANGUAGE_OPTIONS, GetLanguageOptions,
                    ls::LanguageOptionsRequest, gs::LanguageOptionsProto)
      GSQL_DISPATCH(GSQL_GET_ANALYZER_OPTIONS, GetAnalyzerOptions,
                    ls::AnalyzerOptionsRequest, gs::AnalyzerOptionsProto)

      // Irregular: takes a bare id and returns nothing.
      case GSQL_UNREGISTER_CATALOG: {
        ls::UnregisterRequest parsed;
        if (!parsed.ParseFromArray(req, static_cast<int>(req_len))) {
          *err = dup_cstr("failed to parse UnregisterRequest");
          return static_cast<int>(absl::StatusCode::kInvalidArgument);
        }
        absl::Status st = svc->impl.UnregisterCatalog(parsed.registered_id());
        if (!st.ok()) return fail(st, err);
        google::protobuf::Empty empty;
        return emit(empty, out, out_len, err);
      }

      default:
        *err = dup_cstr("unknown method code");
        return static_cast<int>(absl::StatusCode::kUnimplemented);
    }
  } catch (const std::exception& e) {
    *err = dup_cstr(std::string("C++ exception: ") + e.what());
    return static_cast<int>(absl::StatusCode::kInternal);
  } catch (...) {
    *err = dup_cstr("unknown C++ exception");
    return static_cast<int>(absl::StatusCode::kInternal);
  }
}

int gsql_analyze_with_catalog(const uint8_t* options, size_t options_len,
                              const uint8_t* sql, size_t sql_len,
                              const gsql_catalog_callbacks* callbacks,
                              void* ctx, uint8_t** out, size_t* out_len,
                              char** err) {
  *out = nullptr;
  *out_len = 0;
  *err = nullptr;

  // Same exception firewall as `gsql_call`: unwinding across the C ABI is UB.
  try {
    ls::AnalyzeResponse response;
    absl::Status st = AnalyzeWithCatalogImpl(options, options_len, sql,
                                             sql_len, callbacks, ctx,
                                             &response);
    if (!st.ok()) return fail(st, err);
    return emit(response, out, out_len, err);
  } catch (const std::exception& e) {
    *err = dup_cstr(std::string("C++ exception: ") + e.what());
    return static_cast<int>(absl::StatusCode::kInternal);
  } catch (...) {
    *err = dup_cstr("unknown C++ exception");
    return static_cast<int>(absl::StatusCode::kInternal);
  }
}

}  // extern "C"
