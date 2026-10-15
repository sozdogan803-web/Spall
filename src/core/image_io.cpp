#include "core/image_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>

namespace {

uint32_t crc32(const unsigned char* data, size_t size, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v >> 24));
    out.push_back(static_cast<unsigned char>(v >> 16));
    out.push_back(static_cast<unsigned char>(v >> 8));
    out.push_back(static_cast<unsigned char>(v));
}

void chunk(std::vector<unsigned char>& file, const char* type, const std::vector<unsigned char>& data) {
    put32(file, static_cast<uint32_t>(data.size()));
    const size_t start = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), data.begin(), data.end());
    put32(file, crc32(file.data() + start, file.size() - start));
}

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Scanlines with a filter chosen per row by the smallest sum of absolute residuals (the
// usual heuristic), each prefixed with its filter type. `bpp` bytes per pixel.
std::vector<unsigned char> filterRows(const unsigned char* pixels, int width, int height, int bpp) {
    const size_t row = static_cast<size_t>(width) * bpp;
    std::vector<unsigned char> out;
    out.reserve((row + 1) * height);
    std::vector<unsigned char> zero(row, 0), trial(row);
    std::array<std::vector<unsigned char>, 5> candidates;
    for (int y = 0; y < height; ++y) {
        const unsigned char* cur = pixels + y * row;
        const unsigned char* up = y > 0 ? pixels + (y - 1) * row : zero.data();
        int best = 0;
        long bestCost = -1;
        for (int f = 0; f < 5; ++f) {
            long cost = 0;
            for (size_t i = 0; i < row; ++i) {
                const int a = i >= static_cast<size_t>(bpp) ? cur[i - bpp] : 0;
                const int b = up[i];
                const int c = i >= static_cast<size_t>(bpp) ? up[i - bpp] : 0;
                const int predictor = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                trial[i] = static_cast<unsigned char>(cur[i] - predictor);
                cost += trial[i] < 128 ? trial[i] : 256 - trial[i];
            }
            candidates[f] = trial;
            if (bestCost < 0 || cost < bestCost) {
                bestCost = cost;
                best = f;
            }
        }
        out.push_back(static_cast<unsigned char>(best));
        out.insert(out.end(), candidates[best].begin(), candidates[best].end());
    }
    return out;
}

// Deflate (RFC 1951) with fixed Huffman codes and greedy LZ77 matching over a 32 KiB window.
class Deflater {
public:
    explicit Deflater(std::vector<unsigned char>& out) : out_(out) {}

    void compress(const std::vector<unsigned char>& data) {
        bits(1, 1);  // final block
        bits(1, 2);  // fixed Huffman codes
        std::vector<int> head(kHashSize, -1), prev(kWindow, -1);
        const size_t n = data.size();
        size_t i = 0;
        auto hash = [&](size_t p) {
            return ((data[p] << 10) ^ (data[p + 1] << 5) ^ data[p + 2]) & (kHashSize - 1);
        };
        auto insert = [&](size_t p) {
            if (p + 2 >= n) return;
            const uint32_t h = hash(p);
            prev[p & (kWindow - 1)] = head[h];
            head[h] = static_cast<int>(p);
        };
        while (i < n) {
            int bestLen = 0, bestDist = 0;
            if (i + 2 < n) {
                int candidate = head[hash(i)];
                const size_t maxLen = std::min<size_t>(258, n - i);
                for (int chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
                    const size_t dist = i - static_cast<size_t>(candidate);
                    if (dist > kWindow - 1) break;
                    size_t len = 0;
                    while (len < maxLen && data[candidate + len] == data[i + len]) ++len;
                    if (static_cast<int>(len) > bestLen) {
                        bestLen = static_cast<int>(len);
                        bestDist = static_cast<int>(dist);
                        if (len == maxLen) break;
                    }
                    const int next = prev[candidate & (kWindow - 1)];
                    if (next >= candidate) break;  // the slot was reused by a newer position
                    candidate = next;
                }
            }
            if (bestLen >= 3) {
                match(bestLen, bestDist);
                for (int k = 0; k < bestLen; ++k) insert(i + k);
                i += bestLen;
            } else {
                literal(data[i]);
                insert(i);
                ++i;
            }
        }
        symbol(256);  // end of block
        if (count_ > 0) out_.push_back(static_cast<unsigned char>(buffer_));
    }

private:
    static constexpr int kWindow = 32768, kHashSize = 1 << 15, kMaxChain = 48;

    // Least significant bit first, as deflate packs everything but Huffman codes.
    void bits(uint32_t value, int n) {
        buffer_ |= value << count_;
        count_ += n;
        while (count_ >= 8) {
            out_.push_back(static_cast<unsigned char>(buffer_));
            buffer_ >>= 8;
            count_ -= 8;
        }
    }
    // Huffman codes go most significant bit first.
    void code(uint32_t value, int n) {
        uint32_t reversed = 0;
        for (int k = 0; k < n; ++k) reversed |= ((value >> k) & 1) << (n - 1 - k);
        bits(reversed, n);
    }
    void symbol(int s) {
        if (s < 144) code(0x30 + s, 8);
        else if (s < 256) code(0x190 + (s - 144), 9);
        else if (s < 280) code(s - 256, 7);
        else code(0xC0 + (s - 280), 8);
    }
    void literal(unsigned char c) { symbol(c); }
    void match(int length, int distance) {
        static constexpr int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr int kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                              33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        int l = 28;
        while (kLenBase[l] > length) --l;
        symbol(257 + l);
        bits(static_cast<uint32_t>(length - kLenBase[l]), kLenExtra[l]);
        int d = 29;
        while (kDistBase[d] > distance) --d;
        code(static_cast<uint32_t>(d), 5);
        bits(static_cast<uint32_t>(distance - kDistBase[d]), kDistExtra[d]);
    }

    std::vector<unsigned char>& out_;
    uint32_t buffer_ = 0;
    int count_ = 0;
};

} // namespace

bool writePng(const std::filesystem::path& path, int width, int height, const std::vector<unsigned char>& rgba,
              std::string& error) {
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
        error = "writePng: pixel buffer does not match " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }

    // Opaque images are written as RGB.
    bool opaque = true;
    for (size_t i = 3; i < rgba.size() && opaque; i += 4) opaque = rgba[i] == 255;
    std::vector<unsigned char> rgb;
    if (opaque) {
        rgb.resize(static_cast<size_t>(width) * height * 3);
        for (size_t p = 0, q = 0; p < rgba.size(); p += 4, q += 3) {
            rgb[q] = rgba[p];
            rgb[q + 1] = rgba[p + 1];
            rgb[q + 2] = rgba[p + 2];
        }
    }
    const std::vector<unsigned char> raw = filterRows(opaque ? rgb.data() : rgba.data(), width, height, opaque ? 3 : 4);

    std::vector<unsigned char> z = {0x78, 0x01};
    Deflater(z).compress(raw);
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<unsigned char> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<unsigned char> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    // 8 bits per channel, RGB or RGBA, deflate, adaptive filtering, no interlace.
    header.insert(header.end(), {8, static_cast<unsigned char>(opaque ? 2 : 6), 0, 0, 0});
    chunk(file, "IHDR", header);
    chunk(file, "IDAT", z);
    chunk(file, "IEND", {});

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "Cannot write " + path.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) {
        error = "Write failed: " + path.string();
        return false;
    }
    return true;
}
