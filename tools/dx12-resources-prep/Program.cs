using System.Numerics;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Archives;
using Mutagen.Bethesda.Environments;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using NiflySharp;
using NiflySharp.Blocks;
using NiflySharp.Enums;

// One bounded, renderer-independent preparation request. The native host owns
// the temporary output file and removes it even when this process fails.
if (args.Length == 4 && args[0] == "--serve") return await ServeAsync(args);
if (args.Length != 10 || args[0] != "--tile") return 2;
try
{
    var data = Path.GetFullPath(args[1]);
    var plugins = Path.GetFullPath(args[2]);
    var scope = new FormKey(ModKey.FromFileName(args[3]), Convert.ToUInt32(args[4], 16));
    var x = long.Parse(args[5], System.Globalization.CultureInfo.InvariantCulture);
    var y = long.Parse(args[6], System.Globalization.CultureInfo.InvariantCulture);
    var size = int.Parse(args[7], System.Globalization.CultureInfo.InvariantCulture);
    var worldspace = args[8] == "world";
    var output = Path.GetFullPath(args[9]);
    if (size is not (4096 or 6144 or 8192 or 10240 or 12288 or 14336 or 16384) ||
        Math.Abs(x) > 1_000_000 || Math.Abs(y) > 1_000_000)
        return 2;
    // Runtime requests always identify their source form, never an arbitrary
    // plugin-controlled file path. Only a file in the native scratch directory
    // may be written; the host validates and owns the directory's lifetime.
    if (!File.Exists(Path.Combine(data, scope.ModKey.ToString())) ||
        !Directory.Exists(Path.GetDirectoryName(output))) return 2;
    var result = SceneTile.Prepare(data, plugins, scope, x, y, size, worldspace, Path.GetDirectoryName(output)!);
    await using var stream = new FileStream(output, FileMode.CreateNew, FileAccess.Write, FileShare.None);
    await JsonSerializer.SerializeAsync(stream, result, new JsonSerializerOptions
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    });
    return 0;
}
catch (Exception)
{
    // No persistent diagnostics, logs, or partial publication.
    return 1;
}

