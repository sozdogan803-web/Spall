// One particle of a pre-rendered frame (src/render/mpm_snapshots.h): only what the renderer
// draws, packed into 40 bytes (a Particle is 144) so several times as many frames fit the
// budget. Positions keep full precision; the drawn fields go to halves or bytes; stress is kept
// in MPa and the initial position in 1/32 cells.
#ifndef MPM_FRAME_GLSL
#define MPM_FRAME_GLSL

struct Frame {
    vec2 x;           // cells
    uint v;           // half2, m/s
    uint F01;         // half2 (f00, f01)
    uint F23;         // half2 (f10, f11)
    uint flags;
    uint matBurnFrac; // mat | burn * 255 << 8 | fracture * 255 << 16
    uint FzzTemp;     // half2 (Fzz, K)
    uint epsStress;   // half2 (epsP, von Mises MPa)
    uint X0;          // 16.16, 1/32 cells each
};

Frame packFrame(Particle p) {
    Frame f;
    f.x = p.x;
    f.v = packHalf2x16(p.v);
    f.F01 = packHalf2x16(p.F.xy);
    f.F23 = packHalf2x16(p.F.zw);
    f.flags = p.flags;
    f.matBurnFrac = (p.mat & 255u) | (uint(clamp(p.burn, 0.0, 1.0) * 255.0 + 0.5) << 8) |
                    (uint(clamp(p.fracture, 0.0, 1.0) * 255.0 + 0.5) << 16);
    f.FzzTemp = packHalf2x16(vec2(p.Fzz, min(p.temp, 60000.0)));
    f.epsStress = packHalf2x16(vec2(min(p.epsP, 60000.0), min(p.stress * 1e-6, 60000.0)));
    uvec2 q = uvec2(clamp(p.X0 * 32.0 + 0.5, vec2(0.0), vec2(65535.0)));
    f.X0 = q.x | (q.y << 16);
    return f;
}

// The drawn fields back in a Particle; the rest are zero.
Particle unpackFrame(Frame f) {
    Particle p;
    p.x = f.x;
    p.v = unpackHalf2x16(f.v);
    p.C = vec4(0.0);
    p.F = vec4(unpackHalf2x16(f.F01), unpackHalf2x16(f.F23));
    p.tau = vec4(0.0);
    vec2 a = unpackHalf2x16(f.FzzTemp), b = unpackHalf2x16(f.epsStress);
    p.Fzz = a.x;
    p.epsP = b.x;
    p.temp = a.y;
    p.vol = 0.0;
    p.mass = 0.0;
    p.mat = f.matBurnFrac & 255u;
    p.flags = f.flags;
    p.stress = b.y * 1e6;
    p.xlo = vec2(0.0);
    p.pressure = 0.0;
    p.burn = float((f.matBurnFrac >> 8) & 255u) / 255.0;
    p.X0 = vec2(float(f.X0 & 65535u), float(f.X0 >> 16)) / 32.0;
    p.tLight = 0.0;
    p.fracture = float((f.matBurnFrac >> 16) & 255u) / 255.0;
    p.fibre = vec2(0.0);
    p.kick = 0.0;
    p.pad = 0.0;
    return p;
}

#endif
