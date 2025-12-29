# Test Fixtures

YAML-based test data following [LLVM's format](https://github.com/llvm/offload-test-suite) with minimal extension for RenderDoc captures.

## LLVM Standard (Preferred)

```yaml
Buffers:
    - Name: TestData
      Format: Float32
      Data: [0.1, 0.2, 0.3] # Inline data

    - Name: ZeroBuffer
      Format: Float32
      FillSize: 1024 # Filled buffer
      FillValue: 0.0
```

## Extension: Binary Files (RenderDoc Captures)

For large real-game captures (>10KB):

```yaml
Buffers:
    - Name: CapturedDepth
      Format: Float32
      DataFile: data/renderdoc/depth_1920x1080.bin
```

**Create binary files:**

-   **RenderDoc**: Export texture → "Save As Raw"
-   **Python**: `np.array(...).astype(np.float32).tofile('data.bin')`
-   **C++**: `std::ofstream("data.bin", std::ios::binary).write(...)`

## Usage

See [parent README](../README.md) for full testing guide.

**Files:**

-   `common_fixtures.yaml` - Small inline test data (LLVM standard)
-   `renderdoc_captures.yaml` - Binary file references (our extension)
-   `data/` - Binary files (Git LFS for >10MB)
