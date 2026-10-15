// Minimal OpenGL 4.6 core loader for Windows.
// GL 1.1 entry points come from <GL/gl.h> (exported by opengl32.dll); everything newer is
// declared here through an X-macro list and loaded with wglGetProcAddress by gl::load().
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>
#include <cstdint>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync* GLsync;
typedef void(APIENTRY* GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                    GLsizei length, const GLchar* message, const void* userParam);

// ---- Enums beyond GL 1.1 ----------------------------------------------------------------
#define GL_FRAGMENT_SHADER                 0x8B30
#define GL_VERTEX_SHADER                   0x8B31
#define GL_GEOMETRY_SHADER                 0x8DD9
#define GL_COMPUTE_SHADER                  0x91B9
#define GL_COMPILE_STATUS                  0x8B81
#define GL_LINK_STATUS                     0x8B82
#define GL_INFO_LOG_LENGTH                 0x8B84
#define GL_CURRENT_PROGRAM                 0x8B8D
#define GL_SHADING_LANGUAGE_VERSION        0x8B8C

#define GL_ARRAY_BUFFER                    0x8892
#define GL_ELEMENT_ARRAY_BUFFER            0x8893
#define GL_PIXEL_PACK_BUFFER               0x88EB
#define GL_UNIFORM_BUFFER                  0x8A11
#define GL_SHADER_STORAGE_BUFFER           0x90D2
#define GL_COPY_READ_BUFFER                0x8F36
#define GL_COPY_WRITE_BUFFER               0x8F37
#define GL_STREAM_DRAW                     0x88E0
#define GL_STREAM_READ                     0x88E1
#define GL_STATIC_DRAW                     0x88E4
#define GL_DYNAMIC_DRAW                    0x88E8
#define GL_DYNAMIC_READ                    0x88E9
#define GL_DYNAMIC_COPY                    0x88EA
#define GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT 0x90DF
#define GL_MAP_READ_BIT                    0x0001
#define GL_MAP_WRITE_BIT                   0x0002
#define GL_MAP_PERSISTENT_BIT              0x0040
#define GL_MAP_COHERENT_BIT                0x0080
#define GL_DYNAMIC_STORAGE_BIT             0x0100
#define GL_CLIENT_STORAGE_BIT              0x0200

#define GL_TEXTURE0                        0x84C0
#define GL_TEXTURE_3D                      0x806F
#define GL_TEXTURE_WRAP_R                  0x8072
#define GL_CLAMP_TO_EDGE                   0x812F
#define GL_CLAMP_TO_BORDER                 0x812D
#define GL_MIRRORED_REPEAT                 0x8370
#define GL_TEXTURE_BASE_LEVEL              0x813C
#define GL_TEXTURE_MAX_LEVEL               0x813D
#define GL_TEXTURE_BINDING_3D              0x806A
#define GL_R8                              0x8229
#define GL_RG8                             0x822B
#define GL_R16F                            0x822D
#define GL_R32F                            0x822E
#define GL_RG16F                           0x822F
#define GL_RG32F                           0x8230
#define GL_R32UI                           0x8236
#define GL_RGBA32F                         0x8814
#define GL_RGB16F                          0x881B
#define GL_RGBA16F                         0x881A
#define GL_R11F_G11F_B10F                  0x8C3A
#define GL_SRGB8_ALPHA8                    0x8C43
#define GL_RG                              0x8227
#define GL_RED_INTEGER                     0x8D94
#define GL_HALF_FLOAT                      0x140B
#define GL_BGRA                            0x80E1
#define GL_DEPTH_COMPONENT24               0x81A6
#define GL_DEPTH_COMPONENT32F              0x8CAC

