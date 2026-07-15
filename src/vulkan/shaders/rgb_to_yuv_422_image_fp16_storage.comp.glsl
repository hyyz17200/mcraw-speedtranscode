#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=64,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0,std430) readonly buffer RedPlane{uint red[];};
layout(set=0,binding=1,std430) readonly buffer GreenPlane{uint green[];};
layout(set=0,binding=2,std430) readonly buffer BluePlane{uint blue[];};
layout(set=0,binding=3,r16ui) uniform writeonly uimage2D luma_out;
layout(set=0,binding=4,r16ui) uniform writeonly uimage2D cb_out;
layout(set=0,binding=5,r16ui) uniform writeonly uimage2D cr_out;
layout(push_constant) uniform Parameters{uint width;uint height;uint quality_filter;uint enable_dither;uint64_t frame_index;}params;
const float kr=0.2627,kb=0.0593,kg=1.0-kr-kb;
float lane(vec2 v,uint p){return(p&1U)==0U?v.x:v.y;} float rr(uint p){return lane(unpackHalf2x16(red[p>>1U]),p);} float gg(uint p){return lane(unpackHalf2x16(green[p>>1U]),p);} float bb(uint p){return lane(unpackHalf2x16(blue[p>>1U]),p);}
float yl(uint p){return kr*rr(p)+kg*gg(p)+kb*bb(p);} float cb(uint p){float y=yl(p);return(bb(p)-y)/(2.0*(1.0-kb));} float cr(uint p){float y=yl(p);return(rr(p)-y)/(2.0*(1.0-kr));}
uint64_t mix64(uint64_t s){s^=s>>30;s*=0xBF58476D1CE4E5B9ul;s^=s>>27;s*=0x94D049BB133111EBul;return s^(s>>31);}
float noise(uint plane,uint sample_index){uint64_t s=uint64_t(sample_index)*0x9E3779B185EBCA87ul;s^=params.frame_index*0xC2B2AE3D27D4EB4Ful;s^=uint64_t(plane)*0x165667B19E3779F9ul;return float(mix64(s)>>40)*(1.0/16777216.0)-0.5;}
uint q(float v,float lo,float hi,uint plane,uint sample_index){float n=params.enable_dither!=0?noise(plane,sample_index):0.0;return uint(clamp(v,lo,hi)+n+0.5);}
uint cp(int x,uint y){return y*params.width+uint(clamp(x,0,int(params.width)-1));}
float fcb(uint x,uint y){if(params.quality_filter==0)return cb(y*params.width+x);return(-cb(cp(int(x)-2,y))+4.0*cb(cp(int(x)-1,y))+10.0*cb(cp(int(x),y))+4.0*cb(cp(int(x)+1,y))-cb(cp(int(x)+2,y)))/16.0;}
float fcr(uint x,uint y){if(params.quality_filter==0)return cr(y*params.width+x);return(-cr(cp(int(x)-2,y))+4.0*cr(cp(int(x)-1,y))+10.0*cr(cp(int(x),y))+4.0*cr(cp(int(x)+1,y))-cr(cp(int(x)+2,y)))/16.0;}
void main(){uint pair=gl_GlobalInvocationID.x,row=gl_GlobalInvocationID.y;if(row>=params.height||pair>=params.width/2U)return;uint x=pair*2U,a=row*params.width+x,b=a+1U,c=row*(params.width/2U)+pair;
 imageStore(luma_out,ivec2(x,row),uvec4(q(64.0+876.0*yl(a),64.0,940.0,0U,a)));imageStore(luma_out,ivec2(x+1U,row),uvec4(q(64.0+876.0*yl(b),64.0,940.0,0U,b)));
 imageStore(cb_out,ivec2(pair,row),uvec4(q(512.0+896.0*fcb(x,row),64.0,960.0,1U,c)));imageStore(cr_out,ivec2(pair,row),uvec4(q(512.0+896.0*fcr(x,row),64.0,960.0,2U,c)));}
