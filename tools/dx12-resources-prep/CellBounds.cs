internal sealed class CellBounds
{
    private const int PredictiveRadius = 2;
    private double minX = double.PositiveInfinity, minY = double.PositiveInfinity;
    private double maxX = double.NegativeInfinity, maxY = double.NegativeInfinity;

    internal bool Valid => double.IsFinite(minX) && double.IsFinite(minY) &&
        double.IsFinite(maxX) && double.IsFinite(maxY);

    internal void Include(float x, float y)
    {
        if (!float.IsFinite(x) || !float.IsFinite(y))
            throw new InvalidDataException("Invalid placed bounds");
        minX = Math.Min(minX, x);
        minY = Math.Min(minY, y);
        maxX = Math.Max(maxX, x);
        maxY = Math.Max(maxY, y);
    }

    internal (long X, long Y) Center(int size) =>
        ((long)Math.Floor((minX + maxX) / (2.0 * size)),
         (long)Math.Floor((minY + maxY) / (2.0 * size)));

    internal bool Fits(long x, long y, int size) => Valid &&
        minX >= (x - PredictiveRadius) * (double)size &&
        minY >= (y - PredictiveRadius) * (double)size &&
        maxX < (x + PredictiveRadius + 1) * (double)size &&
        maxY < (y + PredictiveRadius + 1) * (double)size;
}
