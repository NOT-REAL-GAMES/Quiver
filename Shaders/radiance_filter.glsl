#ifndef QUIVER_RADIANCE_FILTER_GLSL
#define QUIVER_RADIANCE_FILTER_GLSL
vec3 RC_FilterKernel(vec3 distance, float strength)
{
 vec3 kernel=max(vec3(1.0)-0.25*distance*distance,vec3(0.0));
 kernel*=kernel;
 vec3 narrow=kernel*kernel*(vec3(1.0)-smoothstep(vec3(1.0),vec3(1.5),distance));
 if (strength == 1.0) return narrow;
 if (strength < 1.0) return mix(max(vec3(1.0)-distance,vec3(0.0)),narrow,strength);
 vec3 wide=exp(-0.5*distance*distance)*(vec3(1.0)-smoothstep(vec3(2.0),vec3(2.5),distance));
 return mix(narrow,wide,strength-1.0);
}
#endif
