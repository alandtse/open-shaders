using System.Numerics;
using System.Text.Json;
using Mutagen.Bethesda;
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
    var result = SceneTile.Prepare(data, plugins, scope, x, y, size, worldspace);
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

internal sealed record TileShape(ulong Identity, float[][] Positions, uint[] Indices, float[] Transform);
internal sealed record TileLight(string ReferencePlugin, uint ReferenceLocal, string BasePlugin, uint BaseLocal,
    float[] Position, float[] Color, float Radius, float Fade, float EmitterRadius, float FalloffExponent, uint Flags);
internal sealed record TileResult(uint Version, uint EligibleShapes, uint FailedShapes, bool EnumerationComplete,
    List<TileShape> Shapes, List<TileLight> Lights);
internal sealed record ModelShape(int Index, Vector3[] Positions, uint[] Indices, Matrix4x4 ToModel);

internal static class SceneTile
{
    private const int MaxShapes = 384;
    private const long MaxGeometryBytes = 48L * 1024 * 1024;

    internal static TileResult Prepare(string data, string pluginsPath, FormKey scope, long tileX, long tileY, int size, bool isWorldspace)
    {
        var official = new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
        var ccc = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
        var club = File.Exists(ccc) ? File.ReadLines(ccc).Select(line => line.Trim()).Where(line => line.Length != 0) : [];
        var implicitPlugins = official.Concat(club).Where(name => File.Exists(Path.Combine(data, name)))
            .Select(ModKey.FromFileName);
        var order = implicitPlugins.Concat(PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
            .Where(entry => entry.Enabled).Select(entry => entry.ModKey)).Distinct().ToArray();
        using var environment = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>(GameRelease.SkyrimSE)
            .WithTargetDataFolder(data).WithLoadOrder(order).Build();

        // Last override wins. No mesh processing occurs outside the requested
        // scope; for exterior tiles even candidates are bounded by a halo.
        var candidates = new Dictionary<FormKey, IPlacedGetter>();
        var foundScope = false;
        foreach (var listing in environment.LoadOrder.ListedOrder)
        {
            if (listing.Mod is not { } mod) continue;
            foreach (var cell in mod.EnumerateMajorRecords<ICellGetter>())
            {
                if (!isWorldspace && cell.FormKey != scope) continue;
                if (cell.FormKey == scope) foundScope = true;
                foreach (var reference in cell.Persistent.Concat(cell.Temporary))
                {
                    if (isWorldspace)
                    {
                        if (reference.Placement is not { } p) continue;
                        var px = p.Position.X;
                        var py = p.Position.Y;
                        if (!float.IsFinite(px) || !float.IsFinite(py) ||
                            px < (tileX - 2) * size || px >= (tileX + 3) * size ||
                            py < (tileY - 2) * size || py >= (tileY + 3) * size) continue;
                    }
                    candidates[reference.FormKey] = reference;
                }
            }
        }
        var shapes = new List<TileShape>();
        var lights = new List<TileLight>();
        var models = new Dictionary<string, ModelShape[]>(StringComparer.OrdinalIgnoreCase);
        uint eligible = 0, failed = 0;
        long bytes = 0;
        foreach (var candidate in candidates.Values)
        {
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
                if (obj == null) { ++failed; continue; }
                var baseRecord = obj.Base.TryResolve(environment.LinkCache);
                if (placed.Placement is not { } placement) {
                    if (baseRecord is IStaticGetter or IFurnitureGetter or IMoveableStaticGetter) ++failed;
                    continue;
                }
                var position = new[] { placement.Position.X, placement.Position.Y, placement.Position.Z };
                if (baseRecord is ILightGetter light)
                {
                    if (Inside(position[0], position[1], tileX, tileY, size))
                        lights.Add(new TileLight(placed.FormKey.ModKey.ToString(), placed.FormKey.ID,
                            obj.Base.FormKey.ModKey.ToString(), obj.Base.FormKey.ID, position,
                            [light.Color.R / 255f, light.Color.G / 255f, light.Color.B / 255f],
                            light.Radius, light.FadeValue, 0, light.FalloffExponent, (uint)light.Flags));
                    continue;
                }
                if (baseRecord is not (IStaticGetter or IFurnitureGetter or IMoveableStaticGetter)) continue;

                // Runtime-dependent placement cannot be certified from plugin data.
                if ((placed.MajorRecordFlagsRaw & 0x800) != 0 || obj.EnableParent != null ||
                    obj.VirtualMachineAdapter != null || baseRecord is IMoveableStaticGetter)
                {
                    ++failed;
                    continue;
                }
                var model = (baseRecord as IModeledGetter)?.Model?.File.ToString();
                if (string.IsNullOrWhiteSpace(model)) { ++failed; continue; }
                var mesh = LoadModel(data, model, models);
                var rotation = new[] { placement.Rotation.X, placement.Rotation.Y, placement.Rotation.Z };
                var toWorld = PlacementTransform.FromRecord(position, rotation, obj.Scale);
                foreach (var shape in mesh)
                {
                    var transform = shape.ToModel * toWorld;
                    if (!Overlaps(shape.Positions, transform, tileX, tileY, size)) continue;
                    ++eligible;
                    var shapeBytes = checked((long)shape.Positions.Length * 12 + (long)shape.Indices.Length * 4);
                    if (shapes.Count >= MaxShapes || shapeBytes > MaxGeometryBytes - bytes)
                    {
                        ++failed;
                        continue;
                    }
                    bytes += shapeBytes;
                    shapes.Add(new TileShape(Identity(placed.FormKey.ToString(), shape.Index),
                        shape.Positions.Select(p => new[] { p.X, p.Y, p.Z }).ToArray(), shape.Indices,
                        PlacementTransform.ToRows(transform)));
                }
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                ++failed;
            }
        }
        // A bounded exterior anchor halo cannot prove that a huge off-tile
        // model does not overlap this tile. Exteriors remain incomplete until
        // an authoritative bounds index is available; never offload on a guess.
        return new TileResult(1, eligible, failed, !isWorldspace && foundScope, shapes, lights);
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

