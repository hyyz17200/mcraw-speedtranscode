#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer RedPlane { float red[]; };
layout(set = 0, binding = 1, std430) readonly buffer GreenPlane { float green[]; };
layout(set = 0, binding = 2, std430) readonly buffer BluePlane { float blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer LumaPlane { uint luma_out[]; };
layout(set = 0, binding = 4, std430) writeonly buffer CbPlane { uint cb_out[]; };
layout(set = 0, binding = 5, std430) writeonly buffer CrPlane { uint cr_out[]; };

layout(push_constant) uniform Parameters {
    uint width;
    uint height;
    uint quality_filter;
    uint enable_dither;
    uint64_t frame_index;
} params;

const float kr = 0.2627;
const float kb = 0.0593;
const float kg = 1.0 - kr - kb;

float pixel_luma(uint pixel)
{
    return kr * red[pixel] + kg * green[pixel] + kb * blue[pixel];
}

float pixel_cb(uint pixel)
{
    float y = pixel_luma(pixel);
    return (blue[pixel] - y) / (2.0 * (1.0 - kb));
}

float pixel_cr(uint pixel)
{
    float y = pixel_luma(pixel);
    return (red[pixel] - y) / (2.0 * (1.0 - kr));
}

float deterministic_noise(uint64_t frame, uint plane, uint sample_index)
{
    uint64_t state = uint64_t(sample_index) * 0x9E3779B185EBCA87ul;
    state ^= frame * 0xC2B2AE3D27D4EB4Ful;
    state ^= uint64_t(plane) * 0x165667B19E3779F9ul;
    state ^= state >> 30;
    state *= 0xBF58476D1CE4E5B9ul;
    state ^= state >> 27;
    state *= 0x94D049BB133111EBul;
    state ^= state >> 31;
    // The CPU reference consumes 53 random mantissa bits. FP32 cannot retain
    // more than 24, so use the same high bits at FP32 precision.
    return float(state >> 40) * (1.0 / 16777216.0) - 0.5;
}

uint quantize_code(float value, float minimum_code, float maximum_code,
                   uint plane, uint sample_index)
{
    float noise = params.enable_dither != 0
        ? deterministic_noise(params.frame_index, plane, sample_index) : 0.0;
    return uint(clamp(value, minimum_code, maximum_code) + noise + 0.5);
}

uint clamped_pixel(int x, uint row)
{
    uint safe_x = uint(clamp(x, 0, int(params.width) - 1));
    return row * params.width + safe_x;
}

float filtered_cb(uint x, uint row)
{
    if (params.quality_filter == 0) return pixel_cb(row * params.width + x);
    return (-pixel_cb(clamped_pixel(int(x) - 2, row))
            + 4.0 * pixel_cb(clamped_pixel(int(x) - 1, row))
            + 10.0 * pixel_cb(clamped_pixel(int(x), row))
            + 4.0 * pixel_cb(clamped_pixel(int(x) + 1, row))
            - pixel_cb(clamped_pixel(int(x) + 2, row))) / 16.0;
}

float filtered_cr(uint x, uint row)
{
    if (params.quality_filter == 0) return pixel_cr(row * params.width + x);
    return (-pixel_cr(clamped_pixel(int(x) - 2, row))
            + 4.0 * pixel_cr(clamped_pixel(int(x) - 1, row))
            + 10.0 * pixel_cr(clamped_pixel(int(x), row))
            + 4.0 * pixel_cr(clamped_pixel(int(x) + 1, row))
            - pixel_cr(clamped_pixel(int(x) + 2, row))) / 16.0;
}

void main()
{
    uint pair = gl_GlobalInvocationID.x;
    uint row = gl_GlobalInvocationID.y;
    if (row >= params.height || pair >= params.width / 2) return;
    uint x = pair * 2;
    uint first = row * params.width + x;
    uint second = first + 1;
    luma_out[first] = quantize_code(64.0 + 876.0 * pixel_luma(first),
                                    64.0, 940.0, 0, first);
    luma_out[second] = quantize_code(64.0 + 876.0 * pixel_luma(second),
                                     64.0, 940.0, 0, second);
    uint chroma = row * (params.width / 2) + pair;
    cb_out[chroma] = quantize_code(512.0 + 896.0 * filtered_cb(x, row),
                                   64.0, 960.0, 1, chroma);
    cr_out[chroma] = quantize_code(512.0 + 896.0 * filtered_cr(x, row),
                                   64.0, 960.0, 2, chroma);
}
