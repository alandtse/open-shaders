using System.Security.Cryptography;
using System.Text.Json;
using ICSharpCode.SharpZipLib.Checksum;
using NiflySharp;
using NiflySharp.Blocks;

internal sealed record PgShapeReference(int BlockIndex, string Name, string Type,
    uint ShaderFlags1, uint ShaderFlags2, float Alpha, string[] Textures);

internal sealed record PgMeshReference(string Path, uint ExpectedCrc32, long Size,
    long LastWriteUtcTicks, string Status, string? Sha256, PgShapeReference[] Shapes);

internal sealed record PgReferenceDocument(string Kind, int SchemaVersion, string SourceDiffSha256,
    int MeshCount, int ShapeCount, int CacheHits, int Unresolved, PgMeshReference[] Meshes);

internal static class PgReferenceIndex
{
    private const int MaxMeshes = 200_000;
    private const long MaxDiffBytes = 64L * 1024 * 1024;
    private const long MaxOutputBytes = 256L * 1024 * 1024;
    private const long MaxMeshBytes = 256L * 1024 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    internal static int Run(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("Usage: DX12ResourcesPrep --pg-references <Data directory> <previous-index.json> <work-index.json>");
            return 2;
        }

        try
        {
            var data = Path.GetFullPath(args[0]);
            var previous = Path.GetFullPath(args[1]);
            var output = Path.GetFullPath(args[2]);
            var cacheRoot = Path.GetFullPath(Path.Combine(data, "SKSE", "Plugins", "CommunityShaders", "DX12-cache"));
            var expectedPrevious = Path.Combine(cacheRoot, "pg-references", "index-v1.json");
            var workRoot = Path.Combine(cacheRoot, "work", "pg-references");
            if (!previous.Equals(expectedPrevious, StringComparison.OrdinalIgnoreCase) ||
                !output.StartsWith(workRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                !output.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("PG index paths must use the dedicated DX12-cache locations.");
            for (DirectoryInfo? directory = new(Path.GetDirectoryName(output)!);
                 directory != null && directory.FullName.StartsWith(data, StringComparison.OrdinalIgnoreCase);
                 directory = directory.Parent)
                if (directory.Exists && directory.Attributes.HasFlag(FileAttributes.ReparsePoint))
                    throw new IOException("PG index work path contains a reparse point.");

            var diffPath = Path.Combine(data, "ParallaxGen_Diff.json");
            var diffInfo = new FileInfo(diffPath);
            if (!diffInfo.Exists || diffInfo.Length == 0 || diffInfo.Length > MaxDiffBytes)
                throw new InvalidDataException("Deployed ParallaxGen_Diff.json is missing or oversized.");
            var diffBytes = File.ReadAllBytes(diffPath);
            var diffHash = Convert.ToHexString(SHA256.HashData(diffBytes)).ToLowerInvariant();
            var previousMeshes = LoadPrevious(previous);
            using var diff = JsonDocument.Parse(diffBytes);
            if (diff.RootElement.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException("PG diff must contain patched asset records.");

            var paths = new SortedDictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in diff.RootElement.EnumerateObject())
            {
                var path = PgAssets.Normalize(entry.Name);
                if (!path.StartsWith("meshes\\", StringComparison.Ordinal) ||
                    !path.EndsWith(".nif", StringComparison.Ordinal)) continue;
                if (entry.Value.ValueKind != JsonValueKind.Object ||
                    !entry.Value.TryGetProperty("crc32patched", out var crc) ||
                    !crc.TryGetUInt32(out var expectedCrc)) continue;
                if (!paths.TryAdd(path, expectedCrc))
                    throw new InvalidDataException("PG diff contains duplicate normalized mesh paths.");
                if (paths.Count > MaxMeshes)
                    throw new InvalidDataException("PG reference count exceeds the supported limit.");
            }

            var entries = paths.ToArray();
            var meshes = new PgMeshReference[entries.Length];
            var reused = new bool[entries.Length];
            var parallelism = Math.Clamp(Environment.ProcessorCount / 2, 1, 4);
            Parallel.For(0, entries.Length, new ParallelOptions { MaxDegreeOfParallelism = parallelism }, index =>
            {
                var (path, expectedCrc) = entries[index];
                var fullPath = Path.GetFullPath(Path.Combine(data, path));
                if (!fullPath.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("PG mesh path escaped Data.");
                var info = new FileInfo(fullPath);
                if (!info.Exists)
                {
                    meshes[index] = new(path, expectedCrc, 0, 0, "missing", null, []);
                    return;
                }
                var ticks = info.LastWriteTimeUtc.Ticks;
                if (info.Length > MaxMeshBytes)
                {
                    meshes[index] = new(path, expectedCrc, info.Length, ticks, "oversized-nif", null, []);
                    return;
                }
                if (previousMeshes.TryGetValue(path, out var old) && old.ExpectedCrc32 == expectedCrc &&
                    old.Size == info.Length && old.LastWriteUtcTicks == ticks)
                {
                    meshes[index] = old;
                    reused[index] = true;
                    return;
                }
                try
                {
                    var bytes = File.ReadAllBytes(fullPath);
                    var crc = new Crc32();
                    crc.Update(bytes);
                    if ((uint)crc.Value != expectedCrc)
                    {
                        meshes[index] = new(path, expectedCrc, info.Length, ticks, "crc-mismatch", null, []);
                        return;
                    }
                    var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
                    var nif = new NifFile();
                    if (nif.Load(fullPath) != 0 || !nif.Valid || !nif.Header.Version.IsSSE() || nif.HasUnknownBlocks)
                    {
                        meshes[index] = new(path, expectedCrc, info.Length, ticks, "unsupported-nif", hash, []);
                        return;
                    }
                    var shapes = new List<PgShapeReference>();
                    foreach (var shape in nif.GetShapes())
                    {
                        nif.GetBlockIndex(shape, out var index);
                        if (nif.GetShader(shape) is not BSLightingShaderProperty shader) continue;
                        var textures = nif.GetBlock(shader.TextureSetRef)?.Textures?
                            .Select(texture => texture.Content ?? "").ToArray() ?? [];
                        if (index < 0 || !float.IsFinite(shader.Alpha) ||
                            textures.Length > 16 || textures.Any(texture => texture.Length > 1024)) continue;
                        shapes.Add(new(index, shape.Name?.String ?? "", shape.GetType().Name,
                            (uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2,
                            shader.Alpha, textures));
                    }
                    meshes[index] = new(path, expectedCrc, info.Length, ticks, "verified", hash, shapes.ToArray());
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    meshes[index] = new(path, expectedCrc, info.Length, ticks, "unreadable:" + error.GetType().Name, null, []);
                }
            });

            var cacheHits = reused.Count(hit => hit);
            var document = new PgReferenceDocument("dx12-pg-references", 1, diffHash,
                meshes.Length, meshes.Sum(mesh => mesh.Shapes.Length), cacheHits,
                meshes.Count(mesh => mesh.Status != "verified"), meshes);
            var payload = JsonSerializer.SerializeToUtf8Bytes(document, JsonOptions);
            if (payload.Length > MaxOutputBytes)
                throw new InvalidDataException("PG reference index exceeds the 256 MiB budget.");
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            var temp = output + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllBytes(temp, payload);
                File.Move(temp, output, true);
            }
            finally
            {
                if (File.Exists(temp)) File.Delete(temp);
            }
            Console.WriteLine($"PG references: {document.MeshCount} meshes, {document.ShapeCount} shapes; " +
                $"{cacheHits} unchanged, {document.Unresolved} unresolved.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("PG reference index failed: " + error);
            return 1;
        }
    }

    private static Dictionary<string, PgMeshReference> LoadPrevious(string path)
    {
        try
        {
            if (!File.Exists(path) || new FileInfo(path).Length > MaxOutputBytes)
                return new(StringComparer.OrdinalIgnoreCase);
            var old = JsonSerializer.Deserialize<PgReferenceDocument>(File.ReadAllText(path), JsonOptions);
            if (old?.Kind != "dx12-pg-references" || old.SchemaVersion != 1 ||
                old.MeshCount != old.Meshes.Length || old.MeshCount > MaxMeshes)
                return new(StringComparer.OrdinalIgnoreCase);
            return old.Meshes.Where(mesh => mesh.Status == "verified" && mesh.Sha256?.Length == 64)
                .ToDictionary(mesh => mesh.Path, mesh => mesh, StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            return new(StringComparer.OrdinalIgnoreCase);
        }
    }
}
