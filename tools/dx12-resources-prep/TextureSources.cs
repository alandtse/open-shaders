using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Plugins;

internal sealed record TextureSource(string Path, string Status, string? Source, string? Sha256, string? File,
    uint Width = 0, uint Height = 0, uint MipCount = 0, uint FourCC = 0, uint? DxgiFormat = null,
    uint Caps2 = 0, uint ResourceDimension = 0, uint ArraySize = 1, uint MiscFlag = 0, uint Depth = 0);

internal static class TextureSources
{
    private const uint DdsMagic = 0x20534444;
    private const uint Dx10 = 0x30315844;
    private const uint MaximumDimension = 16384;
    private const long MaximumTextureBytes = 128L * 1024 * 1024;
    private const long MaximumCellBytes = 512L * 1024 * 1024;

    public static void Write(string data, string output, ModKey[] plugins, JsonSerializerOptions options)
    {
        using var layout = JsonDocument.Parse(File.ReadAllText(System.IO.Path.Combine(output, "cell-layout.json")));
        var materials = layout.RootElement.GetProperty("instances").EnumerateArray()
            .Where(row => row.GetProperty("material").ValueKind == JsonValueKind.Object)
            .Select(row => row.GetProperty("material").GetProperty("textures").EnumerateArray().Select(value => value.GetString()).ToArray()).ToArray();
        WriteSources(data, output, plugins, options, materials, layout.RootElement.GetProperty("cell").GetString()!,
            layout.RootElement.GetProperty("layoutDigest").GetString()!, "cell-textures.json", MaximumCellBytes);
    }

