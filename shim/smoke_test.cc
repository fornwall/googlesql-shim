// Behavioral gate for the shipped artifacts: analyzes one statement through
// the same C ABI smallquery uses and asserts the analyzer *computed the
// right answer*, not merely that the build linked.
//
// Why this exists: a `-c opt` build of GoogleSQL once constant-folded
// `CAST(CAST(DATETIME "2006-01-02" AS DATE) AS DATETIME)` to the zero
// datetime for every input -- strict-aliasing UB in GoogleSQL, exploited
// consistently by clang 18 through 22 at -O2/-Oz and invisible to every
// build-time check (the leg was green; only a consumer running queries could
// see it). The release pipeline runs this test in every leg's exact build
// configuration, so a build that computes wrong answers fails its leg
// instead of shipping. -fno-strict-aliasing in .bazelrc is the standing
// mitigation; if it ever stops covering the bug, or a regression arrives by
// another road, this gate is the tripwire.

#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/match.h"
#include "googlesql/local_service/local_service.pb.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/options.pb.h"
#include "shim/shim.h"

namespace {

int fail(const std::string& message) {
  fprintf(stderr, "FAIL: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main() {
  gsql_service* svc = gsql_service_new();
  if (svc == nullptr) return fail("gsql_service_new returned null");

  googlesql::local_service::AnalyzeRequest request;
  request.set_sql_statement(
      R"(SELECT CAST(CAST(DATETIME "2006-01-02" AS DATE) AS DATETIME))");
  request.mutable_simple_catalog();  // Empty catalog; casts need no functions.
  request.mutable_options()
      ->mutable_language_options()
      ->add_enabled_language_features(googlesql::FEATURE_CIVIL_TIME);

  std::string request_bytes;
  if (!request.SerializeToString(&request_bytes)) {
    return fail("serializing AnalyzeRequest");
  }

  uint8_t* out = nullptr;
  size_t out_len = 0;
  char* err = nullptr;
  int code = gsql_call(svc, GSQL_ANALYZE,
                       reinterpret_cast<const uint8_t*>(request_bytes.data()),
                       request_bytes.size(), &out, &out_len, &err);
  if (code != 0) {
    std::string message = absl::StrCat("analyze failed with code ", code, ": ",
                                       err != nullptr ? err : "(no message)");
    if (err != nullptr) gsql_str_free(err);
    gsql_service_free(svc);
    return fail(message);
  }

  googlesql::local_service::AnalyzeResponse response;
  bool parsed = response.ParseFromArray(out, static_cast<int>(out_len));
  gsql_buf_free(out);
  gsql_service_free(svc);
  if (!parsed) return fail("parsing AnalyzeResponse");

  // The analyzer constant-folds the double cast into a datetime literal.
  // 2006-01-02 00:00:00 in DatetimeValue's packed encoding, via the public
  // constructor -- a different code path from the fold, so a fold that
  // produced the zero value (or any other wrong value) cannot match it.
  const googlesql::DatetimeValue expected =
      googlesql::DatetimeValue::FromYMDHMSAndMicros(2006, 1, 2, 0, 0, 0, 0);
  if (!expected.IsValid() || expected.Packed64DatetimeSeconds() == 0) {
    return fail("reference DatetimeValue is broken");
  }
  const std::string needle = absl::StrCat("bit_field_datetime_seconds: ",
                                          expected.Packed64DatetimeSeconds());
  const std::string debug = response.DebugString();
  if (!absl::StrContains(debug, needle)) {
    return fail(absl::StrCat(
        "folded literal is wrong: expected to find \"", needle,
        "\" in the resolved AST; analyzer response was:\n", debug));
  }

  printf("OK: folded datetime literal = %s\n", needle.c_str());
  return 0;
}