static async Task<int> ServeAsync(string[] arguments)
{
    // One isolated companion process owns the runtime's load-order context.
    // Only a small mailbox is written to its private temporary session directory.
    try
    {
        var data = Path.GetFullPath(arguments[1]);
        var plugins = Path.GetFullPath(arguments[2]);
        var sessionRoot = Path.GetFullPath(arguments[3]);
        if (!File.Exists(plugins) || !Directory.Exists(sessionRoot) ||
            !File.Exists(Path.Combine(data, "Skyrim.esm"))) return 2;
        var requestFile = Path.Combine(sessionRoot, "request.json");
        var replyFile = Path.Combine(sessionRoot, "reply.json");
        var replyTemp = Path.Combine(sessionRoot, "reply.tmp");
        var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
        string? cancellationFile = null;
        long nextCancellationCheck = 0;
        const int cancellationCheckIntervalMs = 25;
        void CheckCancellation()
        {
            if (cancellationFile == null) return;
            var now = System.Diagnostics.Stopwatch.GetTimestamp();
            if (now < nextCancellationCheck) return;
            nextCancellationCheck = now + System.Diagnostics.Stopwatch.Frequency * cancellationCheckIntervalMs / 1000;
            if (File.Exists(cancellationFile)) throw new OperationCanceledException();
        }
        var (prepare, warmup, preload, owner) = SceneTile.OpenSession(data, plugins, CheckCancellation);
        using (owner)
        {
            while (true)
            {
                if (!File.Exists(requestFile))
                {
                    await Task.Delay(25);
                    continue;
                }
                TileResult? result = null;
                ulong requestId = 0;
                try
                {
                    using var message = JsonDocument.Parse(await File.ReadAllTextAsync(requestFile));
                    File.Delete(requestFile);
                    var root = message.RootElement;
                    requestId = root.GetProperty("requestId").GetUInt64();
                    if (requestId == 0) throw new InvalidDataException("Invalid request ID");
                    cancellationFile = Path.Combine(sessionRoot, $"cancel-{requestId.ToString(System.Globalization.CultureInfo.InvariantCulture)}");
                    nextCancellationCheck = 0;
                    CheckCancellation();
                    if (root.TryGetProperty("preloadLastSave", out var load) && load.GetBoolean())
                    {
                        var size = root.GetProperty("size").GetInt32();
                        if (size is not (4096 or 6144 or 8192 or 10240 or 12288 or 14336 or 16384))
                            throw new InvalidDataException("Invalid preload size");
                        result = preload(size, sessionRoot) ?? new TileResult(1, 0, 0, true, [], []);
                    }
                    else if (root.TryGetProperty("warmup", out var warm) && warm.GetBoolean())
                    {
                        warmup(sessionRoot);
                        result = new TileResult(1, 0, 0, true, [], []);
                    }
                    else
                    {
                        var scope = new FormKey(ModKey.FromFileName(root.GetProperty("scopePlugin").GetString()!),
                            root.GetProperty("localScopeID").GetUInt32());
                        var x = root.GetProperty("x").GetInt64();
                        var y = root.GetProperty("y").GetInt64();
                        var size = root.GetProperty("size").GetInt32();
                        var worldspace = root.GetProperty("worldspace").GetBoolean();
                        if (size is not (4096 or 6144 or 8192 or 10240 or 12288 or 14336 or 16384) ||
                            Math.Abs(x) > 1_000_000 || Math.Abs(y) > 1_000_000 ||
                            !File.Exists(Path.Combine(data, scope.ModKey.ToString())))
                            throw new InvalidDataException("Invalid companion scope");
                        result = prepare(scope, x, y, size, worldspace, sessionRoot,
                            root.TryGetProperty("consolidate", out var consolidate) && consolidate.GetBoolean());
                    }
                    nextCancellationCheck = 0;
                    CheckCancellation();
                }
                catch (OperationCanceledException)
                {
                    result = new TileResult(1, 0, 0, false, [], [], "Request canceled");
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    // Return the reason through the ephemeral mailbox, never a persistent log.
                    // Native geometry publication must fail closed on a request failure.
                    result = new TileResult(1, 0, 1, false, [], [], DescribeFailure("Request", error));
                }
                // Publish only after serialization completes; the host adopts reply.json atomically.
                await using (var stream = new FileStream(replyTemp, FileMode.Create, FileAccess.Write, FileShare.None))
                    await JsonSerializer.SerializeAsync(stream, (result ?? new TileResult(1, 0, 1, false, [], [])) with
                    { RequestId = requestId }, options);
                File.Move(replyTemp, replyFile, true);
                cancellationFile = null;
            }
        }
    }
    catch (Exception error) when (error is not OutOfMemoryException)
    {
        // Load-order initialization can fail before the request loop starts.
        // Publish a bounded error response so the in-game status page can explain why.
        try
        {
            if (arguments.Length == 4)
            {
                var sessionRoot = Path.GetFullPath(arguments[3]);
                if (Directory.Exists(sessionRoot))
                {
                    var temp = Path.Combine(sessionRoot, "reply.tmp");
                    var reply = Path.Combine(sessionRoot, "reply.json");
                    var failure = new TileResult(1, 0, 1, false, [], [],
                        DescribeFailure("Initialize", error));
                    await using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
                        await JsonSerializer.SerializeAsync(stream, failure, new JsonSerializerOptions
                        {
                            PropertyNamingPolicy = JsonNamingPolicy.CamelCase
                        });
                    File.Move(temp, reply, true);
                }
            }
        }
        catch (Exception) { /* No persistent diagnostics and no failed-output retention. */ }
        return 1;
    }
}

static string DescribeFailure(string stage, Exception error)
{
    var detail = $"{stage}: {error.GetType().Name}: {error.Message}";
    return detail.Length <= 320 ? detail : detail[..320];
}

internal sealed record TileShape(ulong Identity, float[][] Positions, uint[] Indices, float[] Transform);
internal sealed record TileNifSource(string ReferencePlugin, uint ReferenceLocal, string NifPath);
internal sealed record TileMissingNifShape(string ReferencePlugin, uint ReferenceLocal, string NifPath,
    int BlockIndex, string Reason);
internal sealed record TileLight(string ReferencePlugin, uint ReferenceLocal, string BasePlugin, uint BaseLocal,
    float[] Position, float[] Color, float Radius, float Fade, float EmitterRadius, float FalloffExponent, uint Flags);
internal sealed record TileResult(uint Version, uint EligibleShapes, uint FailedShapes, bool EnumerationComplete,
    List<TileShape> Shapes, List<TileLight> Lights, string Failure = "",
    Dictionary<string, uint>? RejectionReasons = null,
    List<TileNifSource>? NifSources = null, List<TileMissingNifShape>? MissingShapes = null,
    bool WholeCell = false, string ScopePlugin = "", uint ScopeLocal = 0, long CenterX = 0, long CenterY = 0,
    bool SpatialBoundsComplete = false, string ResidencyReason = "", ulong RequestId = 0);
internal sealed record ModelShape(int Index, Vector3[] Positions, uint[] Indices, Matrix4x4 ToModel);
internal sealed record MissingModelShape(int Index, string Reason);
internal sealed record ModelExtraction(ModelShape[] Shapes, MissingModelShape[] Missing,
    Dictionary<string, uint> Rejections, ModelShape[] BoundsOnly, bool BoundsComplete);

