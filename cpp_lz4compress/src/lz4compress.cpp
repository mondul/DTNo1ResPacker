#include <vector>
#include <stdexcept>
#include <limits>

#include <emscripten/bind.h>
#include <emscripten/val.h>

extern "C" {
  #include "lz4.h"
  #include "lz4hc.h"
}

using emscripten::val;

static val lz4compress(val input) {
  // Require Uint8Array (not ArrayBuffer, not number[], etc.)
  if (!input.instanceof(val::global("Uint8Array"))) {
    throw std::invalid_argument("lz4compress expects a Uint8Array");
  }

  const size_t len = input["length"].as<size_t>();
  if (len == 0) {
    return val::global("Uint8Array").new_(0);
  }
  if (len > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::range_error("Input too large for LZ4 C API (max INT_MAX bytes)");
  }

  // Copy input into WASM memory
  std::vector<uint8_t> src(len);
  val srcView = val(emscripten::typed_memory_view(len, src.data()));
  srcView.call<void>("set", input);

  const int srcSize = static_cast<int>(len);
  const int maxDst = LZ4_compressBound(srcSize);
  std::vector<uint8_t> dst(static_cast<size_t>(maxDst));

  // Max possible HC level
  const int level = LZ4HC_CLEVEL_MAX; // values > MAX behave like MAX :contentReference[oaicite:2]{index=2}

  const int written = LZ4_compress_HC(
      reinterpret_cast<const char*>(src.data()),
      reinterpret_cast<char*>(dst.data()),
      srcSize,
      maxDst,
      level); // API: LZ4_compress_HC(...) :contentReference[oaicite:3]{index=3}

  if (written <= 0) {
    throw std::runtime_error("LZ4_compress_HC failed");
  }

  // Copy compressed bytes out into a fresh JS Uint8Array
  val out = val::global("Uint8Array").new_(written);
  out.call<void>(
      "set",
      val(emscripten::typed_memory_view(static_cast<size_t>(written), dst.data()))
  );
  return out;
}

EMSCRIPTEN_BINDINGS(lz4_wasm_bindings) {
  emscripten::function("lz4compress", &lz4compress);
}
