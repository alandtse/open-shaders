using System.Security.Cryptography;
using System.Text.Json;

/// <summary>Persistent SHA-verified geometry/shading cache for the companion.
/// Everything stays beneath its approved work/cells subtree; the game's Data
/// asset files are read only. Published CELL/subcell packages remain immutable.</summary>
internal sealed class SharedMeshCache
{
    private readonly string root, output;
    private readonly JsonSerializerOptions options;
    private const long MaxMetadataBytes = 16 * 1024 * 1024;

    internal SharedMeshCache(string data, string workOutput, JsonSerializerOptions options)
    {
        output = workOutput;
        this.options = options;
        var work = Path.GetFullPath(Path.Combine(data,
            "SKSE", "Plugins", "CommunityShaders", "DX12-cache", "work", "cells"));
        root = Path.GetFullPath(Path.Combine(work, ".shared-mesh-cache"));
        if (!root.StartsWith(work + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Shared mesh cache escaped permitted preparation workspace");
        for (DirectoryInfo? node = new(root); node != null &&
             node.FullName.StartsWith(data, StringComparison.OrdinalIgnoreCase); node = node.Parent)
        {
            if (node.Exists && node.Attributes.HasFlag(FileAttributes.ReparsePoint))
                throw new IOException("Unsafe reparse point in shared mesh cache path");
        }
        Directory.CreateDirectory(root);
    }

    private static bool ValidHash(string? sha) =>
        sha?.Length == 64 && sha.All(char.IsAsciiHexDigit);

    private static bool ValidAsset(string? path, string? digest, string kind, string extension) =>
        ValidHash(digest) && path == kind + "/" + digest + extension;

    private static bool Verified(string path, string expectedHash)
    {
        if (!File.Exists(path) || new FileInfo(path).Attributes.HasFlag(FileAttributes.ReparsePoint)) return false;
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream))
            .Equals(expectedHash, StringComparison.OrdinalIgnoreCase);
    }

    private static void CopyVerified(string sourceRoot, string destinationRoot, string? path, string? hash,
        string kind, string extension)
    {
        if (path == null || hash == null || !ValidAsset(path,hash,kind,extension))
            throw new InvalidDataException("Invalid shared mesh blob identity");
        var source = Path.Combine(sourceRoot,path);
        if (!Verified(source,hash)) throw new InvalidDataException("Shared mesh payload digest mismatch");
        var target = Path.Combine(destinationRoot,path);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        if (!Verified(target,hash)) File.Copy(source,target,true);
    }

    internal bool TryRestore(string compilerKey, string sourceHash)
    {
        if (!ValidHash(compilerKey) || !ValidHash(sourceHash)) return false;
        var meta = Path.Combine(root,"compiled",compilerKey,sourceHash+".json");
        try
        {
            if (!File.Exists(meta) || new FileInfo(meta).Length > MaxMetadataBytes ||
                new FileInfo(meta).Attributes.HasFlag(FileAttributes.ReparsePoint)) return false;
            var compiled = JsonSerializer.Deserialize<CompiledNif>(File.ReadAllText(meta),options);
            if (compiled == null || compiled.SourceSha256 != sourceHash ||
                compiled.CompilerKey != compilerKey || compiled.Status != "classified") return false;
            foreach (var shape in compiled.Shapes)
            {
                if (shape.GeometryFile != null)
                    CopyVerified(root,output,shape.GeometryFile,shape.GeometrySha256,"geometry",".rsgm");
                if (shape.ShadingFile != null)
                    CopyVerified(root,output,shape.ShadingFile,shape.ShadingSha256,"shading",".rsga");
            }
            var target = Path.Combine(output,"compiled",compilerKey,sourceHash+".json");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(meta,target,true);
            return true;
        }
        catch (Exception error) when (error is not OutOfMemoryException) { return false; }
    }

    internal void TryStore(CompiledNif value)
    {
        if (value.Status != "classified" || !ValidHash(value.CompilerKey) ||
            !ValidHash(value.SourceSha256)) return;
        try
        {
            foreach (var shape in value.Shapes)
            {
                if (shape.GeometryFile != null)
                    CopyVerified(output,root,shape.GeometryFile,shape.GeometrySha256,"geometry",".rsgm");
                if (shape.ShadingFile != null)
                    CopyVerified(output,root,shape.ShadingFile,shape.ShadingSha256,"shading",".rsga");
            }
            var target = Path.Combine(root,"compiled",value.CompilerKey,value.SourceSha256+".json");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            var temp = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllText(temp,JsonSerializer.Serialize(value,options));
            File.Move(temp,target,true);
        }
        catch (Exception error) when (error is not OutOfMemoryException) { /* cache miss next time */ }
    }
}
