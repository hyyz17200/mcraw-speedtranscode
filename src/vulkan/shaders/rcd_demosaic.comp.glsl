#version 450

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer CalibratedRaw { float v[]; } raw_data;
layout(set = 0, binding = 1, std430) buffer CfaData { precise float v[]; } cfa_data;
layout(set = 0, binding = 2, std430) buffer RedData { precise float v[]; } red_data;
layout(set = 0, binding = 3, std430) buffer GreenData { precise float v[]; } green_data;
layout(set = 0, binding = 4, std430) buffer BlueData { precise float v[]; } blue_data;
layout(set = 0, binding = 5, std430) buffer VhData { precise float v[]; } vh_data;
layout(set = 0, binding = 6, std430) buffer AuxAData { precise float v[]; } aux_a;
layout(set = 0, binding = 7, std430) buffer AuxBData { precise float v[]; } aux_b;
layout(set = 0, binding = 8, std430) buffer AuxCData { precise float v[]; } aux_c;

layout(push_constant) uniform RcdParameters {
    uint width;
    uint height;
    uint cfa_pattern;
    uint pass_index;
} parameters;

const float eps = 1.0e-5;
const float epssq = 1.0e-10;
const float scale = 65536.0;

uint at(uint x, uint y) { return y * parameters.width + x; }
float cfa(int x, int y) { return cfa_data.v[uint(y) * parameters.width + uint(x)]; }
float red(int x, int y) { return red_data.v[uint(y) * parameters.width + uint(x)]; }
float green(int x, int y) { return green_data.v[uint(y) * parameters.width + uint(x)]; }
float blue(int x, int y) { return blue_data.v[uint(y) * parameters.width + uint(x)]; }
float vh(int x, int y) { return vh_data.v[uint(y) * parameters.width + uint(x)]; }
float sqr(float x) { return x * x; }
float intp(float a, float b, float c) { return a * (b - c) + c; }

uint color_at(uint x, uint y) {
    uint phase = ((y & 1U) << 1U) | (x & 1U);
    if (parameters.cfa_pattern == 0U) { // RGGB
        const uint colors[4] = uint[4](0U, 1U, 1U, 2U);
        return colors[phase];
    }
    if (parameters.cfa_pattern == 1U) { // BGGR
        const uint colors[4] = uint[4](2U, 1U, 1U, 0U);
        return colors[phase];
    }
    if (parameters.cfa_pattern == 2U) { // GRBG
        const uint colors[4] = uint[4](1U, 0U, 2U, 1U);
        return colors[phase];
    }
    const uint colors[4] = uint[4](1U, 2U, 0U, 1U); // GBRG
    return colors[phase];
}

float vertical_hpf(int x, int y) {
    float value = (cfa(x, y - 3) - cfa(x, y - 1) - cfa(x, y + 1) + cfa(x, y + 3))
                - 3.0 * (cfa(x, y - 2) + cfa(x, y + 2)) + 6.0 * cfa(x, y);
    return sqr(value);
}

float horizontal_hpf(int x, int y) {
    float value = (cfa(x - 3, y) - cfa(x - 1, y) - cfa(x + 1, y) + cfa(x + 3, y))
                - 3.0 * (cfa(x - 2, y) + cfa(x + 2, y)) + 6.0 * cfa(x, y);
    return sqr(value);
}

void initialize_pass(uint x, uint y, uint index) {
    float raw_value = clamp(raw_data.v[index] / scale, 0.0, 1.0);
    cfa_data.v[index] = raw_value;
    green_data.v[index] = raw_value;
    uint row_color = color_at(0U, y) == 1U ? color_at(1U, y) : color_at(0U, y);
    red_data.v[index] = row_color == 0U ? raw_value : 0.0;
    blue_data.v[index] = row_color == 2U ? raw_value : 0.0;
    vh_data.v[index] = 0.0;
    aux_a.v[index] = 0.0;
    aux_b.v[index] = 0.0;
    aux_c.v[index] = 0.0;
}

