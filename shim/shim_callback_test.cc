// Sanitizer-facing tests for `gsql_analyze_with_catalog`, driving the C ABI
// directly with C++-implemented callbacks standing in for the Rust side.
//
// This exists to run under `--config=asan` / `--config=tsan` /
// `--config=ubsan` (see the root .bazelrc): the riskiest code on the callback
// path -- buffer handoff across the C ABI, the per-options builtin cache, and
// concurrent lookups on the shared SimpleCatalog -- is all C++, so a plain
// cc_test puts the sanitizers exactly where the risk is without a
// mixed-language (Rust + C++) sanitizer build. It runs under the default
// config too, where the alloc/free balance checks still bite.
//
// Deliberately no gtest: the module declares only what the shim needs, and a
// main() with an abort-on-failure macro is enough for a sanitizer harness.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "googlesql/local_service/local_service.pb.h"
#include "googlesql/proto/options.pb.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/simple_table.pb.h"
#include "shim.h"

namespace {

namespace gs = ::googlesql;
namespace ls = ::googlesql::local_service;

#define CHECK_TRUE(cond)                                                \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAILED at %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                              \
      std::abort();                                                     \
    }                                                                   \
  } while (0)

#define CHECK_CONTAINS(haystack, needle)                                 \
  do {                                                                   \
    const std::string& h = (haystack);                                   \
    if (h.find(needle) == std::string::npos) {                           \
      std::fprintf(stderr, "FAILED at %s:%d: expected \"%s\" in:\n%s\n", \
                   __FILE__, __LINE__, needle, h.c_str());               \
      std::abort();                                                      \
    }                                                                    \
  } while (0)

// The Rust stand-in: a table map plus failure-injection knobs, with an
// alloc/free balance the tests assert -- every buffer handed out through
// `find_table` must come back through `free_buf` exactly once, which is the
// ownership contract the Rust side relies on.
struct TestCatalog {
  // Identifier path joined with '\x1f' -> serialized SimpleTableProto.
  std::map<std::string, std::string> tables;
  // If nonzero, `find_table` reports this status with a message buffer.
  int forced_status = 0;
  // If true, `find_table` answers OK with bytes that are not a proto.
  bool return_malformed = false;
  std::atomic<long> allocs{0};
  std::atomic<long> frees{0};

  long outstanding() const { return allocs.load() - frees.load(); }
};

uint8_t* AllocCopy(TestCatalog* catalog, const std::string& bytes) {
  auto* buf =
      static_cast<uint8_t*>(std::malloc(bytes.empty() ? 1 : bytes.size()));
  CHECK_TRUE(buf != nullptr);
  std::memcpy(buf, bytes.data(), bytes.size());
  catalog->allocs.fetch_add(1);
  return buf;
}

extern "C" int TestFindTable(void* ctx, const uint8_t* const* segments,
                             const size_t* segment_lens, size_t num_segments,
                             uint8_t** out, size_t* out_len) {
  auto* catalog = static_cast<TestCatalog*>(ctx);
  if (catalog->forced_status != 0) {
    const std::string message = "backend hiccup";
    *out = AllocCopy(catalog, message);
    *out_len = message.size();
    return catalog->forced_status;
  }
  if (catalog->return_malformed) {
    const std::string garbage = "\xff\xff\xff\xff";
    *out = AllocCopy(catalog, garbage);
    *out_len = garbage.size();
    return 0;
  }
  std::string key;
  for (size_t i = 0; i < num_segments; ++i) {
    if (i > 0) key += '\x1f';
    key.append(reinterpret_cast<const char*>(segments[i]), segment_lens[i]);
  }
  auto it = catalog->tables.find(key);
  if (it == catalog->tables.end()) return 5;  // absl::StatusCode::kNotFound
  *out = AllocCopy(catalog, it->second);
  *out_len = it->second.size();
  return 0;
}

extern "C" void TestFreeBuf(void* ctx, uint8_t* buf, size_t len) {
  (void)len;
  static_cast<TestCatalog*>(ctx)->frees.fetch_add(1);
  std::free(buf);
}

constexpr gsql_catalog_callbacks kCallbacks = {TestFindTable, TestFreeBuf};

std::string TableProto(const std::string& name, const std::string& column,
                       int64_t serialization_id) {
  gs::SimpleTableProto table;
  table.set_name(name);
  table.set_serialization_id(serialization_id);
  gs::SimpleColumnProto* col = table.add_column();
  col->set_name(column);
  col->mutable_type()->set_type_kind(gs::TYPE_INT64);
  return table.SerializeAsString();
}

std::string SerializeOptions(const gs::AnalyzerOptionsProto& options) {
  return options.SerializeAsString();
}

