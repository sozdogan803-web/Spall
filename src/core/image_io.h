// PNG output for screenshots, recordings and self-test slices: RGBA8 input, written as RGB when
// opaque, with adaptive row filters and fixed-Huffman deflate (no zlib).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Writes `rgba` (width * height * 4 bytes, top row first). Creates missing parent folders.
// Returns false and sets `error` on failure.
bool writePng(const std::filesystem::path& path, int width, int height, const std::vector<unsigned char>& rgba,
              std::string& error);
