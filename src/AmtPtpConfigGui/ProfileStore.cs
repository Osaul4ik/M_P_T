using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace AmtPtpConfigGui
{
    internal sealed class GuiProfile
    {
        public string Name { get; set; } = "Profile 1";
        public Native.PalmConfig Palm { get; set; }
        public Native.PointerConfig Pointer { get; set; }
        public Native.ScrollConfig Scroll { get; set; }

        public override string ToString() => Name;
    }

    internal static class ProfileStore
    {
        private const int DefaultProfileCount = 3;
        private const string FileName = "profiles.json";
        private static readonly string DirectoryPath = SettingsPaths.DirectoryPath;
        private static readonly string FilePath = Path.Combine(DirectoryPath, FileName);
        private static readonly string TempFilePath = FilePath + ".tmp";
        private static readonly string BackupFilePath = FilePath + ".bak";
        private static readonly string CorruptBackupFilePath = FilePath + ".corrupt";
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            IncludeFields = true
        };

        public static List<GuiProfile> Load()
        {
            SettingsPaths.MigrateLegacyFile(FileName);

            foreach (string candidate in new[] { FilePath, BackupFilePath })
            {
                try
                {
                    if (!File.Exists(candidate))
                        continue;

                    var json = File.ReadAllText(candidate);
                    var profiles = JsonSerializer.Deserialize<List<GuiProfile>>(json, JsonOptions);
                    if (profiles != null && profiles.Count > 0)
                    {
                        Normalize(profiles);
                        return profiles;
                    }
                }
                catch
                {
                    // Try the backup before falling back to defaults. Never
                    // overwrite a corrupt main file merely by starting the GUI.
                }
            }

            PreserveCorruptMainFile();

            var defaults = new List<GuiProfile>();
            for (int i = 1; i <= DefaultProfileCount; i++)
                defaults.Add(CreateDefault($"Profile {i}"));

            try
            {
                Save(defaults);
            }
            catch
            {
                // The GUI can still start with in-memory defaults even when
                // the settings directory is not writable.
            }

            return defaults;
        }

        public static void Save(List<GuiProfile> profiles)
        {
            Directory.CreateDirectory(DirectoryPath);
            Normalize(profiles);
            var json = JsonSerializer.Serialize(profiles, JsonOptions);

            WriteAtomic(FilePath, TempFilePath, BackupFilePath, json);
        }

        private static void WriteAtomic(string destination, string temporary, string backup, string contents)
        {
            try
            {
                using (var stream = new FileStream(
                    temporary, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream))
                {
                    writer.Write(contents);
                    writer.Flush();
                    stream.Flush(true);
                }

                if (File.Exists(destination))
                    File.Replace(temporary, destination, backup, true);
                else
                    File.Move(temporary, destination);
            }
            finally
            {
                // A failed replacement must not leave a stale .tmp file that
                // can be mistaken for a valid profile database later.
                try
                {
                    if (File.Exists(temporary))
                        File.Delete(temporary);
                }
                catch
                {
                }
            }
        }

        private static void PreserveCorruptMainFile()
        {
            try
            {
                if (File.Exists(FilePath))
                    File.Copy(FilePath, CorruptBackupFilePath, true);
            }
            catch
            {
            }
        }

        public static GuiProfile CreateDefault(string name)
        {
            return new GuiProfile
            {
                Name = name,
                Palm = Native.PalmConfig.Default,
                Pointer = Native.PointerConfig.Default,
                Scroll = Native.ScrollConfig.Default,
            };
        }

        private static void Normalize(List<GuiProfile> profiles)
        {
            foreach (var p in profiles)
            {
                if (string.IsNullOrWhiteSpace(p.Name))
                    p.Name = "Profile";
                p.Palm = p.Palm.StructVersion == 0 ? Native.PalmConfig.Default : p.Palm.Clamped();
                if (p.Pointer.StructVersion < Native.PointerConfig.CurrentVersion)
                {
                    var defaults = Native.PointerConfig.Default;
                    var old = p.Pointer;

                    // Version 3 → 4: add Force Touch enable and the
                    // optional pressure gate. Preserve all existing v3
                    // pointer tuning while adopting the new factory
                    // behavior for the newly added fields.
                    if (p.Pointer.StructVersion < 4)
                    {
                        old.ForceTouchEnabled = defaults.ForceTouchEnabled;
                        old.RequirePressureToActivate = defaults.RequirePressureToActivate;
                    }

                    // Version 4 → 5: add non-Force-Touch small-contact
                    // rejection. Preserve existing v4 tuning while adopting
                    // the factory behavior for the new field.
                    if (p.Pointer.StructVersion < 5)
                    {
                        old.SmallContactRejectionEnabled = defaults.SmallContactRejectionEnabled;
                    }

                    // Version 5 → 6: add the optional continuous/strict
                    // small-contact gate. Existing profiles keep it off.
                    if (p.Pointer.StructVersion < 6)
                    {
                        old.SmallContactRejectionStrict = defaults.SmallContactRejectionStrict;
                    }

                    // Version 6 → 7: add the optional continuous Force-Touch
                    // pressure gate. Existing profiles keep it off.
                    if (p.Pointer.StructVersion < 7)
                    {
                        old.RequirePressureContinuously = defaults.RequirePressureContinuously;
                    }

                    // Version 9 → 10: add the GUI-tunable "M" (Major)
                    // small-contact threshold for non-Force-Touch devices
                    // (previously hardcoded at 50), plus the mirrored
                    // small-contact gate + threshold for Force-Touch
                    // devices. Existing profiles adopt the old hardcoded
                    // Major=50 and keep the new Force-Touch gate off.
                    if (p.Pointer.StructVersion < 10)
                    {
                        old.SmallContactMajorThreshold = defaults.SmallContactMajorThreshold;
                        old.ForceTouchSmallContactRejectionEnabled = defaults.ForceTouchSmallContactRejectionEnabled;
                        old.ForceTouchSmallContactRejectionStrict = defaults.ForceTouchSmallContactRejectionStrict;
                        old.ForceTouchSmallContactMajorThreshold = defaults.ForceTouchSmallContactMajorThreshold;
                    }

                    // Version 10 → 11: split the small-contact gates into two
                    // INDEPENDENT mechanisms, each with its own "M" threshold -
                    // birth-only (previously hardcoded Major<80, now
                    // SmallContactBirthMajorThreshold) and continuous (already
                    // tunable since v10). The Enabled/Strict flags no longer
                    // force each other off, so only the two new threshold
                    // fields need an explicit default here - existing profiles
                    // adopt the old hardcoded birth threshold of 80.
                    if (p.Pointer.StructVersion < 11)
                    {
                        old.SmallContactBirthMajorThreshold = defaults.SmallContactBirthMajorThreshold;
                        old.ForceTouchSmallContactBirthMajorThreshold = defaults.ForceTouchSmallContactBirthMajorThreshold;
                    }

                    if (p.Pointer.StructVersion < 3)
                    {
                        old.CursorDeadzoneSlow = defaults.CursorDeadzoneSlow;
                        old.CursorDeadzoneFast = defaults.CursorDeadzoneFast;
                        old.SmoothingAlphaDen = defaults.SmoothingAlphaDen;
                        old.SmoothingAlphaNumSlow = defaults.SmoothingAlphaNumSlow;
                    }

                    p.Pointer = old;
                }
                p.Pointer = p.Pointer.StructVersion == 0 ? Native.PointerConfig.Default : p.Pointer.Clamped();
                if (p.Scroll.StructVersion < Native.ScrollConfig.CurrentVersion)
                {
                    var defaults = Native.ScrollConfig.Default;
                    var old = p.Scroll;
                    old.ScaleNum = defaults.ScaleNum;
                    old.ScaleDen = defaults.ScaleDen;
                    old.ScaleNumFast = defaults.ScaleNumFast;
                    old.ScaleDenFast = defaults.ScaleDenFast;
                    p.Scroll = old;
                }
                p.Scroll = p.Scroll.StructVersion == 0 ? Native.ScrollConfig.Default : p.Scroll.Clamped();
            }
        }
    }
}