internal static class SceneTile
{
    private const int MaxLights = 2048;
    private const uint InteriorCellFlag = 1;

    internal static TileResult Prepare(string data, string pluginsPath, FormKey scope, long tileX, long tileY, int size,
        bool isWorldspace, string scratch)
    {
        var (run, _, _, owner) = OpenSession(data, pluginsPath);
        using (owner) return run(scope, tileX, tileY, size, isWorldspace, scratch, false);
    }

    internal static (Func<FormKey, long, long, int, bool, string, bool, TileResult> Run, Action<string> Warmup, Func<int, string, TileResult?> Preload, IDisposable Owner)
        OpenSession(string data, string pluginsPath, Action? checkCancellation = null)
    {
        var official = new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var club = File.Exists(ccc) ? File.ReadLines(ccc).Select(line => line.Trim()).Where(line => line.Length != 0) : [];
        var implicitPlugins = official.Concat(club).Where(name => File.Exists(Path.Combine(data, name)))
            .Select(name => ModKey.FromFileName(name));
        var order = implicitPlugins.Concat(PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
            .Where(entry => entry.Enabled).Select(entry => entry.ModKey)).Distinct().ToArray();
        var environment = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>(GameRelease.SkyrimSE)
            .WithTargetDataFolder(data).WithLoadOrder(order).Build();
        var archives = new Lazy<IArchiveReader[]>(() => Archive.GetApplicableArchivePaths(GameRelease.SkyrimSE, data)
            .Select(path => Archive.CreateReader(GameRelease.SkyrimSE, path)).Reverse().ToArray());

        var sourceManifest = new Dictionary<FormKey, string>();
        var missingManifest = new Dictionary<string, MissingModelShape[]>(StringComparer.OrdinalIgnoreCase);
        var warmed = false;

        void Warmup(string scratch)
        {
            if (warmed) return;
            var visited = new HashSet<FormKey>();
            var modelCache = new Dictionary<string, ModelExtraction>(StringComparer.OrdinalIgnoreCase);
            foreach (var listing in environment.LoadOrder.ListedOrder)
            {
                if (listing.Mod is not { } mod) continue;
                foreach (var cell in mod.EnumerateMajorRecords<ICellGetter>())
                foreach (var candidate in cell.Persistent.Concat(cell.Temporary))
                {
                    checkCancellation?.Invoke();
                    if (!visited.Add(candidate.FormKey)) continue;
                    try
                    {
                        var context = environment.LinkCache.ResolveContext<IPlaced, IPlacedGetter>(candidate.FormKey);
                        if (context.Record.IsDeleted || context.Record is not IPlacedObjectGetter obj) continue;
                        var baseRecord = obj.Base.TryResolve(environment.LinkCache);
                        if (baseRecord is not IModeledGetter modeled || modeled.Model?.File is not { } file ||
                            string.IsNullOrWhiteSpace(file.ToString())) continue;
                        var path = NormalizeNifPath(file.ToString());
                        sourceManifest[candidate.FormKey] = path;
                        if (baseRecord is not (IStaticGetter or IFurnitureGetter or IMoveableStaticGetter) ||
                            missingManifest.ContainsKey(path)) continue;
                        try
                        {
                            var model = LoadModel(data, path, modelCache, archives, scratch, checkCancellation);
                            missingManifest[path] = model.Missing;
                        }
                        catch (Exception error) when (error is not OutOfMemoryException and not OperationCanceledException)
                        {
                            var reason = error is InvalidDataException ? error.Message : error.GetType().Name;
                            missingManifest[path] = [new MissingModelShape(-1, reason)];
                        }
                        finally
                        {
                            modelCache.Clear();
                        }
                    }
                    catch (Exception error) when (error is not OutOfMemoryException and not OperationCanceledException)
                    {
                        // Unresolved placements are diagnosed by the tile request.
                    }
                }
            }
            warmed = true;
        }

        // These are placements only, not mesh geometry. The cache is replaced whenever
        // scope, size or the bounded region changes. It never indexes the whole game.
        Dictionary<FormKey, IPlacedGetter>? placementWindow = null;
        FormKey placementScope = default;
        bool placementWorldspace = false, placementFoundScope = false;
        int placementSize = 0;
        long placementMinX = 0, placementMinY = 0, placementMaxX = 0, placementMaxY = 0;

        // Keep Mutagen and archive readers alive across the 5x5 request stream.
        // Model triangles remain per-tile and are not stored between preparations.
        TileResult Run(FormKey scope, long tileX, long tileY, int size, bool isWorldspace, string scratch, bool consolidate)
        {
            if (consolidate && !isWorldspace)
            {
                try
                {
                    var whole = Extract(scope, tileX, tileY, size, false, scratch, true, false);
                    if (whole.WholeCell) return whole;
                    return Extract(scope, tileX, tileY, size, isWorldspace, scratch, false, false) with { ResidencyReason = whole.ResidencyReason };
                }
                catch (OutOfMemoryException) { }
            }
            return Extract(scope, tileX, tileY, size, isWorldspace, scratch, false, false);
        }

        TileResult? Preload(int size, string scratch)
        {
            var hint = SaveLocation.LatestInteriorHint();
            TileResult Unavailable(string reason) => new(1, 0, 0, false, [], [], ResidencyReason: reason);
            if (hint == null) return Unavailable("No readable last-save location");
            var matchingKeys = new HashSet<FormKey>();
            foreach (var listing in environment.LoadOrder.ListedOrder)
            {
                if (listing.Mod is not { } mod) continue;
                foreach (var candidate in mod.EnumerateMajorRecords<ICellGetter>())
                {
                    checkCancellation?.Invoke();
                    if (string.Equals(candidate.EditorID, hint.EditorId, StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(candidate.EditorID, hint.HeaderLocation, StringComparison.OrdinalIgnoreCase))
                        matchingKeys.Add(candidate.FormKey);
                }
            }
            var matches = new List<FormKey>();
            foreach (var key in matchingKeys)
            {
                checkCancellation?.Invoke();
                var cell = environment.LinkCache.Resolve<ICellGetter>(key);
                if (!cell.IsDeleted && ((uint)cell.Flags & InteriorCellFlag) != 0 &&
                    (string.Equals(cell.EditorID, hint.EditorId, StringComparison.OrdinalIgnoreCase) ||
                     string.Equals(cell.EditorID, hint.HeaderLocation, StringComparison.OrdinalIgnoreCase)))
                    matches.Add(key);
            }
            if (matches.Count == 0) return Unavailable("No matching interior CELL for last-save location");
            // Prefer the filename EditorID; any speculative choice must match the actual loaded FormID.
            var selected = matches.OrderByDescending(key => string.Equals(
                    environment.LinkCache.Resolve<ICellGetter>(key).EditorID, hint.EditorId, StringComparison.OrdinalIgnoreCase))
                .ThenBy(key => key.ToString(), StringComparer.OrdinalIgnoreCase).First();
            try
            {
                var whole = Extract(selected, 0, 0, size, false, scratch, true, true);
                if (!whole.WholeCell) return Unavailable(whole.ResidencyReason);
                return matches.Count > 1 ? whole with { ResidencyReason = $"Speculative interior selected from {matches.Count} matches; gameplay scope must match" } : whole;
            }
            catch (OutOfMemoryException) { return Unavailable("Insufficient managed memory for whole-cell preparation"); }
        }

        TileResult Extract(FormKey scope, long tileX, long tileY, int size, bool isWorldspace, string scratch,
            bool wholeCell, bool inferCenter)
        {
        // Last override wins. No mesh processing occurs outside the requested
        // scope; for exterior tiles even candidates are bounded by a halo.
        // A 9x9 placement window contains the complete candidate origin halo
        // for the current 5x5 preparation order (tile position +/- 2, origin +/- 2).
        // Reload only when movement escapes the previous bounded window.
        var reuseWindow = placementWindow != null && placementScope == scope &&
            placementSize == size && placementWorldspace == isWorldspace &&
            (!isWorldspace || (tileX - 2 >= placementMinX && tileY - 2 >= placementMinY &&
                tileX + 3 <= placementMaxX && tileY + 3 <= placementMaxY));
        if (!reuseWindow)
        {
            placementWindow = null;
            var nextWindow = new Dictionary<FormKey, IPlacedGetter>();
            placementScope = scope;
            placementSize = size;
            placementWorldspace = isWorldspace;
            placementMinX = tileX - 4;
            placementMinY = tileY - 4;
            placementMaxX = tileX + 5;
            placementMaxY = tileY + 5;
            placementFoundScope = false;
            foreach (var listing in environment.LoadOrder.ListedOrder)
            {
                if (listing.Mod is not { } mod) continue;
                foreach (var cell in mod.EnumerateMajorRecords<ICellGetter>())
                {
                    checkCancellation?.Invoke();
                    if (!isWorldspace && cell.FormKey != scope) continue;
                    if (cell.FormKey == scope) placementFoundScope = true;
                    foreach (var reference in cell.Persistent.Concat(cell.Temporary))
                    {
                        checkCancellation?.Invoke();
                        if (isWorldspace)
                        {
                            if (reference.Placement is not { } p) continue;
                            var px = p.Position.X;
                            var py = p.Position.Y;
                            if (!float.IsFinite(px) || !float.IsFinite(py) ||
                                px < placementMinX * (double)size || px >= placementMaxX * (double)size ||
                                py < placementMinY * (double)size || py >= placementMaxY * (double)size)
                                continue;
                        }
                        nextWindow[reference.FormKey] = reference;
                    }
                }
            }
            placementWindow = nextWindow;
        }
        var candidates = placementWindow!;
        var foundScope = placementFoundScope;
        var shapes = new List<TileShape>();
        var lights = new List<TileLight>();
        bool lightCapacityExceeded = false;
        var bounds = new CellBounds();
        bool boundsComplete = true, tileBoundsOccupied = false;
        // Retain NIF provenance even when a rigid shape cannot be decoded yet.
        // Runtime supplementation can refer back to the installed loose/BSA mesh.
        var nifSources = new List<TileNifSource>();
        var missingShapes = new List<TileMissingNifShape>();
        var models = new Dictionary<string, ModelExtraction>(StringComparer.OrdinalIgnoreCase);
        uint eligible = 0, failed = 0;
        var rejections = new Dictionary<string, uint>(StringComparer.Ordinal);
        void Reject(string reason, uint count = 1)
        {
            failed += count;
            if (!rejections.ContainsKey(reason) && rejections.Count >= 12) reason = "other";
            rejections[reason] = rejections.GetValueOrDefault(reason) + count;
        }
        void AddMissing(FormKey reference, string nifPath, int blockIndex, string reason)
        {
            missingShapes.Add(new TileMissingNifShape(reference.ModKey.ToString(), reference.ID,
                nifPath, blockIndex, reason.Length > 80 ? reason[..80] : reason));
        }
        void RememberMissingModel(FormKey reference, string? meshPath, string reason)
        {
            if (string.IsNullOrWhiteSpace(meshPath)) return;
            var nifPath = sourceManifest.GetValueOrDefault(reference) ?? NormalizeNifPath(meshPath);
            if (nifPath.Length > 260) {
                Reject("NIF path exceeds manifest limit");
                return;
            }
            nifSources.Add(new TileNifSource(reference.ModKey.ToString(), reference.ID, nifPath));
            AddMissing(reference, nifPath, -1, reason);
        }
        void IncludeBounds(ModelExtraction extraction, Matrix4x4 placement)
        {
            if (!extraction.BoundsComplete) boundsComplete = false;
            foreach (var shape in extraction.Shapes.Concat(extraction.BoundsOnly))
            {
                var transform = shape.ToModel * placement;
                if (wholeCell)
                {
                    foreach (var vertex in shape.Positions)
                    {
                        var point = Vector3.Transform(vertex, transform);
                        bounds.Include(point.X, point.Y);
                    }
                }
                else if (Overlaps(shape.Positions, transform, tileX, tileY, size))
                    tileBoundsOccupied = true;
            }
        }

        foreach (var candidate in candidates.Values)
        {
            checkCancellation?.Invoke();
            // The cached placement window is wider than one subcell's origin
            // halo; discard irrelevant candidates before resolving any records.
            if (isWorldspace)
            {
                if (candidate.Placement is not { } placement) continue;
                var px = placement.Position.X;
                var py = placement.Position.Y;
                if (px < (tileX - 2) * (double)size || px >= (tileX + 3) * (double)size ||
                    py < (tileY - 2) * (double)size || py >= (tileY + 3) * (double)size)
                    continue;
            }
            string? unresolvedNif = null;
            try
            {
                var context = environment.LinkCache.ResolveContext<IPlaced, IPlacedGetter>(candidate.FormKey);
                if (isWorldspace)
                {
                    if (!context.TryGetParent<IWorldspaceGetter>(out var world) || world.FormKey != scope) continue;
                }
                else if (!context.TryGetParent<ICellGetter>(out var cell) || cell.FormKey != scope)
                    continue;
                var placed = context.Record;
                if (placed.IsDeleted) continue;
                var obj = placed as IPlacedObjectGetter;
                if (obj == null) { Reject("unsupported placement record"); continue; }
                var baseRecord = obj.Base.TryResolve(environment.LinkCache);
                if (baseRecord == null)
                {
                    // An unresolved base may describe a shadow-casting static.
                    // Never advertise the tile as complete when its role is unknown.
                    boundsComplete = false;
                    Reject("unresolved base record");
                    continue;
                }
                if (placed.Placement is not { } placement) {
                    if (baseRecord is IModeledGetter or ILightGetter)
                    {
                        boundsComplete = false;
                        Reject("missing placement");
                    }
                    continue;
                }
                var position = new[] { placement.Position.X, placement.Position.Y, placement.Position.Z };
                if (baseRecord is ILightGetter light)
                {
                    if (wholeCell || Inside(position[0], position[1], tileX, tileY, size))
                    {
                        if (wholeCell) bounds.Include(position[0], position[1]);
                        else tileBoundsOccupied = true;
                        if (lights.Count < MaxLights)
                            lights.Add(new TileLight(placed.FormKey.ModKey.ToString(), placed.FormKey.ID,
                                obj.Base.FormKey.ModKey.ToString(), obj.Base.FormKey.ID, position,
                                [light.Color.R / 255f, light.Color.G / 255f, light.Color.B / 255f],
                                light.Radius, light.FadeValue, 0, light.FalloffExponent, (uint)light.Flags));
                        else
                            lightCapacityExceeded = true;
                    }
                    continue;
                }
                var rotation = new[] { placement.Rotation.X, placement.Rotation.Y, placement.Rotation.Z };
                var toWorld = PlacementTransform.FromRecord(position, rotation, obj.Scale ?? 1.0f);
                if (baseRecord is not (IStaticGetter or IFurnitureGetter or IMoveableStaticGetter))
                {
                    // Doors, activators, containers, trees, and other modeled
                    // records can become static raster shadow casters. This
                    // geometry-only companion cannot guarantee their coverage.
                    // Do not silently certify a tile with missing static casters.
                    if (baseRecord is IModeledGetter modeled &&
                        modeled.Model?.File is { } file &&
                        !string.IsNullOrWhiteSpace(file.ToString()) &&
                        (!isWorldspace || Inside(position[0], position[1], tileX, tileY, size)))
                    {
                        Reject("unsupported modeled record");
                        RememberMissingModel(placed.FormKey, file.ToString(), "unsupported modeled record");
                        if (!isWorldspace)
                            IncludeBounds(LoadModel(data, file.ToString(), models, archives, scratch, checkCancellation), toWorld);
                    }
                    continue;
                }

                // Runtime-dependent placement cannot be certified from plugin data.
                if ((placed.MajorRecordFlagsRaw & 0x800) != 0 || obj.EnableParent != null ||
                    obj.VirtualMachineAdapter != null || baseRecord is IMoveableStaticGetter)
                {
                    Reject("runtime-dependent placement");
                    var runtimeModel = (baseRecord as IModeledGetter)?.Model?.File.ToString();
                    RememberMissingModel(placed.FormKey, runtimeModel, "runtime-dependent placement");
                    if (!isWorldspace)
                    {
                        if (string.IsNullOrWhiteSpace(runtimeModel)) boundsComplete = false;
                        else IncludeBounds(LoadModel(data, runtimeModel, models, archives, scratch, checkCancellation), toWorld);
                    }
                    continue;
                }
                var model = (baseRecord as IModeledGetter)?.Model?.File.ToString();
                if (string.IsNullOrWhiteSpace(model)) { boundsComplete = false; Reject("missing mesh path"); continue; }
                unresolvedNif = sourceManifest.GetValueOrDefault(placed.FormKey) ?? NormalizeNifPath(model);
                if (unresolvedNif.Length > 260)
                    throw new InvalidDataException("NIF path exceeds manifest limit");
                nifSources.Add(new TileNifSource(placed.FormKey.ModKey.ToString(),
                    placed.FormKey.ID, unresolvedNif));
                var extraction = LoadModel(data, unresolvedNif, models, archives, scratch, checkCancellation);
                if (!isWorldspace) IncludeBounds(extraction, toWorld);
                foreach (var (reason, count) in extraction.Rejections)
                    Reject(reason, count);
                // The main-menu manifest is scoped to the same load-order session.
                // Refresh it if the current decode has different missing blocks.
                if (!missingManifest.TryGetValue(unresolvedNif, out var missingEntries) ||
                    !missingEntries.SequenceEqual(extraction.Missing))
                    missingManifest[unresolvedNif] = missingEntries = extraction.Missing;
                foreach (var missing in missingEntries)
                    AddMissing(placed.FormKey, unresolvedNif, missing.Index, missing.Reason);
                var mesh = extraction.Shapes;
                foreach (var shape in mesh)
                {
                    checkCancellation?.Invoke();
                    var transform = shape.ToModel * toWorld;
                    if (!wholeCell && !Overlaps(shape.Positions, transform, tileX, tileY, size)) continue;
                    ++eligible;
                    var shapeBytes = checked((long)shape.Positions.Length * 48 + (long)shape.Indices.Length * 4);
                    if (!FitsManagedMemory(shapeBytes))
                        throw new OutOfMemoryException("Insufficient memory for tile geometry");
                    shapes.Add(new TileShape(Identity(placed.FormKey.ToString(), shape.Index),
                        shape.Positions.Select(p => new[] { p.X, p.Y, p.Z }).ToArray(), shape.Indices,
                        PlacementTransform.ToRows(transform)));
                }
            }
            catch (Exception error) when (error is not OutOfMemoryException and not OperationCanceledException)
            {
                boundsComplete = false;
                var message = error is InvalidDataException ? error.Message : error.GetType().Name;
                Reject(message.Length <= 80 ? message : message[..80]);
                if (unresolvedNif != null)
                    AddMissing(candidate.FormKey, unresolvedNif, -1, message);
            }
        }
        // A bounded exterior anchor halo cannot prove that a huge off-tile
        // model does not overlap this tile. Exteriors remain incomplete until
        // an authoritative bounds index is available; never offload on a guess.
        if (lightCapacityExceeded && rejections.Count < 12)
            rejections["tile light capacity"] = 1;
        var complete = !isWorldspace && foundScope && !lightCapacityExceeded;
        var spatialComplete = complete && boundsComplete;
        // Residency depends on the measured footprint, not the completeness of shadow coverage.
        var center = bounds.Valid ? bounds.Center(size) : (tileX, tileY);
        var fits = bounds.Fits(center.Item1, center.Item2, size);
        var consolidated = wholeCell && !isWorldspace && foundScope && fits;
        if (consolidated && inferCenter)
            (tileX, tileY) = center;
        var residencyReason = !wholeCell ? "" : !foundScope ? "Interior scope not found" :
            !bounds.Valid ? "No measurable prepared bounds" : !fits ? "Calculated bounds exceed a 5x5 footprint" :
            "Calculated bounds fit within a 5x5 footprint";
        // A fully bounded tile with no intersecting content is empty even if other tiles have unsupported casters.
        if (!wholeCell && spatialComplete && !tileBoundsOccupied && shapes.Count == 0 && lights.Count == 0)
        {
            failed = 0;
            rejections.Clear();
            nifSources.Clear();
            missingShapes.Clear();
        }
        return new TileResult(1, eligible, failed, complete,
            shapes, lights, "", rejections, nifSources, missingShapes, consolidated,
            consolidated ? scope.ModKey.ToString() : "", consolidated ? scope.ID : 0, tileX, tileY, spatialComplete, residencyReason);
        }
        return (Run, Warmup, Preload, environment);
    }

    private static bool Inside(float x, float y, long tx, long ty, int size) =>
        x >= tx * (double)size && x < (tx + 1) * (double)size &&
        y >= ty * (double)size && y < (ty + 1) * (double)size;

    private static bool Overlaps(Vector3[] vertices, Matrix4x4 transform, long tx, long ty, int size)
    {
        var low = new Vector2(float.PositiveInfinity);
        var high = new Vector2(float.NegativeInfinity);
        foreach (var vertex in vertices)
        {
            var p = Vector3.Transform(vertex, transform);
            if (!float.IsFinite(p.X) || !float.IsFinite(p.Y)) throw new InvalidDataException("Invalid placed bounds");
            low = Vector2.Min(low, new Vector2(p.X, p.Y));
            high = Vector2.Max(high, new Vector2(p.X, p.Y));
        }
        return low.X < (tx + 1) * (double)size && high.X >= tx * (double)size &&
            low.Y < (ty + 1) * (double)size && high.Y >= ty * (double)size;
    }

    private static string NormalizeNifPath(string path)
    {
        path = path.Replace('/', '\\');
        return path.StartsWith("meshes\\", StringComparison.OrdinalIgnoreCase)
            ? "meshes\\" + path[7..] : "meshes\\" + path;
    }

    private static ModelExtraction LoadModel(string data, string path, Dictionary<string, ModelExtraction> cache,
        Lazy<IArchiveReader[]> archives, string scratch, Action? checkCancellation)
    {
        path = NormalizeNifPath(path);
        if (Path.IsPathRooted(path) || path.Split('\\').Any(part => part is ".." or "." or ""))
            throw new InvalidDataException("Invalid mesh path");
        if (cache.TryGetValue(path, out var cached)) return cached;
        var filename = Path.GetFullPath(Path.Combine(data, path));
        if (!filename.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Mesh escaped Data");
        var nif = new NifFile();
        var temporary = string.Empty;
        try
        {
            if (!File.Exists(filename))
            {
                foreach (var archive in archives.Value)
                {
                    checkCancellation?.Invoke();
                    if (!archive.TryGetFolder(Path.GetDirectoryName(path)!, out var folder)) continue;
                    var entry = folder.Files.FirstOrDefault(file =>
                        string.Equals(file.Path.Replace('/', '\\'), path, StringComparison.OrdinalIgnoreCase));
                    if (entry == null) continue;
                    if (entry.Size == 0)
                        throw new InvalidDataException("Empty archive mesh");
                    if (!FitsManagedMemory(checked((long)entry.Size)))
                        throw new OutOfMemoryException("Insufficient memory for archive extraction");
                    temporary = Path.Combine(scratch, "archive-mesh.nif");
                    File.WriteAllBytes(temporary, entry.GetBytes());
                    filename = temporary;
                    break;
                }
            }
            if (nif.Load(filename) != 0)
                throw new InvalidDataException("NIF unavailable or unsupported");
        }
        finally
        {
            if (temporary.Length != 0)
                File.Delete(temporary);
        }
        if (!nif.Valid || !nif.Header.Version.IsSSE())
            throw new InvalidDataException("NIF unavailable or unsupported");
        var hierarchy = new NifTransforms(nif);
        var output = new List<ModelShape>();
        var boundsOnly = new List<ModelShape>();
        bool boundsComplete = !nif.HasUnknownBlocks;
        var rejected = new Dictionary<string, uint>(StringComparer.Ordinal);
        var missing = new List<MissingModelShape>();
        void Skip(string reason, int index)
        {
            rejected[reason] = rejected.GetValueOrDefault(reason) + 1;
            missing.Add(new MissingModelShape(index, reason));
        }
        // Unknown blocks may coexist with valid rigid geometry. Keep the
        // supported shapes, but never certify complete shadow coverage.
        if (nif.HasUnknownBlocks)
            Skip("unknown NIF blocks (incomplete coverage)", -1);
        foreach (var entry in nif.GetShapes())
        {
            checkCancellation?.Invoke();
            nif.GetBlockIndex(entry, out var index);
            // Retain safe opaque rigid triangles even if a NIF contains unsupported shapes.
            if (entry is not BSTriShape shape || shape.HasSkinInstance || shape.IsSkinned ||
                entry.GetType().Name is not ("BSTriShape" or "BSSubIndexTriShape" or
                    "BSMeshLODTriShape" or "BSSegmentedTriShape"))
            {
                boundsComplete = false;
                Skip("unsupported animated or nontriangle shape", index);
                continue;
            }
            try
            {
                var vertices = shape.VertexPositions.ToArray();
                var triangles = entry.Triangles;
                if (vertices.Length == 0 || triangles.Count == 0 ||
                    vertices.Any(v => !float.IsFinite(v.X) || !float.IsFinite(v.Y) || !float.IsFinite(v.Z)))
                    throw new InvalidDataException("Invalid shape vertices");
                var indices = new uint[triangles.Count * 3];
                for (var i = 0; i < triangles.Count; ++i)
                {
                    var t = triangles[i];
                    if (t.V1 >= vertices.Length || t.V2 >= vertices.Length || t.V3 >= vertices.Length)
                        throw new InvalidDataException("Invalid triangle index");
                    indices[i * 3] = t.V1;
                    indices[i * 3 + 1] = t.V2;
                    indices[i * 3 + 2] = t.V3;
                }
                var extractedShape = new ModelShape(index, vertices, indices, hierarchy.ForShape(index));
                if (nif.GetBlock(entry.AlphaPropertyRef) != null)
                {
                    boundsOnly.Add(extractedShape);
                    Skip("alpha-dependent shape (textures required)", index);
                }
                else output.Add(extractedShape);
            }
            catch (Exception error) when (error is not OutOfMemoryException and not OperationCanceledException)
            {
                boundsComplete = false;
                Skip("invalid or unsupported shape data", index);
            }
        }
        if (output.Count == 0 && missing.Count == 0)
            Skip("no supported opaque rigid triangles", -1);
        var extracted = new ModelExtraction(output.ToArray(), missing.ToArray(), rejected, boundsOnly.ToArray(), boundsComplete);
        if (FitsManagedMemory(0))
            cache[path] = extracted;
        return extracted;
    }

    private static bool FitsManagedMemory(long bytes)
    {
        var memory = GC.GetGCMemoryInfo();
        var available = memory.TotalAvailableMemoryBytes - GC.GetTotalMemory(false);
        return bytes >= 0 && available > 0 && bytes <= available;
    }

    private static ulong Identity(string formKey, int blockIndex)
    {
        ulong hash = 14695981039346656037UL;
        foreach (var ch in formKey + ":" + blockIndex.ToString(System.Globalization.CultureInfo.InvariantCulture))
        {
            hash ^= ch;
            hash = unchecked(hash * 1099511628211UL);
        }
        return hash == 0 ? 1 : hash;
    }
}
