#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "lz4.h"
#include "lz4hc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

static bool ReadAllBytes(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  if (n < 0) return false;
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(n));
  if (!out.empty()) {
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) return false;
  }
  return true;
}

static bool WriteAllBytes(const fs::path& p, const std::vector<uint8_t>& data) {
  std::ofstream f(p, std::ios::binary);
  if (!f) return false;
  if (!data.empty()) {
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) return false;
  }
  return true;
}

static void WriteBEU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t((v >> 24) & 0xFF));
  out.push_back(uint8_t((v >> 16) & 0xFF));
  out.push_back(uint8_t((v >> 8) & 0xFF));
  out.push_back(uint8_t(v & 0xFF));
}
static void WriteBEI32(std::vector<uint8_t>& out, int32_t v) {
  WriteBEU32(out, static_cast<uint32_t>(v));
}

static std::string ToLower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

static bool IsJpegExt(const std::string& ext) {
  auto e = ToLower(ext);
  return e == ".jpg" || e == ".jpeg";
}
static bool IsGifExt(const std::string& ext) {
  return ToLower(ext) == ".gif";
}

static bool TryGetImageSize(const fs::path& p, int& w, int& h) {
  int x=0,y=0,comp=0;
  if (!stbi_info(p.string().c_str(), &x, &y, &comp)) return false;
  w=x; h=y;
  return true;
}

static bool EncodeJpegToMemory(const uint8_t* rgba, int w, int h, int quality,
                              std::vector<uint8_t>& out_jpg) {
  out_jpg.clear();
  auto cb = [](void* ctx, void* data, int size) {
    auto* v = reinterpret_cast<std::vector<uint8_t>*>(ctx);
    auto* p = reinterpret_cast<uint8_t*>(data);
    v->insert(v->end(), p, p + size);
  };
  // stb expects components count; we provide RGBA but request 4 components; it will encode RGB, ignoring alpha.
  int ok = stbi_write_jpg_to_func(cb, &out_jpg, w, h, 4, rgba, quality);
  return ok != 0;
}

// Convert RGBA -> rgb8565 payload (type 72) layout used by your unpacker:
// for each pixel: [uint16 little-endian RGB565][uint8 alpha]
static std::vector<uint8_t> RGBA_To_RGB8565(const uint8_t* rgba, int w, int h) {
  const size_t pixels = size_t(w) * size_t(h);
  std::vector<uint8_t> out;
  out.resize(pixels * 3);
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t r = rgba[i * 4 + 0];
    uint8_t g = rgba[i * 4 + 1];
    uint8_t b = rgba[i * 4 + 2];
    uint8_t a = rgba[i * 4 + 3];

    uint16_t r5 = uint16_t((r * 31 + 127) / 255);
    uint16_t g6 = uint16_t((g * 63 + 127) / 255);
    uint16_t b5 = uint16_t((b * 31 + 127) / 255);
    uint16_t rgb565 = uint16_t((r5 << 11) | (g6 << 5) | b5);

    out[i * 3 + 0] = uint8_t(rgb565 & 0xFF);
    out[i * 3 + 1] = uint8_t((rgb565 >> 8) & 0xFF);
    out[i * 3 + 2] = a;
  }
  return out;
}

struct Chunk {
  std::string name;          // filename used in config
  bool is_z = false;         // z_ prefix -> z section
  std::vector<uint8_t> data; // full chunk bytes (16-byte header + payload)
  uint32_t offset = 0;       // offset within its section (img or z_img)
  uint32_t length = 0;       // total chunk length
};

static void WriteChunkHeader(std::vector<uint8_t>& out,
                            uint8_t img_type,
                            bool compressed,
                            uint32_t payload_len,
                            int w, int h) {
  // The format uses 16 bytes:
  // [0]=img_type
  // [1]=compressed flag (0 or 1)
  // [2..4]=payload_len (24-bit little-endian)
  // [5..7]=height/width nibble-packed:
  //   height low8 at [5]
  //   height high4 in low nibble of [6]
  //   width low4 in high nibble of [6]
  //   width remaining in [7] (width >> 4)
  // [8..15]=zeros
  if (payload_len > 0xFFFFFFu) {
    throw std::runtime_error("payload_len exceeds 24-bit field ( > 16,777,215 )");
  }
  out.push_back(img_type);
  out.push_back(compressed ? 1 : 0);
  out.push_back(uint8_t(payload_len & 0xFF));
  out.push_back(uint8_t((payload_len >> 8) & 0xFF));
  out.push_back(uint8_t((payload_len >> 16) & 0xFF));

  out.push_back(uint8_t(h & 0xFF));
  uint8_t b6 = uint8_t(((h >> 8) & 0x0F) | ((w & 0x0F) << 4));
  out.push_back(b6);
  out.push_back(uint8_t((w >> 4) & 0xFF));

  for (int i = 0; i < 8; ++i) out.push_back(0);
}

