from falcor import *

def render_graph_JumpRenderPass():
    g = RenderGraph('JumpRenderPass')
    loadRenderPassLibrary("GBuffer.dll")
    loadRenderPassLibrary('JumpRenderPass.dll')
    GBufferRaster = createPass("GBufferRaster", {'samplePattern': SamplePattern.Center, 'sampleCount': 16})
    g.addPass(GBufferRaster, "GBufferRaster")
    JumpRenderPass = createPass('JumpRenderPass')
    g.addPass(JumpRenderPass, 'JumpRenderPass')
    g.addEdge('GBufferRaster.vbuffer','JumpRenderPass.vbuffer')
    g.addEdge('GBufferRaster.viewW','JumpRenderPass.viewW')
    g.markOutput('JumpRenderPass.output')
    return g

JumpRenderPass = render_graph_JumpRenderPass()
try: m.addGraph(JumpRenderPass)
except NameError: None