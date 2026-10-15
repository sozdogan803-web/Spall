// Draws the deformation simulation (src/sim/mpm_solver.h) as one point sprite per particle (a
// chunk of its store at a time, the plates before the projectile) into a texture that the armour Cross-section window shows, with the window's centre and
// scale. Colour by material or by a field with a colour bar (shaders/mpm/particles.vert). In
// Material colours explosive products are splatted into a float buffer and composited as smoke
// and fire (shaders/mpm/products.*, gas.frag), and the glow of hot metal.
#pragma once

#include "core/gl_util.h"

#include <filesystem>
#include <vector>

class MpmSolver;
class MpmSnapshots;
struct MpmScene;

enum class MpmColour { Material, Stress, PlasticStrain, Temperature, Speed, Failure, Count };

const char* mpmColourLabel(MpmColour c);

// Colour bar of a field mode: value range, unit (values are shown multiplied by `display`)
// and whether the map is viridis (else inferno).
struct MpmColourScale {
    double lo = 0, hi = 1;
    double display = 1;
    const char* unit = "";
    bool viridis = false;
};
MpmColourScale mpmColourScale(MpmColour c, const MpmScene& scene);

class MpmView {
public:
    MpmView();
    ~MpmView();
    MpmView(const MpmView&) = delete;
    MpmView& operator=(const MpmView&) = delete;

    // Compiles the shaders; throws on errors.
    void init(const std::filesystem::path& root);

    struct Camera {
        double cx = 0, cy = 0;  // m, view centre
        double scale = 1000;    // pixels per metre
        int width = 1, height = 1;
    };
    // Renders into the texture (resized to the camera) and leaves framebuffer 0 bound.
    // `colours` are display sRGB per scene material; `shift` moves the projectile along x (m).
    // With complete `snapshots` it draws their time t instead of the solver's particles. The
    // solver is not const: a chunk of its particles that waits in system memory is copied up to
    // be drawn.
    void render(MpmSolver& solver, const Camera& camera, MpmColour mode, const std::vector<unsigned>& colours,
                double shift, MpmSnapshots* snapshots = nullptr, double t = 0);
    GLuint texture() const { return texture_.id(); }

private:
    gl::Program program_;    // particles
    gl::Program products_;   // explosive products into gas_
    gl::Program composite_;  // gas_ over the background as smoke and fire
    gl::Texture2D texture_;
    gl::Framebuffer fbo_;
    gl::Texture2D gas_;      // RGBA16F: density / dense, x temperature (kK), x expansion
    gl::Framebuffer gasFbo_;
    gl::Texture2D glowLut_;  // makeGlowLut over kGlowMinT..kGlowMaxT
    gl::Buffer unused_;      // bound where a pass reads nothing
    float maxPointSize_ = 64;
    GLuint vao_ = 0;
};
