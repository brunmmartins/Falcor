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
#include "JumpRenderPass.h"

const RenderPass::Info JumpRenderPass::kInfo { "JumpRenderPass", "Insert pass description here." };

namespace {

    const std::string kProgramFile = "RenderPasses/JumpRenderPass/JumpRenderPass.slang";
    const std::string kShaderModel = "6_2";

    const RasterizerState::CullMode kDefaultCullMode = RasterizerState::CullMode::Back;
    const std::string kOuputColor = "output";
    const std::string kOuputColorDesc = "Output Color texture";

    const ChannelList kInputChannels =
    {
        { "vbuffer", "gVBuffer", "V-buffer in packed format (indices + barycentrics)", true /* optional */, ResourceFormat::RGBA32Uint   },
        { "viewW", "viewW", "V-buffer in packed format (indices + barycentrics)", true /* optional */, ResourceFormat::RGBA32Float   },
    };
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(JumpRenderPass::kInfo, JumpRenderPass::create);
}

JumpRenderPass::JumpRenderPass() : RenderPass(kInfo)
{
    // Check for required features.
    if (!gpDevice->isFeatureSupported(Device::SupportedFeatures::Barycentrics))
    {
        throw RuntimeError("GBufferRaster: Pixel shader barycentrics are not supported by the current device");
    }
    if (!gpDevice->isFeatureSupported(Device::SupportedFeatures::RasterizerOrderedViews))
    {
        throw RuntimeError("GBufferRaster: Rasterizer ordered views (ROVs) are not supported by the current device");
    }

    // Create raster program
    Program::Desc desc;
    desc.addShaderLibrary(kProgramFile).vsEntry("vsMain").psEntry("psMain");
    desc.setShaderModel(kShaderModel);
    mRaster.pProgram = GraphicsProgram::create(desc);

    // Initialize graphics state
    mRaster.pState = GraphicsState::create();
    mRaster.pState->setProgram(mRaster.pProgram);

    mpFbo = Fbo::create();
}

JumpRenderPass::SharedPtr JumpRenderPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new JumpRenderPass());
    return pPass;
}

Dictionary JumpRenderPass::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection JumpRenderPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addOutput(kOuputColor, kOuputColorDesc).format(ResourceFormat::RGBA32Float);
    addRenderPassInputs(reflector, kInputChannels, Resource::BindFlags::UnorderedAccess);
    return reflector;
}

void JumpRenderPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    mpFbo->attachColorTarget(renderData[kOuputColor]->asTexture(), 0);
    pRenderContext->clearRtv(mpFbo->getRenderTargetView(0).get(), float4(0));

    // If there is no scene, clear depth buffer and return.
    if (!mpScene)
    {
        return;
    }

    if (!mRaster.pVars)
    {
        mRaster.pVars = GraphicsVars::create(mRaster.pProgram.get());
    }

    for (const auto& channel : kInputChannels)
    {
        Texture::SharedPtr pTex = renderData[channel.name]->asTexture();
        mRaster.pVars[channel.texname] = pTex;
    }

    Texture::SharedPtr pTex = renderData[kOuputColor]->asTexture();
    mRaster.pVars[kOuputColor] = pTex;

    mRaster.pState->setFbo(mpFbo); // Sets the viewport

// Rasterize the scene.
    mpScene->rasterize(pRenderContext, mRaster.pState.get(), mRaster.pVars.get(), RasterizerState::CullMode::Back);
}

void JumpRenderPass::renderUI(Gui::Widgets& widget)
{
}

void JumpRenderPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mRaster.pVars = nullptr;

    if (pScene)
    {
        if (pScene->getMeshVao() && pScene->getMeshVao()->getPrimitiveTopology() != Vao::Topology::TriangleList)
        {
            throw RuntimeError("GBufferRaster: Requires triangle list geometry due to usage of SV_Barycentrics.");
        }

        mRaster.pProgram->addDefines(pScene->getSceneDefines());
        mRaster.pProgram->setTypeConformances(pScene->getTypeConformances());
    }
}
