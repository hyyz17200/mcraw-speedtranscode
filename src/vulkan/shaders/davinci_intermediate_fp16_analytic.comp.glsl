#version 460
layout(local_size_x=256,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0,std430) readonly buffer InputRed{uint input_red[];};
layout(set=0,binding=1,std430) readonly buffer InputGreen{uint input_green[];};
layout(set=0,binding=2,std430) readonly buffer InputBlue{uint input_blue[];};
layout(set=0,binding=3,std430) writeonly buffer OutputRed{uint output_red[];};
layout(set=0,binding=4,std430) writeonly buffer OutputGreen{uint output_green[];};
layout(set=0,binding=5,std430) writeonly buffer OutputBlue{uint output_blue[];};
layout(set=0,binding=6,std430) readonly buffer UnusedLut{float unused_lut[];};
layout(set=0,binding=7,std430) buffer ControlStatus{uint status;};
layout(push_constant) uniform Parameters{uint width;uint height;uint negative_policy;uint entries_per_segment;}params;
const float a=0.0075,b=7.0,c=0.07329248,m=10.44426855,cut=0.00262409;
const uint clamp_zero=1,error_policy=2,negative_rejected=1,non_finite=2;
float lane(vec2 v,uint p){return(p&1U)==0U?v.x:v.y;}float rr(uint p){return lane(unpackHalf2x16(input_red[p>>1U]),p);}float gg(uint p){return lane(unpackHalf2x16(input_green[p>>1U]),p);}float bb(uint p){return lane(unpackHalf2x16(input_blue[p>>1U]),p);}
float di(float v){if(isnan(v)||isinf(v)){atomicOr(status,non_finite);return 0.0;}if(v<0.0){if(params.negative_policy==clamp_zero)v=0.0;if(params.negative_policy==error_policy){atomicOr(status,negative_rejected);v=0.0;}}float e=v<=cut?v*m:(log2(v+a)+b)*c;if(isnan(e)||isinf(e)){atomicOr(status,non_finite);return 0.0;}return e;}
void main(){uint pair=gl_GlobalInvocationID.x,first=pair*2U;if(first>=params.width*params.height)return;output_red[pair]=packHalf2x16(vec2(di(rr(first)),di(rr(first+1U))));output_green[pair]=packHalf2x16(vec2(di(gg(first)),di(gg(first+1U))));output_blue[pair]=packHalf2x16(vec2(di(bb(first)),di(bb(first+1U))));}