// One analyze call through the C ABI. On success returns 0 and fills
// `response`; on failure returns the status code and fills `error`.
int Analyze(const std::string& options, const std::string& sql,
            TestCatalog* catalog, ls::AnalyzeResponse* response,
            std::string* error) {
  uint8_t* out = nullptr;
  size_t out_len = 0;
  char* err = nullptr;
  const int code = gsql_analyze_with_catalog(
      reinterpret_cast<const uint8_t*>(options.data()), options.size(),
      reinterpret_cast<const uint8_t*>(sql.data()), sql.size(), &kCallbacks,
      catalog, &out, &out_len, &err);
  if (code == 0) {
    CHECK_TRUE(out != nullptr);
    CHECK_TRUE(err == nullptr);
    if (response != nullptr) {
      CHECK_TRUE(response->ParseFromArray(out, static_cast<int>(out_len)));
    }
    gsql_buf_free(out);
  } else {
    CHECK_TRUE(out == nullptr);
    CHECK_TRUE(err != nullptr);
    if (error != nullptr) *error = err;
    gsql_str_free(err);
  }
  return code;
}

// (Populates in place: TestCatalog is neither copyable nor movable, its
// atomics see to that.)
void AddTwoTables(TestCatalog* catalog) {
  catalog->tables["t"] = TableProto("t", "x", 41);
  catalog->tables[std::string("ds") + '\x1f' + "t"] = TableProto("t", "y", 42);
}

// ---- single-threaded correctness -------------------------------------------

void TestResolvesBareAndQualifiedNames() {
  TestCatalog catalog;
  AddTwoTables(&catalog);
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());

  ls::AnalyzeResponse response;
  CHECK_TRUE(
      Analyze(options, "SELECT x FROM t", &catalog, &response, nullptr) == 0);
  // The serialization_id the callback set must round-trip into the resolved
  // AST -- Rust DML routing depends on exactly this.
  CHECK_CONTAINS(response.resolved_statement().DebugString(),
                 "serialization_id: 41");

  CHECK_TRUE(Analyze(options, "SELECT y FROM ds.t", &catalog, &response,
                     nullptr) == 0);
  CHECK_CONTAINS(response.resolved_statement().DebugString(),
                 "serialization_id: 42");

  CHECK_TRUE(catalog.outstanding() == 0);
}

void TestNotFoundIsTheAnalyzersOwnError() {
  TestCatalog catalog;
  AddTwoTables(&catalog);
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());

  std::string error;
  const int code = Analyze(options, "SELECT 1 FROM no_such_table", &catalog,
                           nullptr, &error);
  // kInvalidArgument, the analyzer's error code, with its own message text.
  CHECK_TRUE(code == 3);
  CHECK_CONTAINS(error, "Table not found: no_such_table");
  CHECK_TRUE(catalog.outstanding() == 0);
}

void TestMalformedTableProtoIsAnErrorNotUb() {
  TestCatalog catalog;
  AddTwoTables(&catalog);
  catalog.return_malformed = true;
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());

  std::string error;
  const int code =
      Analyze(options, "SELECT x FROM t", &catalog, nullptr, &error);
  CHECK_TRUE(code != 0);
  CHECK_CONTAINS(error, "malformed SimpleTableProto");
  // The garbage buffer was still freed exactly once.
  CHECK_TRUE(catalog.outstanding() == 0);
}

void TestCallbackErrorStatusPropagates() {
  TestCatalog catalog;
  AddTwoTables(&catalog);
  catalog.forced_status = 14;  // absl::StatusCode::kUnavailable
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());

  std::string error;
  const int code =
      Analyze(options, "SELECT x FROM t", &catalog, nullptr, &error);
  CHECK_TRUE(code == 14);
  CHECK_CONTAINS(error, "backend hiccup");
  CHECK_TRUE(catalog.outstanding() == 0);
}

// ---- the cache under concurrency -------------------------------------------

// Options whose serialized bytes -- the cache key -- differ per `salt`, by
// enabling the first `salt` features from a fixed list. All eight are real
// LanguageFeature values, several of which change the builtin library
// (NUMERIC, JSON, INTERVAL add functions and types), so distinct fingerprints
// build genuinely different cache entries.
std::string OptionsWithFingerprint(int salt) {
  static constexpr gs::LanguageFeature kFeatures[] = {
      gs::FEATURE_ANALYTIC_FUNCTIONS, gs::FEATURE_TABLESAMPLE,
      gs::FEATURE_TIMESTAMP_NANOS,    gs::FEATURE_DML_UPDATE_WITH_JOIN,
      gs::FEATURE_NUMERIC_TYPE,       gs::FEATURE_GEOGRAPHY,
      gs::FEATURE_JSON_TYPE,          gs::FEATURE_INTERVAL_TYPE,
  };
  gs::AnalyzerOptionsProto options;
  for (int i = 0; i < salt && i < 8; ++i) {
    options.mutable_language_options()->add_enabled_language_features(
        kFeatures[i]);
  }
  return SerializeOptions(options);
}

