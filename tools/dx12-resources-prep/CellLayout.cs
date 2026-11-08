using System.Security.Cryptography;
using System.Text.Json;

internal sealed record PlacedShape(string ReferenceKey, string? BaseKey, string Model, string SourceSha256,
    int BlockIndex, string ShapeName, string GeometryFile, string GeometrySha256, int IndexCount,
    float[] ModelToCell, float[] ShapeToCell, float[] CellBounds, string? BaseEditorId, string RecordClassification, string? RoomReferenceKey,
    bool OpaqueOccluderCandidate, string GeometryStatus, bool TwoSided, string ShadingStatus, string? ShadingFile, string? ShadingSha256, CompiledMaterial? Material, string TangentStatus);
internal sealed record LayoutOmission(string ReferenceKey, string? Model, string Reason, int? BlockIndex);
internal sealed record LayoutResult(int PlacedReferences, int Instances, long Triangles, int OmittedReferences, string Digest);

internal static class CellLayout
{
    public static LayoutResult Write(string output, string cell, string? editorId, string inputDigest,
        List<object> rows, CompiledNif[] assets, JsonSerializerOptions options)
    {
        var records = JsonSerializer.SerializeToElement(rows, options).EnumerateArray().ToArray();
        var models = assets.ToDictionary(asset => asset.SourcePath, StringComparer.OrdinalIgnoreCase);
        var instances = new List<PlacedShape>();
        var omissions = new List<LayoutOmission>();
        foreach (var row in records)
        {
            var key = row.GetProperty("key").GetString()!;
            var model = row.GetProperty("model").GetString();
            var classification = row.GetProperty("classification").GetString();
            if (classification is not ("static-record-nif-uninspected" or "furniture-record-nif-uninspected" or "moveable-static-record-nif-uninspected"))
            {
                omissions.Add(new(key, model, classification!, null));
                continue;
            }
            if (string.IsNullOrWhiteSpace(model))
            {
                omissions.Add(new(key, model, "missing-model", null));
                continue;
            }
            var path = PgAssets.Normalize(model);
            if (!path.StartsWith("meshes\\", StringComparison.Ordinal)) path = "meshes\\" + path;
            if (!models.TryGetValue(path, out var asset) || asset.Status != "classified")
            {
                omissions.Add(new(key, model, asset?.Status ?? "missing-compiled-model", null));
                continue;
            }
            try
            {
                var placement = PlacementTransform.FromRecord(row.GetProperty("position").Deserialize<float[]>() ?? [],
                    row.GetProperty("rotationRadians").Deserialize<float[]>() ?? [], row.GetProperty("scale").GetSingle());
                var placed = new List<PlacedShape>();
                foreach (var shape in asset.Shapes)
                {
                    if (shape.Status != "compiled" && !shape.Status.StartsWith("preview-only:", StringComparison.Ordinal))
                    {
                        omissions.Add(new(key, model, shape.Status, shape.BlockIndex));
                        continue;
                    }
                    var transform = PlacementTransform.FromRows(shape.ShapeToModel!) * placement;
                    var matrix = PlacementTransform.ToRows(transform);
                    if (!matrix.All(float.IsFinite)) throw new InvalidDataException("Non-finite composed placement.");
                    placed.Add(new(key, row.GetProperty("baseKey").GetString(), asset.SourcePath, asset.SourceSha256,
                        shape.BlockIndex, shape.Name, shape.GeometryFile!, shape.GeometrySha256!, shape.IndexCount,
                        PlacementTransform.ToRows(placement), matrix, PlacementTransform.Bounds(shape.LocalBounds!, transform),
                        row.GetProperty("baseEditorId").GetString(), classification, row.GetProperty("roomReferenceKey").GetString(), shape.OpaqueOccluderCandidate, shape.Status, shape.Material?.TwoSided ?? false,
                        shape.ShadingStatus, shape.ShadingFile, shape.ShadingSha256, shape.Material, shape.TangentStatus));
                    if (!shape.OpaqueOccluderCandidate) omissions.Add(new(key, model, shape.Status, shape.BlockIndex));
                }
                instances.AddRange(placed);
                if (asset.Shapes.Length == 0) omissions.Add(new(key, model, "no-shapes", null));
            }
            catch (InvalidDataException error)
            {
                omissions.Add(new(key, model, error.Message, null));
            }
        }
        var lights = records.Where(row => row.GetProperty("light").ValueKind != JsonValueKind.Null).Select(row => new
        {
            referenceKey = row.GetProperty("key"), position = row.GetProperty("position"),
            rotationRadians = row.GetProperty("rotationRadians"), scale = row.GetProperty("scale"),
            referenceSource = row.GetProperty("referenceSource"), baseSource = row.GetProperty("baseSource"),
            classification = row.GetProperty("classification"), baseline = row.GetProperty("light"),
            hasReferenceOverrides = row.GetProperty("hasReferenceLightOverrides"), runtimeValidated = false
        }).ToArray();
        var rooms = records.Where(row => row.GetProperty("roomBounds").ValueKind == JsonValueKind.Array).Select(row =>
        {
            var extents = row.GetProperty("roomBounds").Deserialize<float[]>() ?? [];
            if (extents.Length != 3 || extents.Any(value => !float.IsFinite(value) || value <= 0))
                throw new InvalidDataException("Invalid room half extents.");
            var transform = PlacementTransform.FromRecord(row.GetProperty("position").Deserialize<float[]>() ?? [],
                row.GetProperty("rotationRadians").Deserialize<float[]>() ?? [], row.GetProperty("scale").GetSingle());
            if (!System.Numerics.Matrix4x4.Invert(transform, out var inverse)) throw new InvalidDataException("Singular room transform.");
            return new { referenceKey = row.GetProperty("key"), halfExtents = extents, cellToRoom = PlacementTransform.ToRows(inverse) };
        }).ToArray();
        if (rooms.Length > 128) throw new InvalidDataException("Prepared room count exceeds 128.");
        var digest = Convert.ToHexString(SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = 1, roomSchemaVersion = 1, inputDigest, compilerKeys = assets.Select(asset => asset.CompilerKey).Distinct(), instances, omissions, lights, rooms
        }, options))).ToLowerInvariant();
        var placedReferences = instances.Select(instance => instance.ReferenceKey).Distinct().Count();
        var layout = new
        {
            schemaVersion = 1, rendererSchemaVersion = 1, shadingSchemaVersion = 2, materialSchemaVersion = 1, roomSchemaVersion = 1, rooms, cell, editorId, inputDigest, layoutDigest = digest,
            coordinateSystem = "Skyrim cell coordinates, Z up, native game units",
            rotationConvention = "CommonLibSSE-NG NiMatrix3::SetEulerAnglesXYZ; record radians; runtime comparison pending",
            nifRotationConvention = "Nifly 1.1.0 Matrix33 wire-order fields mapped to native NIF rows, then transposed for System.Numerics",
            matrixConvention = "row-major 3x4, column-vector application; ShapeToCell = ModelToCell * ShapeToModel",
            placementMatricesComposed = true, runtimeCoverageValid = false, accelerationBuilt = false,
            placedReferences, opaqueOccluderCandidateInstances = instances.Count(instance => instance.OpaqueOccluderCandidate),
            previewOnlyInstances = instances.Count(instance => !instance.OpaqueOccluderCandidate), instances, omissions, lights
        };
        File.WriteAllText(Path.Combine(output, "cell-layout.json"), JsonSerializer.Serialize(layout, options));
        var meshes = instances.DistinctBy(instance => instance.GeometrySha256).Select(instance => new
        {
            hash = instance.GeometrySha256, blob = Convert.ToBase64String(File.ReadAllBytes(Path.Combine(output, instance.GeometryFile)))
        }).ToArray();
        var template = File.ReadAllText(Path.Combine(Path.GetDirectoryName(typeof(CellLayout).Assembly.Location)!, "LayoutPreview.html"));
        var payload = JsonSerializer.Serialize(new { layout, meshes }, options);
        File.WriteAllText(Path.Combine(output, "cell-preview.html"), template.Replace("/*SCENE_DATA*/null", payload));
        return new(placedReferences, instances.Count, instances.Sum(instance => (long)instance.IndexCount / 3),
            records.Length - placedReferences, digest);
    }
}
