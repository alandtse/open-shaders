using System.Security.Cryptography;

internal sealed record CellPackEntry(string Sha256, long Offset, long Length);
internal sealed record CellPackDescriptor(string File, string Sha256, long ByteLength, CellPackEntry[] Entries);

/// <summary>One content-verified, 16-byte-aligned stream of original RSGM or RSGA blobs.
/// The JSON layout is the index; identical mesh/shading payloads are stored once per CELL.</summary></summary>
internal static class CellPacks
{
    internal const uint GeometryMagic = 0x50475352; // RSGP
    internal const uint ShadingMagic = 0x50535352; // RSSP
    internal const uint Version = 1;
    internal const long MaximumPackBytes = 512L * 1024 * 1024;

    internal static CellPackDescriptor Write(string output, string filename, string folder, string extension,
        uint magic, IEnumerable<(string Path, string Hash)> sources)
    {
        var unique = sources.DistinctBy(item => item.Hash).OrderBy(item => item.Hash, StringComparer.Ordinal).ToArray();
        if (unique.Length > 65536) throw new InvalidDataException("Prepared pack exceeds entry budget.");
        var entries = new List<CellPackEntry>(unique.Length);
        var destination = System.IO.Path.Combine(output, filename);
        using (var file = File.Create(destination))
        using (var writer = new BinaryWriter(file, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            writer.Write(magic);
            writer.Write(Version);
            writer.Write(checked((uint)unique.Length));
            writer.Write(0u);
            foreach (var item in unique)
            {
                if (item.Hash.Length != 64 || !item.Hash.All(char.IsAsciiHexDigit) ||
                    item.Path != folder + "/" + item.Hash + extension)
                    throw new InvalidDataException("Invalid prepared pack source reference.");
                var sourcePath = System.IO.Path.Combine(output, item.Path);
                using var input = File.OpenRead(sourcePath);
                if (!Convert.ToHexString(SHA256.HashData(input)).Equals(item.Hash, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Prepared pack source SHA-256 mismatch.");
                input.Position = 0;
                var padding = (16 - (file.Position & 15)) & 15;
                for (var i = 0; i < padding; ++i) writer.Write((byte)0);
                var start = file.Position;
                input.CopyTo(file);
                entries.Add(new(item.Hash, start, input.Length));
                if (file.Position > MaximumPackBytes)
                    throw new InvalidDataException("Prepared pack exceeds byte budget.");
            }
            writer.Flush();
        }
        using var verify = File.OpenRead(destination);
        var hash = Convert.ToHexString(SHA256.HashData(verify)).ToLowerInvariant();
        return new(filename, hash, verify.Length, entries.ToArray());
    }
}