void direction_lpf_pass(uint x, uint y, uint index) {
    if (x >= 4U && x + 4U < parameters.width &&
        y >= 4U && y + 4U < parameters.height) {
        float vs = max(epssq, vertical_hpf(int(x), int(y) - 1) +
                              vertical_hpf(int(x), int(y)) +
                              vertical_hpf(int(x), int(y) + 1));
        float hs = max(epssq, horizontal_hpf(int(x) - 1, int(y)) +
                              horizontal_hpf(int(x), int(y)) +
                              horizontal_hpf(int(x) + 1, int(y)));
        vh_data.v[index] = vs / (vs + hs);
    }
    if (x >= 2U && x + 2U < parameters.width &&
        y >= 2U && y + 2U < parameters.height &&
        color_at(x, y) != 1U) {
        int ix = int(x);
        int iy = int(y);
        aux_a.v[index / 2U] = cfa(ix, iy)
            + 0.5 * (cfa(ix, iy - 1) + cfa(ix, iy + 1) +
                     cfa(ix - 1, iy) + cfa(ix + 1, iy))
            + 0.25 * (cfa(ix - 1, iy - 1) + cfa(ix + 1, iy - 1) +
                      cfa(ix - 1, iy + 1) + cfa(ix + 1, iy + 1));
    }
}

void green_pass(uint x, uint y, uint index) {
    if (x < 4U || x + 4U >= parameters.width ||
        y < 4U || y + 4U >= parameters.height || color_at(x, y) == 1U) return;
    int ix = int(x);
    int iy = int(y);
    float center = cfa(ix, iy);
    float ng = eps + abs(cfa(ix, iy - 1) - cfa(ix, iy + 1))
        + abs(center - cfa(ix, iy - 2))
        + abs(cfa(ix, iy - 1) - cfa(ix, iy - 3))
        + abs(cfa(ix, iy - 2) - cfa(ix, iy - 4));
    float sg = eps + abs(cfa(ix, iy - 1) - cfa(ix, iy + 1))
        + abs(center - cfa(ix, iy + 2))
        + abs(cfa(ix, iy + 1) - cfa(ix, iy + 3))
        + abs(cfa(ix, iy + 2) - cfa(ix, iy + 4));
    float wg = eps + abs(cfa(ix - 1, iy) - cfa(ix + 1, iy))
        + abs(center - cfa(ix - 2, iy))
        + abs(cfa(ix - 1, iy) - cfa(ix - 3, iy))
        + abs(cfa(ix - 2, iy) - cfa(ix - 4, iy));
    float eg = eps + abs(cfa(ix - 1, iy) - cfa(ix + 1, iy))
        + abs(center - cfa(ix + 2, iy))
        + abs(cfa(ix + 1, iy) - cfa(ix + 3, iy))
        + abs(cfa(ix + 2, iy) - cfa(ix + 4, iy));
    uint lp = index / 2U;
    float lpc = aux_a.v[lp];
    float ne = cfa(ix, iy - 1) * (lpc + lpc) / (eps + lpc + aux_a.v[lp - parameters.width]);
    float se = cfa(ix, iy + 1) * (lpc + lpc) / (eps + lpc + aux_a.v[lp + parameters.width]);
    float we = cfa(ix - 1, iy) * (lpc + lpc) / (eps + lpc + aux_a.v[lp - 1U]);
    float ee = cfa(ix + 1, iy) * (lpc + lpc) / (eps + lpc + aux_a.v[lp + 1U]);
    float ve = (sg * ne + ng * se) / (ng + sg);
    float he = (wg * ee + eg * we) / (eg + wg);
    float central = vh(ix, iy);
    float neighbor = 0.25 * (vh(ix - 1, iy - 1) + vh(ix + 1, iy - 1) +
                             vh(ix - 1, iy + 1) + vh(ix + 1, iy + 1));
    float disc = abs(0.5 - central) < abs(0.5 - neighbor) ? neighbor : central;
    green_data.v[index] = intp(disc, he, ve);
}