static std::vector<uint8_t> MakeJpegChunk(const fs::path& jpg_path) {
  int w=0,h=0,comp=0;
  unsigned char* pixels = stbi_load(jpg_path.string().c_str(), &w, &h, &comp, 4);
  if (!pixels) throw std::runtime_error("failed to load image: " + jpg_path.string());

  std::vector<uint8_t> jpg_bytes;
  // Re-encode to make sure header width/height matches content, and to ensure consistent output.
  if (!EncodeJpegToMemory(pixels, w, h, /*quality*/95, jpg_bytes)) {
    stbi_image_free(pixels);
    throw std::runtime_error("failed to encode JPEG: " + jpg_path.string());
  }
  stbi_image_free(pixels);

  std::vector<uint8_t> chunk;
  WriteChunkHeader(chunk, /*img_type*/9, /*compressed*/false, uint32_t(jpg_bytes.size()), w, h);
  chunk.insert(chunk.end(), jpg_bytes.begin(), jpg_bytes.end());
  return chunk;
}

static std::vector<uint8_t> MakeGifChunk(const fs::path& gif_path) {
  int w=0,h=0;
  if (!TryGetImageSize(gif_path, w, h)) {
    throw std::runtime_error("failed to read GIF size: " + gif_path.string());
  }
  std::vector<uint8_t> gif_bytes;
  if (!ReadAllBytes(gif_path, gif_bytes)) {
    throw std::runtime_error("failed to read GIF: " + gif_path.string());
  }
  std::vector<uint8_t> chunk;
  WriteChunkHeader(chunk, /*img_type*/3, /*compressed*/false, uint32_t(gif_bytes.size()), w, h);
  chunk.insert(chunk.end(), gif_bytes.begin(), gif_bytes.end());
  return chunk;
}

// If a file already looks like a prebuilt chunk (RGB/COMP pipeline), keep it as-is.
static bool LooksLikePrebuiltChunk(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 16) return false;
  uint8_t img_type = bytes[0];
  uint8_t compressed = bytes[1];
  if (!(compressed == 0 || compressed == 1)) return false;
  static const std::set<uint8_t> ok = {3,9,71,72,73,74,75};
  if (!ok.count(img_type)) return false;
  return true;
}

static std::vector<uint8_t> MakeRGB8565ChunkFromImage(const fs::path& img_path, bool enable_lz4) {
  int w=0,h=0,comp=0;
  unsigned char* rgba = stbi_load(img_path.string().c_str(), &w, &h, &comp, 4);
  if (!rgba) throw std::runtime_error("failed to load image: " + img_path.string());

  std::vector<uint8_t> payload = RGBA_To_RGB8565(rgba, w, h);
  stbi_image_free(rgba);

  std::vector<uint8_t> chunk;

  if (enable_lz4) {
    int src_size = (int)payload.size();
    int max_dst = LZ4_compressBound(src_size);
    std::vector<uint8_t> comp_bytes;
    comp_bytes.resize((size_t)max_dst);

    int comp_size = LZ4_compress_HC(reinterpret_cast<const char*>(payload.data()),
                                    reinterpret_cast<char*>(comp_bytes.data()),
                                    src_size,
                                    max_dst,
                                    LZ4HC_CLEVEL_MAX);
    if (comp_size > 0 && comp_size < src_size) {
      comp_bytes.resize((size_t)comp_size);
      WriteChunkHeader(chunk, /*img_type*/72, /*compressed*/true, (uint32_t)payload.size(), w, h);
      chunk.insert(chunk.end(), comp_bytes.begin(), comp_bytes.end());
      return chunk;
    }
    // else: no gain; fall through to uncompressed
  }

  WriteChunkHeader(chunk, /*img_type*/72, /*compressed*/false, (uint32_t)payload.size(), w, h);
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  return chunk;
}

