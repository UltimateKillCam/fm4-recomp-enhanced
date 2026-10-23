// Real-depth camera reprojection and colour conversion, GPL-3.0-only.
// No learned image models. Motion is camera-derived; reactive rejection also
// covers camera-attached cockpit/HUD and independently moving geometry.
cbuffer Constants : register(b0) {
  row_major float4x4 clip_to_previous;
  uint width,height,out_width,out_height;
  float jitter_x,jitter_y,previous_jitter_x,previous_jitter_y;
  uint reset_history,linear_colour,spatial_only;
  float neural_strength;
};
Texture2D<float4> colour : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float4> previous_colour : register(t2);
Texture2D<float4> upscale : register(t3);
Texture2D<float> rejection : register(t4);
Texture2D<float4> neural_result : register(t5);
RWTexture2D<float4> prepared : register(u0);
RWTexture2D<float2> motion : register(u1);
RWTexture2D<float> reactive : register(u2);
RWTexture2D<float4> destination : register(u3);
RWTexture2D<float4> neural_output0 : register(u4);
RWTexture2D<float4> neural_output1 : register(u5);
RWTexture2D<float4> neural_output2 : register(u6);
SamplerState bilinear_clamp : register(s0);
float3 ToLinear(float3 v) { return lerp(v/12.92,pow(max((v+.055)/1.055,0),2.4),step(.04045,v)); }
float3 ToSrgb(float3 v) { return lerp(v*12.92,1.055*pow(max(v,0),1.0/2.4)-.055,step(.0031308,v)); }
// Shader model 5 uses component-wise conditional expressions.
[numthreads(8,8,1)]
void prepare(uint3 id:SV_DispatchThreadID) {
  if(id.x>=width||id.y>=height) return;
  float2 uv=(float2(id.xy)+.5)/float2(width,height);
  float4 c=colour.Load(int3(id.xy,0));
  prepared[id.xy]=float4(linear_colour&&neural_strength==0?ToLinear(c.rgb):c.rgb,1);
  float z=depth.Load(int3(id.xy,0));
  float2 unjittered_uv=uv-float2(jitter_x,jitter_y)/float2(width,height);
  float4 p=mul(float4(unjittered_uv.x*2-1,1-unjittered_uv.y*2,z,1),clip_to_previous);
  float2 previous_uv=float2(.5+.5*p.x/p.w,.5-.5*p.y/p.w);
  float2 mv=(previous_uv-unjittered_uv)*float2(width,height);
  bool attached=z>=.999999 || z<=0 || p.w<=0 || any(abs(mv)>float2(width,height));
  if(attached||reset_history) mv=0;
  // Compare actual previous colour at the reprojected position. Reject history
  // at disocclusions, car movement, HUD changes and effects without motion.
  float3 old=previous_colour.SampleLevel(bilinear_clamp,uv+(mv+float2(previous_jitter_x-jitter_x,previous_jitter_y-jitter_y))/float2(width,height),0).rgb;
  float difference=max(abs(c.r-old.r),max(abs(c.g-old.g),abs(c.b-old.b)));
  motion[id.xy]=mv;
  reactive[id.xy]=(attached||reset_history)?1:saturate((difference-.03)*5);
}
[numthreads(8,8,1)]
void neural_normalize(uint3 id:SV_DispatchThreadID) {
  if(id.x>=width||id.y>=height) return;
  uint stage=uint(previous_jitter_x);
  float4 value;
  if(stage==0) value=neural_output0[id.xy];
  else if(stage==1) value=neural_output1[id.xy];
  else value=neural_output2[id.xy];
  // The SDR game/upscaler contract is finite [0,1]. Do not feed neural
  // overshoot or invalid values into another neural pass or sRGB conversion.
  if(!all(isfinite(value.rgb))) value=colour.Load(int3(id.xy,0));
  value=float4(saturate(value.rgb),1);
  if(stage==0) neural_output0[id.xy]=value;
  else if(stage==1) neural_output1[id.xy]=value;
  else neural_output2[id.xy]=value;
}
[numthreads(8,8,1)]
void neural_finish(uint3 id:SV_DispatchThreadID) {
  if(id.x>=width||id.y>=height) return;
  float2 uv=(float2(id.xy)+.5)/float2(width,height);
  float3 original=colour.Load(int3(id.xy,0)).rgb;
  float3 enhanced=neural_result.Load(int3(id.xy,0)).rgb;
  // Preserve the original HUD anchors. The network never changes scores,
  // timer digits, gear/speed or map content. Their layout remains FM4's.
  bool hud=(uv.x<.26&&uv.y<.22)||(uv.x>.72&&uv.y<.30)||
           (uv.x>.74&&uv.y>.61)||(uv.x<.16&&uv.y>.51);
  float bright=max(original.r,max(original.g,original.b));
  float dark=min(original.r,min(original.g,original.b));
  float text_hint=smoothstep(.58,.9,bright)*(1-smoothstep(.05,.18,bright-dark));
  // Protect bright neutral HUD glyphs, not a solid screen rectangle that
  // would create a visible colour boundary on scenery when no HUD is drawn.
  float protect=hud?text_hint:0;
  float3 c=lerp(original,enhanced,neural_strength*(1-protect));
  prepared[id.xy]=float4(linear_colour?ToLinear(c):c,1);
}
[numthreads(8,8,1)]
void finish(uint3 id:SV_DispatchThreadID) {
  if(id.x>=out_width||id.y>=out_height) return;
  float2 uv=(float2(id.xy)+.5)/float2(out_width,out_height);
  float3 c=spatial_only?(neural_strength>0?upscale.SampleLevel(bilinear_clamp,uv,0).rgb:colour.SampleLevel(bilinear_clamp,uv,0).rgb):upscale.Load(int3(id.xy,0)).rgb;
  if(!spatial_only&&linear_colour) c=ToSrgb(c);
  // The original guest HUD is composited into the colour surface. Preserve
  // current-frame pixels wherever history is rejected, including the fast
  // race timer, mirrors and camera-attached cockpit. Vendor hints alone are
  // insufficient for the changing digits in FM4's already-composited HUD.
  if(!spatial_only) {
    float reject=saturate((rejection.SampleLevel(bilinear_clamp,uv,0)-.65)/.35);
    c=lerp(c,colour.SampleLevel(bilinear_clamp,uv,0).rgb,reject);
  }
  destination[id.xy]=float4(saturate(c),1);
}
