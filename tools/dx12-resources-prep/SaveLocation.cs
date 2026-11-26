using System.Text;

internal static class SaveLocation
{
    private const string Magic = "TESV_SAVEGAME";
    private const int MaximumHeaderBytes = 8192;
    private const int MaximumStringBytes = 512;

    internal static string? LatestInteriorHint()
    {
        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        if (string.IsNullOrWhiteSpace(documents)) return null;
        var folder = Path.Combine(documents, "My Games", "Skyrim Special Edition", "Saves");
        if (!Directory.Exists(folder)) return null;
        var files = new DirectoryInfo(folder).EnumerateFiles("*.ess")
            .Where(file => (file.Attributes & FileAttributes.ReparsePoint) == 0)
            .OrderByDescending(file => file.LastWriteTimeUtc);
        foreach (var file in files)
        {
            try
            {
                using var reader = new BinaryReader(file.Open(FileMode.Open, FileAccess.Read, FileShare.ReadWrite));
                if (Encoding.ASCII.GetString(reader.ReadBytes(Magic.Length)) != Magic) continue;
                var header = reader.ReadUInt32();
                if (header is < 16 or > MaximumHeaderBytes || reader.ReadUInt32() is < 12 or > 20) continue;
                _ = reader.ReadUInt32();
                if (!ReadString(reader)) continue;
                _ = reader.ReadUInt32();
                if (!ReadString(reader)) continue;
                // The structured filename is only a hint; the caller resolves a unique winning CELL.
                var parts = Path.GetFileNameWithoutExtension(file.Name).Split('_');
                if (parts.Length < 8 || parts[4].Length is 0 or > 120 ||
                    !parts[4].All(c => char.IsAsciiLetterOrDigit(c) || c == '_'))
                    return null;
                return parts[4];
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
        return null;
    }

    private static bool ReadString(BinaryReader reader)
    {
        var length = reader.ReadUInt16();
        if (length is 0 or > MaximumStringBytes) return false;
        var bytes = reader.ReadBytes(length);
        return bytes.Length == length && !bytes.Contains((byte)0);
    }
}
