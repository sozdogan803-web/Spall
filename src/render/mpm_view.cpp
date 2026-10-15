#include "render/mpm_view.h"

#include "render/blackbody.h"
#include "render/mpm_snapshots.h"
#include "sim/mpm_solver.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr unsigned kBackground = 0x16191du;  // the Cross-section window's background
constexpr int kMaxColours = 32;
constexpr float kMaxPointSize = 128.0f;
constexpr float kGlowMinT = 500.0f, kGlowMaxT = 4500.0f;  // K, the glow table's range
constexpr int kGlowLutSize = 256;
constexpr float kSmokeOpacity = 20.0f;  // optical depth of products at the explosive's density
constexpr float kMinPuffPx = 10.0f;    // products puffs never smaller, so a far cloud stays whole
constexpr int kGasLevels = 4;          // the composite blurs the products buffer from its mipmaps

float channel(unsigned rgb, int shift) { return static_cast<float>((rgb >> shift) & 255u) / 255.0f; }

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// A range of points to draw: of a chunk of the solver's store (chunk >= 0, `first` in the chunk) or
// of the frame that is bound (chunk -1).
struct Segment {
    int chunk;
    size_t first, count;
    size_t base;  // index in the whole store of the chunk's first particle (0 for a frame)
};

// Every particle once, the plates' ranges first and the projectile's last so that it stays on top
// (the projectile is the scene's first `projectile` particles).
std::vector<Segment> segmentsOf(const MpmSolver& solver, size_t projectile) {
    std::vector<Segment> plates, body;
    for (int c = 0; c < solver.chunkCount(); ++c) {
        const size_t first = solver.chunkFirst(c), count = solver.chunkSize(c);
        const size_t split = std::clamp(projectile, first, first + count) - first;
        if (split < count) plates.push_back({c, split, count - split, first});
        if (split > 0) body.push_back({c, 0, split, first});
    }
    plates.insert(plates.end(), body.begin(), body.end());
    return plates;
}

// The same for a frame, which keeps one particle in `stride`.
std::vector<Segment> segmentsOfFrame(size_t particles, size_t projectile, int stride) {
    const size_t s = static_cast<size_t>(std::max(stride, 1));
    const size_t split = std::min((projectile + s - 1) / s, particles);
    std::vector<Segment> out;
    if (split < particles) out.push_back({-1, split, particles - split, 0});
    if (split > 0) out.push_back({-1, 0, split, 0});
    return out;
}

}  // namespace

const char* mpmColourLabel(MpmColour c) {
    switch (c) {
    case MpmColour::Material: return "Material";
    case MpmColour::Stress: return "Von Mises stress";
    case MpmColour::PlasticStrain: return "Plastic strain";
    case MpmColour::Temperature: return "Temperature";
    case MpmColour::Speed: return "Speed";
    case MpmColour::Failure: return "Failure cause";
    default: return "?";
    }
}

MpmColourScale mpmColourScale(MpmColour c, const MpmScene& scene) {
    MpmColourScale s;
    switch (c) {
    case MpmColour::Stress: {
        // Up to the strongest material's flow stress at a plastic strain of 1, in 0.5 GPa steps.
        double top = 0.5e9;
        for (const phys::MpmMaterial& m : scene.materials) top = std::max(top, m.yield + m.hardening);
        s.hi = std::ceil(top / 0.5e9) * 0.5e9;
        s.display = 1e-9;
        s.unit = "GPa";
        break;
    }
    case MpmColour::PlasticStrain:
        s.hi = 1.0;
        s.viridis = true;
        break;
    case MpmColour::Temperature: {
        // Room temperature to the plates' highest melting point.
        double melt = 0;
        for (size_t i = 1; i < scene.materials.size(); ++i) melt = std::max(melt, scene.materials[i].meltK);
        s.lo = phys::kRoomTemperature;
        s.hi = std::max(melt > 0 ? melt : scene.materials.empty() ? 1800.0 : scene.materials[0].meltK, s.lo + 100);
        s.unit = "K";
        break;
    }
    case MpmColour::Speed:
        s.hi = std::max(scene.velocity, 1.0);
        s.unit = "m/s";
        s.viridis = true;
        break;
    default: break;
    }
    return s;
}

