using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

// Geometry preparation copies original NIF inputs; native material evaluation stays at runtime.
internal sealed record CompiledMaterial(bool TwoSided, string[] Textures, uint ShaderFlags1, uint ShaderFlags2,
    float Alpha, uint TextureClampMode, float[]? UvOffsetScale);
internal sealed record CompiledShape(int BlockIndex, string Name, string BlockType, string Status, string? GeometryFile,
    string? GeometrySha256, int VertexCount, int IndexCount, float[]? ShapeToModel, float[]? LocalBounds, CompiledMaterial? Material,
    bool OpaqueOccluderCandidate = false, string ShadingStatus = "not-compiled", string? ShadingFile = null, string? ShadingSha256 = null,
    string TangentStatus = "absent:shading");
internal sealed record CompiledNif(string SourcePath, string SourceSha256, string CompilerKey, string Status, bool CacheHit,
    int EmbeddedLights, CompiledShape[] Shapes);

internal sealed class MeshCompiler
{
    private readonly string output;
    private readonly JsonSerializerOptions options;
    private readonly string compilerKey;
    private readonly int quality;
    private readonly string sourceRoot;
    private readonly SharedMeshCache? sharedCache;
    // SHA-check each immutable content-addressed blob once per compilation run.
    private readonly HashSet<string> verifiedCachedFiles = new(StringComparer.OrdinalIgnoreCase);

    public MeshCompiler(string outputDirectory, JsonSerializerOptions jsonOptions, int preparationQuality = 2,
        SharedMeshCache? sharedCache = null)
    {
        if (preparationQuality is < 0 or > 2) throw new ArgumentOutOfRangeException(nameof(preparationQuality));
        // Quality 2 used to activate texture-driven displacement. It is now a
        // compatibility alias for quality 1: original mesh topology plus shading.
        quality = preparationQuality == 2 ? 1 : preparationQuality;
        sourceRoot = outputDirectory;
        output = outputDirectory;
        options = jsonOptions;
        this.sharedCache = sharedCache;
        // Mesh products are governed by a semantic cache schema, not the .NET
        // assembly hash: unrelated CLI and UI edits must not invalidate every NIF.
        // New cache identity rejects any older height-expanded geometry products.
        const string meshSchema = "dx12-resources-rigid-v3-native-material-source/Nifly-1.1.0";
        compilerKey = Convert.ToHexString(SHA256.HashData(
            Encoding.UTF8.GetBytes(meshSchema + "/quality=" + quality))).ToLowerInvariant();
    }

