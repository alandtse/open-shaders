// Experimental DXR-only scene geometry displacement; no raster/DDS changes.
// V0 QUICK TEST: static NIF geometry, version-2 RSga only, PBR height red, RMAOS AO blue.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using BCnEncoder.Decoder;

internal static class DxrHeightQuick
{
    internal sealed record Result(string GeometryFile, string GeometrySha, float[] Bounds,
        int VertexCount, int IndexCount, string ShadingStatus, string? ShadingFile,
        string? ShadingSha, string TangentStatus);

    private sealed class V
    {
        public Vector3 P, N, T, B;
        public Vector2 UV;
        public Vector4 C;
        public bool Boundary;
    }

    private sealed class Texture
    {
        public required int Width;
        public required int Height;
        public required byte[] R;
        public required byte[] B;
        public float Sample(Vector2 uv, bool blue)
        {
            // Prototype: repeat wrap, bilinear sample; adjust for materials using clamp.
            float u = uv.X - MathF.Floor(uv.X), v = uv.Y - MathF.Floor(uv.Y);
            float x = u * Width - .5f, y = v * Height - .5f;
            int x0 = (int)MathF.Floor(x), y0 = (int)MathF.Floor(y);
            float tx = x - x0, ty = y - y0;
            byte[] data = blue ? B : R;
            float F(int xx, int yy)
            {
                xx = ((xx % Width) + Width) % Width;
                yy = ((yy % Height) + Height) % Height;
                return data[yy * Width + xx] / 255f;
            }
            return (F(x0,y0) * (1-tx) + F(x0+1,y0)*tx)*(1-ty) +
                   (F(x0,y0+1)*(1-tx) + F(x0+1,y0+1)*tx)*ty;
        }
    }

    private static float Setting(string name, float fallback)
    {
        var s = Environment.GetEnvironmentVariable(name);
        return float.TryParse(s, System.Globalization.NumberStyles.Float,
            System.Globalization.CultureInfo.InvariantCulture, out var v) && float.IsFinite(v) ? v : fallback;
    }
    private static int IntSetting(string name, int fallback)
        => int.TryParse(Environment.GetEnvironmentVariable(name), out var n) ? n : fallback;

    private static string Normalize(string s) => s.Replace('/', '\\').TrimStart('\\').ToLowerInvariant();

    private static string? FindTexture(string output, string source)
    {
        string relative = Normalize(source);
        string root = Environment.GetEnvironmentVariable("DX12_RESOURCES_DXR_HEIGHT_DATA") ?? "";
        if (!relative.StartsWith("textures\\")) relative = "textures\\" + relative;
        if (Directory.Exists(root))
        {
            var loose = Path.Combine(root, relative);
            if (File.Exists(loose)) return loose;
        }
        // Previously generated manifest supports archived DDS from an earlier preparation.
        var manifest = Path.Combine(output, "cell-textures.json");
        if (!File.Exists(manifest)) return null;
        using var doc = JsonDocument.Parse(File.ReadAllText(manifest));
        foreach (var item in doc.RootElement.GetProperty("textures").EnumerateArray())
        {
            if (!item.TryGetProperty("path", out var path) ||
                !Normalize(path.GetString() ?? "").Equals(relative, StringComparison.OrdinalIgnoreCase) ||
                !item.TryGetProperty("status", out var status) || status.GetString() != "cached" ||
                !item.TryGetProperty("file", out var file)) continue;
            var payload = file.GetString();
            if (payload == null || !System.Text.RegularExpressions.Regex.IsMatch(payload,
                    @"^textures/[0-9a-f]{64}\.dds$")) continue;
            var cached = Path.Combine(output, payload);
            if (File.Exists(cached)) return cached;
        }
        return null;
    }

    private static Texture LoadTexture(string filename)
    {
        using var fs = File.OpenRead(filename);
        var decoded = new BcDecoder().Decode2D(fs);
        int w = decoded.Width, h = decoded.Height;
        if (w < 1 || h < 1 || (long)w*h > 16_777_216) throw new InvalidDataException("Oversized DDS");
        var r = new byte[w*h];
        var b = new byte[w*h];
        for (int y=0; y<h; ++y)
        for (int x=0; x<w; ++x)
        {
            var c = decoded.Span[y,x];
            r[y*w+x] = c.r;
            b[y*w+x] = c.b;
        }
        return new Texture { Width=w, Height=h, R=r, B=b };
    }