#define GL_FRAMEBUFFER                     0x8D40
#define GL_READ_FRAMEBUFFER                0x8CA8
#define GL_DRAW_FRAMEBUFFER                0x8CA9
#define GL_FRAMEBUFFER_COMPLETE            0x8CD5
#define GL_COLOR_ATTACHMENT0               0x8CE0
#define GL_COLOR_ATTACHMENT1               0x8CE1
#define GL_COLOR_ATTACHMENT2               0x8CE2
#define GL_COLOR_ATTACHMENT3               0x8CE3
#define GL_DEPTH_ATTACHMENT                0x8D00
#define GL_FRAMEBUFFER_SRGB                0x8DB9

#define GL_READ_ONLY                       0x88B8
#define GL_WRITE_ONLY                      0x88B9
#define GL_READ_WRITE                      0x88BA
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#define GL_UNIFORM_BARRIER_BIT             0x00000004
#define GL_TEXTURE_FETCH_BARRIER_BIT       0x00000008
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#define GL_COMMAND_BARRIER_BIT             0x00000040
#define GL_PIXEL_BUFFER_BARRIER_BIT        0x00000080
#define GL_TEXTURE_UPDATE_BARRIER_BIT      0x00000100
#define GL_BUFFER_UPDATE_BARRIER_BIT       0x00000200
#define GL_FRAMEBUFFER_BARRIER_BIT         0x00000400
#define GL_SHADER_STORAGE_BARRIER_BIT      0x00002000
#define GL_ALL_BARRIER_BITS                0xFFFFFFFF

#define GL_TIME_ELAPSED                    0x88BF
#define GL_TIMESTAMP                       0x8E28
#define GL_QUERY_RESULT                    0x8866
#define GL_QUERY_RESULT_AVAILABLE          0x8867

#define GL_SYNC_GPU_COMMANDS_COMPLETE      0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT         0x00000001
#define GL_ALREADY_SIGNALED                0x911A
#define GL_TIMEOUT_EXPIRED                 0x911B
#define GL_CONDITION_SATISFIED             0x911C
#define GL_WAIT_FAILED                     0x911D

#define GL_MAJOR_VERSION                   0x821B
#define GL_MINOR_VERSION                   0x821C
#define GL_NUM_EXTENSIONS                  0x821D
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE     0x91BF
#define GL_MAX_3D_TEXTURE_SIZE             0x8073
#define GL_PROGRAM_POINT_SIZE              0x8642
#define GL_FUNC_ADD                        0x8006

#define GL_DEBUG_OUTPUT                    0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS        0x8242
#define GL_DEBUG_SEVERITY_HIGH             0x9146
#define GL_DEBUG_SEVERITY_MEDIUM           0x9147
#define GL_DEBUG_SEVERITY_LOW              0x9148
#define GL_DEBUG_SEVERITY_NOTIFICATION     0x826B
#define GL_DEBUG_TYPE_ERROR                0x824C

// NVX_gpu_memory_info (queried only if the extension is present)
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX   0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049

#define GL_MAX_SHADER_STORAGE_BLOCK_SIZE   0x90DE

