// Fuzz target for ParseHttpRequest.
#include "hyper_derp/http.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data,
                                       size_t size) {
  hyper_derp::HttpRequest req;
  auto result = hyper_derp::ParseHttpRequest(
      data, static_cast<int>(size), &req);
  return 0;
}
