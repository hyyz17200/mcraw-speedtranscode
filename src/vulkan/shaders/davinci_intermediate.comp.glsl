#version 460

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InputRed { float input_red[]; };
layout(set = 0, binding = 1, std430) readonly buffer InputGreen { float input_green[]; };
layout(set = 0, binding = 2, std430) readonly buffer InputBlue { float input_blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer OutputRed { float output_red[]; };
layout(set = 0, binding = 4, std430) writeonly buffer OutputGreen { float output_green[]; };
layout(set = 0, binding = 5, std430) writeonly buffer OutputBlue { float output_blue[]; };
layout(set = 0, binding = 6, std430) readonly buffer CurveLut { float curve_lut[]; };
layout(set = 0, binding = 7, std430) buffer ControlStatus { uint status; };

layout(push_constant) uniform Parameters {
    uint width;
    uint height;
    uint negative_policy;
    uint entries_per_segment;
} params;

const float di_a = 0.0075;
const float di_b = 7.0;
const float di_c = 0.07329248;
const float di_m = 10.44426855;
const float di_linear_cut = 0.00262409;

const uint policy_preserve_by_curve = 0;
const uint policy_clamp_zero = 1;
const uint policy_error = 2;
const uint status_negative_rejected = 1;
const uint status_non_finite = 2;

float interpolate_lut(uint base, float scaled)
{
    uint last_index = params.entries_per_segment - 2;
    uint index = min(uint(scaled), last_index);
    float fraction = scaled - float(index);
    float first = curve_lut[base + index];
    return first + fraction * (curve_lut[base + index + 1] - first);
}

float encode_di(float linear)
{
    if (isnan(linear) || isinf(linear)) {
        atomicOr(status, status_non_finite);
        return 0.0;
    }
    if (linear < 0.0) {
        if (params.negative_policy == policy_clamp_zero) linear = 0.0;
        if (params.negative_policy == policy_error) {
            atomicOr(status, status_negative_rejected);
            linear = 0.0;
        }
    }
    float encoded;
    if (linear <= di_linear_cut) {
        encoded = linear * di_m;
    } else if (linear <= 1.0) {
        float scale = float(params.entries_per_segment - 1) /
                      (1.0 - di_linear_cut);
        encoded = interpolate_lut(0, (linear - di_linear_cut) * scale);
    } else if (linear <= 100.0) {
        float scale = float(params.entries_per_segment - 1) / 99.0;
        encoded = interpolate_lut(params.entries_per_segment,
                                  (linear - 1.0) * scale);
    } else {
        encoded = (log2(linear + di_a) + di_b) * di_c;
    }
    if (isnan(encoded) || isinf(encoded)) {
        atomicOr(status, status_non_finite);
        return 0.0;
    }
    return encoded;
}

void main()
{
    uint pixel = gl_GlobalInvocationID.x;
    uint pixels = params.width * params.height;
    if (pixel >= pixels) return;
    output_red[pixel] = encode_di(input_red[pixel]);
    output_green[pixel] = encode_di(input_green[pixel]);
    output_blue[pixel] = encode_di(input_blue[pixel]);
}
