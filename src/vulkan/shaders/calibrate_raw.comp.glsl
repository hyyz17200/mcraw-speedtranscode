#version 450

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer RawWords {
    uint words[];
} raw_input;
layout(set = 0, binding = 1, std430) writeonly buffer CalibratedRaw {
    float samples[];
} calibrated;

layout(push_constant) uniform CalibrationParameters {
    uint width;
    uint height;
    uint pixel_count;
    uint reserved;
    vec4 black_level;
    vec4 white_level;
} parameters;

void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= parameters.pixel_count) {
        return;
    }
    uint packed = raw_input.words[index >> 1U];
    uint raw = (index & 1U) == 0U ? (packed & 0xffffU) : (packed >> 16U);
    uint x = index % parameters.width;
    uint y = index / parameters.width;
    uint position = ((y & 1U) << 1U) | (x & 1U);
    float black = parameters.black_level[position];
    float white = parameters.white_level[position];
    calibrated.samples[index] =
        ((float(raw) - black) / (white - black)) * 65535.0;
}
