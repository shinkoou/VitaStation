// VitaStation
// Adapted from Qualcomm Snapdragon Game Super Resolution v1 Edge Direction.
// SPDX-License-Identifier: BSD-3-Clause
#version 450
layout(location=0) in vec2 uv_frag;
layout(binding=0) uniform sampler2D fb;
layout(location=0) out vec3 color_frag;
layout(push_constant) uniform PushConstants { vec2 inv_size; } pc;
const float EdgeThreshold=8.0/255.0;
const float EdgeSharpness=2.0;
float fastLanczos2(float x){float a=x-4.0;float b=x*a-a;a*=a;return b*a;}
vec2 edgeDirection(vec4 l,vec4 r){float a=r.x-l.z,b=r.w-l.y;vec2 d=vec2(a+b,a-b);return d*inversesqrt(d.x*d.x+3.075740e-05+d.y*d.y);}
vec2 weightY(float dx,float dy,float c,vec3 data){float e=dx*data.z+dy*data.y;float x=(dx*dx+dy*dy)+(e*e)*(clamp(c*c*data.x,0.0,1.0)*0.7-1.0);float w=fastLanczos2(x);return vec2(w,w*c);}
void main(){
 vec3 color=textureLod(fb,uv_frag,0.0).rgb; float centerY=color.g;
 vec2 size=1.0/pc.inv_size; vec2 ip=uv_frag*size+vec2(-0.5,0.5); vec2 pix=floor(ip); vec2 coord=pix*pc.inv_size; vec2 pl=ip-pix;
 vec4 left=textureGather(fb,coord,1); float vote=abs(left.z-left.y)+abs(centerY-left.y)+abs(centerY-left.z);
 if(vote>EdgeThreshold){
  coord.x+=pc.inv_size.x; vec4 right=textureGather(fb,coord+vec2(pc.inv_size.x,0.0),1); vec4 ud;
  ud.xy=textureGather(fb,coord+vec2(0.0,-pc.inv_size.y),1).wz; ud.zw=textureGather(fb,coord+vec2(0.0,pc.inv_size.y),1).yx;
  float mean=(left.y+left.z+right.x+right.w)*0.25; left-=vec4(mean); right-=vec4(mean); ud-=vec4(mean); float centered=centerY-mean;
  float sumv=abs(left.x)+abs(left.y)+abs(left.z)+abs(left.w)+abs(right.x)+abs(right.y)+abs(right.z)+abs(right.w)+abs(ud.x)+abs(ud.y)+abs(ud.z)+abs(ud.w);
  float sm=10.14185/max(sumv,1e-6); vec3 data=vec3(sm*sm,edgeDirection(left,right)); vec2 a=weightY(pl.x,pl.y+1.0,ud.x,data);
  a+=weightY(pl.x-1.0,pl.y+1.0,ud.y,data); a+=weightY(pl.x-1.0,pl.y-2.0,ud.z,data); a+=weightY(pl.x,pl.y-2.0,ud.w,data);
  a+=weightY(pl.x+1.0,pl.y-1.0,left.x,data); a+=weightY(pl.x,pl.y-1.0,left.y,data); a+=weightY(pl.x,pl.y,left.z,data); a+=weightY(pl.x+1.0,pl.y,left.w,data);
  a+=weightY(pl.x-1.0,pl.y-1.0,right.x,data); a+=weightY(pl.x-2.0,pl.y-1.0,right.y,data); a+=weightY(pl.x-2.0,pl.y,right.z,data); a+=weightY(pl.x-1.0,pl.y,right.w,data);
  float fy=a.y/max(a.x,1e-6); float mx=max(max(left.y,left.z),max(right.x,right.w)); float mn=min(min(left.y,left.z),min(right.x,right.w));
  float dy=clamp(clamp(EdgeSharpness*fy,mn,mx)-centered,-23.0/255.0,23.0/255.0); color=clamp(color+vec3(dy),0.0,1.0);
 }
 color_frag=color;
}
