#include "Scene/SceneDefines.slangh"

import Scene.Raytracing;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;

cbuffer CB
{
    uint gFrameCount; // Frame count since scene was loaded.
}

struct ScatterRayData
{
    float4 color;
    float3 dir;
    float3 origin;
    float3 thp;
    bool terminated;

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).

    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.color = float4(0.0f);
        this.dir = float3(0.0f);
        this.origin = float3(0.0f);
        this.thp = float3(1.0f);
        this.sg = sg;
    }
};

struct ShadowRayData
{
    bool visible;
};

RWTexture2D<float4> outputColor;
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

static const bool kUseImportanceSampling = true;

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/
ShadingData loadShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDir, const ITextureSampler lod)
{
    VertexData v = { };
    uint materialID = { };
    float curveSphereRadius = { };

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedTriangleHit = hit.getDisplacedTriangleHit();
        v = gScene.getVertexData(displacedTriangleHit, -rayDir);
        materialID = gScene.getMaterialID(displacedTriangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        v = gScene.getVertexDataFromCurve(curveHit, curveSphereRadius);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfGridHit = hit.getSDFGridHit();
        v = gScene.getVertexDataFromSDFGrid(sdfGridHit, rayOrigin, rayDir);
        materialID = gScene.getMaterialID(sdfGridHit.instanceID);
    }
#endif

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        sd.curveSphereRadius = curveSphereRadius;
    }
#endif

    return sd;
}

bool prepareNewRay(const ShadingData sd, const IBSDF bsdf, float3 rayOrigin, inout ScatterRayData rayData)
{
    BSDFSample bsdfSample;
    if (bsdf.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        rayData.origin = rayOrigin;
        if (bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeNewRayOrigin(false);
        }
        rayData.dir = bsdfSample.wo;
        rayData.thp *= bsdfSample.weight;
        return any(rayData.thp > 0.f);
    }
    return false;
}

/** Traces a shadow ray towards a light source.
    \param[in] origin Ray origin for the shadow ray.
    \param[in] dir Direction from shading point towards the light source (normalized).
    \param[in] distance Distance to the light source.
    \return True if light is visible, false otherwise.
*/
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if ray is not terminated before
    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, ray, rayData);

    return rayData.visible;
}

float3 evalDirectLight(const ShadingData sd, const IBSDF bsdf, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return float3(0.f, 0.0f, 0.0f);

    // Pick one of the analytic light sources randomly with equal probability.
    const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    AnalyticLightSample ls;
    if (!sampleLight(sd.posW, gScene.getLight(lightIndex), sg, ls))
        return float3(0.f, 0.0f, 0.0f);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobes = bsdf.getLobes(sd);
    const bool hasReflection = lobes & uint(LobeType::Reflection);
    const bool hasTransmission = lobes & uint(LobeType::Transmission);
    if (dot(ls.dir, sd.N) <= kMinCosTheta && !hasTransmission)
        return float3(0.f);
    if (dot(ls.dir, sd.N) >= -kMinCosTheta && !hasReflection)
        return float3(0.f);

    // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
    const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

    // Test visibility by tracing a shadow ray.
    bool V = traceShadowRay(origin, ls.dir, ls.distance);
    if (!V)
        return float3(0.f, 0.0f, 0.0f);

    // Evaluate contribution.
    return bsdf.eval(sd, ls.dir, sg) * ls.Li * invPdf;
}


void fireRay(inout ScatterRayData rayData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.dir;
    ray.TMin = 0.0f;
    ray.TMax = 1000.0f;
        
    uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    rayData.visible = true;
}

[shader("miss")]
void triangleMeshMiss(inout ScatterRayData rayData)
{
    rayData.terminated = true;
}

[shader("closesthit")]
void triangleMeshClosestHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    let lod = ExplicitLodTextureSampler(0.f);

    ShadingData sd = loadShadingData(HitInfo(triangleHit), rayData.origin, rayData.dir, lod);

    let bsdf = gScene.materials.getBSDF(sd, lod);

    rayData.color += float4(rayData.thp * (evalDirectLight(sd, bsdf, rayData.sg) + bsdf.getProperties(sd).emission), 0.0f);

    if (!prepareNewRay(sd, bsdf, sd.computeNewRayOrigin(), rayData))
    {
        rayData.terminated = true;
        return;
    }
     
}

[shader("anyhit")]
void triangleMeshClosestAnyHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{

}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    // Create sample generator.
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount);
    ScatterRayData srd = ScatterRayData(sg);

    float3 origin = gScene.camera.getPosition();
    float3 direction = gScene.camera.computeRayPinhole(pixel, frameDim).dir;
    float3 outColor = float3(0.0f);
    
    HitInfo hit = HitInfo(gVBuffer[pixel]);
    
    if (hit.isValid())
    {

        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hit, origin, direction, lod);
        let bsdf = gScene.materials.getBSDF(sd, lod);

        outColor += evalDirectLight(sd, bsdf, sg) + bsdf.getProperties(sd).emission;
        
        if (!prepareNewRay(sd, bsdf, sd.computeNewRayOrigin(),srd))
            srd.terminated = true;
        
        for (uint i = 0; i < 3 && !srd.terminated; i++)
        {
            fireRay(srd);
        }
        outColor += srd.color.xyz;
    }
    
    outputColor[pixel] = float4(outColor, 0.0f);
}
