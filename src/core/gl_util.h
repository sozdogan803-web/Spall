// Small RAII helpers over the OpenGL 4.6 API: shader programs (with #include and file:line
// error mapping), textures, framebuffers and buffers.
#pragma once

#include "core/gl.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace gl {

// Throws std::runtime_error naming `operation` if glGetError reports an error.
void check(const char* operation);

// Free dedicated video memory, bytes; 0 when the driver does not say (NVX_gpu_memory_info only).
size_t freeVideoBytes();

// The most the driver allows in one shader storage buffer, bytes (GL_MAX_SHADER_STORAGE_BLOCK_SIZE).
size_t maxStorageBlockBytes();

class Program {
public:
    Program() = default;
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&& other) noexcept;
    Program& operator=(Program&& other) noexcept;

    // Build from files. Throws std::runtime_error with "file:line: message" on failure and
    // leaves the previous program (if any) in place.
    void compute(const std::filesystem::path& path);
    void graphics(std::initializer_list<std::pair<GLenum, std::filesystem::path>> stages);

    void use() const { glUseProgram(id_); }
    GLuint id() const { return id_; }
    bool valid() const { return id_ != 0; }
    GLint loc(const char* name) const { return glGetUniformLocation(id_, name); }
    void i(const char* name, int v) const { glUniform1i(loc(name), v); }
    void f(const char* name, float v) const { glUniform1f(loc(name), v); }
    void v2(const char* name, float x, float y) const { glUniform2f(loc(name), x, y); }
    void v3(const char* name, float x, float y, float z) const { glUniform3f(loc(name), x, y, z); }
    void v4(const char* name, float x, float y, float z, float w) const { glUniform4f(loc(name), x, y, z, w); }
    void m4(const char* name, const float* m) const { glUniformMatrix4fv(loc(name), 1, GL_FALSE, m); }

private:
    GLuint id_ = 0;
    void build(const std::vector<std::pair<GLenum, std::filesystem::path>>& stages);
};

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    void allocate(int width, int height, GLenum internalFormat, int levels = 1);
    void release();
    void bind(int unit) const;
    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    GLuint id_ = 0;
    int width_ = 0, height_ = 0;
};

// 1D lookup table stored as a width x 1 RGBA32F 2D texture.
void uploadLut(Texture2D& texture, const std::vector<float>& rgba);

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Attaches the given colour textures (level `level`) in order. Throws if incomplete.
    void attach(std::initializer_list<const Texture2D*> colours, int level = 0);
    void bind() const;
    GLuint id() const { return fbo_; }

private:
    GLuint fbo_ = 0;
    int width_ = 0, height_ = 0;
};

// Reads the RGBA8 contents of a framebuffer, top row first.
std::vector<unsigned char> readPixels(GLuint framebuffer, int width, int height);

class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    void allocate(GLenum target, size_t bytes, const void* data, GLenum usage);
    // Storage the driver keeps in the computer's own memory (not the graphics card's) that shaders
    // can still not read quickly: it is for parking data, copied through a buffer in video memory.
    // False when the driver could not take it (the buffer is then empty).
    bool allocateHost(GLenum target, size_t bytes);
    void update(const void* data, size_t bytes, size_t offset = 0);
    void bindBase(GLenum target, int index) const;
    GLuint id() const { return id_; }
    size_t size() const { return size_; }

private:
    void release();

    GLuint id_ = 0;
    GLenum target_ = GL_UNIFORM_BUFFER;
    size_t size_ = 0;
    bool immutable_ = false;  // glBufferStorage buffers cannot be given new storage
};

} // namespace gl
