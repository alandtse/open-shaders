"""Execute production SSGI metadata paths with delayed readback/provider mocks."""

import re
import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).resolve().parents[1]


class RadianceCacheMetadataTests(unittest.TestCase):
    def test_delayed_capture_and_inference_ownership(self):
        """Execute delayed readback, domain-reset, and batch-publication ownership scenarios."""
        source = (ROOT / "src/Features/ScreenSpaceGI.cpp").read_text(encoding="utf-8")
        backend = (ROOT / "src/Features/Upscaling/FidelityFX/RuntimeDenoiser.cpp").read_text(encoding="utf-8")
        dispatch = braced(backend, "bool FidelityFX::DispatchRadianceCacheCapturedInference(")
        early_dispatch = dispatch[:dispatch.index("\t\tconst uint32_t completedSampleCount =")]
        early_dispatch += "\t}\n\treturn false;\n}"
        enqueue = source[source.index("\t\t\t\tif (!rcCaptureReadbackPending[rcCaptureWriteSlot]) {"):source.index("\t\t\t\t// Poll the oldest paired capture")]
        readiness = braced(source, "if (rcCaptureReadbackPending[readSlot]) {")
        sample_count = source[source.index("\t\t\t\t\tconst uint32_t kRCActiveGridSize ="):source.index("\t\t\t\t\tconstexpr uint32_t kRCInputFloatCount")]
        p95 = source[source.index("\t\t\t\t\t\tconst float3 captureExtent ="):source.index("\t\t\t\t\t\tconst auto selectExtentTier")]
        retire_start = source.index("\t\t\t\t\tcontext->Unmap(rcTrainingTargetReadbacks[readSlot].get(), 0);")
        retire = source[retire_start:source.index("\n\n", retire_start)]
        submit = braced(source, "void ScreenSpaceGI::ProcessRadianceCacheInference(")
        reset = braced(source, "if (rcOriginChanged || rcExtentChanged) {")
        compose = re.search(r"const bool useRCCombinedSignal =(.*?);", source, re.S).group(0)
        replacements = {
            "PRODUCTION_PROCESS": submit,
            "PRODUCTION_COMPOSE": compose,
            "PRODUCTION_EARLY_DISPATCH": early_dispatch,
            "PRODUCTION_ENQUEUE": enqueue,
            "PRODUCTION_READINESS": readiness,
            "PRODUCTION_COUNT": sample_count,
            "PRODUCTION_P95": p95,
            "PRODUCTION_RETIRE": retire,
            "PRODUCTION_RESET": reset,
        }
        fixture = CPP_FIXTURE
        for marker, value in replacements.items():
            fixture = fixture.replace(marker, value)
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture)

    def test_backend_marks_submission_only_after_signal(self):
        """Verify the submission flag is initialized before polling and set only after fence signaling."""
        backend = (ROOT / "src/Features/Upscaling/FidelityFX/RuntimeDenoiser.cpp").read_text(encoding="utf-8")
        dispatch = braced(backend, "bool FidelityFX::DispatchRadianceCacheCapturedInference(")
        self.assertEqual(dispatch.count("*a_outputSubmitted = true;"), 1)
        self.assertLess(dispatch.index("commandQueue->Signal("), dispatch.index("*a_outputSubmitted = true;"))
        self.assertLess(dispatch.index("*a_outputSubmitted = false;"), dispatch.index("if (radianceCacheCapturedInferencePending)"))


