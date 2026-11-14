using System;
using System.IO;
using System.Numerics;
using BCnEncoder.Decoder;

// Experimental offline normal-map slope integration.
// Generates a bounded micro-height residual without changing source DDS.
internal static class DxrNormalQuick
{
    internal sealed class Field
    {
        private readonly float[] data;
        private readonly int size;

        public Field(float[] samples, int gridSize)
        {
            data = samples;
            size = gridSize;
        }

        private int Wrap(int x) => ((x % size) + size) % size;

        private float At(int x, int y)
            => data[Wrap(y) * size + Wrap(x)];

        public float Sample(Vector2 uv)
        {
            float u = uv.X - MathF.Floor(uv.X);
            float v = uv.Y - MathF.Floor(uv.Y);

            float x = u * size - 0.5f;
            float y = v * size - 0.5f;

            int ix = (int)MathF.Floor(x);
            int iy = (int)MathF.Floor(y);
            float fx = x - ix;
            float fy = y - iy;

            float a = At(ix, iy);
            float b = At(ix + 1, iy);
            float c = At(ix, iy + 1);
            float d = At(ix + 1, iy + 1);

            return (a * (1 - fx) + b * fx) * (1 - fy)
                 + (c * (1 - fx) + d * fx) * fy;
        }
    }

    private static int GridSize()
    {
        return int.TryParse(
            Environment.GetEnvironmentVariable("DX12_RESOURCES_DXR_NORMAL_GRID"),
            out int size
        ) ? Math.Clamp(size, 32, 256) : 128;
    }

    public static Field Load(string filename)
    {
        using var fs = File.OpenRead(filename);
        var decoded = new BcDecoder().Decode2D(fs);

        int width = decoded.Width;
        int height = decoded.Height;

        if (width < 1 || height < 1 ||
            (long)width * height > 16_777_216)
            throw new InvalidDataException("Invalid normal DDS dimensions");

        int n = GridSize();
        int count = n * n;
        bool flipY = Environment.GetEnvironmentVariable(
            "DX12_RESOURCES_DXR_NORMAL_FLIP_Y") == "1";

        var gx = new float[count];
        var gy = new float[count];
        float slopeEnergy = 0;

        // Decode XY normal components on a periodic UV grid.
        // Assumes R/G tangent-space normals, appropriate for BC5
        // and ordinary RGB tangent-space maps.
        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            int sx = Math.Min(width - 1, (int)((x + .5f) * width / n));
            int sy = Math.Min(height - 1, (int)((y + .5f) * height / n));

            var color = decoded.Span[sy, sx];

            float nx = color.r / 255f * 2f - 1f;
            float ny = color.g / 255f * 2f - 1f;
            if (flipY) ny = -ny;

            float xy2 = nx * nx + ny * ny;
            if (xy2 > .96f)
            {
                float adjust = MathF.Sqrt(.96f / xy2);
                nx *= adjust;
                ny *= adjust;
                xy2 = nx * nx + ny * ny;
            }

            float nz = MathF.Sqrt(MathF.Max(.04f, 1f - xy2));

            int i = y * n + x;
            gx[i] = Math.Clamp(-nx / nz, -3f, 3f);
            gy[i] = Math.Clamp(-ny / nz, -3f, 3f);

            slopeEnergy += gx[i] * gx[i] + gy[i] * gy[i];
        }

        // Skip almost-flat normal maps and quantization artifacts.
        if (slopeEnergy / count < .0025f)
            return new Field(new float[count], n);

        // Periodic divergence of the gradient field.
        var divergence = new float[count];

        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            int i = y * n + x;
            int left = y * n + (x + n - 1) % n;
            int up = ((y + n - 1) % n) * n + x;

            divergence[i] =
                gx[i] - gx[left] +
                gy[i] - gy[up];
        }

        // Solve the discrete Poisson equation approximately.
        // The periodic solution is defined up to an additive constant.
        var current = new float[count];
        var next = new float[count];

        for (int iteration = 0; iteration < 96; ++iteration)
        {
            for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                int i = y * n + x;
                int l = y * n + (x + n - 1) % n;
                int r = y * n + (x + 1) % n;
                int u = ((y + n - 1) % n) * n + x;
                int d = ((y + 1) % n) * n + x;

                next[i] = .25f * (
                    current[l] + current[r] +
                    current[u] + current[d] -
                    divergence[i]);
            }

            (current, next) = (next, current);
        }

        // High-pass filtering: do not reconstruct large-scale
        // surface shape that the original height map already owns.
        int radius = Math.Max(2, n / 32);
        int kernel = radius * 2 + 1;

        var blurX = new float[count];
        var blur = new float[count];

        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += current[y * n + (x + k + n) % n];

            blurX[y * n + x] = sum / kernel;
        }

        for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += blurX[((y + k + n) % n) * n + x];

            blur[y * n + x] = sum / kernel;
        }

        var residual = new float[count];
        var magnitudes = new float[count];

        for (int i = 0; i < count; ++i)
        {
            residual[i] = current[i] - blur[i];
            magnitudes[i] = MathF.Abs(residual[i]);
        }

        // Robust normalization instead of allowing a few extreme
        // texels to determine the full displacement range.
        Array.Sort(magnitudes);
        float p95 = magnitudes[(int)((count - 1) * .95f)];

        if (p95 <= 1e-4f)
            return new Field(new float[count], n);

        for (int i = 0; i < count; ++i)
            residual[i] = Math.Clamp(
                residual[i] / (2f * p95), -1f, 1f);

        Console.WriteLine(
            $"[DXR normal] integrated {Path.GetFileName(filename)} " +
            $"{width}x{height} -> {n}x{n} micro-height");

        return new Field(residual, n);
    }
}