MpmView::MpmView() = default;

MpmView::~MpmView() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void MpmView::init(const std::filesystem::path& root) {
    const auto dir = root / "shaders" / "mpm";
    program_.graphics({{GL_VERTEX_SHADER, dir / "particles.vert"}, {GL_FRAGMENT_SHADER, dir / "particles.frag"}});
    products_.graphics({{GL_VERTEX_SHADER, dir / "products.vert"}, {GL_FRAGMENT_SHADER, dir / "products.frag"}});
    composite_.graphics({{GL_VERTEX_SHADER, dir / "fullscreen.vert"}, {GL_FRAGMENT_SHADER, dir / "gas.frag"}});
    gl::uploadLut(glowLut_, makeGlowLut(kGlowLutSize, kGlowMinT, kGlowMaxT));
    GLfloat range[2] = {1, 64};
    glGetFloatv(GL_POINT_SIZE_RANGE, range);
    maxPointSize_ = std::clamp(range[1], 16.0f, 512.0f);
    if (!vao_) glGenVertexArrays(1, &vao_);
    unused_.allocate(GL_SHADER_STORAGE_BUFFER, 256, nullptr, GL_STATIC_DRAW);
}

void MpmView::render(MpmSolver& solver, const Camera& camera, MpmColour mode, const std::vector<unsigned>& colours,
                     double shift, MpmSnapshots* snapshots, double t) {
    const int w = std::max(camera.width, 1), h = std::max(camera.height, 1);
    if (texture_.width() != w || texture_.height() != h) {
        texture_.allocate(w, h, GL_RGBA8);
        fbo_.attach({&texture_});
    }
    const bool fire = mode == MpmColour::Material;
    if (fire && (gas_.width() != w || gas_.height() != h)) {
        gas_.allocate(w, h, GL_RGBA16F, kGasLevels);
        gasFbo_.attach({&gas_});
    }
    fbo_.bind();
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(channel(kBackground, 16), channel(kBackground, 8), channel(kBackground, 0), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (solver.loaded() && solver.particleCount() > 0) {
        const MpmScene& s = solver.scene();
        const MpmColourScale scale = mpmColourScale(mode, s);
        const float halfW = static_cast<float>(0.5 * w / camera.scale);
        const float halfH = static_cast<float>(0.5 * h / camera.scale);
        // A pre-rendered run draws between its two frames around t, positions interpolated. A
        // decimated frame keeps one particle in `stride`, drawn that much larger.
        const int frames = snapshots && snapshots->complete() ? 1 : 0;
        const float lerp = frames ? snapshots->bind(t) : 0.0f;
        const int stride = frames ? snapshots->stride() : 1;
        const float spacingPx = static_cast<float>(s.spacing * camera.scale) * std::sqrt(static_cast<float>(stride));
        const std::vector<Segment> segments = frames ? segmentsOfFrame(snapshots->frameParticles(), static_cast<size_t>(s.projectileCount), stride)
                                                     : segmentsOf(solver, static_cast<size_t>(s.projectileCount));
        // Nothing is read from a binding the pass does not use, but all are bound.
        if (frames) {
            unused_.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
        } else {
            unused_.bindBase(GL_SHADER_STORAGE_BUFFER, 7);
            unused_.bindBase(GL_SHADER_STORAGE_BUFFER, 8);
        }
        // Draws the segments with the program in use: a chunk of the store is bound for each (one
        // that waits in system memory is copied into the window of video memory first).
        const auto drawPoints = [&](const gl::Program& pass) {
            const GLint base = pass.loc("uBase");
            for (const Segment& g : segments) {
                if (g.chunk >= 0) solver.chunkForReading(g.chunk).bindBase(GL_SHADER_STORAGE_BUFFER, 0);
                glUniform1ui(base, static_cast<GLuint>(g.base));
                glDrawArrays(GL_POINTS, static_cast<GLint>(g.first), static_cast<GLsizei>(g.count));
            }
        };
        glEnable(GL_PROGRAM_POINT_SIZE);
        glBindVertexArray(vao_);
        if (fire) {
            // Products: density, temperature and expansion added up per pixel ...
            gasFbo_.bind();
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            products_.use();
            products_.f("uDx", static_cast<float>(s.dx));
            products_.v2("uOrigin", static_cast<float>(s.originX), static_cast<float>(s.originY));
            products_.v2("uCentre", static_cast<float>(camera.cx), static_cast<float>(camera.cy));
            products_.v2("uHalfView", halfW, halfH);
            products_.f("uSpacingPx", spacingPx);
            products_.f("uMinPuff", kMinPuffPx);
            products_.f("uMaxPuff", maxPointSize_);
            products_.i("uFrames", frames);
            products_.f("uLerp", lerp);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            drawPoints(products_);
            glDisable(GL_BLEND);
            gas_.bind(0);
            glGenerateMipmap(GL_TEXTURE_2D);
            // ... then laid over the background as smoke and fire, covering every pixel.
            fbo_.bind();
            composite_.use();
            gas_.bind(0);
            glowLut_.bind(1);
            composite_.i("uGas", 0);
            composite_.i("uGlow", 1);
            composite_.v2("uGlowRange", kGlowMinT, kGlowMaxT);
            composite_.v3("uBackground", srgbToLinear(channel(kBackground, 16)), srgbToLinear(channel(kBackground, 8)),
                          srgbToLinear(channel(kBackground, 0)));
            composite_.f("uOpacity", kSmokeOpacity);
            composite_.f("uShadowStep", std::max(2.0f, 1.5f * spacingPx));
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        program_.use();
        glUniform2i(program_.loc("uN"), s.nx, s.ny);
        program_.f("uDx", static_cast<float>(s.dx));
        program_.v2("uOrigin", static_cast<float>(s.originX), static_cast<float>(s.originY));
        program_.v2("uCentre", static_cast<float>(camera.cx), static_cast<float>(camera.cy));
        program_.v2("uHalfView", halfW, halfH);
        // Slightly larger than the particle spacing so the material reads as solid.
        program_.f("uPointSize", std::clamp(1.25f * spacingPx, 1.0f, kMaxPointSize));
        program_.f("uMaxPoint", maxPointSize_);
        program_.f("uShift", static_cast<float>(shift));
        program_.i("uFrames", frames);
        program_.f("uLerp", lerp);
        program_.i("uMode", static_cast<int>(mode));
        program_.v2("uRange", static_cast<float>(scale.lo), static_cast<float>(scale.hi));
        glUniform4f(program_.loc("uBody"), static_cast<float>((s.bodyNose - s.bodyLength - s.originX) / s.dx),
                    static_cast<float>((s.bodyNose - s.originX) / s.dx), static_cast<float>(-s.originY / s.dx),
                    static_cast<float>(std::max(0.5 * s.diameter / s.dx, 1e-6)));
        program_.i("uDart", s.dart ? 1 : 0);
        float rgb[kMaxColours * 3] = {};
        for (size_t i = 0; i < colours.size() && i < kMaxColours; ++i) {
            rgb[3 * i + 0] = channel(colours[i], 16);
            rgb[3 * i + 1] = channel(colours[i], 8);
            rgb[3 * i + 2] = channel(colours[i], 0);
        }
        glUniform3fv(program_.loc("uColours[0]"), kMaxColours, rgb);
        glowLut_.bind(1);
        program_.i("uGlow", 1);
        program_.v2("uGlowRange", kGlowMinT, kGlowMaxT);
        // Products first, blended (the target's alpha stays 1), so the solid material stays on
        // top of the expanding gas; in Material colours the composite above drew them.
        if (!fire) {
            program_.i("uPass", 0);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            drawPoints(program_);
            glDisable(GL_BLEND);
        }
        program_.i("uPass", 1);
        drawPoints(program_);
        if (fire) {
            // Light of hot metal, added.
            program_.i("uPass", 2);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            drawPoints(program_);
            glDisable(GL_BLEND);
        }
        glBindVertexArray(0);
        glDisable(GL_PROGRAM_POINT_SIZE);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::check("MPM view");
}