    private static ModelShape[] LoadModel(string data, string path, Dictionary<string, ModelShape[]> cache)
    {
        path = path.Replace('/', '\\');
        if (!path.StartsWith("meshes\\", StringComparison.OrdinalIgnoreCase)) path = "meshes\\" + path;
        if (Path.IsPathRooted(path) || path.Split('\\').Any(part => part is ".." or "." or ""))
            throw new InvalidDataException("Invalid mesh path");
        if (cache.TryGetValue(path, out var cached)) return cached;
        var filename = Path.GetFullPath(Path.Combine(data, path));
        if (!filename.StartsWith(data + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Mesh escaped Data");
        var nif = new NifFile();
        if (nif.Load(filename) != 0 || !nif.Valid || !nif.Header.Version.IsSSE() || nif.HasUnknownBlocks)
            throw new InvalidDataException("NIF unavailable or unsupported");
        var hierarchy = new NifTransforms(nif);
        var output = new List<ModelShape>();
        foreach (var entry in nif.GetShapes())
        {
            if (entry is not BSTriShape shape || shape.HasSkinInstance || shape.IsSkinned ||
                entry.GetType().Name is not ("BSTriShape" or "BSSubIndexTriShape"))
                throw new InvalidDataException("Unsupported animated or nontriangle shape");
            // Alpha-dependent occluders cannot be handled without textures.
            if (nif.GetBlock(entry.AlphaPropertyRef) != null)
                throw new InvalidDataException("Alpha geometry needs texture coverage");
            nif.GetBlockIndex(entry, out var index);
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
            output.Add(new ModelShape(index, vertices, indices, hierarchy.ForShape(index)));
        }
        if (output.Count == 0) throw new InvalidDataException("No supported triangles");
        cache[path] = output.ToArray();
        return cache[path];
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
