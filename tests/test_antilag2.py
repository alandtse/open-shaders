"""Verify conditional Anti-Lag installation, ownership and zero SDK FPS limiting."""

import os
import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).resolve().parents[1]


class AntiLag2Tests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Requires Windows SDK headers")
    def test_upstream_sdk_state_failure_retry(self):
        """Use the real SDK interface to check rejected state changes, retries, zero FPS limits and delay errors."""
        header = (ROOT / "src/Features/Upscaling/antilag2sdk.h").as_posix()
        fixture = '#include "' + header + '"\n' + r'''
#include <cassert>
struct Driver final : AMD::AntiLag2DX12::IAmdExtAntiLagApi {
    HRESULT stateResult = S_OK, delayResult = S_OK;
    unsigned states = 0, delays = 0;
    bool enabled = false;
    unsigned maxFPS = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 0; }
    HRESULT UpdateAntiLagState(VOID* value) override {
        if (!value) { ++delays; return delayResult; }
        ++states;
        auto& data = *static_cast<AMD::AntiLag2DX12::APIData_v1*>(value);
        assert(data.maxFPS == 0);
        assert(data.uiSize == sizeof(data) && data.uiVersion == 1);
        assert(data.sControlStr == nullptr && data.uiControlStrLength == 0);
        if (SUCCEEDED(stateResult)) { enabled = data.eMode == 1; maxFPS = data.maxFPS; }
        return stateResult;
    }
};
int main() {
    const auto Update = CS::AntiLag2::Update;
    AMD::AntiLag2DX12::Context empty{};
    assert(Update(nullptr, true) == E_NOINTERFACE);
    assert(Update(&empty, true) == E_NOINTERFACE);
    Driver driver;
    AMD::AntiLag2DX12::Context context{};
    context.m_pAntiLagAPI = &driver;
    driver.stateResult = E_FAIL;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        assert(Update(&context, true) == E_FAIL);
        assert(driver.states == attempt && driver.delays == 0);
        assert(!context.m_enabled && context.m_maxFPS == 0);
    }
    driver.stateResult = S_OK;
    assert(Update(&context, true) == S_OK);
    assert(driver.states == 3 && driver.delays == 1);
    assert(driver.enabled && driver.maxFPS == 0);
    assert(context.m_enabled && context.m_maxFPS == 0);
    assert(Update(&context, true) == S_OK);
    assert(driver.states == 3 && driver.delays == 2);
    driver.stateResult = E_FAIL;
    assert(Update(&context, false) == E_FAIL);
    assert(context.m_enabled && context.m_maxFPS == 0);
    driver.stateResult = S_OK;
    assert(Update(&context, false) == S_OK);
    assert(!driver.enabled && driver.maxFPS == 0);
    assert(!context.m_enabled && context.m_maxFPS == 0);
    driver.delayResult = E_FAIL;
    assert(Update(&context, true) == E_FAIL);
    assert(context.m_enabled && context.m_maxFPS == 0);
    const auto states = driver.states;
    driver.delayResult = S_FALSE;
    assert(Update(&context, true) == S_OK);
    assert(driver.states == states);
    assert(AMD::AntiLag2DX12::DeInitialize(&context) == 0);
    assert(!context.m_pAntiLagAPI && !context.m_enabled);
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(
            self, fixture, include_dirs=(ROOT / "extern/AntiLag2-SDK",))

    def test_conditional_session_lifecycle(self):
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text()
        bodies = "\n".join(braced(source, declaration) for declaration in (
            "DX12SwapChain::~DX12SwapChain()",
            "void DX12SwapChain::InitializeAntiLag2()",
            "void DX12SwapChain::UpdateAntiLag2BeforeInput()",
            "void DX12SwapChain::CreateInterop()"))
        proxy = braced(source, "struct AntiLag2SwapChainProxy final") + ";"
        fixture = LIFECYCLE_FIXTURE.replace("PROXY", proxy).replace("PRODUCTION", bodies)
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture)

    def test_optional_hook_installation(self):
        source = (ROOT / "src/Hooks.cpp").read_text()
        installer = braced(source, "bool Hooks::InstallBeforeInputPoll()")
        fixture = r'''
