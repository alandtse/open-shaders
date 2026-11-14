using System.Numerics;

internal static class PlacementTransform
{
    public static Matrix4x4 FromRecord(float[] position, float[] rotation, float scale)
    {
        if (position.Length != 3 || rotation.Length != 3 || !position.All(float.IsFinite) ||
            !rotation.All(float.IsFinite) || !float.IsFinite(scale) || scale <= 0)
            throw new InvalidDataException("Invalid reference placement.");
        var (sx, cx) = MathF.SinCos(rotation[0]);
        var (sy, cy) = MathF.SinCos(rotation[1]);
        var (sz, cz) = MathF.SinCos(rotation[2]);
        return new Matrix4x4(
            cy * cz * scale, (sx * sy * cz - cx * sz) * scale, (cx * sy * cz + sx * sz) * scale, 0,
            cy * sz * scale, (sx * sy * sz + cx * cz) * scale, (cx * sy * sz - sx * cz) * scale, 0,
            -sy * scale, sx * cy * scale, cx * cy * scale, 0,
            position[0], position[1], position[2], 1);
    }

    public static Matrix4x4 FromRows(float[] rows)
    {
        if (rows.Length != 12 || !rows.All(float.IsFinite)) throw new InvalidDataException("Invalid shape transform.");
        return new(rows[0], rows[4], rows[8], 0, rows[1], rows[5], rows[9], 0,
            rows[2], rows[6], rows[10], 0, rows[3], rows[7], rows[11], 1);
    }

    public static float[] ToRows(Matrix4x4 matrix) =>
        [matrix.M11, matrix.M21, matrix.M31, matrix.M41, matrix.M12, matrix.M22, matrix.M32, matrix.M42, matrix.M13, matrix.M23, matrix.M33, matrix.M43];

    public static float[] Bounds(float[] local, Matrix4x4 transform)
    {
        if (local.Length != 6 || !local.All(float.IsFinite)) throw new InvalidDataException("Invalid local bounds.");
        var minimum = new Vector3(float.PositiveInfinity);
        var maximum = new Vector3(float.NegativeInfinity);
        for (var corner = 0; corner < 8; ++corner)
        {
            var position = Vector3.Transform(new Vector3(local[(corner & 1) == 0 ? 0 : 3],
                local[(corner & 2) == 0 ? 1 : 4], local[(corner & 4) == 0 ? 2 : 5]), transform);
            minimum = Vector3.Min(minimum, position);
            maximum = Vector3.Max(maximum, position);
        }
        var result = new[] { minimum.X, minimum.Y, minimum.Z, maximum.X, maximum.Y, maximum.Z };
        if (!result.All(float.IsFinite)) throw new InvalidDataException("Non-finite placed bounds.");
        return result;
    }
}