void pq_hpf_pass(uint x, uint y, uint index) {
    if (x < 3U || x + 3U >= parameters.width || y < 3U ||
        y + 3U >= parameters.height || (x & 1U) == 0U) return;
    int ix = int(x);
    int iy = int(y);
    float p = (cfa(ix - 3, iy - 3) - cfa(ix - 1, iy - 1) -
               cfa(ix + 1, iy + 1) + cfa(ix + 3, iy + 3))
            - 3.0 * (cfa(ix - 2, iy - 2) + cfa(ix + 2, iy + 2))
            + 6.0 * cfa(ix, iy);
    float q = (cfa(ix + 3, iy - 3) - cfa(ix + 1, iy - 1) -
               cfa(ix - 1, iy + 1) + cfa(ix - 3, iy + 3))
            - 3.0 * (cfa(ix + 2, iy - 2) + cfa(ix - 2, iy + 2))
            + 6.0 * cfa(ix, iy);
    aux_b.v[index / 2U] = sqr(p);
    aux_c.v[index / 2U] = sqr(q);
}

void pq_direction_pass(uint x, uint y, uint index) {
    if (x < 4U || x + 4U >= parameters.width || y < 4U ||
        y + 4U >= parameters.height || color_at(x, y) == 1U) return;
    uint center = index / 2U;
    uint nw = (index - parameters.width - 1U) / 2U;
    uint sw = (index + parameters.width - 1U) / 2U;
    float ps = max(epssq, aux_b.v[nw] + aux_b.v[center] + aux_b.v[sw + 1U]);
    float qs = max(epssq, aux_c.v[nw + 1U] + aux_c.v[center] + aux_c.v[sw]);
    aux_a.v[center] = ps / (ps + qs);
}

void opposite_color_pass(uint x, uint y, uint index) {
    if (x < 4U || x + 4U >= parameters.width || y < 4U ||
        y + 4U >= parameters.height || color_at(x, y) == 1U) return;
    int ix = int(x);
    int iy = int(y);
    uint center = index / 2U;
    uint nw_index = (index - parameters.width - 1U) / 2U;
    uint sw_index = (index + parameters.width - 1U) / 2U;
    float central = aux_a.v[center];
    float neighbor = 0.25 * (aux_a.v[nw_index] + aux_a.v[nw_index + 1U] +
                             aux_a.v[sw_index] + aux_a.v[sw_index + 1U]);
    float disc = abs(0.5 - central) < abs(0.5 - neighbor) ? neighbor : central;
    bool write_blue = color_at(x, y) == 0U;
    float nwc = write_blue ? blue(ix - 1, iy - 1) : red(ix - 1, iy - 1);
    float nec = write_blue ? blue(ix + 1, iy - 1) : red(ix + 1, iy - 1);
    float swc = write_blue ? blue(ix - 1, iy + 1) : red(ix - 1, iy + 1);
    float sec = write_blue ? blue(ix + 1, iy + 1) : red(ix + 1, iy + 1);
    float nwg = eps + abs(nwc - sec) +
        abs(nwc - (write_blue ? blue(ix - 3, iy - 3) : red(ix - 3, iy - 3))) +
        abs(green(ix, iy) - green(ix - 2, iy - 2));
    float neg = eps + abs(nec - swc) +
        abs(nec - (write_blue ? blue(ix + 3, iy - 3) : red(ix + 3, iy - 3))) +
        abs(green(ix, iy) - green(ix + 2, iy - 2));
    float swg = eps + abs(nec - swc) +
        abs(swc - (write_blue ? blue(ix - 3, iy + 3) : red(ix - 3, iy + 3))) +
        abs(green(ix, iy) - green(ix - 2, iy + 2));
    float seg = eps + abs(nwc - sec) +
        abs(sec - (write_blue ? blue(ix + 3, iy + 3) : red(ix + 3, iy + 3))) +
        abs(green(ix, iy) - green(ix + 2, iy + 2));
    float nwe = nwc - green(ix - 1, iy - 1);
    float nee = nec - green(ix + 1, iy - 1);
    float swe = swc - green(ix - 1, iy + 1);
    float see = sec - green(ix + 1, iy + 1);
    float pe = (nwg * see + seg * nwe) / (nwg + seg);
    float qe = (neg * swe + swg * nee) / (neg + swg);
    float value = green(ix, iy) + intp(disc, qe, pe);
    if (write_blue) blue_data.v[index] = value;
    else red_data.v[index] = value;
}

float color_sample(uint channel, int x, int y) {
    return channel == 0U ? red(x, y) : blue(x, y);
}

