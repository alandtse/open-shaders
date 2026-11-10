using System.Security.Cryptography;
using System.Text.Json;

/// <summary>
/// Versioned spatial *index* over an existing prepared CELL. This file does
/// not claim a subcell is GPU-ready or that on-demand compilation is active.
/// Both runtime and companion use floor division aligned to world origin (0,0).
/// </summary>
internal static class SubcellIndex
{
    internal const int MinimumSize = 2048;
    internal const int DefaultSize = 4096;
    internal const int MaximumSize = 16384;
    internal const int Step = 1024;
    internal const int SchemaVersion = 1;
    private const int MaximumColumnsPerInstance = 4096;

    internal static bool ValidSize(int size) =>
        size is >= MinimumSize and <= MaximumSize && size % Step == 0;

    internal static long Coordinate(double value, int size)
    {
        if (!ValidSize(size) || !double.IsFinite(value))
            throw new InvalidDataException("Invalid subcell coordinate or size.");
        var cell = Math.Floor(value / size);
        if (cell < -1_000_000_000 || cell > 1_000_000_000)
            throw new InvalidDataException("Subcell coordinate out of supported range.");
        return checked((long)cell);
    }

    private sealed class Membership
    {
        internal readonly List<int> Instances = [];
        internal readonly List<int> Lights = [];
    }

    internal static void Write(string directory, string scope, string layoutDigest, int size,
        IReadOnlyList<PlacedShape> instances, IReadOnlyList<JsonElement> lights, JsonSerializerOptions options)
    {
        if (!ValidSize(size))
            throw new ArgumentOutOfRangeException(nameof(size), "Size must be 2048..16384 in increments of 1024.");
        var subcells = new SortedDictionary<(long X, long Y), Membership>();
        Membership Get(long x, long y)
        {
            var key = (x, y);
            if (!subcells.TryGetValue(key, out var membership))
                subcells.Add(key, membership = new Membership());
            return membership;
        }

        for (int index = 0; index < instances.Count; ++index)
        {
            var bounds = instances[index].CellBounds;
            if (bounds.Length != 6 || bounds.Any(value => !float.IsFinite(value)) ||
                bounds[0] > bounds[3] || bounds[1] > bounds[4])
                throw new InvalidDataException("Invalid instance bounds in prepared subcell index.");
            var minX = Coordinate(bounds[0], size);
            var minY = Coordinate(bounds[1], size);
            // Use half-open bounds when the maximum lies exactly on a grid
            // boundary. Zero-area bounds belong to their one containing cell.
            var maxX = Coordinate(bounds[3] > bounds[0] ?
                Math.BitDecrement((double)bounds[3]) : bounds[3], size);
            var maxY = Coordinate(bounds[4] > bounds[1] ?
                Math.BitDecrement((double)bounds[4]) : bounds[4], size);
            var width = checked(maxX - minX + 1);
            var height = checked(maxY - minY + 1);
            if (width <= 0 || height <= 0 || width > MaximumColumnsPerInstance ||
                height > MaximumColumnsPerInstance / width)
                throw new InvalidDataException("One instance overlaps an excessive number of subcells.");
            for (long y = minY; y <= maxY; ++y)
                for (long x = minX; x <= maxX; ++x)
                    Get(x, y).Instances.Add(index);
        }

        for (int index = 0; index < lights.Count; ++index)
        {
            var row = lights[index];
            if (!row.TryGetProperty("position", out var position) ||
                position.ValueKind != JsonValueKind.Array || position.GetArrayLength() != 3)
                continue;
            var x = position[0].GetDouble();
            var y = position[1].GetDouble();
            if (double.IsFinite(x) && double.IsFinite(y))
                Get(Coordinate(x, size), Coordinate(y, size)).Lights.Add(index);
        }

        var entries = subcells.Select(entry =>
        {
            var (x, y) = entry.Key;
            var membership = entry.Value;
            // This digest is local to the subcell: changes in another column
            // do not invalidate unchanged instance/light content here.
            var payload = JsonSerializer.SerializeToUtf8Bytes(new
            {
                schemaVersion = SchemaVersion, scope, size, x, y,
                instances = membership.Instances.Select(index => instances[index]),
                lights = membership.Lights.Select(index => lights[index])
            }, options);
            var digest = Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
            return new
            {
                x, y, contentDigest = digest,
                instanceIndices = membership.Instances.ToArray(),
                lightIndices = membership.Lights.ToArray()
            };
        }).ToArray();

        var catalog = new
        {
            schemaVersion = SchemaVersion,
            kind = "dx12-native-spatial-subcell-index",
            geometrySource = "cell-layout.json",
            coordinateSpace = "Skyrim CELL local X/Y; fixed world-aligned origin zero; Z unpartitioned",
            scope, layoutDigest, subcellSize = size,
            minSize = MinimumSize, maxSize = MaximumSize, step = Step,
            cells = entries, indexedInstances = instances.Count, indexedLights = lights.Count,
            geometryCompiledPerSubcell = false, accelerationBuilt = false, runtimeCoverageValid = false
        };
        File.WriteAllText(Path.Combine(directory, "subcell-index.json"), JsonSerializer.Serialize(catalog, options));
    }
}
