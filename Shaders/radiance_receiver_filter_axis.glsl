#ifndef RC_RECEIVER_FILTER_AXIS_GLSL
#define RC_RECEIVER_FILTER_AXIS_GLSL
/* 31,988 bytes of shared memory at 416 lanes, below Vulkan's 32 KiB minimum;
 * 34,328 at the 512 lanes used wherever the device allows them. Wider groups
 * waste less on the apron.
 * Cache geometry once per row/column tile; nine logical irradiance taps use at most
 * eighteen compatible endpoint fetches per output. An edge bit blocks every segment crossing that edge.
 * RC_AXIS_RADIUS is the storage limit: each tile loads only the reach its lanes can use. */
#ifndef RC_AXIS_FRACTIONAL
#define RC_AXIS_FRACTIONAL true
#endif
#ifndef RC_AXIS_DIRECT_SCALE
#define RC_AXIS_DIRECT_SCALE(id) 1.0
#endif
const int RC_AXIS_GROUP=int(gl_WorkGroupSize.x);
const int RC_AXIS_RADIUS=448;
const int RC_AXIS_CELLS=RC_AXIS_GROUP+2*RC_AXIS_RADIUS;
const int RC_AXIS_WORDS=(RC_AXIS_CELLS+31)/32;
/* Integer storage keeps all oct codes and sentinels outside float NaN/
 * denormal semantics. Position bits are converted only when used as xyz. */
shared uvec4 rcAxisData[RC_AXIS_CELLS];
/* Exact native primitive identity proves planarity even when a smooth
 * shading normal tilts away from its triangle. Never substitute a hash. */
shared uvec2 rcAxisID[RC_AXIS_CELLS];
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
/* Taps compare against one center, so its decoded guide is passed in. Both cells
 * passed RC_ReceiverGuide, so their identities are nonzero and their guides finite. */
bool RC_AxisContinuousFrom(uvec2 id,uint packedNormal,vec3 position,vec3 normal,int b)
{
    uvec2 otherID=rcAxisID[b];
    /* One triangle with one packed normal: most of a flat surface, with no decode. */
    if(rcAxisData[b].w==packedNormal && all(equal(id,otherID))) return true;
    vec3 otherNormal=RC_AxisDecodeNormal(rcAxisData[b].w);
    float facing=dot(normal,otherNormal),cosine=rcReceiver.normalThreshold.x;
    if(facing<cosine) return false;
    if(all(equal(id,otherID))) return true;
    vec3 delta=RC_AxisPosition(b)-position;
    float allowance=rcReceiver.normalThreshold.y+length(delta)*sqrt(max(1.0-cosine*cosine,0.0));
    return abs(dot(delta,normal))<=allowance && abs(dot(delta,otherNormal))<=allowance;
}
bool RC_AxisContinuous(int a,int b) { return RC_AxisContinuousFrom(rcAxisID[a],rcAxisData[a].w,RC_AxisPosition(a),RC_AxisNormal(a),b); }
/* GI, direct and minimum have independent validity boundaries. In particular,
 * a missing probe must never prevent filtering valid direct illumination. */
