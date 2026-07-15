#version 460

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InputRed { float input_red[]; };
layout(set = 0, binding = 1, std430) readonly buffer InputGreen { float input_green[]; };
layout(set = 0, binding = 2, std430) readonly buffer InputBlue { float input_blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer OutputRed { float output_red[]; };
layout(set = 0, binding = 4, std430) writeonly buffer OutputGreen { float output_green[]; };
layout(set = 0, binding = 5, std430) writeonly buffer OutputBlue { float output_blue[]; };

layout(push_constant) uniform Parameters {
    uint width;
    uint height;
    float amount;
    float threshold;
} params;

const float kr = 0.2627;
const float kb = 0.0593;
const float kg = 1.0 - kr - kb;

uint pixel_at(int x, int y)
{
    uint safe_x = uint(clamp(x, 0, int(params.width) - 1));
    uint safe_y = uint(clamp(y, 0, int(params.height) - 1));
    return safe_y * params.width + safe_x;
}

float luma_at(int x, int y)
{
    uint pixel = pixel_at(x, y);
    return kr * input_red[pixel] + kg * input_green[pixel] + kb * input_blue[pixel];
}

void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= params.width || y >= params.height) return;
    uint pixel = y * params.width + x;
    float detail = luma_at(int(x), int(y)) - 0.25 * (
        luma_at(int(x) - 1, int(y)) + luma_at(int(x) + 1, int(y)) +
        luma_at(int(x), int(y) - 1) + luma_at(int(x), int(y) + 1));
    float delta = 0.0;
    if (abs(detail) > params.threshold && params.amount > 0.0) {
        delta = params.amount * sign(detail) * (abs(detail) - params.threshold);
    }
    output_red[pixel] = input_red[pixel] + delta;
    output_green[pixel] = input_green[pixel] + delta;
    output_blue[pixel] = input_blue[pixel] + delta;
}
