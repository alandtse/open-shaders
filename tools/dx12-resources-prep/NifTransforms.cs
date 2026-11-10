using System.Numerics;
using NiflySharp;
using NiflySharp.Blocks;

internal sealed class NifTransforms
{
    private readonly NifFile file;
    private readonly Dictionary<int, List<int>> parents = [];
    private readonly int root;

    public NifTransforms(NifFile nif)
    {
        file = nif;
        for (var index = 0; index < file.Blocks.Count; ++index)
        {
            if (file.Blocks[index] is not NiNode node || node.Children == null) continue;
            foreach (var child in node.Children.Indices)
            {
                if (child == -1) continue;
                if (child < 0 || child >= file.Blocks.Count) throw new InvalidDataException("Invalid NIF child index.");
                if (!parents.TryGetValue(child, out var entries)) parents[child] = entries = [];
                entries.Add(index);
            }
        }
        var roots = Enumerable.Range(0, file.Blocks.Count).Where(index => file.Blocks[index] is NiNode && !parents.ContainsKey(index)).ToArray();
        if (roots.Length != 1) throw new InvalidDataException("Only a single rooted NiNode graph is supported.");
        root = roots[0];
    }

    public (float[]? Matrix, string? Exclusion) ForShape(int shapeIndex)
    {
        var accumulated = Matrix4x4.Identity;
        var visited = new HashSet<int>();
        var current = shapeIndex;
        while (true)
        {
            if (!visited.Add(current)) return (null, "cyclic-node-hierarchy");
            if (file.Blocks[current] is not NiAVObject node) return (null, "unsupported-transform-node");
            if (node.Controller != null && !node.Controller.IsEmpty()) return (null, "controlled-node-or-shape");
            if ((node.Flags_ui & 1) != 0) return (null, "hidden-node-or-shape");
            if (current != shapeIndex && node.GetType().Name is not ("NiNode" or "BSFadeNode" or "BSMultiBoundNode" or "BSOrderedNode"))
                return (null, "unsupported-parent-node:" + node.GetType().Name);
            var r = node.Rotation;
            var s = node.Scale;
            var t = node.Translation;
            // Nifly 1.1.0 names Matrix33 fields in wire order; treating those names as native rows reverses NIF rotations.
            var local = new Matrix4x4(
                r.M11 * s, r.M12 * s, r.M13 * s, 0,
                r.M21 * s, r.M22 * s, r.M23 * s, 0,
                r.M31 * s, r.M32 * s, r.M33 * s, 0,
                t.X, t.Y, t.Z, 1);
            var elements = ToDxr(local);
            if (!elements.All(float.IsFinite) || !float.IsFinite(s) || s == 0 || !Matrix4x4.Invert(local, out _))
                return (null, "invalid-node-transform");
            accumulated *= local;
            if (current == root) break;
            if (!parents.TryGetValue(current, out var entries) || entries.Count != 1) return (null, "unreachable-or-multiply-parented-shape");
            current = entries[0];
        }
        var result = ToDxr(accumulated);
        return result.All(float.IsFinite) ? (result, null) : (null, "non-finite-composed-transform");
    }

    private static float[] ToDxr(Matrix4x4 matrix) =>
        [matrix.M11, matrix.M21, matrix.M31, matrix.M41, matrix.M12, matrix.M22, matrix.M32, matrix.M42, matrix.M13, matrix.M23, matrix.M33, matrix.M43];
}
