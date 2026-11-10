using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Reflection;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

internal sealed record CompiledMaterial(uint ShaderFlags1, uint ShaderFlags2, string ShaderType, bool TwoSided,
    float Alpha, uint? AlphaFlags, string[] Textures, float[] UvOffsetScale,
    byte? AlphaThreshold, uint TextureClampMode, float[] EmissiveColor, float EmissiveMultiple,
    float[] SpecularColor, float SpecularStrength, float Glossiness, float EnvironmentMapScale,
    string Interpretation = "raw-patched-nif-parameters; material evaluation and reference overrides unresolved");
internal sealed record CompiledShape(int BlockIndex, string Name, string BlockType, string Status, string? GeometryFile,
    string? GeometrySha256, int VertexCount, int IndexCount, float[]? ShapeToModel, float[]? LocalBounds, CompiledMaterial? Material,
    bool OpaqueOccluderCandidate = false, string ShadingStatus = "not-compiled", string? ShadingFile = null, string? ShadingSha256 = null,
    string TangentStatus = "absent:shading");
internal sealed record CompiledNif(string SourcePath, string SourceSha256, string CompilerKey, string Status, bool CacheHit,
    int EmbeddedLights, CompiledShape[] Shapes);

internal sealed class MeshCompiler
{
    private static readonly FieldInfo TextureClampField = typeof(BSLightingShaderProperty).GetField("_textureClampMode", BindingFlags.Instance | BindingFlags.NonPublic)
        ?? throw new InvalidOperationException("Pinned Nifly texture clamp field is unavailable.");
    private readonly string output;
    private readonly JsonSerializerOptions options;
    private readonly string compilerKey;
    private readonly int quality;
    private readonly string sourceRoot;
    private readonly bool displacementInputsEnabled;
    // SHA-check each immutable content-addressed blob once per compilation run.
    private readonly HashSet<string> verifiedCachedFiles = new(StringComparer.OrdinalIgnoreCase);

    public MeshCompiler(string outputDirectory, JsonSerializerOptions jsonOptions, int preparationQuality = 2)
    {
        if (preparationQuality is < 0 or > 2) throw new ArgumentOutOfRangeException(nameof(preparationQuality));
        quality = preparationQuality;
        displacementInputsEnabled = quality == 2 &&
            ((int.TryParse(Environment.GetEnvironmentVariable("DX12_RESOURCES_DXR_HEIGHT_ENABLED"), out var h) && h != 0) ||
             (int.TryParse(Environment.GetEnvironmentVariable("DX12_RESOURCES_DXR_NORMAL_ENABLED"), out var n) && n != 0));
        sourceRoot = outputDirectory;
        output = outputDirectory;
        options = jsonOptions;
        // Mesh products are governed by a semantic cache schema, not the .NET
        // assembly hash: unrelated CLI and UI edits must not invalidate every NIF.
        // Bump this when shape classification/serialization/displacement changes.
        const string meshSchema = "dx12-resources-rigid-v1/Nifly-1.1.0";
        var variant = quality == 2
            ? string.Join("/", new[] {
                "DX12_RESOURCES_DXR_HEIGHT_ENABLED", "DX12_RESOURCES_DXR_HEIGHT_SCALE", "DX12_RESOURCES_DXR_AO_SCALE",
                "DX12_RESOURCES_DXR_SUBDIV", "DX12_RESOURCES_DXR_TRI_CAP", "DX12_RESOURCES_DXR_HEIGHT_REF",
                "DX12_RESOURCES_DXR_HEIGHT_TARGET", "DX12_RESOURCES_DXR_FLIP_V", "DX12_RESOURCES_DXR_NORMAL_ENABLED",
                "DX12_RESOURCES_DXR_NORMAL_MICRO_SCALE", "DX12_RESOURCES_DXR_NORMAL_GRID", "DX12_RESOURCES_DXR_NORMAL_FLIP_Y"
            }.Select(k => Environment.GetEnvironmentVariable(k) ?? ""))
            : "no-displacement";
        compilerKey = Convert.ToHexString(SHA256.HashData(
            Encoding.UTF8.GetBytes(meshSchema + "/quality=" + quality + "/" + variant))).ToLowerInvariant();
    }

    public CompiledNif Compile(PgAsset asset)
    {
        if (asset.CachedSource == null || asset.Sha256 == null) return new(asset.Path, "", compilerKey, asset.Status, false, 0, []);
        var cacheFile = Path.Combine(output, "compiled", compilerKey, asset.Sha256 + ".json");
        // External height/normal DDS can change without modifying the NIF.
        // Until their hashes are part of the cache key, never reuse potentially stale displaced geometry.
        if (!displacementInputsEnabled && File.Exists(cacheFile))
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
            material = new((uint)shader.ShaderFlags_SSPF1, (uint)shader.ShaderFlags_SSPF2, shader.ShaderType_SK_FO4.ToString(),
                shader.ShaderFlags_SSPF2.HasFlag(SkyrimShaderPropertyFlags2.Double_Sided), shader.Alpha,
                alpha == null ? null : alpha.Flags.Value, textures, [shader.UVOffset.U, shader.UVOffset.V, shader.UVScale.U, shader.UVScale.V],
                alpha?.Threshold, Convert.ToUInt32(TextureClampField.GetValue(shader)),
                [shader.EmissiveColor.R, shader.EmissiveColor.G, shader.EmissiveColor.B], shader.EmissiveMultiple,
                [shader.SpecularColor.R, shader.SpecularColor.G, shader.SpecularColor.B], shader.SpecularStrength, shader.Glossiness, shader.EnvironmentMapScale);
            var scalars = new[] { material.Alpha, material.EmissiveMultiple, material.SpecularStrength, material.Glossiness, material.EnvironmentMapScale };
            if (!scalars.Concat(material.EmissiveColor).Concat(material.SpecularColor).Concat(material.UvOffsetScale).All(float.IsFinite))
                throw new InvalidDataException("Non-finite patched material parameters.");
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
            var quick = quality == 2
                ? DxrHeightQuick.Apply(output, material, blob, shading, vertices.Count, checked(triangles.Count * 3))
                : new DxrHeightQuick.Result(blob.Path, blob.Sha256, blob.Bounds, vertices.Count,
                    checked(triangles.Count * 3), shading.Status, shading.File, shading.Sha256, shading.TangentStatus);
            return new(index, name, type, previewOnly ? "preview-only:" + reason : "compiled",
                quick.GeometryFile, quick.GeometrySha, quick.VertexCount, quick.IndexCount, transform.Matrix,
                quick.Bounds, quality == 0 ? null : material, !previewOnly, quick.ShadingStatus, quick.ShadingFile, quick.ShadingSha, quick.TangentStatus);
        }
        catch (InvalidDataException error)
        {
            return new(index, name, type, "excluded:" + error.Message, null, null, 0, 0, transform.Matrix, null, material);
        }
    }
}