#include <cassert>
constexpr long NO_ERROR = 0;
struct Address { unsigned value = 0; unsigned address() const { return value; } };
struct BSInputDeviceManager_BeforeInputPoll { static inline Address func; };
namespace logger { void info(const char*) {} }
namespace REL { struct RelocationID { RelocationID(unsigned, unsigned) {} }; }
namespace stl {
unsigned calls = 0; long result = 1;
template<class T> long detour_thunk(REL::RelocationID) {
    ++calls; T::func.value = result == NO_ERROR ? 1 : 0; return result;
}
}
namespace Hooks { bool InstallBeforeInputPoll(); }
PRODUCTION
int main() {
    assert(!Hooks::InstallBeforeInputPoll());
    assert(stl::calls == 1 && !BSInputDeviceManager_BeforeInputPoll::func.address());
    stl::result = NO_ERROR;
    assert(Hooks::InstallBeforeInputPoll());
    assert(Hooks::InstallBeforeInputPoll());
    assert(stl::calls == 2);
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", installer))

    def test_startup_and_disabled_path_boundaries(self):
        hooks = (ROOT / "src/Hooks.cpp").read_text()
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text()
        upscaling = (ROOT / "src/Features/Upscaling.cpp").read_text()
        self.assertNotIn("BSInputDeviceManager_BeforeInputPoll", braced(hooks, "void Install()"))
        self.assertNotIn("InstallBeforeInputPoll", braced(hooks, "void Install()"))
        for declaration in ("void DX12SwapChain::CreateD3D12Device(", "void DX12SwapChain::CreateSwapChainDirect("):
            self.assertNotIn("AntiLag2", braced(source, declaration))
        fsr = braced(source, "void DX12SwapChain::CreateSwapChain(")
        self.assertLess(fsr.index("CreateD3D12Device(adapter);"), fsr.index("InitializeAntiLag2();"))
        interop = braced(source, "void DX12SwapChain::CreateInterop()")
        self.assertIn("if (antiLag2Context && !Hooks::InstallBeforeInputPoll())", interop)
        self.assertIn("if (antiLag2Context) {\n\t\tswapChainProxy = new AntiLag2SwapChainProxy", interop)
        self.assertIn("PresentImpl<false>", braced(source, "HRESULT DX12SwapChain::Present("))
        self.assertIn("PresentImpl<true>", braced(source, "HRESULT DX12SwapChain::PresentWithAntiLag2("))
        self.assertNotIn("AntiLag2", braced(source, "HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present("))
        self.assertNotIn("antiLag2Enabled", upscaling)
        self.assertNotIn('FEATURE_COMMAND("setAntiLag2"', upscaling)
        self.assertNotIn("ShouldUseFrameGeneration", braced(upscaling, "void Upscaling::OnBeforeInputPoll()"))
        self.assertNotIn("settings", braced(upscaling, "void Upscaling::OnBeforeInputPoll()"))

    def test_submission_and_shutdown_order(self):
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text()
        present = braced(source, "HRESULT DX12SwapChain::PresentImpl(")
        self.assertIn("if constexpr (WithAntiLag2)\n\t\t\tPublishAntiLag2SwapChainData();", present)
        self.assertIn("if constexpr (WithAntiLag2)\n\t\tMarkAntiLag2EndOfFrame();", present)
        self.assertLess(present.index("PublishAntiLag2SwapChainData();"), present.index("upscaling.fidelityFX.Present("))
        self.assertLess(present.index("ExecuteCommandLists("), present.index("MarkAntiLag2EndOfFrame();"))
        self.assertLess(present.index("MarkAntiLag2EndOfFrame();"), present.index("swapChain->Present("))
        shutdown = braced(source, "DX12SwapChain::~DX12SwapChain()")
        self.assertLess(shutdown.index("ffx::Dispatch("), shutdown.index("PublishAntiLag2SwapChainData();"))
        self.assertLess(shutdown.index("PublishAntiLag2SwapChainData();"), shutdown.index("DeInitialize("))
        self.assertIn("antiLag2Context.release()", shutdown)


LIFECYCLE_FIXTURE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
using HRESULT = int;
using UINT = unsigned;
#define STDMETHODCALLTYPE
constexpr HRESULT S_OK = 0;
bool FAILED(HRESULT hr) { return hr < 0; }
namespace logger {
unsigned warnings = 0, infos = 0;
template<class... Args> void info(const char*, Args...) { ++infos; }
template<class... Args> void warn(const char*, Args...) { ++warnings; }
}
namespace AMD::AntiLag2DX12 {
struct Context { void* m_pAntiLagAPI = nullptr; bool m_enabled = false; unsigned m_maxFPS = 0; };
int initializeResult = 0;
bool partialInterface = false;
unsigned initializations = 0, releases = 0;
HRESULT Initialize(Context* context, void*) {
    ++initializations;
    if (initializeResult == S_OK || partialInterface) context->m_pAntiLagAPI = context;
    return initializeResult;
}
void DeInitialize(Context* context) {
    assert(context && context->m_pAntiLagAPI);
    ++releases; context->m_pAntiLagAPI = nullptr; context->m_enabled = false;
}
}
namespace CS::AntiLag2 {
int result = 0; unsigned updates = 0;
HRESULT Update(AMD::AntiLag2DX12::Context* context, bool enabled) {
    assert(context && context->m_pAntiLagAPI && enabled);
    ++updates;
    if (result >= 0) context->m_enabled = enabled;
    return result;
}
}
namespace ffx {
struct DispatchDescFrameGenerationSwapChainWaitForPresentsDX12 {};
enum class ReturnCode { Ok, Error };
bool drainSucceeds = true; unsigned drains = 0;
ReturnCode Dispatch(void*, DispatchDescFrameGenerationSwapChainWaitForPresentsDX12) {
    ++drains; return drainSucceeds ? ReturnCode::Ok : ReturnCode::Error;
}
}
struct { bool Dispatch = true; } ffxModule;
namespace Hooks {
unsigned calls = 0; bool succeeds = true, resourcesReady = false;
bool InstallBeforeInputPoll() { assert(resourcesReady); ++calls; return succeeds; }
}
struct DXGISwapChainProxy {
    explicit DXGISwapChainProxy(void*) {}
    virtual ~DXGISwapChainProxy() = default;
    virtual HRESULT Present(UINT, UINT) { return 1; }
};
struct DX12SwapChain {
    struct Device { void* get() { return this; } } d3d12Device, d3d11Device;
    struct Fence { void Create(void*, void*, const char*) {} } interopFence;
    void* swapChain = this;
    unsigned swapChainDesc = 0, publications = 0;
    std::unique_ptr<AMD::AntiLag2DX12::Context> antiLag2Context;
    bool antiLag2Enabled = false, antiLag2UpdateFailureLogged = false;
    DXGISwapChainProxy* swapChainProxy = nullptr;
    ~DX12SwapChain();
    void InitializeAntiLag2();
    void CreateInterop();
    void UpdateAntiLag2BeforeInput();
    void RecreateWrappedResources(unsigned) { Hooks::resourcesReady = true; }
    void PublishAntiLag2SwapChainData() { assert(!antiLag2Enabled); ++publications; }
    HRESULT PresentWithAntiLag2(UINT, UINT) { return 2; }
};
namespace globals::features {
struct Upscaling {
    struct { void* swapChainContext = this; } fidelityFX;
    DX12SwapChain dx12SwapChain;
} upscaling;
}
PROXY
PRODUCTION
int main() {
    using namespace AMD::AntiLag2DX12;
    for (bool fsr : {false, true})
        for (int availability : {-1, 0})
            for (bool partial : {false, true})
                for (bool hookSuccess : {false, true}) {
                    initializations = releases = 0; ffx::drains = 0;
                    Hooks::calls = 0; Hooks::succeeds = hookSuccess; Hooks::resourcesReady = false;
                    initializeResult = availability; partialInterface = partial;
                    const bool supported = fsr && availability == S_OK;
                    const bool installed = supported && hookSuccess;
                    DXGISwapChainProxy* proxy;
                    {
                        DX12SwapChain chain;
                        if (fsr) chain.InitializeAntiLag2();
                        chain.CreateInterop();
                        proxy = chain.swapChainProxy;
                        assert(initializations == unsigned(fsr));
                        assert(Hooks::calls == unsigned(supported));
                        assert(bool(chain.antiLag2Context) == installed);
                        assert(proxy->Present(0, 0) == (installed ? 2 : 1));
                        assert(releases == unsigned((fsr && availability != S_OK && partial) || (supported && !hookSuccess)));
                        if (installed) {
                            CS::AntiLag2::updates = 0; logger::warnings = 0;
                            for (int result : {-1, -1, 0}) {
                                CS::AntiLag2::result = result;
                                chain.UpdateAntiLag2BeforeInput();
                                assert(chain.antiLag2Enabled == (result >= 0));
                            }
                            assert(CS::AntiLag2::updates == 3 && logger::warnings == 1);
                        }
                    }
                    delete proxy;
                    assert(ffx::drains == unsigned(installed));
                    assert(releases == unsigned(fsr && (availability == S_OK || partial)));
                }
    for (bool dispatchAvailable : {false, true}) {
        auto* retained = new Context{reinterpret_cast<void*>(1), true};
        const auto previousReleases = releases;
        ffxModule.Dispatch = dispatchAvailable; ffx::drainSucceeds = false;
        {
            DX12SwapChain chain;
            chain.antiLag2Context.reset(retained);
        }
        assert(releases == previousReleases);
        assert(retained->m_pAntiLagAPI && retained->m_enabled);
        delete retained;
    }
    ffx::drainSucceeds = true; ffxModule.Dispatch = true;
}
'''


if __name__ == "__main__":
    unittest.main()
