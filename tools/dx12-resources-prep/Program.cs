using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Environments;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Skyrim;

return args.Length == 5 && args[0] == "--list-cells" ? PluginCellIndex.Run(args[1..]) :
    args.Length == 5 ? ScenePreparation.Run(args[..4], int.Parse(args[4], System.Globalization.CultureInfo.InvariantCulture)) :
    ScenePreparation.Run(args);

internal static class ScenePreparation
{
    internal static string LastFailure = "";

    internal static int Run(string[] args, int quality = 2)
    {
if (args.Length != 4)
{
    Console.Error.WriteLine("Usage: DX12ResourcesPrep <Data directory> <plugin:local-cell-id> <output directory> <deployed Plugins.txt>");
    return 2;
}

try
{
    LastFailure = "";
    if (quality is < 0 or > 2) throw new ArgumentOutOfRangeException(nameof(quality));
    var data = Path.GetFullPath(args[0]);
    var output = Path.GetFullPath(args[2]);
    if (output.Equals(data, StringComparison.OrdinalIgnoreCase) || output.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
        throw new ArgumentException("Output must be outside the game Data directory.");
    var parts = args[1].Split(':');
    if (parts.Length != 2) throw new ArgumentException("Cell identity must be plugin:hex-local-id.");
    var cellKey = new FormKey(ModKey.FromFileName(parts[0]), Convert.ToUInt32(parts[1], 16));
    var pluginsPath = Path.GetFullPath(args[3]);
    string[] officialMasters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];
    var creationClubPath = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
    var creationClub = File.Exists(creationClubPath) ? File.ReadLines(creationClubPath).Select(line => line.Trim()).Where(line => line.Length > 0) : [];
    var implicitListings = officialMasters.Concat(creationClub).Where(name => File.Exists(Path.Combine(data, name))).Select(name => ModKey.FromFileName(name));
    var listings = implicitListings.Concat(PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
        .Where(listing => listing.Enabled).Select(listing => listing.ModKey)).Distinct().ToArray();
    using var environment = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>(GameRelease.SkyrimSE)
        .WithTargetDataFolder(data).WithLoadOrder(listings).Build();
    Console.WriteLine($"Loaded {environment.LoadOrder.Count} plugin listings; locating {cellKey}.");
    var cellSources = new Dictionary<FormKey, List<string>>();
    var cells = new Dictionary<FormKey, ICellGetter>();
    var references = new Dictionary<FormKey, IPlacedGetter>();
    foreach (var listing in environment.LoadOrder.ListedOrder)
    {
        if (listing.Mod is not { } mod) continue;
        foreach (var cell in mod.EnumerateMajorRecords<ICellGetter>())
        {
            if (!cellSources.TryGetValue(cell.FormKey, out var sources)) cellSources[cell.FormKey] = sources = [];
            sources.Add(mod.ModKey.ToString());
            cells[cell.FormKey] = cell;
            if (cell.FormKey != cellKey) continue;
            foreach (var reference in cell.Persistent.Concat(cell.Temporary)) references[reference.FormKey] = reference;
        }
    }
    if (!cells.TryGetValue(cellKey, out var target)) throw new InvalidDataException("Requested cell was not found in the effective load order.");
    Console.WriteLine($"Indexed {cells.Count} cells; target has {references.Count} candidate references.");
    var rows = new List<object>();
    var paths = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
    var staticPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    var lightCount = 0;
    var staticCount = 0;
    var furnitureCount = 0;
    var unresolvedBases = 0;
    foreach (var candidate in references.Values.OrderBy(r => r.FormKey.ToString(), StringComparer.Ordinal))
    {
        var context = environment.LinkCache.ResolveContext<IPlaced, IPlacedGetter>(candidate.FormKey);
        if (!context.TryGetParent<ICellGetter>(out var containingCell) || containingCell.FormKey != cellKey) continue;
        var reference = context.Record;
        var obj = reference as IPlacedObjectGetter;
        var baseRecord = obj?.Base.TryResolve(environment.LinkCache);
        string? baseSource = null;
        if (obj != null && obj.Base.TryResolveSimpleContext(environment.LinkCache, out var baseContext)) baseSource = baseContext.ModKey.ToString();
        if (obj != null && baseRecord == null) ++unresolvedBases;
        if (baseRecord is ILightGetter) ++lightCount;
        if (baseRecord is IStaticGetter) ++staticCount;
        if (baseRecord is IFurnitureGetter) ++furnitureCount;
        var model = (baseRecord as IModeledGetter)?.Model?.File.ToString();
        var hasBaseScripts = baseRecord is IFurnitureGetter furniture && furniture.VirtualMachineAdapter != null;
        if (!string.IsNullOrWhiteSpace(model)) paths.Add(model.Replace('/', '\\'));
        var classification = reference.IsDeleted ? "deleted" : (reference.MajorRecordFlagsRaw & 0x800) != 0 ? "initially-disabled" :
            obj?.EnableParent != null || obj?.VirtualMachineAdapter != null || hasBaseScripts ? "conditional-runtime-state" :
            baseRecord is IStaticGetter ? "static-record-nif-uninspected" :
            baseRecord is IFurnitureGetter ? "furniture-record-nif-uninspected" :
            baseRecord is IMoveableStaticGetter ? "moveable-static-record-nif-uninspected" : baseRecord is ILightGetter ? "placed-light-baseline" : "outside-rigid-static-scope";
        if (classification is "static-record-nif-uninspected" or "furniture-record-nif-uninspected" or "moveable-static-record-nif-uninspected" && !string.IsNullOrWhiteSpace(model))
        {
            var normalized = PgAssets.Normalize(model);
            staticPaths.Add(normalized.StartsWith("meshes\\", StringComparison.Ordinal) ? normalized : "meshes\\" + normalized);
        }
        rows.Add(new
        {
            key = reference.FormKey.ToString(), baseKey = obj?.Base.FormKey.ToString(), resolvedBase = baseRecord != null,
            referenceSource = context.ModKey.ToString(), baseSource,
            editorId = reference.EditorID, baseEditorId = baseRecord?.EditorID, baseType = baseRecord?.GetType().Name,
            classification,
            model, position = reference.Placement is { } placement ? new[] { placement.Position.X, placement.Position.Y, placement.Position.Z } : null,
            rotationRadians = reference.Placement is { } transform ? new[] { transform.Rotation.X, transform.Rotation.Y, transform.Rotation.Z } : null,
            scale = obj?.Scale ?? 1f, flags = reference.MajorRecordFlagsRaw,
            roomReferenceKey = obj?.MultiBoundReference.FormKeyNullable is { } roomKey && roomKey != FormKey.Null ? roomKey.ToString() : null,
            roomBounds = obj?.BoundHalfExtents is { } roomBounds ? new[] { roomBounds.X, roomBounds.Y, roomBounds.Z } : null,
            hasEnableParent = obj?.EnableParent != null, hasScripts = obj?.VirtualMachineAdapter != null, hasBaseScripts,
            hasReferenceLightOverrides = obj?.LightData != null,
            light = baseRecord is ILightGetter light ? new
            {
                referenceKey = reference.FormKey.ToString(), baseKey = obj?.Base.FormKey.ToString(),
                valueInterpretation = "raw-plugin-record-values; no color/radiometric conversion or reference-override application",
                radius = light.Radius, color = new int[] { light.Color.R, light.Color.G, light.Color.B },
                flags = light.Flags.ToString(), falloffExponent = light.FalloffExponent, fov = light.FOV,
                nearClip = light.NearClip, fadeValue = light.FadeValue,
                flickerPeriod = light.FlickerPeriod, flickerIntensityAmplitude = light.FlickerIntensityAmplitude,
                flickerMovementAmplitude = light.FlickerMovementAmplitude,
                referenceOverrides = obj?.LightData is { } lightData ? new
                {
                    fovOffset = lightData.FovOffset, fadeOffset = lightData.FadeOffset,
                    endDistanceCap = lightData.EndDistanceCap, shadowDepthBias = lightData.ShadowDepthBias,
                    versioning = lightData.Versioning.ToString(), unknown = lightData.Unknown
                } : null
            } : null
        });
    }
    Directory.CreateDirectory(output);
    var options = new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    var pgAssets = new List<PgAsset>();
    using (var pg = new PgAssets(data, output))
        foreach (var path in paths) pgAssets.Add(pg.Read(path));
    var compiler = new MeshCompiler(output, options, quality);
    var compiledAssets = pgAssets.Where(asset => staticPaths.Contains(asset.Path)).Select(compiler.Compile).ToArray();
    var compiledShapes = compiledAssets.SelectMany(asset => asset.Shapes).Where(shape => shape.Status == "compiled").ToArray();
    File.WriteAllText(Path.Combine(output, "cell-meshes.json"), JsonSerializer.Serialize(new
    {
        schemaVersion = 1, reader = "Nifly/1.1.0", assets = compiledAssets,
        placementMatricesComposed = false, runtimeCoverageValid = false
    }, options));
    var pluginSources = listings.Select(key => new
    {
        plugin = key.ToString(), sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(data, key.ToString())))).ToLowerInvariant()
    }).ToArray();
    var inputDigest = Convert.ToHexString(SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(new
    {
        schemaVersion = 1, pluginSources, cell = target.FormKey.ToString(),
        pgDiffSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(data, "ParallaxGen_Diff.json")))),
        assets = pgAssets.Select(asset => new { asset.Path, asset.Status, asset.Sha256 })
    }))).ToLowerInvariant();
    var layout = CellLayout.Write(output, target.FormKey.ToString(), target.EditorID, inputDigest, rows, compiledAssets, options);
    File.WriteAllText(Path.Combine(output, "cell-records.json"), JsonSerializer.Serialize(new
    {
        schemaVersion = 1, tool = "DX12ResourcesPrep", recordReader = "Mutagen.Bethesda.Skyrim/0.54.4",
        cellKey = target.FormKey.ToString(), editorId = target.EditorID,
        inputDigest, pluginsPath, pluginSources, cellSources = cellSources[cellKey], references = rows, models = paths,
        geometryReady = false, runtimeCoverageValid = false
    }, options));
    File.WriteAllText(Path.Combine(output, "cell-index.json"), JsonSerializer.Serialize(cells.OrderBy(c => c.Key.ToString()).Select(c => new
    {
        key = c.Key.ToString(), editorId = c.Value.EditorID, flags = c.Value.Flags.ToString(),
        sources = cellSources[c.Key], winner = cellSources[c.Key][^1]
    }), options));
    File.WriteAllText(Path.Combine(output, "cell-preparation.json"), JsonSerializer.Serialize(new
    {
        schemaVersion = 1, formatContract = "dx12-resources-cell", formatVersion = 2,
        inputDigest, cell = target.FormKey.ToString(), editorId = target.EditorID,
        assetProvider = "PGPatcher deployed output; no conflict re-resolution", references = rows.Count, staticRecordCandidates = staticCount,
        furnitureRecordCandidates = furnitureCount, placedLights = lightCount, unresolvedBases, assets = pgAssets,
        sourceCacheHits = pgAssets.Count(asset => asset.CacheHit),
        pgVerifiedSources = pgAssets.Count(asset => asset.Status == "pg-patched-crc-verified"),
        pgGeneratedSources = pgAssets.Count(asset => asset.Status == "pg-generated-deployment-linked"),
        unresolvedAssets = pgAssets.Count(asset => asset.CachedSource == null),
        compiledAssets = compiledAssets.Length, compiledCacheHits = compiledAssets.Count(asset => asset.CacheHit),
        compiledShapes = compiledShapes.Length, compiledTriangles = compiledShapes.Sum(shape => (long)shape.IndexCount / 3),
        previewOnlyShapes = compiledAssets.SelectMany(asset => asset.Shapes).Count(shape => shape.Status.StartsWith("preview-only:", StringComparison.Ordinal)),
        placedReferences = layout.PlacedReferences, placedShapeInstances = layout.Instances, placedTriangles = layout.Triangles,
        layoutDigest = layout.Digest, placementMatricesComposed = true,
        stage = "cell-packed-partial", geometryCompiled = compiledShapes.Length > 0, accelerationBuilt = false, runtimeCoverageValid = false,
        outstanding = new[] { "Runtime placement/state comparison", "Excluded mesh/material coverage", "Shader coordinator and DX12 integration" }
    }, options));
    Console.WriteLine($"Prepared {rows.Count} references, {staticCount} static-record candidates and {lightCount} placed lights.");
    Console.WriteLine($"PG inputs: {pgAssets.Count(asset => asset.CachedSource != null)} cached / {pgAssets.Count} requested; unresolved bases: {unresolvedBases}.");
    Console.WriteLine($"Compiled {compiledShapes.Length} shapes / {compiledShapes.Sum(shape => (long)shape.IndexCount / 3)} triangles; {compiledAssets.Count(asset => asset.CacheHit)} mesh cache hits.");
    Console.WriteLine($"Placed {layout.PlacedReferences} references / {layout.Instances} shape instances / {layout.Triangles} triangles; packed geometry + shading.");
    return 0;
}
catch (Exception error)
{
    LastFailure = error.ToString();
    Console.Error.WriteLine(error);
    return 1;
}

    }
}
