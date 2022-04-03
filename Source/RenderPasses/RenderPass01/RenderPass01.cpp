/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "RenderPass01.h"

const RenderPass::Info RenderPass01::kInfo { "RenderPass01", "Insert pass description here." };
const std::string cShaderFile{ "RenderPasses/RenderPass01/RenderPass01.slang" };
const uint32_t cMaxRecursionDepth{ 2u };
const uint32_t cMaxPayloadSizeBytes{ 72u };

const ChannelList kOutputChannels =
{
    { "color",          "outputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float },
};

const ChannelList kInputChannels =
{
    { "vbuffer","gVBuffer", "Visibility buffer in packed format" },
    { "viewW",    "gViewW",       "World-space view direction (xyz float format)", true /* optional */ }

};

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RenderPass01::kInfo, RenderPass01::create);
}

RenderPass01::RenderPass01(const Dictionary& dict) : RenderPass(kInfo), mFrameCount{ 0 }
{
    // Create a sample generator.
    mSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mSampleGenerator);
}

RenderPass01::SharedPtr RenderPass01::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new RenderPass01(dict));
    return pPass;
}

Dictionary RenderPass01::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection RenderPass01::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void RenderPass01::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mScene)
    {
        // renderData holds the requested resources
        const auto& pTexture = renderData["color"]->asTexture();
        pRenderContext->clearUAV(pTexture->getUAV().get(), float4(1.f));
        return;
    }

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    if (!mTracer.pVars)
        prepareVars();

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    auto var = mTracer.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount;

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    for (auto channel : kInputChannels) bind(channel);
    for (auto channel : kOutputChannels) bind(channel);

    mScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
    mFrameCount++;
}

void RenderPass01::renderUI(Gui::Widgets& widget)
{
}

void RenderPass01::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;

    mScene = pScene;

    if (mScene)
    {
        if (mScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("RenderPass01: This render pass does not support custom primitives.");
        }

        RtProgram::Desc desc;
        desc.addShaderLibrary(cShaderFile);
        desc.setMaxPayloadSize(cMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(cMaxRecursionDepth);

        mTracer.pBindingTable = RtBindingTable::create(2, 2, mScene->getGeometryCount());
        auto& sbt = mTracer.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("triangleMeshMiss"));
        sbt->setMiss(1, desc.addMiss("shadowMiss"));

        if (mScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("triangleMeshClosestHit", "triangleMeshClosestAnyHit"));
            sbt->setHitGroup(1, mScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit"));
        }

        mTracer.pProgram = RtProgram::create(desc, mScene->getSceneDefines());
    }
}

void RenderPass01::prepareVars()
{
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mTracer.pProgram);

    // Configure program.
    mTracer.pProgram->addDefines(mSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mScene->getTypeConformances());

    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracer.pVars->getRootVar();
    mSampleGenerator->setShaderData(var);
}