    public static Result Apply(string output, CompiledMaterial? material,
        (string Path, string Sha256, float[] Bounds) blob, ShadingAttributes shading,
        int inputVertices, int inputIndices)
    {
        var original = new Result(blob.Path, blob.Sha256, blob.Bounds, inputVertices,
            inputIndices, shading.Status, shading.File, shading.Sha256, shading.TangentStatus);
        if ((Setting("DX12_RESOURCES_DXR_HEIGHT_ENABLED", 0) == 0 && Setting("DX12_RESOURCES_DXR_NORMAL_ENABLED", 0) == 0) ||
            material == null || shading.Status != "compiled" ||
            shading.TangentStatus != "compiled:source-basis" || shading.File == null ||
            material.Textures.Length == 0 || string.IsNullOrWhiteSpace(material.Textures[0]))
            return original;

        string diffuse = Normalize(material.Textures[0]);
        string target = Normalize(Environment.GetEnvironmentVariable("DX12_RESOURCES_DXR_HEIGHT_TARGET")
            ?? @"pbr\dlc01\dungeons\castle");
        if (!diffuse.Contains(target, StringComparison.OrdinalIgnoreCase)) return original;
        string heightName = diffuse.EndsWith("_d.dds", StringComparison.OrdinalIgnoreCase)
            ? diffuse[..^6] + "_p.dds" : diffuse[..^4] + "_p.dds";
        string? heightPath = FindTexture(output, heightName);
        if (heightPath == null) Console.WriteLine($"[DXR surface] height absent: {heightName}");

        try
        {
                        Texture? height = Setting("DX12_RESOURCES_DXR_HEIGHT_ENABLED", 0) != 0 && heightPath != null
                ? LoadTexture(heightPath) : null;

            DxrNormalQuick.Field? micro = null;
            if (Setting("DX12_RESOURCES_DXR_NORMAL_ENABLED", 0) != 0 &&
                material.Textures.Length > 1 &&
                !string.IsNullOrWhiteSpace(material.Textures[1]))
            {
                string? normalPath = FindTexture(output, material.Textures[1]);
                if (normalPath != null)
                {
                    try { micro = DxrNormalQuick.Load(normalPath); }
                    catch (Exception e) {
                        Console.WriteLine("[DXR normal] unavailable: " + e.Message);
                    }
                }
            }

            if (height == null && micro == null)
                return original;
            // AO source is optional; never modifies RMAOS/lighting AO.
            string aoName = diffuse.EndsWith("_d.dds", StringComparison.OrdinalIgnoreCase)
                ? diffuse[..^6] + "_rmaos.dds" : diffuse[..^4] + "_rmaos.dds";
            Texture? ao = null;
            var aoPath = FindTexture(output, aoName);
            if (aoPath != null && Setting("DX12_RESOURCES_DXR_AO_SCALE", .5f) != 0)
            {
                try { ao = LoadTexture(aoPath); }
                catch (Exception e) { Console.WriteLine("[DXR height] AO ignored: " + e.Message); }
            }
            var outputData = Build(output, blob.Path, shading.File, material, height, ao, micro);
            Console.WriteLine($"[DXR surface] {diffuse}: {inputIndices/3} -> {outputData.IndexCount/3} triangles; height={(height!=null ? "yes":"no")}; normal={(micro!=null ? "yes":"no")}; AO={(ao!=null ? "yes":"no")}");
            return outputData;
        }
        catch (Exception e)
        {
            Console.WriteLine($"[DXR height] FALLBACK {heightName}: {e.Message}");
            return original;
        }
    }