shared uint rcAxisBarrier[RC_AXIS_WORDS*3];
/* Per-tile cache reach: [0] GI, [1] direct and minimum. */
shared uint rcAxisReach[2];
int RC_AxisBarrierBase(uint field) { return field==1u?0:field==0u?RC_AXIS_WORDS:2*RC_AXIS_WORDS; }
ivec2 RC_AxisPixel(int axis,int line,bool vertical)
{
    return vertical?ivec2(line,axis):ivec2(axis,line);
}
bool RC_TextureAxisUnknown(ivec2 p,uint field,bool vertical)
{
    if(field==1u) return RC_TextureIndirectScheduled(p) && (RC_ResolvePackedStatus(p)&4u)==0u;
    /* H reads original checker evidence; V reads the reconstructed H field. */
    if(!vertical && rcLighting.receiver_filter.y>0.5 && !RC_CheckerHasSample(p,field)) return false;
    vec3 value; return !RC_TextureRaw(p,field,value);
}
float RC_AxisSupport(float spacing,float strength)
{
    return (clamp(strength,0.0,2.0)>1.0?2.5:1.5)*spacing;
}
/* World distance to an adjacent cell, per receiver texel. */
float RC_AxisSeparation(int center,int axis,int direction,int line,bool vertical)
{
    float separation=length(RC_AxisPosition(center+direction)-RC_AxisPosition(center));
    float divisor=rcReceiver.normalThreshold.z;
    if(fract(divisor)!=0.0) {
        ivec2 a=RC_ReceiverFirstPixel(RC_AxisPixel(axis,line,vertical),divisor);
        ivec2 b=RC_ReceiverFirstPixel(RC_AxisPixel(axis+direction,line,vertical),divisor);
        int span=vertical?abs(b.y-a.y):abs(b.x-a.x);
        separation=separation/float(span)*divisor;
    }
    return separation;
}
void RC_AxisLoadCell(int i,int base,int line,bool vertical,ivec2 extent,uint fields)
{
    ivec2 p=RC_AxisPixel(base+i-RC_AXIS_RADIUS,line,vertical);
    uvec2 id; vec3 position,normal;
    bool guide=RC_ReceiverGuide(p,extent,id,position,normal);
    bool unknown=fields==2u && guide && RC_TextureAxisUnknown(p,1u,vertical);

    /* A scheduled lookup without original evidence is a barrier even
     * when another pass could have produced a filtered value there. */
    uint packed=!guide?RC_AXIS_INVALID:unknown?RC_AXIS_UNKNOWN:RC_AxisPackNormal(normal);
    rcAxisData[i]=uvec4(floatBitsToUint(position),packed);
    rcAxisID[i]=id;
}
/* Three cells cover a stride-two interpolation endpoint past the last tap. */
const int RC_AXIS_REACH_MARGIN=3;
uint RC_AxisReachFor(float support,float nearest)
{
    float radius=support>0.0?min(support/nearest,float(RC_AXIS_RADIUS)):0.0;
    return uint(min(int(ceil(radius))+RC_AXIS_REACH_MARGIN,RC_AXIS_RADIUS));
}
void RC_TextureAxisSetEdge(int edge,uint field)
{
    if(edge>=0 && edge<RC_AXIS_CELLS-1) atomicOr(rcAxisBarrier[RC_AxisBarrierBase(field)+(edge>>5)],1u<<uint(edge&31));
}
/* The bound uses every valid neighbor, including ones a field's barrier would
 * exclude, so it can only exceed the radius RC_TextureFilterAxisField derives. */
