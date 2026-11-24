using System.Numerics;

internal sealed record ProjectedSnowArea(string Basis, bool Candidate, double ProjectedArea,
    double SurfaceArea, int UpwardTriangles, int TotalTriangles);

internal sealed class ProjectedSnowAreas
{
    private const uint GeometryMagic = 0x4d475352;
    private const int MaxCachedFaceBytes = 64 * 1024 * 1024;
    private readonly string root;
    private readonly Dictionary<string, Vector3[]> faces = new(StringComparer.Ordinal);
    private long cachedFaceBytes;

    internal ProjectedSnowAreas(string outputDirectory) => root = outputDirectory;

    internal ProjectedSnowArea Analyze(string geometryFile, Matrix4x4 shapeToCell, bool opaque)
    {
        const string basis = "world-up-face-projection-v1";
        if (!opaque)
            return new(basis, false, 0, 0, 0, 0);
        if (!Matrix4x4.Invert(shapeToCell, out var inverse))
            throw new InvalidDataException("Snow projection transform is singular.");
        var determinant = shapeToCell.GetDeterminant();
        if (!float.IsFinite(determinant) || determinant == 0)
            throw new InvalidDataException("Snow projection transform is invalid.");
        var normalTransform = Matrix4x4.Transpose(inverse);
        var triangles = GetFaces(geometryFile);
        double projectedArea = 0, surfaceArea = 0;
        var upwardTriangles = 0;
        foreach (var triangle in triangles)
        {
            var worldCross = Vector3.TransformNormal(triangle, normalTransform) * determinant;
            var length = Math.Sqrt((double)worldCross.X * worldCross.X +
                (double)worldCross.Y * worldCross.Y + (double)worldCross.Z * worldCross.Z);
            if (!double.IsFinite(length))
                throw new InvalidDataException("Snow projection area is not finite.");
            surfaceArea += 0.5 * length;
            if (worldCross.Z > 0)
            {
                projectedArea += 0.5 * worldCross.Z;
                ++upwardTriangles;
            }
        }
        return new(basis, projectedArea > 0, projectedArea, surfaceArea,
            upwardTriangles, triangles.Length);
    }

    private Vector3[] GetFaces(string geometryFile)
    {
        if (faces.TryGetValue(geometryFile, out var existing)) return existing;
        if (!geometryFile.StartsWith("geometry/", StringComparison.Ordinal) ||
            !geometryFile.EndsWith(".rsgm", StringComparison.Ordinal) ||
            geometryFile.Length != "geometry/".Length + 64 + ".rsgm".Length ||
            !geometryFile.AsSpan("geometry/".Length, 64).ToArray().All(Uri.IsHexDigit))
            throw new InvalidDataException("Invalid projection geometry path.");
        using var file = File.OpenRead(Path.Combine(root, geometryFile));
        using var reader = new BinaryReader(file);
        if (reader.ReadUInt32() != GeometryMagic || reader.ReadUInt32() != 1)
            throw new InvalidDataException("Invalid projection geometry format.");
        var vertexCount = reader.ReadUInt32();
        var indexCount = reader.ReadUInt32();
        if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0 ||
            vertexCount > 8_000_000 || indexCount > 24_000_000 ||
            file.Length != 16L + 12L * vertexCount + 4L * indexCount)
            throw new InvalidDataException("Invalid projection geometry length.");
        var vertices = new Vector3[checked((int)vertexCount)];
        for (var i = 0; i < vertices.Length; ++i)
            vertices[i] = new(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle());
        var result = new Vector3[checked((int)(indexCount / 3))];
        for (var i = 0; i < result.Length; ++i)
        {
            var a = reader.ReadUInt32();
            var b = reader.ReadUInt32();
            var c = reader.ReadUInt32();
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
                throw new InvalidDataException("Projection geometry index out of range.");
            result[i] = Vector3.Cross(vertices[checked((int)b)] - vertices[checked((int)a)],
                vertices[checked((int)c)] - vertices[checked((int)a)]);
        }
        var bytes = (long)result.Length * 12;
        if (bytes <= MaxCachedFaceBytes - cachedFaceBytes)
        {
            faces.Add(geometryFile, result);
            cachedFaceBytes += bytes;
        }
        return result;
    }
}
