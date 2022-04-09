import Scene.Raster;

// GBufferRaster channels
RasterizerOrderedTexture2D<PackedHitInfo> gVBuffer;
RasterizerOrderedTexture2D<float4> viewW;
RWTexture2D<float4> output;

VSOut vsMain(VSIn vsIn)
{
    return defaultVS(vsIn);
}

void psMain(VSOut vsOut)
{
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = prepareShadingData(vsOut, vsOut.materialID, viewW[vsOut.posH.xy].xyz, lod);
    let bsdf = gScene.materials.getBSDF(sd, lod);
    output[vsOut.posH.xy] = float4(bsdf.getProperties(sd).diffuseReflectionAlbedo, 0.0f);
}
