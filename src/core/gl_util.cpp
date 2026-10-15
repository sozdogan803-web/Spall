#include "core/gl_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace gl {

namespace {

GLenum pixelFormat(GLenum internalFormat) {
    switch (internalFormat) {
    case GL_R8:
    case GL_R16F:
    case GL_R32F: return GL_RED;
    case GL_RG8:
    case GL_RG16F:
    case GL_RG32F: return GL_RG;
    default: return GL_RGBA;
    }
}

int channelCount(GLenum internalFormat) {
    switch (pixelFormat(internalFormat)) {
    case GL_RED: return 1;
    case GL_RG: return 2;
    default: return 4;
    }
}

bool isHalf(GLenum internalFormat) {
    return internalFormat == GL_R16F || internalFormat == GL_RG16F || internalFormat == GL_RGBA16F;
}

// Expands #include "file" recursively. Each file gets a GLSL source-string number so that
// compiler messages can be mapped back to file:line.
std::string expandIncludes(const std::filesystem::path& file, int depth, std::vector<std::string>& files,
                           std::set<std::filesystem::path>& active) {
    if (depth > 16) throw std::runtime_error("Shader include nesting exceeds 16 levels: " + file.string());
    const std::filesystem::path key = std::filesystem::weakly_canonical(file);
    if (!active.insert(key).second) throw std::runtime_error("Shader include cycle at " + file.string());
    std::ifstream stream(file);
    if (!stream) throw std::runtime_error("Cannot read shader " + file.string());
    const int id = static_cast<int>(files.size());
    files.push_back(file.lexically_normal().string());

    std::ostringstream out;
    std::string line;
    int lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        const size_t hash = line.find_first_not_of(" \t");
        if (hash != std::string::npos && line.compare(hash, 8, "#include") == 0) {
            const size_t first = line.find('"');
            const size_t last = first == std::string::npos ? std::string::npos : line.find('"', first + 1);
            if (last == std::string::npos)
                throw std::runtime_error(file.string() + ":" + std::to_string(lineNumber) + ": malformed #include");
            const std::filesystem::path child = file.parent_path() / line.substr(first + 1, last - first - 1);
            out << "#line 1 " << files.size() << '\n';
            out << expandIncludes(child, depth + 1, files, active);
            out << "#line " << lineNumber + 1 << ' ' << id << '\n';
        } else {
            out << line << '\n';
            // #version must stay first; start numbering after it.
            if (lineNumber == 1 && line.rfind("#version", 0) == 0 && depth == 0) out << "#line 2 0\n";
        }
    }
    active.erase(key);
    return out.str();
}

// Rewrites driver messages such as "0(12) : error ..." (NVIDIA) or "ERROR: 0:12: ..." as
// "file:12: error ...".
std::string mapErrors(const std::string& log, const std::vector<std::string>& files) {
    static const std::regex nvidia(R"(^(\d+)\((\d+)\)\s*:\s*(.*)$)");
    static const std::regex khronos(R"(^\w+:\s*(\d+):(\d+):\s*(.*)$)");
    std::istringstream in(log);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, nvidia) || std::regex_match(line, m, khronos)) {
            const size_t index = std::stoul(m[1].str());
            const std::string name = index < files.size() ? files[index] : "source " + m[1].str();
            out << name << ':' << m[2].str() << ": " << m[3].str() << '\n';
        } else if (!line.empty()) {
            out << line << '\n';
        }
    }
    return out.str();
}

std::string shaderLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string text(static_cast<size_t>(std::max(length, 1)), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, text.data());
    text.resize(static_cast<size_t>(written));
    return text;
}

std::string programLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string text(static_cast<size_t>(std::max(length, 1)), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, text.data());
    text.resize(static_cast<size_t>(written));
    return text;
}

} // namespace

size_t freeVideoBytes() {
    if (!hasExtension("GL_NVX_gpu_memory_info")) return 0;
    GLint kb = 0;
    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &kb);
    return kb > 0 ? static_cast<size_t>(kb) * 1024 : 0;
}

size_t maxStorageBlockBytes() {
    GLint64 bytes = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &bytes);
    return bytes > 0 ? static_cast<size_t>(bytes) : 0;
}

void check(const char* operation) {
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%04X", error);
        throw std::runtime_error(std::string(operation) + ": OpenGL error " + buf);
    }
}

// ---------------------------------------------------------------------------------- Program

Program::~Program() {
    if (id_) glDeleteProgram(id_);
}

Program::Program(Program&& other) noexcept : id_(std::exchange(other.id_, 0)) {}

