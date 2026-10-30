"""CPU fault-regression tests for the native NR queue retirement methods.

The test compiles the production End/Drain/Wait/ParallelList method bodies against
small queue/fence mocks. No D3D runtime or GPU is involved.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/Features/Upscaling/NeuralRendering/D3D12Interop.cpp"


def braced(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"Unbalanced C++ declaration: {declaration}")


CPP_FIXTURE = r'''#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

template <class... T> std::string formatForFixture(const char*, T&&...) { return "mocked GPU fence failure"; }

using HRESULT = int;
using DWORD = unsigned long;
using UINT = unsigned int;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
constexpr bool FAILED(HRESULT hr) { return hr < 0; }
#define D3D12_COMMAND_LIST_TYPE_DIRECT 0
#define D3D12_COMMAND_LIST_TYPE_COMPUTE 1
#define IID_PPV_ARGS(x) 0, reinterpret_cast<void**>(x)
namespace DX { inline void ThrowIfFailed(HRESULT result) { if (FAILED(result)) throw std::runtime_error("HRESULT failure"); } }

namespace winrt {
struct hresult_error { std::string messageText; explicit hresult_error(std::string value) : messageText(std::move(value)) {} std::string message() const { return messageText; } };
inline std::string to_string(const std::string& value) { return value; }
inline void check_hresult(HRESULT result) { if (FAILED(result)) throw std::runtime_error("HRESULT failure"); }
template <class T> class com_ptr {
public:
    com_ptr() = default;
    com_ptr(std::nullptr_t) {}
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;
    com_ptr(com_ptr&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    com_ptr& operator=(com_ptr&& other) noexcept {
        if (this != &other) { delete value; value = std::exchange(other.value, nullptr); }
        return *this;
    }
    ~com_ptr() { delete value; }
    T* get() const { return value; }
    T* operator->() const { return value; }
    T** put() { delete value; value = new T(); return &value; }
    com_ptr& operator=(std::nullptr_t) { delete value; value = nullptr; return *this; }
    explicit operator bool() const { return value != nullptr; }
    void set(T* item) { delete value; value = item; }
private:
    T* value = nullptr;
};
}

enum class Fault { None, ParallelSignal, MainJoinWait, MainSignal };
enum class SetupFailure { None, Winrt, Standard };
Fault fault = Fault::None;
bool pauseParallel = false;
SetupFailure setupFailure = SetupFailure::None;

struct SharedFence {
    enum class WaitOutcome { kComplete, kFailed, kTimeout };
    static constexpr unsigned kFenceTimeoutMs = 5;
    struct Token { SharedFence* owner = nullptr; } token12, token11;
    struct Handle {
        Token* value = nullptr;
        Token* get() const { return value; }
        explicit operator bool() const { return value != nullptr; }
        Handle& operator=(std::nullptr_t) { value = nullptr; return *this; }
        Handle& operator=(Token* token) { value = token; return *this; }
    } fence12{&token12}, fence11{&token11};
    bool isParallel = false;
    uint64_t value = 0;
    uint64_t completed = 0;
    SharedFence() { token12.owner = token11.owner = this; }
    uint64_t Next() { return ++value; }
    void Reset() { value = completed = 0; fence12 = nullptr; fence11 = nullptr; }
    void Create(void*, void*, const char*) {
        if (setupFailure == SetupFailure::Winrt) throw winrt::hresult_error("injected HRESULT fence failure");
        if (setupFailure == SetupFailure::Standard) throw std::runtime_error("injected standard fence failure");
        fence12 = &token12;
        fence11 = &token11;
    }
    WaitOutcome CpuWaitOutcome(uint64_t target, unsigned, DWORD*) const {
        return completed >= target ? WaitOutcome::kComplete : WaitOutcome::kTimeout;
    }
};

struct ID3D12CommandList { virtual ~ID3D12CommandList() = default; };
struct D3D12_COMMAND_QUEUE_DESC { int Type = 0; };
struct ID3D12GraphicsCommandList : ID3D12CommandList {
    bool closed = true;
    int Close() { closed = true; return S_OK; }
    int Reset(void*, void*) { closed = false; return S_OK; }
};
struct CommandAllocator { bool wasReset = false; int Reset() { wasReset = true; return S_OK; } };
struct Commands {
    winrt::com_ptr<CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    uint64_t completion = 0;
    bool allocatorWasReset() const { return allocator && allocator->wasReset; }
};

struct Queue {
    bool parallel = false;
    std::vector<std::function<bool()>> operations;
    void Run() {
        if (parallel && pauseParallel) return;
        while (!operations.empty()) {
            if (!operations.front()()) break;
            operations.erase(operations.begin());
        }
    }
    void ExecuteCommandLists(UINT, ID3D12CommandList* const*) {
        operations.emplace_back([] { return true; });
        Run();
    }
    int Signal(SharedFence::Token*, uint64_t value);
    int Wait(SharedFence::Token*, uint64_t value);
    int SetName(const wchar_t* name) {
        if (std::wstring(name).find(L"Parallel") != std::wstring::npos) parallel = true;
        return S_OK;
    }
};
struct Queue;
struct FakeDevice {
    int GetDeviceRemovedReason() const { return S_OK; }
    int CreateCommandQueue(void*, int, void** out);
};
struct Context {
    SharedFence* mainFence = nullptr;
    int Signal(SharedFence::Token*, uint64_t value) { mainFence->completed = std::max(mainFence->completed, value); return S_OK; }
    void Flush() {}
    int Wait(SharedFence::Token*, uint64_t) { return S_OK; }
};
namespace logger { template<class... T> void warn(const char*, T&&...) {} }

struct D3D12Interop {
    static constexpr uint32_t kFramesInFlight = 3;
    winrt::com_ptr<FakeDevice> device;
    winrt::com_ptr<FakeDevice> device11;
    winrt::com_ptr<Queue> queue, parallelQueue;
    std::array<Commands, kFramesInFlight> commands, parallelCommands;
    SharedFence fence, parallelFence;
    winrt::com_ptr<Context> context;
    uint64_t inputReady = 0;
    bool parallelUsed = false, parallelSubmitted = false, parallelFailed = false;
    uint32_t cursor = 0;

    bool DeviceRemoved() const { return false; }
    void Check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("device HRESULT failure"); }
    void CreateRing(std::array<Commands, kFramesInFlight>& ring, int, const wchar_t*, const wchar_t*) {
        for (auto& slot : ring) {
            slot.allocator.set(new CommandAllocator());
            slot.list.set(new ID3D12GraphicsCommandList());
        }
    }
    void Wait(uint64_t completion);
    void Wait(SharedFence& target, uint64_t completion);
    ID3D12GraphicsCommandList* Begin();
    void End();
    ID3D12GraphicsCommandList* ParallelList();
    void Drain();
};

int Queue::Signal(SharedFence::Token* token, uint64_t value) {
    const bool inject = parallel && fault == Fault::ParallelSignal;
    if (!parallel && fault == Fault::MainSignal) {
        fault = Fault::None;
        return E_FAIL;
    }
    if (inject) {
        fault = Fault::None;
        return E_FAIL;
    }
    auto* fencePointer = token->owner;
    operations.emplace_back([fencePointer, value] { fencePointer->completed = value; return true; });
    Run();
    return S_OK;
}
int FakeDevice::CreateCommandQueue(void*, int, void** out) {
    *reinterpret_cast<Queue**>(out) = new Queue();
    return S_OK;
}
int Queue::Wait(SharedFence::Token* token, uint64_t value) {
    if (!parallel && fault == Fault::MainJoinWait) {
        fault = Fault::None;
        return E_FAIL;
    }
    auto* fencePointer = token->owner;
    operations.emplace_back([fencePointer, value] { return fencePointer->completed >= value; });
    Run();
    return S_OK;
}

// ACTUAL_METHODS

void initialize(D3D12Interop& interop) {
    interop.device.set(new FakeDevice());
    interop.queue.set(new Queue());
    interop.parallelQueue.set(new Queue());
    interop.parallelQueue->parallel = true;
    auto* ctx = new Context();
    ctx->mainFence = &interop.fence;
    interop.context.set(ctx);
    interop.CreateRing(interop.commands, 0, L"", L"");
    interop.CreateRing(interop.parallelCommands, 1, L"", L"");
    interop.parallelFence.isParallel = true;
}
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void submitAndDrainForFault(Fault injected) {
    fault = injected;
    pauseParallel = true;
    D3D12Interop interop;
    initialize(interop);
    interop.parallelUsed = true;
    bool endThrew = false;
    try { interop.End(); } catch (...) { endThrew = true; }
    check(endThrew, "injected End failure must throw");
    bool drainThrew = false;
    try { interop.Drain(); } catch (...) { drainThrew = true; }
    const bool parallelPending = !interop.parallelQueue->operations.empty();
    check(drainThrew || !parallelPending, "Drain must not report retirement while parallel queue is pending");
    check(drainThrew ? parallelPending : true, "A failed drain must leave the pending parallel submission owned");
    check(interop.fence.completed > 0, "D3D11/main fence may progress while the parallel queue is paused");
    check(!interop.queue->operations.empty(), "main queue remains gated at the parallel join");
    const bool mayReleaseAfterFirstDrain = !drainThrew;
    check(!mayReleaseAfterFirstDrain, "a throwing Drain retains caller ownership of referenced resources");
    pauseParallel = false;
    interop.parallelQueue->Run();
    interop.queue->Run();
    interop.Drain();
    const bool mayReleaseAfterRetry = true; // Drain returned after both queue timelines retired.
    check(mayReleaseAfterRetry && interop.parallelQueue->operations.empty(), "retry Drain retires parallel work before release");
}
int main() {
    try {
        for (auto injected : {Fault::ParallelSignal, Fault::MainJoinWait, Fault::MainSignal})
            submitAndDrainForFault(injected);

        // Successful dual-queue retirement waits both timelines.
        fault = Fault::None; pauseParallel = false;
        { D3D12Interop interop; initialize(interop); interop.parallelUsed = true; interop.End(); interop.Drain();
          check(interop.parallelFence.completed >= 1, "successful parallel work retires on its own fence"); }
        // The one-queue path retains its previous retirement behavior.
        { D3D12Interop interop; initialize(interop); interop.parallelQueue = nullptr; interop.parallelUsed = false;
          interop.End(); interop.Drain(); check(interop.fence.completed > 0, "single-queue work retires"); }
        // A queue that was used earlier remains part of Drain even when the current End did not use it.
        pauseParallel = true;
        { D3D12Interop interop; initialize(interop); interop.parallelQueue->operations.emplace_back([] { return true; });
          bool threw = false; try { interop.Drain(); } catch (...) { threw = true; }
          check(threw, "Drain includes previously queued parallel work when parallelUsed is false"); }
        // A setup failure tears down a partially created ring/fence and selects the one-queue path.
        setupFailure = SetupFailure::Winrt;
        { D3D12Interop interop; initialize(interop); interop.parallelQueue = nullptr; interop.parallelFailed = false;
          const auto result = interop.ParallelList();
          check(result == nullptr && interop.parallelQueue.get() == nullptr, "parallel setup failure falls back cleanly");
          check(interop.parallelFailed && interop.parallelFence.fence12.get() == nullptr, "failed setup fence is reset and parallel mode disabled"); }
        setupFailure = SetupFailure::Standard;
        { D3D12Interop interop; initialize(interop); interop.parallelQueue = nullptr; interop.parallelFailed = false;
          const auto result = interop.ParallelList();
          check(result == nullptr && interop.parallelQueue.get() == nullptr, "standard parallel setup failure falls back cleanly");
          check(interop.parallelFailed && interop.parallelFence.fence12.get() == nullptr, "standard setup failure resets partial state"); }
        setupFailure = SetupFailure::None;
        // Allocator reuse is guarded by the parallel fence, independent of main-fence progress.
        { D3D12Interop interop; initialize(interop); interop.parallelCommands[0].completion = 1;
          interop.parallelFence.completed = 0; interop.fence.completed = 100;
          bool threw = false; try { interop.ParallelList(); } catch (...) { threw = true; }
          check(threw && !interop.parallelCommands[0].allocatorWasReset(), "parallel allocator reuse waits on its own fence"); }
        std::puts("PASS: queue retirement fault, fallback, and reuse cases");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected exception: %s\n", error.what());
        return 2;
    }
}
'''


def build_fixture(source):
    methods = "\n".join(
        braced(source, declaration)
        for declaration in (
            "void D3D12Interop::Wait(SharedFence& target, uint64_t completion)",
            "ID3D12GraphicsCommandList* D3D12Interop::ParallelList()",
            "void D3D12Interop::End()",
            "void D3D12Interop::Drain()",
        )
    )
    methods = methods.replace("std::format(", "formatForFixture(")
    return CPP_FIXTURE.replace("// ACTUAL_METHODS", methods)


def compiler_command(cpp, exe):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if compiler:
        return [compiler, "-std=c++20", str(cpp), "-o", str(exe)]
    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.exists():
            installation = subprocess.check_output(
                [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                text=True,
            ).strip()
            if not installation:
                raise unittest.SkipTest("MSVC C++ compiler unavailable")
            vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
            return f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++20 "{cpp}" /Fe:"{exe}"'
    raise unittest.SkipTest("Native C++ compiler unavailable")


class NativeInteropRetirementTests(unittest.TestCase):
    def compile_run(self):
        source = build_fixture(SOURCE.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="nr-interop-retirement-") as temp:
            temp = Path(temp)
            cpp = temp / "interop.cpp"
            exe = temp / ("interop.exe" if os.name == "nt" else "interop")
            cpp.write_text(source, encoding="utf-8")
            command = compiler_command(cpp, exe)
            built = subprocess.run(command, shell=isinstance(command, str),
                                   cwd=temp, text=True, capture_output=True, timeout=120)
            if built.returncode:
                self.fail(f"native interop harness did not compile:\n{built.stdout}\n{built.stderr}")
            ran = subprocess.run([str(exe)], cwd=temp, text=True, capture_output=True, timeout=15)
            return ran

    def test_current_methods_preserve_pending_resources_and_retire_successfully(self):
        run = self.compile_run()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS: queue retirement", run.stdout)


if __name__ == "__main__":
    unittest.main()
