#version 460

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer CameraRed { float camera_red[]; };
layout(set = 0, binding = 1, std430) readonly buffer CameraGreen { float camera_green[]; };
layout(set = 0, binding = 2, std430) readonly buffer CameraBlue { float camera_blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer TargetRed { float target_red[]; };
layout(set = 0, binding = 4, std430) writeonly buffer TargetGreen { float target_green[]; };
layout(set = 0, binding = 5, std430) writeonly buffer TargetBlue { float target_blue[]; };

layout(push_constant) uniform Parameters {
    uint width;
    uint height;
    float exposure_scale;
    uint reserved;
    vec4 matrix_row_0;
    vec4 matrix_row_1;
    vec4 matrix_row_2;
} params;

void main()
{
    uint pixel = gl_GlobalInvocationID.x;
    uint pixels = params.width * params.height;
    if (pixel >= pixels) return;
    vec3 camera = vec3(camera_red[pixel], camera_green[pixel], camera_blue[pixel]);
    target_red[pixel] = dot(params.matrix_row_0.xyz, camera) * params.exposure_scale;
    target_green[pixel] = dot(params.matrix_row_1.xyz, camera) * params.exposure_scale;
    target_blue[pixel] = dot(params.matrix_row_2.xyz, camera) * params.exposure_scale;
}
