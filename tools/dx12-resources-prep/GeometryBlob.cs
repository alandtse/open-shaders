using System.Numerics;
using System.Security.Cryptography;
using NiflySharp.Structs;

internal static class GeometryBlob
{
    private const uint Magic = 0x4d475352;
    private const uint Version = 1;

    public static (string Path, string Sha256, float[] Bounds) Write(string output, IReadOnlyList<Vector3> vertices, IReadOnlyList<Triangle> triangles)
    {
        if (vertices.Count == 0 || triangles.Count == 0) throw new InvalidDataException("Empty geometry.");
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write(checked((uint)vertices.Count));
        writer.Write(checked((uint)triangles.Count * 3));
        var minimum = new Vector3(float.PositiveInfinity);
        var maximum = new Vector3(float.NegativeInfinity);
        foreach (var vertex in vertices)
        {
            if (!float.IsFinite(vertex.X) || !float.IsFinite(vertex.Y) || !float.IsFinite(vertex.Z)) throw new InvalidDataException("Non-finite position.");
            minimum = Vector3.Min(minimum, vertex);
            maximum = Vector3.Max(maximum, vertex);
            writer.Write(vertex.X); writer.Write(vertex.Y); writer.Write(vertex.Z);
        }
        foreach (var triangle in triangles)
        {
            if (triangle.V1 >= vertices.Count || triangle.V2 >= vertices.Count || triangle.V3 >= vertices.Count)
                throw new InvalidDataException("Index exceeds vertex count.");
            writer.Write((uint)triangle.V1); writer.Write((uint)triangle.V2); writer.Write((uint)triangle.V3);
        }
        writer.Flush();
        var bytes = stream.ToArray();
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var relative = $"geometry/{hash}.rsgm";
        var destination = System.IO.Path.Combine(output, relative);
        if (!File.Exists(destination) || !Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(destination))).Equals(hash, StringComparison.OrdinalIgnoreCase))
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(destination)!);
            File.WriteAllBytes(destination + ".tmp", bytes);
            File.Move(destination + ".tmp", destination, true);
        }
        return (relative, hash, [minimum.X, minimum.Y, minimum.Z, maximum.X, maximum.Y, maximum.Z]);
    }
}