// ---- Entry points loaded at runtime --------------------------------------------------------
#define BV_GL_FUNCTIONS(X)                                                                        \
    X(GLuint, glCreateShader, (GLenum type))                                                      \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)) \
    X(void, glCompileShader, (GLuint shader))                                                     \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))                          \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteShader, (GLuint shader))                                                      \
    X(GLuint, glCreateProgram, (void))                                                            \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                      \
    X(void, glDetachShader, (GLuint program, GLuint shader))                                      \
    X(void, glLinkProgram, (GLuint program))                                                      \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params))                        \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteProgram, (GLuint program))                                                    \
    X(void, glUseProgram, (GLuint program))                                                       \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                          \
    X(void, glUniform1i, (GLint location, GLint v0))                                              \
    X(void, glUniform2i, (GLint location, GLint v0, GLint v1))                                    \
    X(void, glUniform3i, (GLint location, GLint v0, GLint v1, GLint v2))                          \
    X(void, glUniform4i, (GLint location, GLint v0, GLint v1, GLint v2, GLint v3))                \
    X(void, glUniform1ui, (GLint location, GLuint v0))                                            \
    X(void, glUniform1f, (GLint location, GLfloat v0))                                            \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                    \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))        \
    X(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat* value))                  \
    X(void, glUniform2fv, (GLint location, GLsizei count, const GLfloat* value))                  \
    X(void, glUniform3fv, (GLint location, GLsizei count, const GLfloat* value))                  \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat* value))                  \
    X(void, glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void, glGenBuffers, (GLsizei n, GLuint* buffers))                                           \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                                  \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                         \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))       \
    X(void, glBufferStorage, (GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void, glGetBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, void* data))    \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))                           X(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)) \
    X(void*, glMapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)) \
    X(GLboolean, glUnmapBuffer, (GLenum target))                                                  \
    X(void, glCopyBufferSubData, (GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)) \
    X(void, glClearBufferData, (GLenum target, GLenum internalformat, GLenum format, GLenum type, const void* data)) \
    X(void, glClearBufferSubData, (GLenum target, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* arrays))                                       \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                              \
    X(void, glBindVertexArray, (GLuint array))                                                    \
    X(void, glEnableVertexAttribArray, (GLuint index))                                            \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
    X(void, glActiveTexture, (GLenum texture))                                                    \
    X(void, glTexImage3D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)) \
    X(void, glTexSubImage3D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)) \
    X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glTexStorage3D, (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)) \
    X(void, glGenerateMipmap, (GLenum target))                                                    \
    X(void, glBindImageTexture, (GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format)) \
    X(void, glCopyImageSubData, (GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth)) \
    X(void, glClearTexImage, (GLuint texture, GLint level, GLenum format, GLenum type, const void* data)) \
    X(void, glDispatchCompute, (GLuint x, GLuint y, GLuint z))                                    \
    X(void, glMemoryBarrier, (GLbitfield barriers))                                               \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* framebuffers))                                 \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                        \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                               \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                          \
    X(void, glDrawBuffers, (GLsizei n, const GLenum* bufs))                                       \
    X(void, glBlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)) \
    X(void, glClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat* value))             \
    X(void, glBlendFuncSeparate, (GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)) \
    X(void, glBlendEquation, (GLenum mode))                                                       \
    X(void, glBlendFunci, (GLuint buf, GLenum src, GLenum dst))                                   \
    X(void, glEnablei, (GLenum target, GLuint index))                                             \
    X(void, glDisablei, (GLenum target, GLuint index))                                            \
    X(void, glGenQueries, (GLsizei n, GLuint* ids))                                               \
    X(void, glDeleteQueries, (GLsizei n, const GLuint* ids))                                      \
    X(void, glQueryCounter, (GLuint id, GLenum target))                                           \
    X(void, glGetQueryObjectiv, (GLuint id, GLenum pname, GLint* params))                         \
    X(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64* params))                   \
    X(void, glGetInteger64v, (GLenum pname, GLint64* data))                                       \
    X(GLsync, glFenceSync, (GLenum condition, GLbitfield flags))                                  \
    X(GLenum, glClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                \
    X(void, glDeleteSync, (GLsync sync))                                                          \
    X(const GLubyte*, glGetStringi, (GLenum name, GLuint index))                                  \
    X(void, glGetIntegeri_v, (GLenum target, GLuint index, GLint* data))                          \
    X(void, glDebugMessageCallback, (GLDEBUGPROC callback, const void* userParam))                \
    X(void, glDebugMessageControl, (GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled))

#define BV_GL_DECLARE(ret, name, params)          \
    typedef ret(APIENTRY* PFN_BV_##name) params; \
    extern PFN_BV_##name name;
BV_GL_FUNCTIONS(BV_GL_DECLARE)
#undef BV_GL_DECLARE

namespace gl {
// Loads every entry point in BV_GL_FUNCTIONS. On failure returns false and stores the name
// of the first missing function in *missing (if non-null). Requires a current context.
bool load(const char** missing = nullptr);

// True if the current context advertises the named extension.
bool hasExtension(const char* name);
} // namespace gl
