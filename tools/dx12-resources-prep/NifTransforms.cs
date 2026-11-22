using System.Numerics;
using NiflySharp;
using NiflySharp.Blocks;

internal sealed class NifTransforms
{
    private readonly NifFile file;
    private readonly Dictionary<int, List<int>> parents = [];

    public NifTransforms(NifFile nif)
    {
        file = nif;
        for (var index = 0; index < file.Blocks.Count; ++index)
        {
            if (file.Blocks[index] is not NiNode node || node.Children == null) continue;
            foreach (var child in node.Children.Indices)
            {
                if (child == -1) continue;
                if (child < 0 || child >= file.Blocks.Count)
                    throw new InvalidDataException("Invalid NIF child index");
                if (!parents.TryGetValue(child, out var entries)) parents[child] = entries = [];
                entries.Add(index);
            }
        }
        // Multi-root meshes are permitted. Each rigid shape must still
        // resolve through one unambiguous, static NiNode ancestry.
    }

    public Matrix4x4 ForShape(int shapeIndex)
    {
        var accumulated = Matrix4x4.Identity;
        var visited = new HashSet<int>();
        var current = shapeIndex;
        while (true)
        {
            if (!visited.Add(current) || file.Blocks[current] is not NiAVObject node ||
                (node.Controller != null && !node.Controller.IsEmpty()) || (node.Flags_ui & 1) != 0)
                throw new InvalidDataException("Animated or hidden geometry");
            if (current != shapeIndex && node.GetType().Name is not
                ("NiNode" or "BSFadeNode" or "BSMultiBoundNode" or "BSOrderedNode" or "BSFurnitureMarkerNode"))
                throw new InvalidDataException("Unsupported parent transform");
            var r = node.Rotation;
            var s = node.Scale;
            var t = node.Translation;
            var local = new Matrix4x4(
                r.M11 * s, r.M12 * s, r.M13 * s, 0,
                r.M21 * s, r.M22 * s, r.M23 * s, 0,
                r.M31 * s, r.M32 * s, r.M33 * s, 0,
                t.X, t.Y, t.Z, 1);
            if (!PlacementTransform.ToRows(local).All(float.IsFinite) ||
                !Matrix4x4.Invert(local, out _))
                throw new InvalidDataException("Invalid NIF transform");
            accumulated *= local;
            if (!parents.TryGetValue(current, out var entries))
            {
                if (file.Blocks[current] is not NiNode)
                    throw new InvalidDataException("Unsupported NIF root hierarchy");
                break;
            }
            if (entries.Count != 1)
                throw new InvalidDataException("Ambiguous NIF hierarchy");
            current = entries[0];
        }
        if (!PlacementTransform.ToRows(accumulated).All(float.IsFinite))
            throw new InvalidDataException("Non-finite transform");
        return accumulated;
    }
}