CPP_FIXTURE = r'''#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using uint = unsigned;
struct float3 { float x=0, y=0, z=0; };
struct float4 { float x=0, y=0, z=0, w=0; };
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
namespace logger { template<class... T> void error(T...) {} }
using HRESULT = int;
constexpr HRESULT DXGI_ERROR_WAS_STILL_DRAWING = -1;
bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
bool FAILED(HRESULT hr) { return hr < 0; }
constexpr int D3D11_MAP_READ = 1, D3D11_MAP_FLAG_DO_NOT_WAIT = 2;
struct D3D11_MAPPED_SUBRESOURCE { void* pData=nullptr; };
struct Resource { int id=0; Resource* get() { return this; } };
struct Texture { Resource resource; };
struct Context {
 unsigned copies=0, uploads=0, unmaps=0; bool inputReady=false, targetReady=false;
 void CopyResource(Resource*,Resource*) { ++copies; }
 void Unmap(Resource*,int) { ++unmaps; }
 HRESULT Map(Resource* r,int,int mode,int flags,D3D11_MAPPED_SUBRESOURCE* data) {
  check(mode==D3D11_MAP_READ&&flags==D3D11_MAP_FLAG_DO_NOT_WAIT,"readback must never wait");
  bool ready = r->id==1 ? targetReady : inputReady;
  if (!ready) return DXGI_ERROR_WAS_STILL_DRAWING;
  data->pData=this;return 0;
 }
 void UpdateSubresource(Resource*,int,void*,float*,unsigned,int) { ++uploads; }
} contextInstance;
namespace globals::d3d { Context* context=&contextInstance; }
struct Data { float4 RadianceCacheVolumeExtent{4096,8192,16384,0}; uint32_t RadianceCacheResultGridSize=16; };
struct ConstantBuffer { unsigned updates=0; Data uploaded; void Update(const Data& data) { ++updates;uploaded=data; } };
struct Provider {
 bool ready=false, submitted=false, ok=true;unsigned resets=0;
 void SetRRDiagnosticsEnabled(bool) {}
 void SetRadianceCacheTrainingParameters(float,float) {}
 void RequestRadianceCacheReset() { ++resets; }
 bool DispatchRadianceCacheCapturedInference(const float*,const uint32_t*,uint32_t,const float*,const float*,uint32_t,float* radiance,uint32_t* indices,uint32_t* count,bool* output,bool* submit) {
  *count=ready?1:0;*output=ready;*submit=submitted;
  if (ready) { radiance[0]=1;radiance[1]=2;radiance[2]=3;indices[0]=65; }
  return ok;
 }
} provider;
Provider* GetLightingReconstructionProvider() { return &provider; }
struct Fence { uint64_t completed=0; uint64_t GetCompletedValue() { return completed; } };
struct FidelityFX {
 uint32_t radianceCacheTrainingCapacity=4096;
 bool radianceCacheFailureLatched=false,radianceCacheContext=true,radianceCacheInputUpload=true,radianceCacheTrainingInputUpload=true,radianceCacheTrainingTargetUpload=true,radianceCacheOutputReadback=true;
 struct { Fence* fence12; } runtimeFence;
 bool radianceCacheCapturedInferencePending=true, rrDiagnosticsEnabled=true;
 uint64_t radianceCacheCapturedInferenceFence=4;
 unsigned radianceCacheCapturedInferencePollCount=0;
 bool EnsureRadianceCacheBuffers() { return !radianceCacheFailureLatched; }
 bool DispatchRadianceCacheCapturedInference(const float*,const uint32_t*,uint32_t,const float*,const float*,uint32_t,float*,uint32_t*,uint32_t*,bool*,bool*);
};
struct { bool Dispatch=true; } ffxModule;
PRODUCTION_EARLY_DISPATCH
struct ScreenSpaceGI {
 struct Settings { bool EnableRadianceCacheDiagnostics=true,EnableRadianceCacheTraining=true,EnableRadianceCache=true;unsigned RadianceCacheOutputMode=2;float RadianceCacheLearningRate=.02f,RadianceCacheWeightSmoothing=1; } settings;
 struct Scratch { float predictedRadiance[12288]{},compactedInputs[45056]{},compactedTrainingInputs[45056]{},compactedTrainingTargets[12288]{},radiancePixels[16384]{}; uint32_t predictedSourceQueryIndices[4096]{},sourceQueryIndices[4096]{}; } rcScratch;
 static constexpr unsigned kRCCaptureReadbackRingSize=3;
 Resource rcCaptureReadbacks[3],rcTrainingTargetReadbacks[3],rcCaptureBuffer,rcTrainingTargetBuffer;
 bool rcCaptureReadbackPending[3]{};uint32_t rcCaptureReadbackGridSize[3]{};float3 rcCaptureReadbackExtent[3]{};uint64_t rcCaptureReadbackDomainGeneration[3]{};
 uint32_t rcCaptureWriteSlot=0,rcCaptureReadSlot=0,rcPendingInferenceGridSize=0,rcResultGridSize=0;
 uint64_t rcVolumeDomainGeneration=0,rcPendingInferenceDomainGeneration=0,rcResultDomainGeneration=0;
 uint32_t rcTelemetryValidQueries=0,rcTelemetryTrainingSamples=0,rcTelemetryPredictionCount=0,rcTelemetryFinitePredictions=0,rcTelemetryValidOutputCells=0,rcTelemetryResultAgeFrames=0,rcAdaptiveDomainResetCount=0;
 float rcTelemetryMeanLuminance=0,rcTelemetryMeanRGBMagnitude=0,rcTelemetryMaxLuminance=0,rcTelemetryPositionP95X=.25f,rcTelemetryPositionP95Y=.125f,rcTelemetryPositionP95Z=.0625f;
 Texture texture;Texture* texRCRadiance=&texture;Texture* texRRCombinedInput=&texture;bool rcComposeRadianceCompute=true;Data data;Data* ssgiData=&data;ConstantBuffer buffer;ConstantBuffer* ssgiCB=&buffer;
 void ProcessRadianceCacheInference(uint32_t,uint32_t,uint32_t);
 bool CanCompose() { PRODUCTION_COMPOSE return useRCCombinedSignal; }
 void Enqueue(uint32_t rcGridSize) { auto* context=&contextInstance;
PRODUCTION_ENQUEUE
 }
 bool Ready() { auto* context=&contextInstance;const uint32_t readSlot=rcCaptureReadSlot;D3D11_MAPPED_SUBRESOURCE mapped{},mappedTrainingTarget{};HRESULT mapResult=DXGI_ERROR_WAS_STILL_DRAWING,targetMapResult=DXGI_ERROR_WAS_STILL_DRAWING;
PRODUCTION_READINESS
 return SUCCEEDED(mapResult)&&mapped.pData&&SUCCEEDED(targetMapResult)&&mappedTrainingTarget.pData;
 }
 uint32_t SampleCount() {const uint32_t readSlot=rcCaptureReadSlot;
PRODUCTION_COUNT
 return kRCCaptureSampleCount;
 }
 float3 WorldP95() {const uint32_t readSlot=rcCaptureReadSlot;
PRODUCTION_P95
 return {worldP95X,worldP95Y,worldP95Z};
 }
 void Retire() { auto* context=&contextInstance;const uint32_t readSlot=rcCaptureReadSlot;
PRODUCTION_RETIRE
 }
 void ResetDomain() { bool rcOriginChanged=true,rcExtentChanged=false;float3 rcPreviousVolumeOrigin{},rcCurrentVolumeOrigin{},rcPreviousVolumeExtent{},rcCurrentVolumeExtent{};
PRODUCTION_RESET
 }
};
PRODUCTION_PROCESS
int main() {
 ScreenSpaceGI gi;
 check(!gi.CanCompose(),"unpublished radiance must not be composed");
 gi.rcTrainingTargetReadbacks[0].id=1;
 gi.Enqueue(16);gi.Enqueue(32);gi.Enqueue(64);gi.Enqueue(16);
 check(contextInstance.copies==6&&gi.rcCaptureWriteSlot==0,"full ring must preserve occupied slot");
 check(gi.rcCaptureReadbackGridSize[0]==16,"quality changes must not replace queued topology");
 check(!gi.Ready()&&gi.rcCaptureReadbackPending[0],"input busy must retain slot");
 contextInstance.inputReady=true;
 check(!gi.Ready()&&contextInstance.unmaps==1&&gi.rcCaptureReadbackPending[0],"teacher busy must retain the paired slot");
 contextInstance.targetReady=true;check(gi.Ready(),"both maps ready");
 gi.ssgiData->RadianceCacheVolumeExtent={131072,131072,131072,0};
 auto p95=gi.WorldP95();check(p95.x==2048&&p95.y==2048&&p95.z==2048,"delayed anisotropic extent must remain the producing extent");
 check(gi.SampleCount()==256,"queued quality owns sample count");
 gi.ResetDomain();check(gi.SampleCount()==0&&gi.rcCaptureReadbackPending[0],"reset invalidates payload without freeing GPU-owned slots");
 gi.Retire();check(!gi.rcCaptureReadbackPending[0]&&gi.rcCaptureReadSlot==1,"paired readback retires only after readiness");
 gi.Enqueue(64);check(gi.rcCaptureReadbackDomainGeneration[0]==1&&gi.rcCaptureReadbackExtent[0].x==131072,"reused slot receives new metadata");
 gi.ResetDomain();check(gi.rcCaptureReadbackDomainGeneration[0]==1&&gi.SampleCount()==0,"returning to a prior domain cannot revive an old capture");
 gi.rcVolumeDomainGeneration=0;
 provider={false,true,true};gi.ProcessRadianceCacheInference(16,1,0);
 check(gi.rcPendingInferenceGridSize==16,"initial inference topology");
 provider={false,false,true};gi.ProcessRadianceCacheInference(64,1,0);
 check(gi.rcPendingInferenceGridSize==16&&gi.rcResultGridSize==0,"busy provider must preserve pending topology");
 provider={true,true,true};gi.ProcessRadianceCacheInference(64,1,0);
 check(gi.rcPendingInferenceGridSize==64&&gi.rcResultGridSize==16&&gi.buffer.uploaded.RadianceCacheResultGridSize==16,"completion and submission retain separate topology, including GPU constants");
 provider={true,false,true};gi.ProcessRadianceCacheInference(32,0,0);
 check(gi.rcPendingInferenceGridSize==0&&gi.rcResultGridSize==64&&gi.buffer.uploaded.RadianceCacheResultGridSize==64,"poll-only completion publishes its own topology");
 provider={false,true,true};gi.ProcessRadianceCacheInference(32,1,0);
 check(gi.CanCompose(),"published current-domain result is composable");
 gi.ResetDomain();check(!gi.CanCompose(),"old-domain texture must not be composed");unsigned uploads=contextInstance.uploads;
 provider={true,true,true};gi.ProcessRadianceCacheInference(64,1,0);
 check(contextInstance.uploads==uploads&&gi.rcPendingInferenceDomainGeneration==1,"reset rejects old completion while recording new submission");
 provider={true,false,true};gi.ProcessRadianceCacheInference(16,0,0);
 check(gi.rcResultGridSize==64&&gi.rcResultDomainGeneration==1&&gi.CanCompose(),"current generation publishes normally");
 uploads=contextInstance.uploads;provider={true,false,false};gi.ProcessRadianceCacheInference(16,1,0);
 check(contextInstance.uploads==uploads&&gi.rcPendingInferenceGridSize==0,"failed submission cannot replace completed topology or publish partial output");
 Fence fence;FidelityFX backend;backend.runtimeFence.fence12=&fence;
 bool ready=true,submitted=true;uint32_t count=123;float input[11]{};uint32_t index=0;
 check(backend.DispatchRadianceCacheCapturedInference(input,&index,1,nullptr,nullptr,0,nullptr,nullptr,&count,&ready,&submitted),"busy inference is a successful poll");
 check(!ready&&!submitted&&count==0&&backend.radianceCacheCapturedInferencePollCount==1,"production busy path must clear both output flags");
 check(!backend.DispatchRadianceCacheCapturedInference(input,&index,4097,nullptr,nullptr,0,nullptr,nullptr,&count,&ready,&submitted)&&!submitted,"invalid input cannot submit");
 backend.radianceCacheFailureLatched=true;
 check(!backend.DispatchRadianceCacheCapturedInference(input,&index,1,nullptr,nullptr,0,nullptr,nullptr,&count,&ready,&submitted)&&!submitted,"latched provider cannot submit");
 std::puts("PASS: delayed capture, topology, generation and publication ownership");
}
'''