Program& Program::operator=(Program&& other) noexcept {
    if (this != &other) {
        if (id_) glDeleteProgram(id_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void Program::compute(const std::filesystem::path& path) { build({{GL_COMPUTE_SHADER, path}}); }

void Program::graphics(std::initializer_list<std::pair<GLenum, std::filesystem::path>> stages) {
    build({stages.begin(), stages.end()});
}

void Program::build(const std::vector<std::pair<GLenum, std::filesystem::path>>& stages) {
    const GLuint program = glCreateProgram();
    std::vector<GLuint> shaders;
    try {
        for (const auto& [kind, path] : stages) {
            std::vector<std::string> files;
            std::set<std::filesystem::path> active;
            const std::string source = expandIncludes(path, 0, files, active);
            const char* text = source.c_str();
            const GLuint shader = glCreateShader(kind);
            shaders.push_back(shader);
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) throw std::runtime_error(mapErrors(shaderLog(shader), files));
            glAttachShader(program, shader);
        }
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            std::string names;
            for (const auto& stage : stages) names += stage.second.filename().string() + " ";
            throw std::runtime_error("link failed (" + names + "): " + programLog(program));
        }
    } catch (...) {
        for (GLuint s : shaders) glDeleteShader(s);
        glDeleteProgram(program);
        throw;
    }
    for (GLuint s : shaders) {
        glDetachShader(program, s);
        glDeleteShader(s);
    }
    if (id_) glDeleteProgram(id_);
    id_ = program;
}

// -------------------------------------------------------------------------------- Texture2D

Texture2D::~Texture2D() { release(); }

Texture2D::Texture2D(Texture2D&& other) noexcept
    : id_(std::exchange(other.id_, 0)), width_(other.width_), height_(other.height_) {}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        release();
        id_ = std::exchange(other.id_, 0);
        width_ = other.width_;
        height_ = other.height_;
    }
    return *this;
}

void Texture2D::allocate(int width, int height, GLenum internalFormat, int levels) {
    if (width < 1 || height < 1 || width > 16384 || height > 16384) throw std::runtime_error("Invalid 2D texture size");
    release();
    glGenTextures(1, &id_);
    width_ = width;
    height_ = height;
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    check("Allocate 2D texture");
}

void Texture2D::release() {
    if (id_) glDeleteTextures(1, &id_);
    id_ = 0;
}

void Texture2D::bind(int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

void uploadLut(Texture2D& texture, const std::vector<float>& rgba) {
    const int width = static_cast<int>(rgba.size() / 4);
    texture.allocate(width, 1, GL_RGBA32F);
    glBindTexture(GL_TEXTURE_2D, texture.id());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 1, GL_RGBA, GL_FLOAT, rgba.data());
    check("Upload lookup table");
}

// ------------------------------------------------------------------------------ Framebuffer

Framebuffer::~Framebuffer() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
}

void Framebuffer::attach(std::initializer_list<const Texture2D*> colours, int level) {
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    std::vector<GLenum> buffers;
    int index = 0;
    for (const Texture2D* t : colours) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, t->id(), level);
        buffers.push_back(GL_COLOR_ATTACHMENT0 + index);
        width_ = std::max(1, t->width() >> level);
        height_ = std::max(1, t->height() >> level);
        ++index;
    }
    glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Framebuffer is incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Framebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

std::vector<unsigned char> readPixels(GLuint framebuffer, int width, int height) {
    std::vector<unsigned char> bottomUp(static_cast<size_t>(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    check("Read pixels");
    std::vector<unsigned char> topDown(bottomUp.size());
    const size_t row = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(topDown.data() + y * row, bottomUp.data() + (height - 1 - y) * row, row);
    return topDown;
}

// ----------------------------------------------------------------------------------- Buffer

Buffer::~Buffer() { release(); }

void Buffer::release() {
    if (id_) glDeleteBuffers(1, &id_);
    id_ = 0;
    size_ = 0;
    immutable_ = false;
}

Buffer::Buffer(Buffer&& other) noexcept
    : id_(other.id_), target_(other.target_), size_(other.size_), immutable_(other.immutable_) {
    other.id_ = 0;
    other.size_ = 0;
    other.immutable_ = false;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        id_ = other.id_;
        target_ = other.target_;
        size_ = other.size_;
        immutable_ = other.immutable_;
        other.id_ = 0;
        other.size_ = 0;
        other.immutable_ = false;
    }
    return *this;
}

void Buffer::allocate(GLenum target, size_t bytes, const void* data, GLenum usage) {
    if (immutable_) release();  // storage that was fixed once needs a new name
    if (!id_) glGenBuffers(1, &id_);
    target_ = target;
    size_ = bytes;
    glBindBuffer(target, id_);
    glBufferData(target, static_cast<GLsizeiptr>(bytes), data, usage);
}

bool Buffer::allocateHost(GLenum target, size_t bytes) {
    release();
    glGenBuffers(1, &id_);
    target_ = target;
    glBindBuffer(target, id_);
    while (glGetError() != GL_NO_ERROR) {}
    glBufferStorage(target, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_STORAGE_BIT | GL_CLIENT_STORAGE_BIT);
    if (glGetError() != GL_NO_ERROR) {
        release();
        return false;
    }
    size_ = bytes;
    immutable_ = true;
    return true;
}

void Buffer::update(const void* data, size_t bytes, size_t offset) {
    if (offset >= size_) return;
    glBindBuffer(target_, id_);
    glBufferSubData(target_, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(std::min(bytes, size_ - offset)), data);
}

void Buffer::bindBase(GLenum target, int index) const { glBindBufferBase(target, static_cast<GLuint>(index), id_); }

} // namespace gl
