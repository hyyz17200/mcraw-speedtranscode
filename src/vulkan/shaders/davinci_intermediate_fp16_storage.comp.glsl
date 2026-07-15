#version 460
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer InputRed { uint input_red[]; };
layout(set = 0, binding = 1, std430) readonly buffer InputGreen { uint input_green[]; };
layout(set = 0, binding = 2, std430) readonly buffer InputBlue { uint input_blue[]; };
layout(set = 0, binding = 3, std430) writeonly buffer OutputRed { uint output_red[]; };
layout(set = 0, binding = 4, std430) writeonly buffer OutputGreen { uint output_green[]; };
layout(set = 0, binding = 5, std430) writeonly buffer OutputBlue { uint output_blue[]; };
layout(set = 0, binding = 6, std430) readonly buffer CurveLut { float curve_lut[]; };
layout(set = 0, binding = 7, std430) buffer ControlStatus { uint status; };
layout(push_constant) uniform Parameters { uint width; uint height; uint negative_policy; uint entries_per_segment; } params;
const float di_a=0.0075, di_b=7.0, di_c=0.07329248, di_m=10.44426855, di_linear_cut=0.00262409;
const uint policy_clamp_zero=1, policy_error=2, status_negative_rejected=1, status_non_finite=2;
float lane(vec2 v,uint p){return(p&1U)==0U?v.x:v.y;} float rr(uint p){return lane(unpackHalf2x16(input_red[p>>1U]),p);} float gg(uint p){return lane(unpackHalf2x16(input_green[p>>1U]),p);} float bb(uint p){return lane(unpackHalf2x16(input_blue[p>>1U]),p);}
float lut(uint base,float scaled){uint i=min(uint(scaled),params.entries_per_segment-2U);float f=scaled-float(i);return mix(curve_lut[base+i],curve_lut[base+i+1U],f);}
float encode_di(float v){
    if(isnan(v)||isinf(v)){atomicOr(status,status_non_finite);return 0.0;}
    if(v<0.0){if(params.negative_policy==policy_clamp_zero)v=0.0;if(params.negative_policy==policy_error){atomicOr(status,status_negative_rejected);v=0.0;}}
    float e=v<=di_linear_cut?v*di_m:v<=1.0?lut(0U,(v-di_linear_cut)*float(params.entries_per_segment-1U)/(1.0-di_linear_cut)):v<=100.0?lut(params.entries_per_segment,(v-1.0)*float(params.entries_per_segment-1U)/99.0):(log2(v+di_a)+di_b)*di_c;
    if(isnan(e)||isinf(e)){atomicOr(status,status_non_finite);return 0.0;} return e;
}
void main(){uint pair=gl_GlobalInvocationID.x;uint first=pair*2U;if(first>=params.width*params.height)return;
    output_red[pair]=packHalf2x16(vec2(encode_di(rr(first)),encode_di(rr(first+1U))));
    output_green[pair]=packHalf2x16(vec2(encode_di(gg(first)),encode_di(gg(first+1U))));
    output_blue[pair]=packHalf2x16(vec2(encode_di(bb(first)),encode_di(bb(first+1U))));}