void RC_TextureAxisLoadFields(int base,int line,bool vertical,ivec2 extent,uint fields,float indirectSupport,float directSupport)
{
    const int coreFirst=RC_AXIS_RADIUS-1,coreLast=RC_AXIS_RADIUS+RC_AXIS_GROUP;
    uint lane=gl_LocalInvocationID.x;
    for(int i=coreFirst+int(lane);i<=coreLast;i+=RC_AXIS_GROUP) RC_AxisLoadCell(i,base,line,vertical,extent,fields);
    if(lane<uint(RC_AXIS_WORDS*3)) rcAxisBarrier[lane]=0u;
    if(lane<2u) rcAxisReach[lane]=uint(RC_AXIS_REACH_MARGIN);
    barrier();
    int center=RC_AXIS_RADIUS+int(lane);
    if(RC_AxisState(center)>0.0) {
        float nearest=3.402823466e+38;
        for(int direction=-1;direction<=1;direction+=2) {
            if(RC_AxisState(center+direction)<=0.0) continue;
            float separation=RC_AxisSeparation(center,base+int(lane),direction,line,vertical);
            if(separation>0.00001) nearest=min(nearest,separation);
        }
        if(nearest<3.402823466e+38) {
            if((fields&2u)!=0u) atomicMax(rcAxisReach[0],RC_AxisReachFor(indirectSupport,nearest));
            if((fields&5u)!=0u) atomicMax(rcAxisReach[1],RC_AxisReachFor(directSupport*RC_AXIS_DIRECT_SCALE(rcAxisID[center]),nearest));
        }
    }
    barrier();
    int reachIndirect=(fields&2u)!=0u?int(rcAxisReach[0]):0;
    int reachDirect=(fields&5u)!=0u?int(rcAxisReach[1]):0;
    int reach=max(reachIndirect,reachDirect);
    int lower=RC_AXIS_RADIUS-reach,upper=RC_AXIS_RADIUS+RC_AXIS_GROUP-1+reach;
    for(int i=lower+int(lane);i<coreFirst;i+=RC_AXIS_GROUP) RC_AxisLoadCell(i,base,line,vertical,extent,fields);
    for(int i=coreLast+1+int(lane);i<=upper;i+=RC_AXIS_GROUP) RC_AxisLoadCell(i,base,line,vertical,extent,fields);
    barrier();
    for(int i=lower+int(lane);i<=upper;i+=RC_AXIS_GROUP) {
        bool blocked=i==upper || RC_AxisState(i)<=0.0 || RC_AxisState(i+1)<=0.0 || !RC_AxisContinuous(i,i+1);
        bool cell=fields!=2u && RC_AxisState(i)>0.0;
        for(uint field=0u;field<3u;++field) {
            if((fields&(1u<<field))==0u) continue;
            int fieldReach=field==1u?reachIndirect:reachDirect;
            if(i<RC_AXIS_RADIUS-fieldReach || i>RC_AXIS_RADIUS+RC_AXIS_GROUP-1+fieldReach) continue;
            if(blocked) RC_TextureAxisSetEdge(i,field);
            /* An unknown cell blocks both of its edges, once per cell. */
            if(cell && RC_TextureAxisUnknown(RC_AxisPixel(base+i-RC_AXIS_RADIUS,line,vertical),field,vertical)) {
                RC_TextureAxisSetEdge(i-1,field); RC_TextureAxisSetEdge(i,field);
            }
        }
    }
    /* Segment searches must stop at the loaded range, never read past it. */
    if(lane<3u && (fields&(1u<<lane))!=0u) {
        int fieldReach=lane==1u?reachIndirect:reachDirect;
        if(fieldReach<RC_AXIS_RADIUS) {
            RC_TextureAxisSetEdge(RC_AXIS_RADIUS-fieldReach-1,lane);
            RC_TextureAxisSetEdge(RC_AXIS_RADIUS+RC_AXIS_GROUP-1+fieldReach,lane);
        }
    }
    barrier();
}
void RC_TextureAxisLoad(int base,int line,bool vertical,ivec2 extent)
{ RC_TextureAxisLoadFields(base,line,vertical,extent,2u,RC_AxisSupport(rcLevels[0].origin_spacing.w,rcLighting.receiver_filter.x),0.0); }
bool RC_TextureAxisClearField(int a,int b,uint field)
{
    int first=min(a,b),last=max(a,b)-1;
    if(last<first) return true;
    for(int word=first>>5;word<=last>>5;++word) {
        int lo=max(first-word*32,0),hi=min(last-word*32,31);
        uint mask=(0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if((rcAxisBarrier[RC_AxisBarrierBase(field)+word]&mask)!=0u) return false;
    }
    return true;
}
bool RC_TextureAxisClear(int a,int b) { return RC_TextureAxisClearField(a,b,1u); }
/* Locate the maximal visible continuous segment around this receiver. Bits
 * describe edges, so a blocking edge e permits cells up to e on its left and
 * from e+1 on its right. Search whole words, never fetch guides again. */
ivec2 RC_TextureAxisSegmentField(int center,uint field)
{
    int lower=center-RC_AXIS_RADIUS,upper=center+RC_AXIS_RADIUS;
    for(int word=(center-1)>>5;word>=lower>>5;--word) {
        int lo=max(lower-word*32,0),hi=min(center-1-word*32,31);
        uint bits=rcAxisBarrier[RC_AxisBarrierBase(field)+word] & (0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if(bits!=0u) { lower=max(lower,word*32+findMSB(bits)+1); break; }
    }
    for(int word=center>>5;word<=(upper-1)>>5;++word) {
        int lo=max(center-word*32,0),hi=min(upper-1-word*32,31);
        uint bits=rcAxisBarrier[RC_AxisBarrierBase(field)+word] & (0xffffffffu<<uint(lo)) & (0xffffffffu>>uint(31-hi));
        if(bits!=0u) { upper=min(upper,word*32+findLSB(bits)); break; }
    }
    return ivec2(lower,upper);
}
ivec2 RC_TextureAxisSegment(int center) { return RC_TextureAxisSegmentField(center,1u); }
/* Places a tap between two lattice cells. first/last lie inside the nearest-
 * barrier segment, so clamped a/b have clear paths to center without rescanning
 * its bitset; only their continuity with the center remains to be proved. */
vec3 RC_TextureAxisPlace(float coordinate,int first,int last,int stride,int base,out int a,out int b,out float f)
{
    a=first+int(floor((coordinate-float(first))/float(stride)))*stride;
    b=min(a+stride,last);
    f=b==a?0.0:(coordinate-float(a))/float(b-a);
    return mix(RC_AxisPosition(a-base+RC_AXIS_RADIUS),RC_AxisPosition(b-base+RC_AXIS_RADIUS),f);
}
bool RC_TextureAxisFetch(int a,int b,float f,int line,bool vertical,uint field,out vec3 value)
{
    value=vec3(0);
    vec3 va,vb;
    if(!RC_TextureRaw(RC_AxisPixel(a,line,vertical),field,va)) return false;
    vb=va;
    if(b!=a && !RC_TextureRaw(RC_AxisPixel(b,line,vertical),field,vb)) return false;
    value=mix(va,vb,f);
    return true;
}
bool RC_TextureAxisFinish(vec3 sum,float total,ivec2 pixel,ivec2 extent,bool vertical,uint field,float strength,out vec3 value)
{
    value=vec3(0);
    if(total<=0.0) return false;
    value=sum/total;
    if(RC_AXIS_FRACTIONAL && strength<1.0) {
        vec3 baseline;
        bool valid=RC_TextureRaw(pixel,field,baseline);
        if(!valid && !vertical) valid=field==1u?RC_TextureFilterIndirect(pixel,extent,baseline):RC_CheckerReconstruct(pixel,extent,field,baseline);
        if(valid) value=mix(baseline,value,strength);
    }
    return RC_ReceiverFinite(value) && !any(greaterThan(abs(value),vec3(65504.0)));
}
/* pair also filters minimum alongside direct. The caller proves both fields
 * share a segment, so positions, weights and continuity are computed once. */
bool RC_TextureAxisFilterCore(ivec2 pixel,ivec2 extent,int base,int line,bool vertical,uint field,bool pair,float spacing,float strength,
    out vec3 value,out vec3 second,out bool secondValid)
{
    value=vec3(0); second=vec3(0); secondValid=false;
    int axis=vertical?pixel.y:pixel.x;
    int center=axis-base+RC_AXIS_RADIUS;
    if(RC_AxisState(center)<=0.0 || RC_TextureAxisUnknown(pixel,field,vertical)) return false;
    strength=clamp(strength,0.0,2.0);
    if(isnan(spacing) || isinf(spacing) || spacing<=0.0) return false;
    float support=RC_AxisSupport(spacing,strength);
    uvec2 id=rcAxisID[center];
    uint packedNormal=rcAxisData[center].w;
    vec3 position=RC_AxisPosition(center),normal=RC_AxisNormal(center);
    float derivative=3.402823466e+38;
    for(int direction=-1;direction<=1;direction+=2) {
        int neighbor=center+direction;
        if(RC_TextureAxisClearField(center,neighbor,field))
            derivative=min(derivative,RC_AxisSeparation(center,axis,direction,line,vertical));
    }
    float radius=0.0;
    if(derivative>0.00001 && derivative<3.402823466e+38)
        radius=min(support/derivative,float(RC_AXIS_RADIUS));
    ivec2 segment=RC_TextureAxisSegmentField(center,field);
    int first=base+segment.x-RC_AXIS_RADIUS,last=base+segment.y-RC_AXIS_RADIUS;
    int stride=1;
    /* H interpolates only original known checker samples, on a stride-two
     * lattice. V sees H's reconstructed values and uses every texel. */
    if(!vertical && rcLighting.receiver_filter.y>0.5) {
        stride=2;
        if(!RC_CheckerHasSample(RC_AxisPixel(first,line,false),field)) ++first;
        if(!RC_CheckerHasSample(RC_AxisPixel(last,line,false),field)) --last;
    }
    if(first>last) return false;
    float negative=min(radius,max(float(axis-first),0.0));
    float positive=min(radius,max(float(last-axis),0.0));
    /* Nine logical taps, one behind so each knows its neighbours. A tap's weight
     * needs the next position, and the extra pass flushes the last one. Sliding
     * values stay in registers; arrays indexed by a live count would not. */
    float total=0.0,secondTotal=0.0,currentCoordinate=0.0;
    vec3 sum=vec3(0),secondSum=vec3(0),previous=vec3(0),current=vec3(0);
    bool hasCurrent=false,hasPrevious=false;
    int currentA=0,currentB=0,nextA=0,nextB=0;
    float currentF=0.0,nextF=0.0;
    [[dont_unroll]] for(int tap=-4;tap<=5;++tap) {
        bool flush=tap==5;
        float coordinate=currentCoordinate;
        vec3 next=current;
        if(!flush) {
            float offset=float(tap)*(tap<0?negative:positive)*0.25;
            coordinate=clamp(float(axis)+offset,float(first),float(last));
            if(hasCurrent && abs(coordinate-currentCoordinate)<=0.000001) continue;
            next=RC_TextureAxisPlace(coordinate,first,last,stride,base,nextA,nextB,nextF);
        }
        if(hasCurrent) {
            /* Trapezoid/Voronoi measure in world units. A short clipped side
             * contributes proportionally less as its four nodes crowd together;
             * when it collapses, their measure tends continuously to zero. */
            float measure=0.0;
            if(hasPrevious) measure+=length(current-previous)*0.5;
            if(!flush) measure+=length(next-current)*0.5;
            if(flush && !hasPrevious) measure=1.0;
            float distance=length(current-position);
            float weight=distance>support?0.0:RC_FilterKernel(vec3(distance/spacing),strength).x*measure;
            int b=currentF<=0.000001?currentA:currentB;
            if(weight>0.0 && RC_AxisContinuousFrom(id,packedNormal,position,normal,currentA-base+RC_AXIS_RADIUS) &&
                (b==currentA || RC_AxisContinuousFrom(id,packedNormal,position,normal,b-base+RC_AXIS_RADIUS))) {
                vec3 raw;
                if(RC_TextureAxisFetch(currentA,b,currentF,line,vertical,field,raw)) { sum+=weight*raw; total+=weight; }
                if(pair && RC_TextureAxisFetch(currentA,b,currentF,line,vertical,2u,raw)) { secondSum+=weight*raw; secondTotal+=weight; }
            }
            previous=current; hasPrevious=true;
        }
        current=next; currentCoordinate=coordinate; hasCurrent=true;
        currentA=nextA; currentB=nextB; currentF=nextF;
    }
    if(pair) secondValid=RC_TextureAxisFinish(secondSum,secondTotal,pixel,extent,vertical,2u,strength,second);
    return RC_TextureAxisFinish(sum,total,pixel,extent,vertical,field,strength,value);
}
bool RC_TextureFilterAxisField(ivec2 pixel,ivec2 extent,int base,int line,bool vertical,uint field,float spacing,float strength,out vec3 value)
{
    vec3 unused; bool unusedValid;
    return RC_TextureAxisFilterCore(pixel,extent,base,line,vertical,field,false,spacing,strength,value,unused,unusedValid);
}
/* Direct and minimum almost always share a segment. Their validity can still
 * differ, so any mismatch filters them apart. One loop keeps a single inlined
 * copy of the filter body. Without local-minimum lights that field is zero
 * wherever direct is valid, so it needs no taps. */
void RC_TextureFilterAxisDirectPair(ivec2 pixel,ivec2 extent,int base,int line,bool vertical,float spacing,float strength,bool minimumLights,
    out vec3 direct,out bool directValid,out vec3 minimum,out bool minimumValid)
{
    int center=(vertical?pixel.y:pixel.x)-base+RC_AXIS_RADIUS;
    bool pair=minimumLights && RC_AxisState(center)>0.0 && !RC_TextureAxisUnknown(pixel,0u,vertical) && !RC_TextureAxisUnknown(pixel,2u,vertical) &&
        all(equal(RC_TextureAxisSegmentField(center,0u),RC_TextureAxisSegmentField(center,2u)));
    direct=vec3(0); minimum=vec3(0); directValid=false; minimumValid=false;
    [[dont_unroll]] for(uint field=0u;field<=2u;field+=2u) {
        vec3 value,second; bool secondValid;
        bool valid=RC_TextureAxisFilterCore(pixel,extent,base,line,vertical,field,pair,spacing,strength,value,second,secondValid);
        if(field==0u) { direct=value; directValid=valid; }
        else { minimum=value; minimumValid=valid; }
        if(pair) { minimum=second; minimumValid=secondValid; break; }
        if(!minimumLights) { minimumValid=directValid; break; }
    }
}
bool RC_TextureFilterAxis(ivec2 pixel,ivec2 extent,int base,int line,bool vertical,out vec3 value)
{ return RC_TextureFilterAxisField(pixel,extent,base,line,vertical,1u,rcLevels[0].origin_spacing.w,rcLighting.receiver_filter.x,value); }
#endif