    private static Result Build(string output, string geometryFile, string shadingFile,
        CompiledMaterial material, Texture? height, Texture? ao, DxrNormalQuick.Field? micro)
    {
        List<V> verts = new();
        List<(int A, int B, int C)> tris = new();
        using (var br = new BinaryReader(File.OpenRead(Path.Combine(output, geometryFile))))
        {
            if (br.ReadUInt32() != 0x4d475352 || br.ReadUInt32() != 1)
                throw new InvalidDataException("Unknown RSGM header");
            int vc = checked((int)br.ReadUInt32()), ic = checked((int)br.ReadUInt32());
            if (vc > 1_000_000 || ic > 3_000_000 || ic%3!=0) throw new InvalidDataException("Geometry budget");
            for (int i=0;i<vc;i++)
                verts.Add(new V { P = new(br.ReadSingle(), br.ReadSingle(), br.ReadSingle()) });
            for (int i=0;i<ic/3;i++)
                tris.Add((checked((int)br.ReadUInt32()), checked((int)br.ReadUInt32()), checked((int)br.ReadUInt32())));
        }
        using (var br = new BinaryReader(File.OpenRead(Path.Combine(output, shadingFile))))
        {
            if (br.ReadUInt32() != 0x41475352 || br.ReadUInt32() != 2)
                throw new InvalidDataException("Requires RSga v2 tangents");
            int n = checked((int)br.ReadUInt32()), stride = checked((int)br.ReadUInt32());
            if (stride != 60 || n != verts.Count) throw new InvalidDataException("RSGM / RSga mismatch");
            foreach (var v in verts)
            {
                v.N = new(br.ReadSingle(), br.ReadSingle(), br.ReadSingle());
                v.UV = new(br.ReadSingle(), br.ReadSingle());
                v.C = new(br.ReadSingle(), br.ReadSingle(), br.ReadSingle(), br.ReadSingle());
                v.T = new(br.ReadSingle(), br.ReadSingle(), br.ReadSingle());
                v.B = new(br.ReadSingle(), br.ReadSingle(), br.ReadSingle());
            }
        }
        foreach (var (a,b,c) in tris)
            if ((uint)a >= verts.Count || (uint)b >= verts.Count || (uint)c >= verts.Count)
                throw new InvalidDataException("Index out of range");

        // Mark source open edges to taper displacement to zero at mesh boundaries.
        var edgeCounts = new Dictionary<ulong,int>();
        static ulong Edge(int a,int b) => ((ulong)(uint)Math.Min(a,b)<<32) | (uint)Math.Max(a,b);
        foreach (var (a,b,c) in tris)
            foreach (var k in new[]{ Edge(a,b), Edge(b,c), Edge(c,a) })
                edgeCounts[k] = edgeCounts.GetValueOrDefault(k)+1;
        var boundaryEdges = edgeCounts.Where(kv => kv.Value == 1).Select(kv => kv.Key).ToHashSet();
        foreach (var k in boundaryEdges)
        {
            verts[(int)(k>>32)].Boundary = true;
            verts[(int)(k&0xffffffff)].Boundary = true;
        }

        int levels = Math.Clamp(IntSetting("DX12_RESOURCES_DXR_SUBDIV", 3), 0, 5);
        int cap = Math.Clamp(IntSetting("DX12_RESOURCES_DXR_TRI_CAP", 600_000), 1000, 3_000_000);
        long count = tris.Count;
        for (int i=0;i<levels;i++) count*=4;
        if (count > cap || count > 3_000_000) throw new InvalidDataException($"Triangle cap {count} > {cap}");

        for (int round=0;round<levels;round++)
        {
            var mids = new Dictionary<ulong,int>();
            var nextBoundaryEdges = new HashSet<ulong>();
            int Mid(int a, int b)
            {
                var key=Edge(a,b);
                if (mids.TryGetValue(key,out var id)) return id;
                var va=verts[a]; var vb=verts[b];
                bool isBoundary = boundaryEdges.Contains(key);
                id=verts.Count;
                verts.Add(new V {
                    P=(va.P+vb.P)*.5f, N=(va.N+vb.N)*.5f,
                    T=(va.T+vb.T)*.5f, B=(va.B+vb.B)*.5f,
                    UV=(va.UV+vb.UV)*.5f, C=(va.C+vb.C)*.5f,
                    Boundary=isBoundary
                });
                if (isBoundary)
                {
                    nextBoundaryEdges.Add(Edge(a,id));
                    nextBoundaryEdges.Add(Edge(id,b));
                }
                mids[key]=id; return id;
            }
            var next=new List<(int A,int B,int C)>(checked(tris.Count*4));
            foreach(var (a,b,c) in tris)
            {
                int ab=Mid(a,b), bc=Mid(b,c), ca=Mid(c,a);
                next.Add((a,ab,ca)); next.Add((ab,b,bc));
                next.Add((ca,bc,c)); next.Add((ab,bc,ca));
            }
            tris=next;
            boundaryEdges=nextBoundaryEdges;
        }

        float scale=Math.Clamp(Setting("DX12_RESOURCES_DXR_HEIGHT_SCALE", 4f),-32f,32f);
        float aoScale=Math.Clamp(Setting("DX12_RESOURCES_DXR_AO_SCALE",.5f),0f,16f);
                float reference=Math.Clamp(Setting("DX12_RESOURCES_DXR_HEIGHT_REF", .5f),0f,1f);
        float microScale=Math.Clamp(Setting("DX12_RESOURCES_DXR_NORMAL_MICRO_SCALE", .5f),0f,8f);
        var uv=material.UvOffsetScale;
        float su=uv.Length>2 ? uv[2] : 1, sv=uv.Length>3 ? uv[3] : 1;
        float ou=uv.Length>0 ? uv[0] : 0, ov=uv.Length>1 ? uv[1] : 0;
        foreach (var v in verts)
        {
            // Since this is a ray-traced-only test, keep ORIGINAL boundary vertices fixed.
            if (v.Boundary) continue;
            float vv = Setting("DX12_RESOURCES_DXR_FLIP_V", 0) != 0 ? 1f-v.UV.Y : v.UV.Y;
            var tc = new Vector2(v.UV.X*su+ou, vv*sv+ov);
            float h=height?.Sample(tc,false) ?? reference;
            float cavity=Math.Clamp((reference-h)*3f,0f,1f);
            float a=ao?.Sample(tc,true) ?? 1f;
                        float d = scale*(h-reference) - aoScale*(1-a)*(1-a)*cavity;
            if (micro != null)
                d += micro.Sample(tc) * microScale;
            if (!float.IsFinite(d)) throw new InvalidDataException("Nonfinite displacement");
            var n=Vector3.Normalize(v.N);
            if (!float.IsFinite(n.X)) throw new InvalidDataException("Invalid vertex normal");
            v.P+=n*d;
        }

        // Rebuild shading normals so the DXR geometry view shows the displaced silhouette.
        var acc = new Vector3[verts.Count];
        foreach(var (a,b,c) in tris)
        {
            Vector3 n=Vector3.Cross(verts[b].P-verts[a].P,verts[c].P-verts[a].P);
            acc[a]+=n; acc[b]+=n; acc[c]+=n;
        }
        for(int i=0;i<verts.Count;i++)
        {
            var v=verts[i]; Vector3 oldN=Vector3.Normalize(v.N);
            var n=acc[i].LengthSquared() > 1e-12f ? Vector3.Normalize(acc[i]) : oldN;
            if (Vector3.Dot(n,oldN)<0) n=-n;
            v.N=n;
            var t=v.T-Vector3.Dot(v.T,n)*n;
            v.T=t.LengthSquared() > 1e-10f ? Vector3.Normalize(t) : Vector3.Normalize(Vector3.Cross(n,Vector3.UnitZ));
            if (!float.IsFinite(v.T.X) || v.T.LengthSquared()<.01f) v.T=Vector3.Normalize(Vector3.Cross(n,Vector3.UnitX));
            float handed=Vector3.Dot(Vector3.Cross(oldN,v.T),v.B)<0 ? -1f : 1f;
            v.B=Vector3.Cross(v.N,v.T)*handed;
        }

        // Same RSGM v1 / RSga v2 wire format; runtime decoder needs no changes.
        using var gb=new MemoryStream(); using(var w=new BinaryWriter(gb, System.Text.Encoding.UTF8, true))
        {
            w.Write(0x4d475352u);w.Write(1u);w.Write((uint)verts.Count);w.Write((uint)(tris.Count*3));
            foreach(var v in verts) {w.Write(v.P.X);w.Write(v.P.Y);w.Write(v.P.Z);}
            foreach(var (a,b,c) in tris) {w.Write((uint)a);w.Write((uint)b);w.Write((uint)c);}
        }
        using var sb=new MemoryStream(); using(var w=new BinaryWriter(sb, System.Text.Encoding.UTF8, true))
        {
            w.Write(0x41475352u);w.Write(2u);w.Write((uint)verts.Count);w.Write(60u);
            foreach(var v in verts)
            {
                foreach(var f in new []{v.N.X,v.N.Y,v.N.Z,v.UV.X,v.UV.Y,v.C.X,v.C.Y,v.C.Z,v.C.W,
                                      v.T.X,v.T.Y,v.T.Z,v.B.X,v.B.Y,v.B.Z})
                    w.Write(f);
            }
        }
        var gh=SaveBlob(output,"geometry", ".rsgm",gb.ToArray());
        var sh=SaveBlob(output,"shading",".rsga",sb.ToArray());
        Vector3 min=new(float.PositiveInfinity), max=new(float.NegativeInfinity);
        foreach(var v in verts) {min=Vector3.Min(min,v.P); max=Vector3.Max(max,v.P);}
        return new Result(gh.Path,gh.Hash,[min.X,min.Y,min.Z,max.X,max.Y,max.Z],
            verts.Count, tris.Count*3,"compiled",sh.Path,sh.Hash,"compiled:source-basis");
    }
    private static (string Path,string Hash) SaveBlob(string output, string folder, string ext,byte[] bytes)
    {
        string hash=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        string rel=$"{folder}/{hash}{ext}", dest=Path.Combine(output,rel);
        Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
        if(!File.Exists(dest)) {File.WriteAllBytes(dest+".tmp",bytes);File.Move(dest+".tmp",dest,true);}
        return(rel,hash);
    }
}