struct Args {
  fs::path thumbnail;
  fs::path src;
  std::optional<fs::path> output_folder;
  std::optional<uint32_t> clock_id;
  bool idle = false;
  bool in_clock = false;   // set high bit (built-in)
  bool lz4 = true;         // compress RGB chunks if it helps
};

static void Usage(const char* exe) {
  std::cerr
    << "Usage:"
    << std::endl
    << "  " << exe << " --thumbnail <image> --src <folder> [--output-folder <folder>] [--clock-id <n>] [--idle] [--in] [--no-lz4]"
    << std::endl
    << std::endl
    << "Notes:"
    << std::endl
    << "  - Output filename is always: Clock{id}_res"
    << std::endl
    << "  - The base id must be 50000..65535 (inclusive)"
    << std::endl;
}

static bool ParseU32(const std::string& s, uint32_t& out) {
  char* end=nullptr;
  unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (!end || *end!='\0') return false;
  if (v > 0xFFFFFFFFull) return false;
  out = (uint32_t)v;
  return true;
}

static std::optional<Args> ParseArgs(int argc, char** argv) {
  Args a;
  for (int i=1;i<argc;i++) {
    std::string k = argv[i];
    auto need = [&](const char* flag)->std::optional<std::string>{
      if (i+1>=argc) { std::cerr<<"Missing value for "<<flag<<std::endl; return std::nullopt; }
      return std::string(argv[++i]);
    };

    if (k=="--thumbnail") {
      auto v=need("--thumbnail"); if(!v) return std::nullopt;
      a.thumbnail=*v;
    } else if (k=="--src") {
      auto v=need("--src"); if(!v) return std::nullopt;
      a.src=*v;
    } else if (k=="--output-folder") {
      auto v=need("--output-folder"); if(!v) return std::nullopt;
      a.output_folder=fs::path(*v);
    } else if (k=="--clock-id") {
      auto v=need("--clock-id"); if(!v) return std::nullopt;
      uint32_t id=0;
      if(!ParseU32(*v,id)) { std::cerr<<"Invalid --clock-id"<<std::endl; return std::nullopt; }
      a.clock_id=id;
    } else if (k=="--idle") {
      a.idle=true;
    } else if (k=="--in") {
      a.in_clock=true;
    } else if (k=="--no-lz4") {
      a.lz4=false;
    } else {
      std::cerr<<"Unknown arg: "<<k<<std::endl;
      return std::nullopt;
    }
  }
  if (a.thumbnail.empty() || a.src.empty()) return std::nullopt;
  return a;
}

// Mapping from the python generator script.
static const std::unordered_map<std::string, uint32_t> kClockIdPrefix = {
  {"454_454", 983040},
  {"400_400", 917504},
  {"466_466", 851968},
  {"390_390", 786432},
  {"410_502", 720896},
  {"320_384", 655360},
  {"320_385", 655360},
  {"368_448", 589824},
  {"390_450", 524288},
  {"360_360", 458752},
};

static std::string DetectClockSizeFromFirstConfigImage(const json& config, const fs::path& src_dir) {
  for (const auto& layer : config) {
    if (!layer.contains("imgArr")) continue;
    for (const auto& img : layer["imgArr"]) {
      if (img.is_string()) {
        fs::path p = src_dir / img.get<std::string>();
        int w=0,h=0;
        if (!TryGetImageSize(p, w, h)) continue;
        return std::to_string(w) + "_" + std::to_string(h);
      }
      if (img.is_array() && img.size() >= 3 && img[2].is_string()) {
        fs::path p = src_dir / img[2].get<std::string>();
        int w=0,h=0;
        if (!TryGetImageSize(p, w, h)) continue;
        return std::to_string(w) + "_" + std::to_string(h);
      }
    }
  }
  return "";
}

struct ClockIds {
  uint32_t base = 0;   // 50000..65535
  uint32_t full = 0;   // base OR prefix OR optional 0x80000000
};