    public CompiledNif Compile(PgAsset asset)
    {
        if (asset.CachedSource == null || asset.Sha256 == null) return new(asset.Path, "", compilerKey, asset.Status, false, 0, []);
        var cacheFile = Path.Combine(output, "compiled", compilerKey, asset.Sha256 + ".json");
        if (!File.Exists(cacheFile))
            sharedCache?.TryRestore(compilerKey, asset.Sha256);
        if (File.Exists(cacheFile))
        {
            try
            {
                var cached = JsonSerializer.Deserialize<CompiledNif>(File.ReadAllText(cacheFile), options);
                if (cached != null && cached.CompilerKey == compilerKey && cached.SourceSha256 == asset.Sha256 && cached.Shapes != null &&
                    cached.Shapes.All(shape => shape != null && (shape.GeometryFile == null ? shape.Status != "compiled" && !shape.Status.StartsWith("preview-only:", StringComparison.Ordinal) : VerifyGeometry(shape)) && VerifyShading(shape)))
                    return cached with { SourcePath = asset.Path, CacheHit = true };
            }
            catch (JsonException) { }
        }
        CompiledNif result;
        try
        {
            var source = Path.Combine(sourceRoot, asset.CachedSource);
            if (!Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(source))).Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Cached NIF source hash mismatch.");
            var file = new NifFile();
            if (file.Load(source) != 0 || !file.Valid) throw new InvalidDataException("NIF reader rejected file.");
            if (!file.Header.Version.IsSSE()) throw new InvalidDataException($"Unsupported NIF version: {file.Header.Version.VersionString}, user {file.Header.Version.UserVersion}, stream {file.Header.Version.StreamVersion}.");
            if (file.HasUnknownBlocks) throw new InvalidDataException("Unknown NIF blocks prevent classification.");
            var transforms = new NifTransforms(file);
            var shapes = file.GetShapes().Select(shape => CompileShape(file, shape, transforms)).ToArray();
            result = new(asset.Path, asset.Sha256, compilerKey, "classified", false, file.Blocks.OfType<NiLight>().Count(), shapes);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            result = new(asset.Path, asset.Sha256, compilerKey, "excluded:" + error.Message, false, 0, []);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(cacheFile)!);
        File.WriteAllText(cacheFile + ".tmp", JsonSerializer.Serialize(result, options));
        File.Move(cacheFile + ".tmp", cacheFile, true);
        sharedCache?.TryStore(result);
        return result;
    }

    private bool VerifyGeometry(CompiledShape shape)
    {
        if (shape.GeometrySha256 == null || shape.GeometrySha256.Length != 64 || !shape.GeometrySha256.All(Uri.IsHexDigit) ||
            shape.GeometryFile != $"geometry/{shape.GeometrySha256}.rsgm") return false;
        var path = Path.Combine(output, shape.GeometryFile);
        return VerifyCachedPayload(path, shape.GeometrySha256);
    }

    private bool VerifyShading(CompiledShape shape)
    {
        if (shape.ShadingStatus != "compiled") return shape.ShadingFile == null && shape.ShadingSha256 == null;
        var hash = shape.ShadingSha256;
        if (hash == null || hash.Length != 64 || !hash.All(Uri.IsHexDigit) || shape.ShadingFile != $"shading/{hash}.rsga") return false;
        var path = Path.Combine(output, shape.ShadingFile);
        return VerifyCachedPayload(path, hash);
    }

    private bool VerifyCachedPayload(string path, string expectedHash)
    {
        if (verifiedCachedFiles.Contains(path)) return true;
        if (!File.Exists(path)) return false;
        using var input = File.OpenRead(path);
        if (!Convert.ToHexString(SHA256.HashData(input)).Equals(expectedHash, StringComparison.OrdinalIgnoreCase))
            return false;
        verifiedCachedFiles.Add(path);
        return true;
    }

    private CompiledShape CompileShape(NifFile file, INiShape shape, NifTransforms transforms)
    {
        file.GetBlockIndex(shape, out var index);
        var name = shape.Name?.String ?? "";
        var type = shape.GetType().Name;
        var transform = transforms.ForShape(index);
        var shader = file.GetShader(shape) as BSLightingShaderProperty;
        var alpha = file.GetBlock(shape.AlphaPropertyRef);
        CompiledMaterial? material = null;
        if (shader != null)
        {
            var textures = file.GetBlock(shader.TextureSetRef)?.Textures?.Select(texture => texture.Content).ToArray() ?? [];
            var clampMode = ReadTextureClampMode(shader);
            if (clampMode.HasValue)
                material = new(shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.Double_Sided), textures,
                    (uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2, shader.Alpha,
                    clampMode.Value, ReadUvTransform(shader));
        }
        var reason = shape is not BSTriShape || type is not ("BSTriShape" or "BSSubIndexTriShape") ? "unsupported-shape-type" :
            shape.HasSkinInstance || shape.IsSkinned ? "skinned-shape" : transform.Exclusion;
        if (reason == null && shader == null) reason = "non-lighting-material";
        if (reason == null && shader!.Controller != null && !shader.Controller.IsEmpty()) reason = "controlled-material";
        if (reason == null && (alpha?.Flags.AlphaBlend == true || alpha?.Flags.AlphaTest == true)) reason = "alpha-test-or-blend";
        if (reason == null && (!float.IsFinite(shader!.Alpha) || shader.Alpha != 1)) reason = "non-opaque-material";
        if (reason == null && (shader!.HasVertexAlpha || shader.HasGreyscaleToPaletteAlpha)) reason = "vertex-or-palette-alpha";
        if (reason == null && (shader!.ShaderFlags_SSPF1 & (SkyrimShaderPropertyFlags1.Refraction | SkyrimShaderPropertyFlags1.Temp_Refraction | SkyrimShaderPropertyFlags1.Fire_Refraction)) != 0)
            reason = "refractive-material";
        if (reason == null && (shader!.ShaderFlags_SSPF2 & (SkyrimShaderPropertyFlags2.Tree_Anim | SkyrimShaderPropertyFlags2.LOD_Landscape)) != 0)
            reason = "deformed-or-landscape-material";
        if (reason == null && (!shader!.ShaderFlags_SSPF1.HasFlag(SkyrimShaderPropertyFlags1.Cast_Shadows) || !shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.ZBuffer_Write)))
            reason = "shadow-or-depth-flags";
        var materialController = shader == null ? null : file.GetBlock(shader.Controller);
        var previewOnly = reason == "alpha-test-or-blend" ||
            (reason == "vertex-or-palette-alpha" && !shader!.HasGreyscaleToPaletteAlpha) ||
            (reason == "controlled-material" && materialController is BSLightingShaderPropertyColorController or BSLightingShaderPropertyFloatController);
        if (reason != null && !previewOnly) return new(index, name, type, "excluded:" + reason, null, null, 0, 0, transform.Matrix, null, material);
        try
        {
            var vertices = ((BSTriShape)shape).VertexPositions;
            var triangles = shape.Triangles;
            var blob = GeometryBlob.Write(output, vertices, triangles);
            var shading = quality == 0 ? new ShadingAttributes("absent:quality") : ShadingBlob.Write(output, (BSTriShape)shape);
            // Preserve source NIF topology: never subdivide or displace vertices from texture data.
            return new(index, name, type, previewOnly ? "preview-only:" + reason : "compiled",
                blob.Path, blob.Sha256, vertices.Count, checked(triangles.Count * 3), transform.Matrix,
                blob.Bounds, quality == 0 ? null : material, !previewOnly, shading.Status, shading.File, shading.Sha256, shading.TangentStatus);
        }
        catch (InvalidDataException error)
        {
            return new(index, name, type, "excluded:" + error.Message, null, null, 0, 0, transform.Matrix, null, material);
        }
    }

    private static uint? ReadTextureClampMode(BSLightingShaderProperty shader)
    {
        var type = shader.GetType();
        // Nifly 1.1.0 generates the clamp field but has no public accessor.
        var value = type.GetProperty("TextureClampMode")?.GetValue(shader) ??
            type.GetField("_textureClampMode", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(shader);
        if (value is not IConvertible number) return null;
        var mode = number.ToInt64(null);
        return mode is >= 0 and <= 3 ? (uint)mode : null;
    }

    private static float[]? ReadUvTransform(BSLightingShaderProperty shader)
    {
        static float? Component(object value, string first, string second)
        {
            var type = value.GetType();
            var member = type.GetProperty(first) ?? type.GetProperty(second);
            var field = type.GetField(first) ?? type.GetField(second);
            var component = member?.GetValue(value) ?? field?.GetValue(value);
            return component is IConvertible number ? Convert.ToSingle(number) : null;
        }
        var type = shader.GetType();
        var offset = (type.GetProperty("UVOffset") ?? type.GetProperty("UvOffset"))?.GetValue(shader);
        var scale = (type.GetProperty("UVScale") ?? type.GetProperty("UvScale"))?.GetValue(shader);
        if (offset == null || scale == null) return null;
        var ox = Component(offset, "X", "U");
        var oy = Component(offset, "Y", "V");
        var sx = Component(scale, "X", "U");
        var sy = Component(scale, "Y", "V");
        return ox.HasValue && oy.HasValue && sx.HasValue && sy.HasValue &&
            float.IsFinite(ox.Value) && float.IsFinite(oy.Value) &&
            float.IsFinite(sx.Value) && float.IsFinite(sy.Value) ?
            [ox.Value, oy.Value, sx.Value, sy.Value] : null;
    }
}
