#include "NativeShadowMask.hlsli"
bool AdmitShadowCandidate(uint instance,uint primitive,float2 barycentric,inout bool valid)
{
    if (instance>=InstanceCount || GeometryBuffers[NonUniformResourceIndex(instance)].Load(ValidationOffsets[instance])!=0) {
        valid=false; return false;
    }
    SurfaceRecord binding=Surfaces[instance];
    if (binding.material.z!=0) return true;
    MaterialRecord material=Materials[binding.material.x];
    TriangleSurface receiverSurface;
    if ((material.flags.x&256u)==0 || material.flags.w!=0 || (material.flags.x&16u)==0 ||
        !LoadTriangle(instance,primitive,barycentric,receiverSurface)) { valid=false; return false; }
    return AdmitCandidate(instance,primitive,barycentric);
}
void TraceNativeShadow(uint2 pixel)
{
    Shadow[pixel]=float4(1,0,0,0);
    float4 cameraRay=Receivers[pixel];
    if (cameraRay.w!=1 || !all(isfinite(cameraRay))) return;
    RayDesc primary;
    primary.Origin=CameraOrigin; primary.Direction=cameraRay.xyz;
    primary.TMin=MinimumDistance; primary.TMax=MaximumDistance;
    bool geometryValid=true;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> surface;
    surface.TraceRayInline(Scene,RAY_FLAG_NONE,3,primary);
    while (surface.Proceed()) {
        if (surface.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (AdmitShadowCandidate(surface.CandidateInstanceID(),surface.CandidatePrimitiveIndex(),surface.CandidateTriangleBarycentrics(),geometryValid))
            surface.CommitNonOpaqueTriangleHit();
    }
    if (surface.CommittedStatus()!=COMMITTED_TRIANGLE_HIT) return;
    float distance=surface.CommittedRayT();
    if (!NativeShadowValid(true,geometryValid,distance)) return;
    uint instance = surface.CommittedInstanceID(), primitive = surface.CommittedPrimitiveIndex();
    MaterialRecord record = Materials[Surfaces[instance].material.x];
    if ((record.flags.x & 3u) != 3u || record.flags.w != 0u) return;
    float4 adjacent = Receivers[min(pixel+uint2(1,1),Size-1)];
    float footprint = max(length(adjacent.xyz-primary.Direction),1e-6) * distance;
    EvaluatedSourceMaterial material;
    TriangleSurface triSurface;
    if (!LoadSourceMaterial(instance,primitive,surface.CommittedTriangleBarycentrics(),surface.CommittedObjectToWorld3x4(),surface.CommittedWorldToObject3x4(),footprint,material)) return;
    if (!LoadTriangle(instance,primitive,surface.CommittedTriangleBarycentrics(),triSurface)) return;
    float3 edge0 = mul((float3x3)surface.CommittedObjectToWorld3x4(),triSurface.position1-triSurface.position0);
    float3 edge1 = mul((float3x3)surface.CommittedObjectToWorld3x4(),triSurface.position2-triSurface.position0);
    uint normalMode = DirectNormalMode & 3u;
    float3 geometryNormal = normalize(cross(edge0,edge1));
    float3 normal = normalMode == 1u ? geometryNormal : normalize(material.normal);
    if (!all(isfinite(geometryNormal)) || (normalMode != 0u && !all(isfinite(normal)))) return;
    float3 position = primary.Origin + primary.Direction * distance;
    uint count = Padding.z != 0u ? Padding.z : 1u;
    if (count > MaximumDirectLights) return;
    float totalWeight = 0, visibleWeight = 0;
    const bool traced = NativeTraceSample(pixel.x,pixel.y,DirectTraceMode,DirectStochastic.x >= 0.5,FrameIndex,0u);
    bool valid = true;
    [loop] for (uint lightIndex=0;lightIndex<count;++lightIndex) {
        uint base = lightIndex * LightSnapshotTexels;
        float4 source = LightSnapshot.Load(int3(base+6u,0,0));
        float4 radiance = LightSnapshot.Load(int3(base+7u,0,0));
        if (source.w != 1 || radiance.w != 1) continue;
        float3 lightPosition = LightSnapshot.Load(int3(base+2u,0,0)).xyz + source.xyz;
        float3 delta = lightPosition-position;
        float dist = length(delta);
        if (!isfinite(dist) || dist <= MinimumDistance) { valid=false; continue; }
        float4 falloff = LightSnapshot.Load(int3(base+1u,0,0));
        float radius = min(max(falloff.x*PointLightFalloff.x,MinimumDistance),MaximumDistance);
        if (dist >= radius) continue;
        int admission = LightRoomAdmission(ReceiverRoom::Unknown,lightIndex);
        if (admission < 0) { valid=false; continue; }
        if (admission == 0) continue;
        float sourceRadius = max(falloff.w,MinimumDistance);
        float fade = saturate((radius-dist)/max(radius*PointLightFalloff.y,MinimumDistance));
        float cutoff = pow(fade*fade*(3-2*fade),PointLightFalloff.z);
        float attenuation = cutoff/max(dist*dist+sourceRadius*sourceRadius,1e-4);
        float3 direction = delta/dist;
        float facing = normalMode == 0u ? 1 : max(0,dot(normal,direction));
        float weight = NativeShadowLightWeight(dot(max(radiance.rgb,0),float3(0.2126,0.7152,0.0722)),attenuation,facing);
        if (!isfinite(weight)) { valid=false; continue; }
        if (weight <= 0) continue;
        if (!traced) { totalWeight += weight; continue; }
        bool visible;
        if (!TracePreparedPointVisibility(lightIndex,position,lightPosition,sourceRadius,direction,dist,pixel,0u,true,visible)) { valid=false; continue; }
        totalWeight += weight;
        visibleWeight += weight * NativeShadowLightVisibility(visible,cutoff,PointLightFalloff.w >= 0.5);
    }
    if (valid && isfinite(totalWeight) && totalWeight > 1e-6)
        Shadow[pixel]=float4(traced ? saturate(NativeShadowWeightedVisibility(visibleWeight,totalWeight)) : 1,NativeShadowSampleState(traced),distance,float(instance+1u));
}