void green_color_pass(uint x, uint y, uint index) {
    if (x < 4U || x + 4U >= parameters.width || y < 4U ||
        y + 4U >= parameters.height || color_at(x, y) != 1U) return;
    int ix = int(x);
    int iy = int(y);
    float central = vh(ix, iy);
    float neighbor = 0.25 * (vh(ix - 1, iy - 1) + vh(ix + 1, iy - 1) +
                             vh(ix - 1, iy + 1) + vh(ix + 1, iy + 1));
    float disc = abs(0.5 - central) < abs(0.5 - neighbor) ? neighbor : central;
    float g = green(ix, iy);
    float n1 = eps + abs(g - green(ix, iy - 2));
    float s1 = eps + abs(g - green(ix, iy + 2));
    float w1 = eps + abs(g - green(ix - 2, iy));
    float e1 = eps + abs(g - green(ix + 2, iy));
    for (uint channel = 0U; channel <= 2U; channel += 2U) {
        float n = color_sample(channel, ix, iy - 1);
        float s = color_sample(channel, ix, iy + 1);
        float w = color_sample(channel, ix - 1, iy);
        float e = color_sample(channel, ix + 1, iy);
        float sn = abs(n - s);
        float ew = abs(w - e);
        float ng = n1 + sn + abs(n - color_sample(channel, ix, iy - 3));
        float sg = s1 + sn + abs(s - color_sample(channel, ix, iy + 3));
        float wg = w1 + ew + abs(w - color_sample(channel, ix - 3, iy));
        float eg = e1 + ew + abs(e - color_sample(channel, ix + 3, iy));
        float ne = n - green(ix, iy - 1);
        float se = s - green(ix, iy + 1);
        float we = w - green(ix - 1, iy);
        float ee = e - green(ix + 1, iy);
        float ve = (ng * se + sg * ne) / (ng + sg);
        float he = (eg * we + wg * ee) / (eg + wg);
        float value = g + intp(disc, he, ve);
        if (channel == 0U) red_data.v[index] = value;
        else blue_data.v[index] = value;
    }
}

void final_pass(uint x, uint y, uint index) {
    const uint border = 9U;
    if (x >= border && x + border < parameters.width &&
        y >= border && y + border < parameters.height) {
        red_data.v[index] = max(0.0, red_data.v[index] * scale);
        green_data.v[index] = max(0.0, green_data.v[index] * scale);
        blue_data.v[index] = max(0.0, blue_data.v[index] * scale);
        return;
    }
    float sums[3] = float[3](0.0, 0.0, 0.0);
    float counts[3] = float[3](0.0, 0.0, 0.0);
    int min_y = max(0, int(y) - 1);
    int max_y = min(int(parameters.height) - 1, int(y) + 1);
    int min_x = max(0, int(x) - 1);
    int max_x = min(int(parameters.width) - 1, int(x) + 1);
    for (int sy = min_y; sy <= max_y; ++sy) {
        for (int sx = min_x; sx <= max_x; ++sx) {
            uint channel = color_at(uint(sx), uint(sy));
            sums[channel] += raw_data.v[uint(sy) * parameters.width + uint(sx)];
            counts[channel] += 1.0;
        }
    }
    uint own = color_at(x, y);
    red_data.v[index] = own == 0U ? raw_data.v[index] : sums[0] / counts[0];
    green_data.v[index] = own == 1U ? raw_data.v[index] : sums[1] / counts[1];
    blue_data.v[index] = own == 2U ? raw_data.v[index] : sums[2] / counts[2];
}

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= parameters.width || y >= parameters.height) return;
    uint index = at(x, y);
    if (parameters.pass_index == 0U) initialize_pass(x, y, index);
    else if (parameters.pass_index == 1U) direction_lpf_pass(x, y, index);
    else if (parameters.pass_index == 2U) green_pass(x, y, index);
    else if (parameters.pass_index == 3U) pq_hpf_pass(x, y, index);
    else if (parameters.pass_index == 4U) pq_direction_pass(x, y, index);
    else if (parameters.pass_index == 5U) opposite_color_pass(x, y, index);
    else if (parameters.pass_index == 6U) green_color_pass(x, y, index);
    else final_pass(x, y, index);
}