static ClockIds DeriveClockIds(const Args& args, const std::string& clock_size) {
  uint32_t base = 0;
  if (args.clock_id) {
    base = *args.clock_id;
  } else {
    std::string name = args.src.filename().string();
    std::string digits;
    for (char c : name) if (std::isdigit((unsigned char)c)) digits.push_back(c);
    if (digits.empty()) throw std::runtime_error("Could not derive clock id from --src folder name; pass --clock-id");
    base = (uint32_t)std::stoul(digits);
  }

  if (base < 50000u || base > 65535u) {
    throw std::runtime_error("Clock id must be in range 50000..65535 (got " + std::to_string(base) + ")");
  }

  auto it = kClockIdPrefix.find(clock_size);
  if (it == kClockIdPrefix.end()) {
    throw std::runtime_error("Unsupported watchface resolution '" + clock_size + "' (no known clock-id prefix mapping)");
  }

  uint32_t full = base | it->second;
  if (args.in_clock) full |= 0x80000000u;

  ClockIds ids;
  ids.base = base;
  ids.full = full;
  return ids;
}

static void ValidateThumbnail(const fs::path& thumb_path) {
  int w=0,h=0;
  if (!TryGetImageSize(thumb_path, w, h)) throw std::runtime_error("Failed to read thumbnail image size");
  bool ok = (w==300 && h==300) || (w==210 && h==256);
  if (!ok) {
    throw std::runtime_error("Thumbnail must be 300x300 or 210x256 (got " + std::to_string(w) + "x" + std::to_string(h) + ")");
  }
}

static json LoadConfig(const fs::path& src_dir) {
  fs::path cfg = src_dir / "config.json";
  std::ifstream f(cfg);
  if (!f) throw std::runtime_error("Failed to open config.json in --src");
  json j;
  f >> j;
  if (!j.is_array()) throw std::runtime_error("config.json must be a JSON array");
  return j;
}

