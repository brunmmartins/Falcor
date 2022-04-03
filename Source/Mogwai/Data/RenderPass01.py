from falcor import *

def render_graph_RenderPass01():
    g = RenderGraph('RenderPass01')
    loadRenderPassLibrary("GBuffer.dll")
    loadRenderPassLibrary("AccumulatePass.dll")
    loadRenderPassLibrary('RenderPass01.dll')
    AccumulatePass = createPass("AccumulatePass", {'enabled': True, 'precisionMode': AccumulatePrecision.Single})
    g.addPass(AccumulatePass, "AccumulatePass")
    RenderPass01 = createPass('RenderPass01')
    g.addPass(RenderPass01, 'RenderPass01')
    VBufferRT = createPass("VBufferRT", {'samplePattern': SamplePattern.Stratified, 'sampleCount': 16})
    g.addPass(VBufferRT, "VBufferRT")
    g.addEdge("VBufferRT.vbuffer","RenderPass01.vbuffer")
    g.addEdge("VBufferRT.viewW", "RenderPass01.viewW")
    g.addEdge('RenderPass01.color','AccumulatePass.input')
    g.markOutput('AccumulatePass.output')
    return g

RenderPass01 = render_graph_RenderPass01()
try: m.addGraph(RenderPass01)
except NameError: None