// (a) All threads share one options fingerprint: after the first build every
// analysis is a concurrent cache hit plus concurrent FindFunction/FindType
// lookups on the one shared SimpleCatalog -- the path whose safety rests on
// SimpleCatalog's documented internal locking. Each thread owns its own
// TestCatalog so the callback buffers allocate and free concurrently too.
void TestConcurrentAnalysesOnOneFingerprint() {
  constexpr int kThreads = 8;
  constexpr int kIterations = 100;
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&options] {
      TestCatalog catalog;
      AddTwoTables(&catalog);
      for (int i = 0; i < kIterations; ++i) {
        ls::AnalyzeResponse response;
        CHECK_TRUE(Analyze(options, "SELECT x FROM t", &catalog, &response,
                           nullptr) == 0);
        CHECK_TRUE(Analyze(options, "SELECT y FROM ds.t", &catalog, &response,
                           nullptr) == 0);
      }
      CHECK_TRUE(catalog.outstanding() == 0);
    });
  }
  for (std::thread& t : threads) t.join();
}

// (b) A distinct fingerprint per thread: every thread races to build its own
// cache entry (construction outside the lock, first insert wins), then keeps
// analyzing against it while other entries are still being built.
void TestConcurrentConstructionOnDistinctFingerprints() {
  constexpr int kThreads = 8;
  constexpr int kIterations = 25;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
      const std::string options = OptionsWithFingerprint(t + 1);
      TestCatalog catalog;
      AddTwoTables(&catalog);
      for (int i = 0; i < kIterations; ++i) {
        CHECK_TRUE(Analyze(options, "SELECT x FROM t", &catalog, nullptr,
                           nullptr) == 0);
      }
      CHECK_TRUE(catalog.outstanding() == 0);
    });
  }
  for (std::thread& t : threads) t.join();
}

// Several threads hammering the *same cold* fingerprint from the start: all
// may build, one insert wins, the losers' entries are dropped -- LSan
// verifies the dropped copies do not leak. Uses a fingerprint no other test
// uses (FEATURE_RANGE_TYPE) so the entry is guaranteed cold whatever ran
// before.
void TestColdKeyRace() {
  gs::AnalyzerOptionsProto proto;
  proto.mutable_language_options()->add_enabled_language_features(
      gs::FEATURE_RANGE_TYPE);
  const std::string options = SerializeOptions(proto);

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&options] {
      TestCatalog catalog;
      AddTwoTables(&catalog);
      CHECK_TRUE(
          Analyze(options, "SELECT x FROM t", &catalog, nullptr, nullptr) == 0);
      CHECK_TRUE(catalog.outstanding() == 0);
    });
  }
  for (std::thread& t : threads) t.join();
}

// ---- the leak-shaped loop --------------------------------------------------

// Many sequential analyses on one fingerprint. Everything per-call must be
// freed -- LSan's verdict on this binary is what pins it -- while the one
// cache entry per fingerprint is *retained by design*, reachable from the
// shim's static map, which is exactly what LSan does not report.
void TestSequentialAnalysesFreeEverything() {
  TestCatalog catalog;
  AddTwoTables(&catalog);
  const std::string options = SerializeOptions(gs::AnalyzerOptionsProto());
  for (int i = 0; i < 200; ++i) {
    ls::AnalyzeResponse response;
    CHECK_TRUE(
        Analyze(options, "SELECT x FROM t", &catalog, &response, nullptr) == 0);
    std::string error;
    CHECK_TRUE(Analyze(options, "SELECT 1 FROM no_such_table", &catalog,
                       nullptr, &error) == 3);
  }
  CHECK_TRUE(catalog.outstanding() == 0);
}

}  // namespace

int main() {
  TestResolvesBareAndQualifiedNames();
  TestNotFoundIsTheAnalyzersOwnError();
  TestMalformedTableProtoIsAnErrorNotUb();
  TestCallbackErrorStatusPropagates();
  TestConcurrentAnalysesOnOneFingerprint();
  TestConcurrentConstructionOnDistinctFingerprints();
  TestColdKeyRace();
  TestSequentialAnalysesFreeEverything();
  std::printf("OK\n");
  return 0;
}
