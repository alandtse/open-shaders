using System.Numerics;

internal static class PlacementTransform
{
    public static Matrix4x4 FromRecord(float[] position, float[] rotation, float scale)
    {
        if (position.Length != 3 || rotation.Length != 3 || !position.All(float.IsFinite) ||
            !rotation.All(float.IsFinite) || !float.IsFinite(scale) || scale <= 0)
            throw new InvalidDataException("Invalid placement");
        var (sx, cx) = MathF.SinCos(rotation[0]);
        var (sy, cy) = MathF.SinCos(rotation[1]);
        var (sz, cz) = MathF.SinCos(rotation[2]);
        return new Matrix4x4(
            cy * cz * scale, (sx * sy * cz - cx * sz) * scale, (cx * sy * cz + sx * sz) * scale, 0,
            cy * sz * scale, (sx * sy * sz + cx * cz) * scale, (cx * sy * sz - sx * cz) * scale, 0,
            -sy * scale, sx * cy * scale, cx * cy * scale, 0,
            position[0], position[1], position[2], 1);
    }

    public static Matrix4x4 FromRows(float[] rows) =>
        new(rows[0], rows[4], rows[8], 0, rows[1], rows[5], rows[9], 0,
            rows[2], rows[6], rows[10], 0, rows[3], rows[7], rows[11], 1);

    public static float[] ToRows(Matrix4x4 m) =>
        [m.M11, m.M21, m.M31, m.M41, m.M12, m.M22, m.M32, m.M42, m.M13, m.M23, m.M33, m.M43];
}
