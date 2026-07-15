#version 460

layout(local_size_x = 8, local_size_y = 16, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer InputRed { uint input_red[]; };
layout(set = 0, binding = 1, std430) readonly buffer InputGreen { uint input_green[]; };
layout(set = 0, binding = 2, std430) readonly buffer InputBlue { uint input_blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer OutputRed { uint output_red[]; };
layout(set = 0, binding = 4, std430) writeonly buffer OutputGreen { uint output_green[]; };
layout(set = 0, binding = 5, std430) writeonly buffer OutputBlue { uint output_blue[]; };
layout(push_constant) uniform Parameters { uint width; uint height; float amount; float threshold; } params;
const float kr = 0.2627; const float kb = 0.0593; const float kg = 1.0 - kr - kb;

float lane(vec2 v,uint p){return(p&1U)==0U?v.x:v.y;}
float rr(uint p){return lane(unpackHalf2x16(input_red[p>>1U]),p);}
float gg(uint p){return lane(unpackHalf2x16(input_green[p>>1U]),p);}
float bb(uint p){return lane(unpackHalf2x16(input_blue[p>>1U]),p);}
uint pixel_at(int x, int y) {
    return uint(clamp(y, 0, int(params.height) - 1)) * params.width +
           uint(clamp(x, 0, int(params.width) - 1));
}
float luma_at(int x, int y) {
    uint p = pixel_at(x, y);
    return kr * rr(p) + kg * gg(p) + kb * bb(p);
}
vec3 sharpen(uint x, uint y) {
    uint p = y * params.width + x;
    float detail = luma_at(int(x), int(y)) - 0.25 *
        (luma_at(int(x)-1, int(y)) + luma_at(int(x)+1, int(y)) +
         luma_at(int(x), int(y)-1) + luma_at(int(x), int(y)+1));
    float delta = abs(detail) > params.threshold && params.amount > 0.0
        ? params.amount * sign(detail) * (abs(detail) - params.threshold) : 0.0;
    return vec3(rr(p),gg(p),bb(p)) + delta;
}
void main() {
    uint pair_x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    uint x = pair_x * 2U;
    if (y >= params.height || x >= params.width) return;
    vec3 a = sharpen(x, y); vec3 b = sharpen(x + 1U, y);
    uint pair = (y * params.width + x) >> 1U;
    output_red[pair] = packHalf2x16(vec2(a.r, b.r));
    output_green[pair] = packHalf2x16(vec2(a.g, b.g));
    output_blue[pair] = packHalf2x16(vec2(a.b, b.b));
}