    private static void WriteSources(string data, string output, ModKey[] plugins, JsonSerializerOptions options,
        string?[][] materials, string identity, string digest, string manifestName, long byteBudget)
    {
        var referencedPaths = materials.SelectMany(textures => textures)
            .Where(path => !string.IsNullOrWhiteSpace(path))
            .Select(path => PgAssets.Normalize(path!)).Select(path => path.StartsWith("textures\\", StringComparison.Ordinal) ? path : "textures\\" + path)
            .Distinct(StringComparer.OrdinalIgnoreCase).Order().ToArray();
        var parallaxSources = materials.Where(textures => textures.Length > 0 && !string.IsNullOrWhiteSpace(textures[0]))
            .Select(textures => PgAssets.Normalize(textures[0]!))
            .Select(path => path.StartsWith("textures\\", StringComparison.Ordinal) ? path : "textures\\" + path)
            .Where(path => path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Select(path => new { diffuse = path, parallax = path[..^4] + "_p.dds" }).ToArray();
        var requiredPaths = referencedPaths.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var paths = referencedPaths.Concat(parallaxSources.Select(source => source.parallax))
            .Distinct(StringComparer.OrdinalIgnoreCase).Order().ToArray();
        var ini = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Skyrim Special Edition");
        var iniPaths = new[] { "Skyrim.ini", "SkyrimCustom.ini" }.Select(name => System.IO.Path.Combine(ini, name)).Where(File.Exists).ToArray();
        var settings = ArchiveSettings(iniPaths);
        var iniArchives = new[] { "sResourceArchiveList", "sResourceArchiveList2" }
            .SelectMany(key => settings.GetValueOrDefault(key, "").Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        var looseEnabled = settings.GetValueOrDefault("bInvalidateOlderFiles") == "1";
        var missing = paths.Where(path => !File.Exists(System.IO.Path.Combine(data, path))).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var archived = new Dictionary<string, List<(string Archive, IArchiveFile File)>>(StringComparer.OrdinalIgnoreCase);
        var archiveErrors = new List<object>();
        if (missing.Count > 0)
            foreach (var archive in Directory.EnumerateFiles(data, "*.bsa").Order(StringComparer.OrdinalIgnoreCase))
            {
                var name = System.IO.Path.GetFileName(archive);
                if (!iniArchives.Contains(name) && !plugins.Any(plugin => Archive.IsApplicable(GameRelease.SkyrimSE, plugin, name))) continue;
                try
                {
                    var reader = Archive.CreateReader(GameRelease.SkyrimSE, archive);
                    foreach (var file in reader.Files)
                    {
                        var path = PgAssets.Normalize(file.Path);
                        if (!missing.Contains(path)) continue;
                        if (!archived.TryGetValue(path, out var sources)) archived[path] = sources = [];
                        sources.Add((name, file));
                    }
                }
                catch (Exception error) when (error is not OutOfMemoryException) { archiveErrors.Add(new { archive = name, reason = error.Message }); }
            }
        var results = new List<TextureSource>();
        long total = 0;
        foreach (var path in paths)
        {
            if (!requiredPaths.Contains(path) && !File.Exists(System.IO.Path.Combine(data, path)) && !archived.ContainsKey(path)) continue;
            try
            {
                var full = System.IO.Path.Combine(data, path);
                byte[] bytes;
                string source;
                if (File.Exists(full))
                {
                    if (!looseEnabled) { results.Add(new(path, "unresolved:loose-precedence", null, null, null)); continue; }
                    if (new FileInfo(full).Length > MaximumTextureBytes) throw new InvalidDataException("Texture exceeds source byte limit.");
                    bytes = File.ReadAllBytes(full);
                    source = "loose:" + path;
                }
                else
                {
                    if (archiveErrors.Count > 0) { results.Add(new(path, "unresolved:archive-index-incomplete", null, null, null)); continue; }
                    if (!archived.TryGetValue(path, out var sources)) { results.Add(new(path, "unresolved:missing", null, null, null)); continue; }
                    if (sources.Count != 1) { results.Add(new(path, "unresolved:archive-precedence", string.Join(";", sources.Select(entry => entry.Archive)), null, null)); continue; }
                    if (sources[0].File.Size > MaximumTextureBytes) throw new InvalidDataException("Archive texture exceeds source byte limit.");
                    bytes = sources[0].File.GetBytes();
                    source = "archive:" + sources[0].Archive;
                }
                if (bytes.Length > MaximumTextureBytes || total > byteBudget - bytes.Length) throw new InvalidDataException("Texture source budget exceeded.");
                var item = Cache(output, path, source, bytes);
                results.Add(item);
                total += bytes.Length;
            }
            catch (Exception error) when (error is not OutOfMemoryException) { results.Add(new(path, "invalid:" + error.Message, null, null, null)); }
        }
        var manifest = new
        {
            schemaVersion = 1, cell = identity,
            layoutDigest = digest,
            runtimeCoverageValid = false, interpretation = "source DDS bytes only; map semantics and material readiness are separate",
            resolution = "explicit loose override or unique modeled active archive; ambiguous archive winners rejected",
            iniSources = iniPaths.Select(path => new { path, sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant() }),
            plugins = plugins.Select(plugin => plugin.ToString()), archiveErrors, textures = results, sourceBytes = total,
            parallaxSources = parallaxSources.Where(source => results.Any(texture => texture.Path == source.parallax && texture.Status == "cached")),
            textureDigest = Convert.ToHexString(SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(results, options))).ToLowerInvariant()
        };
        File.WriteAllText(System.IO.Path.Combine(output, manifestName + ".tmp"), JsonSerializer.Serialize(manifest, options));
        File.Move(System.IO.Path.Combine(output, manifestName + ".tmp"), System.IO.Path.Combine(output, manifestName), true);
        Console.WriteLine($"Texture sources: {results.Count(item => item.Status == "cached")} cached / {results.Count}; {total} source bytes.");
    }

    private static TextureSource Cache(string output, string path, string source, byte[] bytes)
    {
        if (bytes.Length < 128) throw new InvalidDataException("Truncated DDS header.");
        uint Read(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset, 4));
        if (Read(0) != DdsMagic || Read(4) != 124 || Read(76) != 32) throw new InvalidDataException("Invalid DDS header.");
        var width = Read(16);
        var height = Read(12);
        var mips = Math.Max(1u, Read(28));
        if (width == 0 || height == 0 || width > MaximumDimension || height > MaximumDimension || mips > 15)
            throw new InvalidDataException("Invalid DDS dimensions or mip count.");
        var fourCC = Read(84);
        uint? format = null;
        if (fourCC == Dx10)
        {
            if (bytes.Length < 148) throw new InvalidDataException("Truncated DX10 DDS header.");
            format = Read(128);
        }
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var relative = $"textures/{hash}.dds";
        var destination = System.IO.Path.Combine(output, relative);
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(destination)!);
        if (!File.Exists(destination) || !Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(destination))).Equals(hash, StringComparison.OrdinalIgnoreCase))
        {
            File.WriteAllBytes(destination + ".tmp", bytes);
            File.Move(destination + ".tmp", destination, true);
        }
        return new(path, "cached", source, hash, relative, width, height, mips, fourCC, format, Read(112),
            fourCC == Dx10 ? Read(132) : 0, fourCC == Dx10 ? Read(140) : 1, fourCC == Dx10 ? Read(136) : 0, Read(24));
    }

    private static Dictionary<string, string> ArchiveSettings(string[] iniPaths)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in iniPaths)
        {
            var archiveSection = false;
            foreach (var raw in File.ReadLines(path))
            {
                var line = raw.Split(';')[0].Trim();
                if (line.StartsWith('[')) { archiveSection = line.Equals("[Archive]", StringComparison.OrdinalIgnoreCase); continue; }
                var separator = line.IndexOf('=');
                if (archiveSection && separator > 0) result[line[..separator].Trim()] = line[(separator + 1)..].Trim();
            }
        }
        return result;
    }
}
