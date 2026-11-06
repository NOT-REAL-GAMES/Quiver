#ifndef RC_RECEIVER_FILTER_AXIS_GLSL
#define RC_RECEIVER_FILTER_AXIS_GLSL
/* At most 16,512 bytes of shared memory, below Vulkan's 32 KiB minimum.
 * Cache geometry once per row/column tile; nine logical irradiance taps use at most
 * eighteen compatible endpoint fetches per output. An edge bit blocks every segment crossing that edge. */
const int RC_AXIS_GROUP=128;
const int RC_AXIS_RADIUS=448;
const int RC_AXIS_CELLS=RC_AXIS_GROUP+2*RC_AXIS_RADIUS;
const int RC_AXIS_WORDS=(RC_AXIS_CELLS+31)/32;
/* Integer storage keeps all oct codes and sentinels outside float NaN/
 * denormal semantics. Position bits are converted only when used as xyz. */
shared uvec4 rcAxisData[RC_AXIS_CELLS];
vec3 RC_AxisPosition(int index) { return uintBitsToFloat(rcAxisData[index].xyz); }
/* SNORM16 cannot encode -32768, leaving two spare sentinel codes. */
const uint RC_AXIS_INVALID=0x80008000u;
const uint RC_AXIS_UNKNOWN=0x80008001u;
vec2 RC_AxisSign(vec2 v) { return vec2(v.x<0?-1:1,v.y<0?-1:1); }
uint RC_AxisPackNormal(vec3 normal)
{
    normal/=abs(normal.x)+abs(normal.y)+abs(normal.z);
    vec2 p=normal.xy;
    if(normal.z<0.0) p=(vec2(1)-abs(p.yx))*RC_AxisSign(p);
    return packSnorm2x16(p);
}
vec3 RC_AxisDecodeNormal(uint packed)
{
    vec2 p=unpackSnorm2x16(packed);
    vec3 n=vec3(p,1.0-abs(p.x)-abs(p.y));
    if(n.z<0.0) n.xy=(vec2(1)-abs(n.yx))*RC_AxisSign(n.xy);
    return normalize(n);
}
vec3 RC_AxisNormal(int index)
{
    return RC_AxisDecodeNormal(rcAxisData[index].w);
}
float RC_AxisState(int index)
{
    uint packed=rcAxisData[index].w;
    return packed==RC_AXIS_INVALID?0.0:packed==RC_AXIS_UNKNOWN?-1.0:1.0;
}
shared uint rcAxisBarrier[RC_AXIS_WORDS];
ivec2 RC_AxisPixel(int axis,int line,bool vertical)
{
    return vertical?ivec2(line,axis):ivec2(axis,line);
}
void RC_TextureAxisLoad(int base,int line,bool vertical,ivec2 extent)
{
    uint lane=gl_LocalInvocationID.x;
    for(int i=int(lane);i<RC_AXIS_CELLS;i+=RC_AXIS_GROUP) {
        ivec2 p=RC_AxisPixel(base+i-RC_AXIS_RADIUS,line,vertical);
        uvec2 id; vec3 position,normal;
        bool guide=RC_ReceiverGuide(p,extent,id,position,normal);
        bool unknown=guide && RC_TextureIndirectScheduled(p) && (RC_ResolvePackedStatus(p)&4u)==0u;

        /* A scheduled lookup without original evidence is a barrier even
         * when another pass could have produced a filtered value there. */
        uint packed=!guide?RC_AXIS_INVALID:unknown?RC_AXIS_UNKNOWN:RC_AxisPackNormal(normal);
        rcAxisData[i]=uvec4(floatBitsToUint(position),packed);
    }
    if(lane<uint(RC_AXIS_WORDS)) rcAxisBarrier[lane]=0u;
    barrier();
    for(int i=int(lane);i<RC_AXIS_CELLS-1;i+=RC_AXIS_GROUP) {
        if(RC_AxisState(i)<=0.0 || RC_AxisState(i+1)<=0.0 ||
            !RC_ReceiverContinuous(RC_AxisPosition(i),RC_AxisNormal(i),
                RC_AxisPosition(i+1),RC_AxisNormal(i+1)))
            atomicOr(rcAxisBarrier[i>>5],1u<<uint(i&31));
    }
    barrier();
}
bool RC_TextureAxisClear(int a,int b)
{
    int first=min(a,b),last=max(a,b)-1;
    if(last<first) return true;
    for(int word=first>>5;word<=last>>5;++word) {
        int lo=max(first-word*32,0),hi=min(last-word*32,31);
        uint mask=(0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if((rcAxisBarrier[word]&mask)!=0u) return false;
    }
    return true;
}
/* Locate the maximal visible continuous segment around this receiver. Bits
 * describe edges, so a blocking edge e permits cells up to e on its left and
 * from e+1 on its right. Search whole words, never fetch guides again. */
ivec2 RC_TextureAxisSegment(int center)
{
    int lower=center-RC_AXIS_RADIUS,upper=center+RC_AXIS_RADIUS;
    for(int word=(center-1)>>5;word>=lower>>5;--word) {
        int lo=max(lower-word*32,0),hi=min(center-1-word*32,31);
        uint bits=rcAxisBarrier[word] & (0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if(bits!=0u) { lower=max(lower,word*32+findMSB(bits)+1); break; }
    }
    for(int word=center>>5;word<=(upper-1)>>5;++word) {
        int lo=max(center-word*32,0),hi=min(upper-1-word*32,31);
        uint bits=rcAxisBarrier[word] & (0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if(bits!=0u) { upper=min(upper,word*32+findLSB(bits)); break; }
    }
    return ivec2(lower,upper);
}
vec3 RC_TextureAxisPosition(float coordinate,int first,int last,int stride,int base)
{
    int a=first+int(floor((coordinate-float(first))/float(stride)))*stride;
    int b=min(a+stride,last);
    float f=b==a?0.0:(coordinate-float(a))/float(b-a);
    return mix(RC_AxisPosition(a-base+RC_AXIS_RADIUS),RC_AxisPosition(b-base+RC_AXIS_RADIUS),f);
}
bool RC_TextureAxisLinear(float coordinate,int first,int last,int stride,int center,int base,int line,bool vertical,
    vec3 position,vec3 normal,out vec3 value)
{
    value=vec3(0);
    coordinate=clamp(coordinate,float(first),float(last));
    int a=first+int(floor((coordinate-float(first))/float(stride)))*stride;
    int b=min(a+stride,last);
    float f=b==a?0.0:(coordinate-float(a))/float(b-a);
    if(f<=0.000001) b=a;
    int ia=a-base+RC_AXIS_RADIUS,ib=b-base+RC_AXIS_RADIUS;
    /* first/last lie inside the nearest-barrier segment; clamped a/b
     * therefore have clear paths to center without rescanning its bitset. */
    if(!RC_ReceiverContinuous(position,normal,RC_AxisPosition(ia),RC_AxisNormal(ia)) ||
        !RC_ReceiverContinuous(position,normal,RC_AxisPosition(ib),RC_AxisNormal(ib))) return false;
    vec3 va,vb;
    if(!RC_TextureRaw(RC_AxisPixel(a,line,vertical),1u,va)) return false;
    vb=va;
    if(b!=a && !RC_TextureRaw(RC_AxisPixel(b,line,vertical),1u,vb)) return false;
    value=mix(va,vb,f);
    return true;
}
bool RC_TextureFilterAxis(ivec2 pixel,ivec2 extent,int base,int line,bool vertical,out vec3 value)
{
    value=vec3(0);
    int axis=vertical?pixel.y:pixel.x;
    int center=axis-base+RC_AXIS_RADIUS;
    if(RC_AxisState(center)<=0.0) return false;
    float spacing=rcLevels[0].origin_spacing.w;
    float strength=clamp(rcLighting.receiver_filter.x,0.0,2.0);
    if(isnan(spacing) || isinf(spacing) || spacing<=0.0) return false;
    float support=(strength>1.0?2.5:1.5)*spacing;
    vec3 position=RC_AxisPosition(center),normal=RC_AxisNormal(center);
    float derivative=3.402823466e+38;
    for(int direction=-1;direction<=1;direction+=2) {
        int neighbor=center+direction;
        if(RC_TextureAxisClear(center,neighbor)) {
            float separation=length(RC_AxisPosition(neighbor)-position);
            if(rcReceiver.normalThreshold.z==1.5) {
                ivec2 a=RC_ReceiverFirstPixel(pixel,1.5);
                ivec2 b=RC_ReceiverFirstPixel(RC_AxisPixel(axis+direction,line,vertical),1.5);
                int span=vertical?abs(b.y-a.y):abs(b.x-a.x);
                separation=separation/float(span)*1.5;
            }
            derivative=min(derivative,separation);
        }
    }
    float radius=0.0;
    if(derivative>0.00001 && derivative<3.402823466e+38)
        radius=min(support/derivative,float(RC_AXIS_RADIUS));
    ivec2 segment=RC_TextureAxisSegment(center);
    int first=base+segment.x-RC_AXIS_RADIUS,last=base+segment.y-RC_AXIS_RADIUS;
    int stride=1;
    /* H interpolates only original known checker samples, on a stride-two
     * lattice. V sees H's reconstructed values and uses every texel. */
    if(!vertical && rcLighting.receiver_filter.y>0.5) {
        stride=2;
        if(!RC_TextureIndirectScheduled(RC_AxisPixel(first,line,false))) ++first;
        if(!RC_TextureIndirectScheduled(RC_AxisPixel(last,line,false))) --last;
    }
    if(first>last) return false;
    float negative=min(radius,max(float(axis-first),0.0));
    float positive=min(radius,max(float(last-axis),0.0));
    float coordinates[9];
    vec3 worldPositions[9];
    int count=0;
    for(int tap=-4;tap<=4;++tap) {
        float offset=float(tap)*(tap<0?negative:positive)*0.25;
        float coordinate=clamp(float(axis)+offset,float(first),float(last));
        if(count>0 && abs(coordinate-coordinates[count-1])<=0.000001) continue;
        coordinates[count]=coordinate;
        worldPositions[count]=RC_TextureAxisPosition(coordinate,first,last,stride,base);
        ++count;
    }
    float total=0.0;
    for(int tap=0;tap<count;++tap) {
        /* Trapezoid/Voronoi measure in world units. A short clipped side
         * contributes proportionally less as its four nodes crowd together;
         * when it collapses, their measure tends continuously to zero. */
        float measure=0.0;
        if(tap>0) measure+=length(worldPositions[tap]-worldPositions[tap-1])*0.5;
        if(tap+1<count) measure+=length(worldPositions[tap+1]-worldPositions[tap])*0.5;
        if(count==1) measure=1.0;
        float distance=length(worldPositions[tap]-position);
        if(distance>support) continue;
        float weight=RC_FilterKernel(vec3(distance/spacing),strength).x*measure;
        if(weight<=0.0) continue;
        vec3 raw;
        if(!RC_TextureAxisLinear(coordinates[tap],first,last,stride,center,base,line,vertical,position,normal,raw)) continue;
        value+=weight*raw; total+=weight;
    }
    if(total<=0.0) return false;
    value/=total;
    if(strength<1.0) {
        vec3 baseline;
        bool valid=RC_TextureRaw(pixel,1u,baseline);
        if(!valid && !vertical) valid=RC_TextureFilterIndirect(pixel,extent,baseline);
        if(valid) value=mix(baseline,value,strength);
    }
    return RC_ReceiverFinite(value) && !any(greaterThan(abs(value),vec3(65504.0)));
}
#endif
