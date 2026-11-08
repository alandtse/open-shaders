using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Environments;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

/// <summary>
/// Reads the selected plugin's interior records directly from its on-disk
/// load-order entry. The game's resident CELL registry is not consulted.
/// </summary>
internal static class PluginCellIndex
{
    internal static int Run(string[] args)
    {
        if (args.Length != 4)
        {
            Console.Error.WriteLine("Usage: DX12ResourcesPrep --list-cells <Data directory> <SelectedPlugin.esm> <Plugins.txt> <output.json>");
            return 2;
        }

        try
        {
            var data = Path.GetFullPath(args[0]);
            var selectedName = args[1];
            if (selectedName.Length is < 5 or > 120 ||
                !string.Equals(Path.GetFileName(selectedName), selectedName, StringComparison.Ordinal) ||
                selectedName.IndexOfAny(['/', '\\', ':', '*', '?', '"', '<', '>', '|']) >= 0)
                throw new ArgumentException("Selected plugin must be a filename, not a path.");
            var selected = ModKey.FromFileName(selectedName);
            var pluginsPath = Path.GetFullPath(args[2]);
            var output = Path.GetFullPath(args[3]);
            if (!File.Exists(Path.Combine(data, selected.ToString())))
                throw new FileNotFoundException("Selected plugin file is missing", selected.ToString());

            string[] officialMasters = ["Skyrim.esm", "Update.esm", "Dawnguard.esm",
                "HearthFires.esm", "Dragonborn.esm"];
            var cccPath = Path.Combine(Path.GetDirectoryName(data)!, "Skyrim.ccc");
            var creationClub = File.Exists(cccPath)
                ? File.ReadLines(cccPath).Select(line => line.Trim()).Where(line => line.Length > 0)
                : [];
            var implicitListings = officialMasters.Concat(creationClub)
                .Where(name => File.Exists(Path.Combine(data, name)))
                .Select(ModKey.FromFileName);
            var listings = implicitListings.Concat(
                    PluginListings.LoadOrderListingsFromPath(pluginsPath, GameRelease.SkyrimSE, data)
                        .Where(listing => listing.Enabled).Select(listing => listing.ModKey))
                .Distinct().ToArray();
            if (!listings.Contains(selected))
                throw new InvalidDataException($"Selected plugin is not enabled: {selected}");

            using var environment = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>(GameRelease.SkyrimSE)
                .WithTargetDataFolder(data).WithLoadOrder(listings).Build();
            var listing = environment.LoadOrder.ListedOrder.FirstOrDefault(
                item => item.ModKey == selected);
            if (listing?.Mod is not { } selectedMod)
                throw new InvalidDataException($"Selected plugin was not loaded from disk: {selected}");

            // FormKey retains the defining master and local FormID even for an
            // overriding CELL record. Never use the overriding plugin as owner.
            var records = selectedMod.EnumerateMajorRecords<ICellGetter>()
                .Where(cell => (cell.Flags & Cell.Flag.IsInteriorCell) != 0 && !cell.IsDeleted)
                .GroupBy(cell => cell.FormKey)
                .Select(group => group.Last())
                .Select(cell => new
                {
                    plugin = cell.FormKey.ModKey.ToString(),
                    localId = cell.FormKey.ID,
                    editorId = cell.EditorID ?? "",
                    displayName = cell.Name?.ToString() ?? "",
                    relation = cell.FormKey.ModKey == selected ? "defined" : "override"
                })
                .OrderBy(cell => cell.displayName, StringComparer.OrdinalIgnoreCase)
                .ThenBy(cell => cell.editorId, StringComparer.OrdinalIgnoreCase)
                .ThenBy(cell => cell.plugin, StringComparer.OrdinalIgnoreCase)
                .ThenBy(cell => cell.localId)
                .ToArray();

            var document = new
            {
                kind = "dx12-resources-plugin-interiors",
                schemaVersion = 1,
                selectedPlugin = selected.ToString(),
                interiorCount = records.Length,
                interiors = records
            };
            var bytes = JsonSerializer.SerializeToUtf8Bytes(document);
            if (bytes.Length > 32 * 1024 * 1024)
                throw new InvalidDataException("Plugin interior index exceeds the 32 MiB report budget.");
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            var temp = output + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllBytes(temp, bytes);
                File.Move(temp, output, true);
            }
            finally
            {
                if (File.Exists(temp)) File.Delete(temp);
            }
            Console.WriteLine($"DX12 Resources: {records.Length} interior CELL records in {selected}.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("DX12 Resources plugin scan failed: " + error);
            return 1;
        }
    }
}