int main(int argc, char** argv) {
  auto args_opt = ParseArgs(argc, argv);
  if (!args_opt) { Usage(argv[0]); return 1; }
  Args args = *args_opt;

  if (!fs::exists(args.src) || !fs::is_directory(args.src)) {
    std::cerr << "Error: --src is not a directory" << std::endl;
    return 2;
  }

  try {
    ValidateThumbnail(args.thumbnail);
    json config = LoadConfig(args.src);

    // Basic sanity: imgArr length should match num for each layer (python script enforces this).
    for (size_t i=0;i<config.size();++i) {
      const auto& layer = config[i];
      if (!layer.contains("num") || !layer.contains("imgArr")) continue;
      int num = layer["num"].get<int>();
      int have = (int)layer["imgArr"].size();
      if (num != have) {
        throw std::runtime_error("Layer[" + std::to_string(i) + "]: num != imgArr.size()");
      }
    }

    std::string clock_size = DetectClockSizeFromFirstConfigImage(config, args.src);
    if (clock_size.empty()) throw std::runtime_error("Could not detect clock size from config images");
    ClockIds ids = DeriveClockIds(args, clock_size);
    uint32_t clock_id = ids.full;

    // Build a set of referenced images.
    std::set<std::string> img_names;
    for (const auto& layer : config) {
      if (!layer.contains("imgArr")) continue;
      for (const auto& img : layer["imgArr"]) {
        if (img.is_string()) {
          auto s = img.get<std::string>();
          if (!s.empty()) img_names.insert(s);
        } else if (img.is_array() && img.size() >= 3 && img[2].is_string()) {
          auto s = img[2].get<std::string>();
          if (!s.empty()) img_names.insert(s);
        }
      }
    }

    // Build chunks.
    std::vector<Chunk> chunks;
    chunks.reserve(img_names.size());

    std::vector<Chunk> z_chunks;
    z_chunks.reserve(16);

    auto make_from_file = [&](const std::string& name) -> Chunk {
      Chunk c;
      c.name = name;
      c.is_z = (name.rfind("z_", 0) == 0);

      fs::path p = args.src / name;
      if (!fs::exists(p)) throw std::runtime_error("Missing image referenced by config: " + p.string());

      std::string ext = ToLower(p.extension().string());

      std::vector<uint8_t> bytes;
      if (!ReadAllBytes(p, bytes)) throw std::runtime_error("Failed to read: " + p.string());

      if (LooksLikePrebuiltChunk(bytes)) {
        // Keep as-is.
        c.data = std::move(bytes);
      } else if (IsJpegExt(ext)) {
        c.data = MakeJpegChunk(p);
      } else if (IsGifExt(ext)) {
        c.data = MakeGifChunk(p);
      } else {
        // Treat as "decodeable image" (png/bmp/etc.) and convert to rgb8565.
        c.data = MakeRGB8565ChunkFromImage(p, args.lz4);
      }

      c.length = (uint32_t)c.data.size();
      return c;
    };

    for (const auto& name : img_names) {
      Chunk c = make_from_file(name);
      if (c.is_z) z_chunks.push_back(std::move(c));
      else chunks.push_back(std::move(c));
    }

    // Assign offsets in each section.
    uint32_t img_off = 0;
    std::unordered_map<std::string, std::pair<uint32_t,uint32_t>> img_objs; // name -> (offset,len)
    for (auto& c : chunks) {
      c.offset = img_off;
      img_objs[c.name] = {c.offset, c.length};
      img_off += c.length;
    }
    uint32_t z_off = 0;
    for (auto& c : z_chunks) {
      c.offset = z_off;
      img_objs[c.name] = {c.offset, c.length}; // stored offset within z-section; absolute added later
      z_off += c.length;
    }

    // Thumbnail chunk (always JPEG type 9, stored as a complete chunk).
    // We decode the thumbnail, validate, then encode as JPEG.
    int tw=0,th=0,tc=0;
    unsigned char* tpx = stbi_load(args.thumbnail.string().c_str(), &tw, &th, &tc, 4);
    if (!tpx) throw std::runtime_error("Failed to load thumbnail: " + args.thumbnail.string());
    std::vector<uint8_t> thumb_jpg;
    if (!EncodeJpegToMemory(tpx, tw, th, 95, thumb_jpg)) {
      stbi_image_free(tpx);
      throw std::runtime_error("Failed to encode thumbnail JPEG");
    }
    stbi_image_free(tpx);
    std::vector<uint8_t> thumb_chunk;
    WriteChunkHeader(thumb_chunk, 9, false, (uint32_t)thumb_jpg.size(), tw, th);
    thumb_chunk.insert(thumb_chunk.end(), thumb_jpg.begin(), thumb_jpg.end());

    // Sections.
    std::vector<uint8_t> img_section;
    img_section.reserve(img_off);
    for (const auto& c : chunks) img_section.insert(img_section.end(), c.data.begin(), c.data.end());

    std::vector<uint8_t> z_section;
    z_section.reserve(z_off);
    for (const auto& c : z_chunks) z_section.insert(z_section.end(), c.data.begin(), c.data.end());

    // Layer table.
    // This mirrors the python script's serialization rules.
    const uint32_t thumb_start = 32;
    const uint32_t thumb_len = (uint32_t)thumb_chunk.size();
    const uint32_t img_start = thumb_start + thumb_len;
    const uint32_t img_len = (uint32_t)img_section.size();
    const uint32_t z_start = img_start + img_len;
    const uint32_t z_len = (uint32_t)z_section.size();
    const uint32_t layer_start = z_start + z_len;

    std::vector<uint8_t> layer_data;

    for (const auto& layer : config) {
      int drawType = layer.value("drawType", 0);
      int dataType = layer.value("dataType", 0);
      int alignType = layer.value("alignType", 0);
      int x = layer.value("x", 0);
      int y = layer.value("y", 0);
      int num = layer.value("num", 0);

      WriteBEI32(layer_data, drawType);
      WriteBEI32(layer_data, dataType);

      if (dataType == 130 || dataType == 59 || dataType == 52) {
        if (!layer.contains("interval")) throw std::runtime_error("Layer missing interval for dataType requiring it");
        WriteBEI32(layer_data, layer["interval"].get<int>());
      }
      if (dataType == 112) {
        if (!layer.contains("area_num")) throw std::runtime_error("Layer missing area_num for dataType 112");
        for (const auto& v : layer["area_num"]) WriteBEI32(layer_data, v.get<int>());
      }

      WriteBEI32(layer_data, alignType);
      WriteBEI32(layer_data, x);
      WriteBEI32(layer_data, y);
      WriteBEI32(layer_data, num);

      const auto& imgArr = layer["imgArr"];
      for (int idx = 0; idx < num; ++idx) {
        const auto& img = imgArr[idx];

        if ((drawType == 10 || drawType == 15 || drawType == 21) && img.is_array() && img.size() >= 3) {
          int p0 = img[0].get<int>();
          int p1 = img[1].get<int>();
          std::string name = img[2].get<std::string>();

          WriteBEI32(layer_data, p0);
          WriteBEI32(layer_data, p1);

          auto it = img_objs.find(name);
          if (it == img_objs.end()) throw std::runtime_error("config references unknown image: " + name);

          bool is_z = (name.rfind("z_", 0) == 0);
          uint32_t off = it->second.first;
          uint32_t len = it->second.second;

          if (is_z) WriteBEI32(layer_data, (int32_t)(z_start + off));
          else WriteBEI32(layer_data, (int32_t)off);
          WriteBEI32(layer_data, (int32_t)len);
          continue;
        }

        if (drawType == 55 && idx == 2 && img.is_string()) {
          std::string s = img.get<std::string>();
          if (s.size() > 30) s.resize(30);
          char buf[30];
          std::memset(buf, 0, sizeof(buf));
          std::memcpy(buf, s.data(), s.size());
          layer_data.insert(layer_data.end(), buf, buf + 30);
          continue;
        }

        if ((dataType == 64 || dataType == 65 || dataType == 66 || dataType == 67) && (idx == 10 || idx == 11) && img.is_number_integer()) {
          WriteBEI32(layer_data, img.get<int>());
          continue;
        }

        if (drawType == 8 && (idx == 0 || idx == 1 || idx == 2) && img.is_number_integer()) {
          WriteBEI32(layer_data, img.get<int>());
          continue;
        }

        if (img.is_number_integer()) {
          WriteBEI32(layer_data, img.get<int>());
          continue;
        }

        if (img.is_string()) {
          std::string name = img.get<std::string>();
          auto it = img_objs.find(name);
          if (it == img_objs.end()) throw std::runtime_error("config references unknown image: " + name);

          bool is_z = (name.rfind("z_", 0) == 0);
          uint32_t off = it->second.first;
          uint32_t len = it->second.second;

          if (is_z) WriteBEI32(layer_data, (int32_t)(z_start + off));
          else WriteBEI32(layer_data, (int32_t)off);
          WriteBEI32(layer_data, (int32_t)len);
          continue;
        }

        throw std::runtime_error("Unsupported imgArr entry type at layer drawType=" + std::to_string(drawType));
      }
    }

    // File header (32 bytes)
    std::vector<uint8_t> out;
    out.reserve(layer_start + layer_data.size());

    const char* magic = args.idle ? "II@*24dG" : "Sb@*O2GG";
    out.insert(out.end(), magic, magic + 8);
    WriteBEU32(out, clock_id);
    WriteBEU32(out, thumb_start);
    WriteBEU32(out, thumb_len);
    WriteBEU32(out, img_start);
    WriteBEU32(out, img_len);
    WriteBEU32(out, layer_start);

    // Sanity: header size must be 32
    if (out.size() != 32) throw std::runtime_error("Internal error: header size != 32");

    // Append sections.
    out.insert(out.end(), thumb_chunk.begin(), thumb_chunk.end());
    out.insert(out.end(), img_section.begin(), img_section.end());
    out.insert(out.end(), z_section.begin(), z_section.end());
    out.insert(out.end(), layer_data.begin(), layer_data.end());

    fs::path out_dir = args.output_folder ? *args.output_folder : args.src.parent_path();
    if (!out_dir.empty()) {
      std::error_code ec;
      fs::create_directories(out_dir, ec);
      if (ec) throw std::runtime_error("Failed to create output folder: " + out_dir.string());
    }

    fs::path out_path = out_dir / ("Clock" + std::to_string(ids.base) + "_res");
    if (!WriteAllBytes(out_path, out)) throw std::runtime_error("Failed to write output: " + out_path.string());

    std::cout << "Wrote: " << out_path << std::endl;
    std::cout << "clock_size=" << clock_size << " base_id=" << ids.base << " clock_id=0x" << std::hex << clock_id << std::dec << std::endl;
    std::cout << "thumb=" << thumb_len << " img=" << img_len << " z=" << z_len << " layer=" << layer_data.size() << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 3;
  }
}
