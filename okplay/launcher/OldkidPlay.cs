// Classic Netplay for AoE2 - launcher for the okplay DirectPlay providers (branded per server via okplay.ini).
//   green checks for DirectPlay + both providers + the oldkid.net server, Install/Repair
//   (one UAC prompt), Launch Game (always un-elevated), logs, uninstall.
// Built with the C# compiler that ships in Windows (.NET Framework 4.x) - C# 5 syntax only.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Management;
using System.Net;
using System.Net.NetworkInformation;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Classic Netplay for AoE2")]
[assembly: AssemblyProduct("Classic Netplay for AoE2")]
[assembly: AssemblyCompany("oldkid.net")]
[assembly: AssemblyVersion("0.2.0.0")]
[assembly: AssemblyFileVersion("0.2.0.0")]

namespace OldkidPlay
{
    static class K
    {
        public const string Version = "0.2.0";
        public const string Project = "Classic Netplay for AoE2";
        public const string OnlineGuid = "{66D8DC59-28E8-4D3E-BA93-2E98D251C0F3}";
        public const string LanGuid = "{6BCFF704-CE20-4150-98B4-5A7198175D7D}";
        // opened through the 32-bit registry view, so these land under WOW6432Node like the game expects
        public const string SpKeyBase = @"SOFTWARE\Microsoft\DirectPlay\Service Providers\";
        public const string AppsKey = @"SOFTWARE\Microsoft\DirectPlay\Applications";
        // Video for Windows codecs (32-bit view): the intro movies are Indeo 5, which Windows no longer has
        public const string Drivers32Key = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32";
        public const string CodecValue = "vidc.IV50";
        public const string CodecDll = "okindeo.dll";
        public const string DefaultServer = "https://oldkid.net/aoe2net/okplay.php";
        public const string DefaultKey = "okp_7Qm2xV9rLs4Hd8Wn";
        public const string FwPrefix = Project + " - ";
        public static readonly string[] OldFwPrefixes = { "Oldkid.net Online Play - ", "Oldkid.net Play - " };
        public static readonly string[] OldShortcutNames = { "Oldkid.net Play.lnk" };
        public static string InstallDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), Project);
        // v0.1 installed here; Install cleans its program files (logs are kept)
        public static string[] OldInstallDirs = { Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Oldkid.net Online Play") };
        // --dryrun-install <dir>: real code paths, but files go to <dir>, the registry to a HKCU
        // test key, shortcuts to <dir>, and dism/netsh are logged instead of run (no admin needed)
        public static bool DryRun;
        public static string DryShortcutDir;
        public static readonly string AppDir = Path.GetDirectoryName(Application.ExecutablePath);
        public static readonly string CfgDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), Project);
        public static readonly string OldCfgDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Oldkid.net Play");

        // Names are per server, from okplay.ini: a self-hoster's players see their names.
        static string brand, onlineName, lanName;
        public static string Brand
        {
            get { return brand ?? (brand = Sys.Ini("brand", Project)); }
            set { brand = value; }
        }
        public static string OnlineName
        {
            get { return onlineName ?? (onlineName = Sys.Ini("online_name", Brand + " Online")); }
            set { onlineName = value; }
        }
        public static string LanName
        {
            get { return lanName ?? (lanName = Sys.Ini("lan_name", Brand + " LAN")); }
            set { lanName = value; }
        }
        public static string LauncherExe { get { return SafeFile(Brand) + ".exe"; } }
        public static string ShortcutName { get { return SafeFile(Brand) + ".lnk"; } }

        public static string SafeFile(string name)
        {
            foreach (var ch in Path.GetInvalidFileNameChars()) name = name.Replace(ch.ToString(), "");
            return name.Trim().Length > 0 ? name.Trim() : Project;
        }
    }

    // ------------------------------------------------------------------ system state

    static class Sys
    {
        public static RegistryKey Hklm32()
        {
            if (K.DryRun)
                return RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32)
                                  .CreateSubKey(@"Software\OldkidPlayDryRun");
            return RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
        }

        /// <summary>1 = enabled, 2 = disabled, 0 = could not tell.</summary>
        public static int DirectPlayState()
        {
            try
            {
                using (var s = new ManagementObjectSearcher(
                    "SELECT InstallState FROM Win32_OptionalFeature WHERE Name='DirectPlay'"))
                {
                    foreach (ManagementObject o in s.Get()) return Convert.ToInt32(o["InstallState"]);
                }
            }
            catch (Exception) { }
            return 0;
        }

        /// <summary>DLL path the provider is registered with, or null if missing/wrong GUID.</summary>
        /// <summary>Registry key names (under Service Providers) whose Guid is the given one.</summary>
        public static List<string> KeysWithGuid(string guid)
        {
            var list = new List<string>();
            using (var root = Hklm32())
            using (var sp = root.OpenSubKey(K.SpKeyBase.TrimEnd('\\')))
            {
                if (sp == null) return list;
                foreach (var n in sp.GetSubKeyNames())
                    using (var k = sp.OpenSubKey(n))
                    {
                        var g = k == null ? null : k.GetValue("Guid") as string;
                        if (g != null && g.Equals(guid, StringComparison.OrdinalIgnoreCase)) list.Add(n);
                    }
            }
            return list;
        }

        /// <summary>DLL path our provider is registered with (found by GUID, whatever its display name).</summary>
        public static string ProviderPath(string guid)
        {
            var keys = KeysWithGuid(guid);
            if (keys.Count == 0) return null;
            using (var root = Hklm32())
            using (var k = root.OpenSubKey(K.SpKeyBase + keys[0]))
                return k == null ? null : k.GetValue("Path") as string;
        }

        public static string FileVersion(string path)
        {
            try
            {
                if (!File.Exists(path)) return null;
                var v = FileVersionInfo.GetVersionInfo(path).FileVersion;
                return string.IsNullOrEmpty(v) ? "0.1.0" : v;   // v0.1 had no version resource
            }
            catch (Exception) { return null; }
        }

        public static string BundledDll()
        {
            var p = Path.Combine(K.AppDir, "okplay.dll");
            return File.Exists(p) ? p : null;
        }

        /// <summary>The Indeo 5 decoder registered with Video for Windows (null = none); file = its DLL
        /// if that exists (bare names live in SysWOW64 for 32-bit programs).</summary>
        public static string IndeoCodec(out string file)
        {
            file = null;
            string v = null;
            try
            {
                using (var root = Hklm32())
                using (var k = root.OpenSubKey(K.Drivers32Key))
                    v = k == null ? null : k.GetValue(K.CodecValue) as string;
                if (string.IsNullOrEmpty(v)) return null;
                var f = Path.IsPathRooted(v) ? v : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), v);
                if (File.Exists(f)) file = f;
            }
            catch (Exception) { }
            return string.IsNullOrEmpty(v) ? null : v;
        }

        public static bool IsOurCodec(string value)
        {
            try { return value != null && Path.GetFileName(value).Equals(K.CodecDll, StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }

        public static bool IsAdmin()
        {
            return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
        }

        public static Process[] GameProcesses()
        {
            var list = new List<Process>();
            foreach (var p in Process.GetProcesses())
            {
                var n = p.ProcessName.ToLowerInvariant();
                if (n.StartsWith("age2") || n.StartsWith("empires2")) list.Add(p);
            }
            return list.ToArray();
        }

        // best first: UserPatch Conquerors, stock Conquerors, Age of Kings
        static readonly string[] GameExeNames = { "age2_x1.5.exe", "age2_x1.exe", "empires2.exe" };

        /// <summary>Game exes found through DirectPlay's lobby registrations, best first. The
        /// registrations name the SafeDisc stub (e.g. "\age2_x1.icd"), so look for the real exes in
        /// each registered folder (and its age2_x1 subfolder) instead of trusting "File".</summary>
        public static List<string> KnownGameExes()
        {
            var dirs = new List<string>();
            try
            {
                using (var root = Hklm32())
                using (var apps = root.OpenSubKey(K.AppsKey))
                {
                    if (apps != null)
                    {
                        foreach (var n in apps.GetSubKeyNames())
                        {
                            using (var k = apps.OpenSubKey(n))
                            {
                                var dir = k.GetValue("Path") as string;
                                var file = (k.GetValue("File") as string ?? "").TrimStart('\\').ToLowerInvariant();
                                if (dir == null || !(file.StartsWith("age2") || file.StartsWith("empires2"))) continue;
                                if (!dirs.Contains(dir)) dirs.Add(dir);
                                var sub = Path.Combine(dir, "age2_x1");
                                if (Directory.Exists(sub) && !dirs.Contains(sub)) dirs.Add(sub);
                            }
                        }
                    }
                }
            }
            catch (Exception) { }
            var found = new List<string>();
            foreach (var name in GameExeNames)
                foreach (var d in dirs)
                {
                    var full = Path.Combine(d, name);
                    if (File.Exists(full) && !found.Contains(full)) found.Add(full);
                }
            return found;
        }

        public static string Ini(string key, string def)
        {
            foreach (var path in new[] { Path.Combine(K.AppDir, "okplay.ini"), Path.Combine(K.InstallDir, "okplay.ini") })
            {
                try
                {
                    if (!File.Exists(path)) continue;
                    foreach (var raw in File.ReadAllLines(path))
                    {
                        var line = raw.Trim();
                        if (line.StartsWith(";") || !line.Contains("=")) continue;
                        var i = line.IndexOf('=');
                        if (line.Substring(0, i).Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                            return line.Substring(i + 1).Trim();
                    }
                }
                catch (Exception) { }
            }
            return def;
        }

        /// <summary>Returns "ok|<rooms>|<your ip>" or "fail|<reason>".</summary>
        public static string ServerStatus()
        {
            try
            {
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2
                var server = Ini("server", K.DefaultServer);
                var body = Encoding.ASCII.GetBytes("k=" + Uri.EscapeDataString(Ini("key", K.DefaultKey)) + "&a=list");
                var req = (HttpWebRequest)WebRequest.Create(server);
                req.Method = "POST";
                req.Timeout = 6000;
                req.ContentType = "application/x-www-form-urlencoded";
                req.UserAgent = "ClassicNetplay/" + K.Version;
                using (var s = req.GetRequestStream()) s.Write(body, 0, body.Length);
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var rd = new StreamReader(resp.GetResponseStream()))
                {
                    var lines = rd.ReadToEnd().Split('\n');
                    if (lines.Length == 0 || !lines[0].StartsWith("ok")) return "fail|unexpected reply";
                    int rooms = 0;
                    foreach (var l in lines) if (l.StartsWith("room ")) rooms++;
                    var parts = lines[0].Trim().Split(' ');
                    return "ok|" + rooms + "|" + (parts.Length > 1 ? parts[1] : "");
                }
            }
            catch (Exception e) { return "fail|" + e.Message; }
        }
    }

    // ------------------------------------------------------------------ config

    static class Cfg
    {
        static string FilePath { get { return Path.Combine(K.CfgDir, "launcher.cfg"); } }

        public static string Get(string key)
        {
            foreach (var f in new[] { FilePath, Path.Combine(K.OldCfgDir, "launcher.cfg") })
            {
                try
                {
                    if (!File.Exists(f)) continue;
                    foreach (var l in File.ReadAllLines(f))
                        if (l.StartsWith(key + "=")) return l.Substring(key.Length + 1);
                }
                catch (Exception) { }
            }
            return null;
        }

        public static void Set(string key, string value)
        {
            try
            {
                Directory.CreateDirectory(K.CfgDir);
                var lines = new List<string>();
                if (File.Exists(FilePath))
                    foreach (var l in File.ReadAllLines(FilePath)) if (!l.StartsWith(key + "=")) lines.Add(l);
                lines.Add(key + "=" + value);
                File.WriteAllLines(FilePath, lines.ToArray());
            }
            catch (Exception) { }
        }
    }

    // ------------------------------------------------------------------ install / uninstall (elevated)

    static class Setup
    {
        public static bool Install(string gameExe, bool hideOthers, Action<string> log)
        {
            // 1. DirectPlay
            int dp = K.DryRun ? 2 : Sys.DirectPlayState();   // dry run: exercise the enable path
            if (dp == 1) log("DirectPlay is enabled");
            else
            {
                log("Turning on the Windows DirectPlay feature (can take a minute)...");
                int rc = Run("dism.exe", "/online /enable-feature /featurename:DirectPlay /all /norestart /quiet");
                if (rc == 0) log("DirectPlay enabled");
                else if (rc == 3010) log("DirectPlay enabled - Windows wants a restart before games can use it");
                else { log("ERROR: enabling DirectPlay failed (dism exit " + rc + ")"); return false; }
            }

            // 2. files
            if (Sys.GameProcesses().Length > 0)
            {
                log("ERROR: Age of Empires II is running. Close the game, then click Install / Repair again.");
                return false;
            }
            Directory.CreateDirectory(K.InstallDir);
            var logs = Path.Combine(K.InstallDir, "logs");
            Directory.CreateDirectory(logs);
            bool fromInstallDir = string.Equals(Path.GetFullPath(K.AppDir).TrimEnd('\\'),
                                                Path.GetFullPath(K.InstallDir).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
            if (!fromInstallDir && !File.Exists(Path.Combine(K.AppDir, "okplay.dll")))
            {
                log("ERROR: okplay.dll is not next to the launcher. Unzip the whole folder first, " +
                    "then run " + K.LauncherExe + " from the unzipped folder.");
                return false;
            }
            if (!fromInstallDir)
            {
                try
                {
                    Copy("okplay.dll", "okplay.dll");
                    var ini = Path.Combine(K.InstallDir, "okplay.ini");
                    if (File.Exists(ini)) File.Copy(ini, ini + ".previous", true);
                    Copy("okplay.ini", "okplay.ini");
                    Copy("README.txt", "README.txt");
                    CopyAny(new[] { "dptest.exe", @"support\dptest.exe" }, "dptest.exe");
                    CopyAny(new[] { K.CodecDll }, K.CodecDll);
                    CopyAny(new[] { "vfwprobe.exe", @"support\vfwprobe.exe" }, "vfwprobe.exe");
                    File.Copy(Application.ExecutablePath, Path.Combine(K.InstallDir, K.LauncherExe), true);
                    foreach (var f in Directory.GetFiles(K.InstallDir, "*.exe"))   // a launcher left from an older brand
                        if (!f.EndsWith(K.LauncherExe, StringComparison.OrdinalIgnoreCase) &&
                            !Path.GetFileName(f).Equals("dptest.exe", StringComparison.OrdinalIgnoreCase) &&
                            !Path.GetFileName(f).Equals("vfwprobe.exe", StringComparison.OrdinalIgnoreCase))
                            try { File.Delete(f); } catch (Exception) { }
                }
                catch (IOException e)
                {
                    log("ERROR: could not copy files (" + e.Message + "). Close the game and try again.");
                    return false;
                }
                // v0.1 scripts are replaced by this launcher
                foreach (var old in new[] { "collect-logs.cmd", "collect-logs.ps1", "uninstall.cmd", "uninstall.ps1", "install.cmd", "install.ps1" })
                {
                    try { File.Delete(Path.Combine(K.InstallDir, old)); } catch (Exception) { }
                }
                log("Files copied to " + K.InstallDir);
            }
            else log("Running from " + K.InstallDir + " - files already in place");
            foreach (var old in K.OldInstallDirs)
            {
                if (!Directory.Exists(old) || SamePath(old, K.InstallDir)) continue;
                foreach (var f in Directory.GetFiles(old)) try { File.Delete(f); } catch (Exception) { }
                log("Removed the old program files from " + old + " (its logs folder is kept)");
            }

            LockDown(K.InstallDir, log);
            // the game runs as a normal user and writes its logs here
            try
            {
                var sec = Directory.GetAccessControl(logs);
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
                    FileSystemRights.Modify, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                    PropagationFlags.None, AccessControlType.Allow));
                Directory.SetAccessControl(logs, sec);
            }
            catch (Exception e) { log("warning: could not set log folder permissions: " + e.Message); }

            // 3. DirectPlay registrations (both providers point at the same DLL)
            var dll = Path.Combine(K.InstallDir, "okplay.dll");
            using (var root = Sys.Hklm32())
            {
                Register(K.OnlineName, K.OnlineGuid, dll);
                Register(K.LanName, K.LanGuid, dll);
            }
            log("Registered '" + K.OnlineName + "' and '" + K.LanName + "' with DirectPlay");
            if (hideOthers) HideOthers(log);
            InstallVideoFix(log);

            // 4. firewall: allow the game to receive UDP (hole punching, LAN discovery)
            var exes = new List<string>();
            if (!string.IsNullOrEmpty(gameExe) && File.Exists(gameExe)) exes.Add(gameExe);
            foreach (var e in Sys.KnownGameExes()) if (!exes.Contains(e)) exes.Add(e);
            foreach (var exe in exes)
            {
                var file = Path.GetFileName(exe);
                foreach (var pre in K.OldFwPrefixes)
                    Run("netsh.exe", "advfirewall firewall delete rule name=\"" + pre + file + "\"");
                Run("netsh.exe", "advfirewall firewall delete rule name=\"" + K.FwPrefix + file + "\"");
                int rc = Run("netsh.exe", "advfirewall firewall add rule name=\"" + K.FwPrefix + file +
                             "\" dir=in action=allow program=\"" + exe + "\" protocol=UDP profile=any enable=yes");
                log(rc == 0 ? "Firewall: allowed " + exe : "warning: firewall rule for " + exe + " failed (" + rc + ")");
            }
            if (exes.Count == 0) log("warning: no game exe known yet - Windows may ask to allow the game through the firewall (click Allow)");

            // 5. shortcuts to the installed launcher
            try
            {
                var target = Path.Combine(K.InstallDir, K.LauncherExe);
                var desk = K.DryRun ? K.DryShortcutDir : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                var menu = K.DryRun ? K.DryShortcutDir : Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);
                foreach (var dir in new[] { desk, menu })
                    foreach (var old in K.OldShortcutNames)
                        if (!old.Equals(K.ShortcutName, StringComparison.OrdinalIgnoreCase))
                            try { File.Delete(Path.Combine(dir, old)); } catch (Exception) { }
                Shortcut(Path.Combine(desk, K.ShortcutName), target);
                Shortcut(Path.Combine(menu, K.DryRun ? "Start menu - " + K.ShortcutName : K.ShortcutName), target);
                log("Shortcuts: Desktop and Start menu");
            }
            catch (Exception e) { log("warning: shortcut: " + e.Message); }
            return true;
        }

        public static bool Uninstall(Action<string> log)
        {
            UnhideOthers(log);   // put Windows' own connection types back first
            using (var root = Sys.Hklm32())
                foreach (var guid in new[] { K.OnlineGuid, K.LanGuid })
                    foreach (var name in Sys.KeysWithGuid(guid))
                        try { root.DeleteSubKeyTree(K.SpKeyBase + name, false); } catch (Exception) { }
            log("Removed DirectPlay registrations");
            RemoveVideoFix(log);
            var exes = Sys.KnownGameExes();
            var cfgGame = Cfg.Get("game");
            if (cfgGame != null && !exes.Contains(cfgGame)) exes.Add(cfgGame);
            foreach (var exe in exes)
            {
                Run("netsh.exe", "advfirewall firewall delete rule name=\"" + K.FwPrefix + Path.GetFileName(exe) + "\"");
                foreach (var pre in K.OldFwPrefixes)
                    Run("netsh.exe", "advfirewall firewall delete rule name=\"" + pre + Path.GetFileName(exe) + "\"");
            }
            log("Removed firewall rules");
            var lnks = new List<string>();
            if (K.DryRun)
            {
                lnks.Add(Path.Combine(K.DryShortcutDir, K.ShortcutName));
                lnks.Add(Path.Combine(K.DryShortcutDir, "Start menu - " + K.ShortcutName));
            }
            else
            {
                foreach (var dir in new[] { Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                                            Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms) })
                {
                    lnks.Add(Path.Combine(dir, K.ShortcutName));
                    foreach (var old in K.OldShortcutNames) lnks.Add(Path.Combine(dir, old));
                }
            }
            foreach (var lnk in lnks)
            {
                try { File.Delete(lnk); } catch (Exception) { }
            }
            if (Directory.Exists(K.InstallDir))
            {
                foreach (var f in Directory.GetFiles(K.InstallDir))
                {
                    if (string.Equals(f, Application.ExecutablePath, StringComparison.OrdinalIgnoreCase)) continue;
                    try { File.Delete(f); } catch (Exception) { }
                }
            }
            log("Removed program files (logs kept in " + Path.Combine(K.InstallDir, "logs") + ")");
            log("The Windows DirectPlay feature was left on.");
            return true;
        }

        // ---- the game's connection list shows every registered DirectPlay provider. "Hiding" moves
        // Windows' own entries (TCP/IP, IPX, Modem, Serial) intact to a backup key that DirectPlay
        // never reads; unhide/uninstall copies them back exactly (values, kinds, sub-keys).
        public const string HiddenBase = @"SOFTWARE\Classic Netplay for AoE2\Hidden DirectPlay Providers";

        static bool IsOurs(string guid)
        {
            return guid != null && (guid.Equals(K.OnlineGuid, StringComparison.OrdinalIgnoreCase) ||
                                    guid.Equals(K.LanGuid, StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>Providers visible in games that are not ours.</summary>
        public static List<string> ForeignProviders()
        {
            var list = new List<string>();
            using (var root = Sys.Hklm32())
            using (var sp = root.OpenSubKey(K.SpKeyBase.TrimEnd('\\')))
            {
                if (sp == null) return list;
                foreach (var n in sp.GetSubKeyNames())
                    using (var k = sp.OpenSubKey(n))
                        if (k != null && !IsOurs(k.GetValue("Guid") as string)) list.Add(n);
            }
            return list;
        }

        public static List<string> HiddenProviders()
        {
            var list = new List<string>();
            using (var root = Sys.Hklm32())
            using (var h = root.OpenSubKey(HiddenBase))
                if (h != null) list.AddRange(h.GetSubKeyNames());
            return list;
        }

        static void CopyKey(RegistryKey src, RegistryKey dst)
        {
            foreach (var v in src.GetValueNames())
                dst.SetValue(v, src.GetValue(v, null, RegistryValueOptions.DoNotExpandEnvironmentNames), src.GetValueKind(v));
            foreach (var sub in src.GetSubKeyNames())
                using (var s = src.OpenSubKey(sub))
                using (var d = dst.CreateSubKey(sub))
                    CopyKey(s, d);
        }

        public static int HideOthers(Action<string> log)
        {
            int n = 0;
            using (var root = Sys.Hklm32())
            {
                foreach (var name in ForeignProviders())
                {
                    using (var src = root.OpenSubKey(K.SpKeyBase + name))
                    using (var dst = root.CreateSubKey(HiddenBase + "\\" + name))
                        CopyKey(src, dst);
                    // only remove the original once the backup demonstrably holds it
                    using (var chk = root.OpenSubKey(HiddenBase + "\\" + name))
                    using (var orig = root.OpenSubKey(K.SpKeyBase + name))
                    {
                        if (chk == null || chk.ValueCount != orig.ValueCount || chk.SubKeyCount != orig.SubKeyCount)
                        {
                            log("warning: backup of '" + name + "' incomplete - left it visible");
                            continue;
                        }
                    }
                    root.DeleteSubKeyTree(K.SpKeyBase + name);
                    log("Hid '" + name + "' from the game's connection list");
                    n++;
                }
            }
            if (n == 0) log("Only " + K.Brand + " connections are listed in games");
            return n;
        }

        public static int UnhideOthers(Action<string> log)
        {
            int n = 0;
            using (var root = Sys.Hklm32())
            {
                foreach (var name in HiddenProviders())
                {
                    using (var existing = root.OpenSubKey(K.SpKeyBase + name))
                    {
                        if (existing == null)
                        {
                            using (var src = root.OpenSubKey(HiddenBase + "\\" + name))
                            using (var dst = root.CreateSubKey(K.SpKeyBase + name))
                                CopyKey(src, dst);
                            log("Restored '" + name + "' to the game's connection list");
                        }
                        else log("'" + name + "' was already back (Windows re-added it)");
                    }
                    root.DeleteSubKeyTree(HiddenBase + "\\" + name);
                    n++;
                }
                try { root.DeleteSubKeyTree(@"SOFTWARE\Classic Netplay for AoE2", false); } catch (Exception) { }
            }
            return n;
        }

        static bool SamePath(string a, string b)
        {
            return string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'),
                                 StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>Program folder: everyone may read, only admins may add or change files. The video
        /// decoder is loaded by any program that plays Indeo video, so nothing may be planted next to it
        /// (ProgramData would otherwise let every user create files here). logs\ is opened up after this.</summary>
        static void LockDown(string dir, Action<string> log)
        {
            try
            {
                var inh = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
                var sec = new DirectorySecurity();
                sec.SetAccessRuleProtection(true, false);
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                    FileSystemRights.FullControl, inh, PropagationFlags.None, AccessControlType.Allow));
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
                    FileSystemRights.FullControl, inh, PropagationFlags.None, AccessControlType.Allow));
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
                    FileSystemRights.ReadAndExecute, inh, PropagationFlags.None, AccessControlType.Allow));
                if (K.DryRun)   // the unelevated test still has to write and clean up its folder
                    sec.AddAccessRule(new FileSystemAccessRule(WindowsIdentity.GetCurrent().User,
                        FileSystemRights.FullControl, inh, PropagationFlags.None, AccessControlType.Allow));
                Directory.SetAccessControl(dir, sec);
            }
            catch (Exception e) { log("warning: could not set program folder permissions: " + e.Message); }
        }

        /// <summary>Intro movies: register our Indeo 5 decoder with Video for Windows, unless a working
        /// one is already there (e.g. Intel's own). No game file is touched.</summary>
        public static void InstallVideoFix(Action<string> log)
        {
            var ours = Path.Combine(K.InstallDir, K.CodecDll);
            if (!File.Exists(ours)) { log("Intro videos: " + K.CodecDll + " is not in this package - skipped"); return; }
            string file;
            var cur = Sys.IndeoCodec(out file);
            if (cur != null && file != null && !Sys.IsOurCodec(cur))
            {
                log("Intro videos: an Indeo 5 decoder is already installed (" + cur + ") - left as it is");
                return;
            }
            using (var root = Sys.Hklm32())
            using (var k = root.CreateSubKey(K.Drivers32Key))
                k.SetValue(K.CodecValue, ours, RegistryValueKind.String);
            log(cur != null && !Sys.IsOurCodec(cur)
                ? "Intro videos: replaced a broken Indeo 5 registration (" + cur + ", file missing) with " + K.CodecDll
                : "Intro videos: Indeo 5 decoder installed - the intro movies now play with picture");
        }

        public static void RemoveVideoFix(Action<string> log)
        {
            string file;
            var cur = Sys.IndeoCodec(out file);
            if (!Sys.IsOurCodec(cur)) return;   // someone else's decoder stays
            using (var root = Sys.Hklm32())
            using (var k = root.OpenSubKey(K.Drivers32Key, true))
                if (k != null) k.DeleteValue(K.CodecValue, false);
            log("Removed the Indeo 5 video decoder");
        }

        /// <summary>One key per GUID: drop any copy under another name (a renamed server, v0.1), then (re)write.</summary>
        static void Register(string name, string guid, string dll)
        {
            using (var root = Sys.Hklm32())
            {
                foreach (var old in Sys.KeysWithGuid(guid))
                    if (!old.Equals(name, StringComparison.OrdinalIgnoreCase)) root.DeleteSubKeyTree(K.SpKeyBase + old, false);
                RegisterKey(root, name, guid, dll);
            }
        }

        static void RegisterKey(RegistryKey root, string name, string guid, string dll)
        {
            using (var k = root.CreateSubKey(K.SpKeyBase + name))
            {
                k.SetValue("DescriptionA", name, RegistryValueKind.String);
                k.SetValue("DescriptionW", name, RegistryValueKind.String);
                k.SetValue("Guid", guid, RegistryValueKind.String);
                k.SetValue("Path", dll, RegistryValueKind.String);
                k.SetValue("dwReserved1", 50, RegistryValueKind.DWord);
                k.SetValue("dwReserved2", 0, RegistryValueKind.DWord);
            }
        }

        static void Copy(string from, string to)
        {
            File.Copy(Path.Combine(K.AppDir, from), Path.Combine(K.InstallDir, to), true);
        }

        static void CopyAny(string[] froms, string to)
        {
            foreach (var f in froms)
                if (File.Exists(Path.Combine(K.AppDir, f))) { Copy(f, to); return; }
        }

        public static int Run(string exe, string args)
        {
            if (K.DryRun) { DryLog("RUN " + exe + " " + args); return 0; }
            try
            {
                var psi = new ProcessStartInfo(exe, args);
                psi.UseShellExecute = false;
                psi.CreateNoWindow = true;
                psi.RedirectStandardOutput = true;
                psi.RedirectStandardError = true;
                using (var p = Process.Start(psi))
                {
                    p.StandardOutput.ReadToEnd();
                    p.StandardError.ReadToEnd();
                    p.WaitForExit();
                    return p.ExitCode;
                }
            }
            catch (Exception) { return -1; }
        }

        public static Action<string> DryLog = delegate { };

        static void Shortcut(string lnk, string target)
        {
            var t = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(t);
            object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
            var st = sc.GetType();
            st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { Path.GetDirectoryName(target) });
            st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { "Age of Empires II online & LAN play" });
            st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
        }
    }

    // ------------------------------------------------------------------ diagnostics zip

    static class Diag
    {
        public static string Collect()
        {
            var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var work = Path.Combine(Path.GetTempPath(), "okplay-diag-" + stamp);
            Directory.CreateDirectory(work);
            int n = 0;
            var dirList = new List<string> { Path.Combine(K.InstallDir, "logs"), Path.Combine(K.AppDir, "logs"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"okplay\logs") };
            foreach (var old in K.OldInstallDirs) dirList.Add(Path.Combine(old, "logs"));
            var dirs = dirList.ToArray();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var d in dirs)
            {
                if (!Directory.Exists(d) || !seen.Add(Path.GetFullPath(d))) continue;
                var logs = new List<FileInfo>(new DirectoryInfo(d).GetFiles("okplay-*.log"));
                logs.Sort(delegate (FileInfo a, FileInfo b) { return b.LastWriteTime.CompareTo(a.LastWriteTime); });
                for (int i = 0; i < logs.Count && i < 20; i++) { logs[i].CopyTo(Path.Combine(work, logs[i].Name), true); n++; }
                foreach (var dmp in new DirectoryInfo(d).GetFiles("okplay-crash-*.dmp"))
                    if (dmp.LastWriteTime > DateTime.Now.AddDays(-7)) { dmp.CopyTo(Path.Combine(work, dmp.Name), true); n++; }
            }

            var sb = new StringBuilder();
            sb.AppendLine(K.Brand + " launcher (" + K.Project + ") " + K.Version + " on " + Environment.MachineName + " at " + DateTime.Now.ToString("s"));
            sb.AppendLine("windows: " + Environment.OSVersion.VersionString + (Environment.Is64BitOperatingSystem ? " (64-bit)" : ""));
            sb.AppendLine("DirectPlay feature state: " + Sys.DirectPlayState() + " (1 = enabled)");
            sb.AppendLine("game: " + (Cfg.Get("game") ?? "(not set)"));
            foreach (var e in Sys.KnownGameExes()) sb.AppendLine("known game exe: " + e);
            sb.AppendLine();
            sb.AppendLine("--- DirectPlay service providers (32-bit view)");
            using (var root = Sys.Hklm32())
            using (var sp = root.OpenSubKey(K.SpKeyBase.TrimEnd('\\')))
            {
                if (sp != null)
                    foreach (var name in sp.GetSubKeyNames())
                        using (var k = sp.OpenSubKey(name))
                        {
                            var path = k.GetValue("Path") as string;
                            sb.AppendLine(name + " | " + k.GetValue("Guid") + " | " + path +
                                          (path != null && Path.IsPathRooted(path) ? " | v" + (Sys.FileVersion(path) ?? "missing") : ""));
                        }
            }
            sb.AppendLine();
            foreach (var ini in new[] { Path.Combine(K.InstallDir, "okplay.ini"), Path.Combine(K.AppDir, "okplay.ini") })
            {
                if (!File.Exists(ini)) continue;
                sb.AppendLine("--- " + ini);
                sb.AppendLine(File.ReadAllText(ini));
            }
            sb.AppendLine("--- server: " + Sys.ServerStatus());
            string codecFile;
            var codec = Sys.IndeoCodec(out codecFile);
            sb.AppendLine("--- intro video decoder (" + K.CodecValue + "): " + (codec ?? "(none)") +
                          (codec != null ? (codecFile != null ? " [file ok]" : " [FILE MISSING]") : ""));
            var probe = Path.Combine(K.InstallDir, "vfwprobe.exe");
            var gameExe = Cfg.Get("game");
            if (File.Exists(probe) && gameExe != null)
            {
                // the Avi folder sits next to the game, or one level up (UserPatch's age2_x1 subfolder)
                var gdir = Path.GetDirectoryName(gameExe);
                foreach (var avi in new[] { Path.Combine(gdir, @"Avi\MSLOGO.AVI"), Path.Combine(gdir, @"..\Avi\MSLOGO.AVI") })
                {
                    if (!File.Exists(avi)) continue;
                    try
                    {
                        var psi = new ProcessStartInfo(probe, "\"" + Path.GetFullPath(avi) + "\"");
                        psi.UseShellExecute = false; psi.CreateNoWindow = true; psi.RedirectStandardOutput = true;
                        using (var p = Process.Start(psi)) { sb.Append(p.StandardOutput.ReadToEnd()); p.WaitForExit(10000); }
                    }
                    catch (Exception e) { sb.AppendLine("vfwprobe failed: " + e.Message); }
                    break;
                }
            }
            sb.AppendLine();
            sb.AppendLine("--- network adapters (up)");
            foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up) continue;
                var addrs = new List<string>();
                foreach (var ua in nic.GetIPProperties().UnicastAddresses) addrs.Add(ua.Address.ToString());
                sb.AppendLine(nic.Name + " [" + nic.NetworkInterfaceType + "] " + string.Join(", ", addrs.ToArray()));
            }
            var dpt = Path.Combine(K.InstallDir, "dptest.exe");
            if (File.Exists(dpt))
            {
                sb.AppendLine();
                sb.AppendLine("--- dptest conns");
                try
                {
                    var psi = new ProcessStartInfo(dpt, "conns");
                    psi.UseShellExecute = false; psi.CreateNoWindow = true; psi.RedirectStandardOutput = true;
                    using (var p = Process.Start(psi)) { sb.Append(p.StandardOutput.ReadToEnd()); p.WaitForExit(5000); }
                }
                catch (Exception e) { sb.AppendLine("failed: " + e.Message); }
            }
            File.WriteAllText(Path.Combine(work, "system.txt"), sb.ToString());

            var zip = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                                   "okplay-diagnostics-" + Environment.MachineName + "-" + stamp + ".zip");
            ZipFile.CreateFromDirectory(work, zip);
            Directory.Delete(work, true);
            return zip + "|" + n;
        }
    }

    // ------------------------------------------------------------------ UI

    class StatusRow
    {
        public Label Icon, Name, Detail;
        public void Set(int level, string detail)   // 0 ok, 1 warn, 2 bad, 3 pending
        {
            Icon.Text = level == 0 ? "✔" : level == 1 ? "▲" : level == 2 ? "✖" : "…";
            Icon.ForeColor = level == 0 ? Color.FromArgb(46, 125, 50) : level == 1 ? Color.FromArgb(184, 134, 11)
                           : level == 2 ? Color.FromArgb(198, 40, 40) : Color.Gray;
            Detail.Text = detail;
        }
    }

    class MainForm : Form
    {
        public readonly StatusRow RowDp, RowOnline, RowLan, RowServer, RowVideo;
        public readonly Label GameLabel, Hint;
        public readonly Button ChangeGame, InstallBtn, LaunchBtn;
        public readonly LinkLabel LinkLogs, LinkCollect, LinkUninstall;
        public readonly CheckBox HideBox;
        public readonly Label HideNote;
        bool settingHideBox;
        string game;
        bool dpOk, onlineOk, lanOk, needsUpdate;

        public MainForm()
        {
            Text = K.Brand;
            Font = new Font("Segoe UI", 9.75f);
            BackColor = Color.White;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            try { Icon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }

            var root = new TableLayoutPanel();
            root.ColumnCount = 1;
            root.AutoSize = true;
            root.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            root.Padding = new Padding(18, 14, 18, 12);
            root.Dock = DockStyle.Fill;
            Controls.Add(root);

            // header
            var title = new Label();
            title.Text = K.Brand;
            title.Font = new Font("Segoe UI Semibold", 16f);
            title.AutoSize = true;
            title.Margin = new Padding(0, 0, 0, 0);
            root.Controls.Add(title);
            var sub = new Label();
            sub.Text = "Age of Empires II online and LAN play  ·  v" + K.Version;
            sub.ForeColor = Color.DimGray;
            sub.AutoSize = true;
            sub.Margin = new Padding(2, 0, 0, 12);
            root.Controls.Add(sub);

            // game picker
            var gameRow = new TableLayoutPanel();
            gameRow.ColumnCount = 3;
            gameRow.AutoSize = true;
            gameRow.Dock = DockStyle.Fill;
            gameRow.Margin = new Padding(0, 0, 0, 10);
            gameRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            gameRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 340));
            gameRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            var gl = new Label();
            gl.Text = "Game:";
            gl.Font = new Font(Font, FontStyle.Bold);
            gl.AutoSize = true;
            gl.Anchor = AnchorStyles.Left;
            gameRow.Controls.Add(gl, 0, 0);
            GameLabel = new Label();
            GameLabel.AutoEllipsis = true;
            GameLabel.Dock = DockStyle.Fill;
            GameLabel.TextAlign = ContentAlignment.MiddleLeft;
            gameRow.Controls.Add(GameLabel, 1, 0);
            ChangeGame = new Button();
            ChangeGame.Text = "Change…";
            ChangeGame.AutoSize = true;
            ChangeGame.Click += delegate { PickGame(); };
            gameRow.Controls.Add(ChangeGame, 2, 0);
            root.Controls.Add(gameRow);

            // status rows
            var grid = new TableLayoutPanel();
            grid.ColumnCount = 3;
            grid.AutoSize = true;
            grid.Dock = DockStyle.Fill;
            grid.Margin = new Padding(0, 0, 0, 12);
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 34));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 200));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 242));
            RowDp = AddRow(grid, "DirectPlay (Windows)");
            RowOnline = AddRow(grid, K.OnlineName);
            RowLan = AddRow(grid, K.LanName);
            RowServer = AddRow(grid, "oldkid.net server");
            RowVideo = AddRow(grid, "Intro videos");
            root.Controls.Add(grid);

            HideBox = new CheckBox();
            HideBox.Text = "Only show " + K.Brand + " connections in the game";
            HideBox.AutoSize = true;
            HideBox.Margin = new Padding(2, 0, 0, 0);
            HideBox.CheckedChanged += delegate { HideBoxChanged(); };
            root.Controls.Add(HideBox);
            HideNote = new Label();
            HideNote.AutoSize = true;
            HideNote.MaximumSize = new Size(470, 0);
            HideNote.ForeColor = Color.DimGray;
            HideNote.Margin = new Padding(20, 0, 0, 12);
            root.Controls.Add(HideNote);

            // actions
            var actions = new TableLayoutPanel();
            actions.ColumnCount = 2;
            actions.AutoSize = true;
            actions.Dock = DockStyle.Fill;
            actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            InstallBtn = new Button();
            InstallBtn.Text = "Install / Repair";
            InstallBtn.Size = new Size(180, 40);
            InstallBtn.Anchor = AnchorStyles.Left;
            InstallBtn.Click += delegate { RunElevated(Cfg.Get("hide_others") == "0" ? "install-show" : "install-hide"); };
            actions.Controls.Add(InstallBtn, 0, 0);
            LaunchBtn = new Button();
            LaunchBtn.Text = "Launch Game";
            LaunchBtn.Font = new Font("Segoe UI Semibold", 12f);
            LaunchBtn.Size = new Size(200, 40);
            LaunchBtn.Anchor = AnchorStyles.Right;
            LaunchBtn.FlatStyle = FlatStyle.Flat;
            LaunchBtn.FlatAppearance.BorderSize = 0;
            LaunchBtn.Click += delegate { Launch(); };
            actions.Controls.Add(LaunchBtn, 1, 0);
            root.Controls.Add(actions);

            Hint = new Label();
            Hint.AutoSize = true;
            Hint.MaximumSize = new Size(470, 0);
            Hint.ForeColor = Color.DimGray;
            Hint.Margin = new Padding(2, 8, 0, 6);
            root.Controls.Add(Hint);

            var links = new FlowLayoutPanel();
            links.AutoSize = true;
            links.Margin = new Padding(0, 4, 0, 0);
            LinkLogs = Link(links, "Open logs folder", delegate { OpenLogs(); });
            LinkCollect = Link(links, "Collect logs for support", delegate { CollectLogs(); });
            LinkUninstall = Link(links, "Uninstall", delegate {
                if (MessageBox.Show(this, "Remove " + K.OnlineName + " and " + K.LanName + " from this PC?\n(Logs are kept.)",
                    "Uninstall", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes) RunElevated("uninstall");
            });
            root.Controls.Add(links);

            game = Cfg.Get("game");
            if (game == null || !File.Exists(game))
            {
                var known = Sys.KnownGameExes();
                game = known.Count > 0 ? known[0] : null;
            }
            Shown += delegate { Refresh2(); };
        }

        static StatusRow AddRow(TableLayoutPanel grid, string name)
        {
            var r = new StatusRow();
            r.Icon = new Label();
            r.Icon.Font = new Font("Segoe UI Symbol", 11f, FontStyle.Bold);
            r.Icon.AutoSize = true;
            r.Icon.Margin = new Padding(0, 3, 0, 3);
            r.Name = new Label();
            r.Name.Text = name;
            r.Name.AutoSize = true;
            r.Name.Margin = new Padding(0, 5, 0, 3);
            r.Detail = new Label();
            r.Detail.AutoEllipsis = true;
            r.Detail.Dock = DockStyle.Fill;
            r.Detail.TextAlign = ContentAlignment.MiddleLeft;
            r.Detail.ForeColor = Color.DimGray;
            int row = grid.RowCount++;
            grid.Controls.Add(r.Icon, 0, row);
            grid.Controls.Add(r.Name, 1, row);
            grid.Controls.Add(r.Detail, 2, row);
            r.Set(3, "checking…");
            return r;
        }

        static LinkLabel Link(FlowLayoutPanel p, string text, EventHandler click)
        {
            var l = new LinkLabel();
            l.Text = text;
            l.AutoSize = true;
            l.Margin = new Padding(0, 0, 14, 0);
            l.LinkClicked += delegate { click(null, EventArgs.Empty); };
            p.Controls.Add(l);
            return l;
        }

        public void Refresh2()
        {
            GameLabel.Text = game ?? "(not found - click Change… and pick age2_x1.exe)";
            GameLabel.ForeColor = game == null ? Color.FromArgb(198, 40, 40) : Color.Black;

            int dp = Sys.DirectPlayState();
            dpOk = dp == 1;
            RowDp.Set(dpOk ? 0 : 2, dpOk ? "enabled" : dp == 2 ? "turned off - Install / Repair turns it on" : "unknown");

            string bundled = Sys.BundledDll();
            string bundledVer = bundled != null ? Sys.FileVersion(bundled) : null;
            needsUpdate = false;
            onlineOk = ProviderRow(RowOnline, K.OnlineGuid, bundledVer);
            lanOk = ProviderRow(RowLan, K.LanGuid, bundledVer);

            RowServer.Set(3, "checking…");
            ThreadPool.QueueUserWorkItem(delegate {
                var s = Sys.ServerStatus();
                try
                {
                    BeginInvoke((MethodInvoker)delegate {
                        var p = s.Split('|');
                        if (p[0] == "ok")
                        {
                            var rooms = int.Parse(p[1]);
                            RowServer.Set(0, "online · " + (rooms == 0 ? "no open games" : rooms == 1 ? "1 open game" : rooms + " open games"));
                        }
                        else RowServer.Set(1, "unreachable · LAN Play still works");
                    });
                }
                catch (Exception) { }
            });

            bool installedSomething = onlineOk || lanOk;

            string codecFile;
            var codec = Sys.IndeoCodec(out codecFile);
            if (codec != null && codecFile != null)
                RowVideo.Set(0, Sys.IsOurCodec(codec) ? "fixed · Indeo 5 decoder installed" : "Indeo 5 decoder present");
            else if (codec != null)
                RowVideo.Set(1, "decoder file missing · Repair fixes it");
            else
                RowVideo.Set(1, "no picture · " + (installedSomething ? "Repair" : "Install") + " adds a decoder");

            var foreign = Setup.ForeignProviders();
            var hidden = Setup.HiddenProviders();
            settingHideBox = true;
            HideBox.Checked = installedSomething ? foreign.Count == 0 : Cfg.Get("hide_others") != "0";
            settingHideBox = false;
            if (!installedSomething)
                HideNote.Text = "Applied when you click Install. Hides TCP/IP, IPX, Modem and Serial (Uninstall brings them back).";
            else if (foreign.Count == 0)
                HideNote.Text = hidden.Count > 0 ? hidden.Count + " other connection type(s) hidden. Untick to bring them back."
                                                 : "No other connection types are registered.";
            else
                HideNote.Text = "Also listed in the game: " + string.Join(", ", ShortNames(foreign)) + ".";
            InstallBtn.Text = needsUpdate ? "Update" : (dpOk && onlineOk && lanOk ? "Repair" : "Install");
            bool canLaunch = game != null && dpOk && installedSomething;
            LaunchBtn.Enabled = canLaunch;
            LaunchBtn.BackColor = canLaunch ? Color.FromArgb(46, 125, 50) : Color.FromArgb(200, 200, 200);
            LaunchBtn.ForeColor = Color.White;

            if (game == null) Hint.Text = "Pick your game: click Change… and choose age2_x1.exe (or age2_x1.5.exe with UserPatch).";
            else if (!installedSomething && bundled == null)
                Hint.Text = "Unzip the whole " + K.Brand + " folder first, then run " + K.LauncherExe + " from there.";
            else if (!dpOk || !installedSomething) Hint.Text = "Click Install. Windows will ask for permission once.";
            else if (needsUpdate) Hint.Text = "A newer version is bundled with this launcher. Click Update, then Launch Game.";
            else Hint.Text = "In the game: Multiplayer → pick “" + K.OnlineName + "” (internet) or “" + K.LanName +
                             "” (same network) → Create or Join.";
        }

        static string[] ShortNames(List<string> names)
        {
            var r = new List<string>();
            foreach (var n in names)
                r.Add(n.Replace(" Connection For DirectPlay", "").Replace("Internet ", ""));
            return r.ToArray();
        }

        void HideBoxChanged()
        {
            if (settingHideBox) return;
            Cfg.Set("hide_others", HideBox.Checked ? "1" : "0");
            if (onlineOk || lanOk) RunElevated(HideBox.Checked ? "hide" : "unhide");
            else Refresh2();
        }

        bool ProviderRow(StatusRow row, string guid, string bundledVer)
        {
            var path = Sys.ProviderPath(guid);
            if (path == null) { row.Set(2, "not installed"); return false; }
            var ver = Sys.FileVersion(path);
            if (ver == null) { row.Set(2, "registered, but okplay.dll is missing"); return false; }
            if (bundledVer != null && CompareVer(bundledVer, ver) > 0)
            {
                needsUpdate = true;
                row.Set(1, "v" + ver + " → update to v" + bundledVer);
                return true;
            }
            row.Set(0, "installed · v" + ver);
            return true;
        }

        static int CompareVer(string a, string b)
        {
            try { return new Version(a).CompareTo(new Version(b)); } catch (Exception) { return 0; }
        }

        void PickGame()
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = "Pick your Age of Empires II program (age2_x1.exe, age2_x1.5.exe or empires2.exe)";
                d.Filter = "Age of Empires II|age2*.exe;empires2*.exe|Programs|*.exe";
                if (game != null) d.InitialDirectory = Path.GetDirectoryName(game);
                if (d.ShowDialog(this) != DialogResult.OK) return;
                game = d.FileName;
                Cfg.Set("game", game);
                Refresh2();
            }
        }

        void Launch()
        {
            if (game == null) return;
            if (Sys.GameProcesses().Length > 0 &&
                MessageBox.Show(this, "Age of Empires II already seems to be running. Start another copy?", "Launch Game",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try
            {
                var psi = new ProcessStartInfo(game);
                psi.WorkingDirectory = Path.GetDirectoryName(game);
                psi.UseShellExecute = true;
                Process.Start(psi);
                Cfg.Set("game", game);
                WindowState = FormWindowState.Minimized;
            }
            catch (Exception e) { MessageBox.Show(this, "Could not start the game:\n" + e.Message, "Launch Game"); }
        }

        void RunElevated(string action)
        {
            try
            {
                var psi = new ProcessStartInfo(Application.ExecutablePath,
                    "--elevated " + action + (game != null ? " \"" + game + "\"" : ""));
                psi.Verb = "runas";
                psi.UseShellExecute = true;
                Enabled = false;
                using (var p = Process.Start(psi)) { while (!p.WaitForExit(100)) Application.DoEvents(); }
            }
            catch (System.ComponentModel.Win32Exception) { /* user said No to the UAC prompt */ }
            finally { Enabled = true; Refresh2(); }
        }

        void OpenLogs()
        {
            var d = Path.Combine(K.InstallDir, "logs");
            if (!Directory.Exists(d)) d = K.InstallDir;
            if (!Directory.Exists(d)) { MessageBox.Show(this, "Nothing installed yet.", "Logs"); return; }
            Process.Start("explorer.exe", "\"" + d + "\"");
        }

        void CollectLogs()
        {
            Cursor = Cursors.WaitCursor;
            try
            {
                var r = Diag.Collect().Split('|');
                Process.Start("explorer.exe", "/select,\"" + r[0] + "\"");
                MessageBox.Show(this, "Saved " + r[1] + " log file(s) to your Desktop:\n\n" + Path.GetFileName(r[0]) +
                                "\n\nSend that zip back for troubleshooting.", "Collect logs");
            }
            catch (Exception e) { MessageBox.Show(this, "Collecting logs failed:\n" + e.Message, "Collect logs"); }
            finally { Cursor = Cursors.Default; }
        }
    }

    /// <summary>Progress window for the elevated install/uninstall child.</summary>
    class WorkForm : Form
    {
        readonly TextBox box;
        readonly Button close;
        public WorkForm(string title)
        {
            Text = title;
            Font = new Font("Segoe UI", 9.75f);
            Size = new Size(640, 360);
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            box = new TextBox();
            box.Multiline = true;
            box.ReadOnly = true;
            box.ScrollBars = ScrollBars.Vertical;
            box.Dock = DockStyle.Fill;
            box.BackColor = Color.White;
            close = new Button();
            close.Text = "Close";
            close.Dock = DockStyle.Bottom;
            close.Height = 34;
            close.Enabled = false;
            close.Click += delegate { Close(); };
            Controls.Add(box);
            Controls.Add(close);
        }
        public void Log(string line)
        {
            if (InvokeRequired) { BeginInvoke((MethodInvoker)delegate { Log(line); }); return; }
            box.AppendText(line + "\r\n");
        }
        public void Done(bool ok)
        {
            if (InvokeRequired) { BeginInvoke((MethodInvoker)delegate { Done(ok); }); return; }
            Log(ok ? "\r\nDone." : "\r\nStopped - see the message above.");
            close.Enabled = true;
            close.Focus();
            if (ok)
            {
                var t = new System.Windows.Forms.Timer();
                t.Interval = 1500;
                t.Tick += delegate { t.Stop(); Close(); };
                t.Start();
            }
        }
    }

    // ------------------------------------------------------------------ entry

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            try { SetProcessDPIAware(); } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            if (args.Length >= 2 && args[0] == "--elevated") return Elevated(args[1], args.Length > 2 ? args[2] : null);
            if (args.Length >= 1 && args[0] == "--selftest") return SelfTest(args.Length > 1 ? args[1] : null);
            if (args.Length >= 3 && args[0] == "--dryrun-install") return DryRun(args[1], args[2]);

            Application.Run(new MainForm());
            return 0;
        }

        static int Elevated(string action, string game)
        {
            var f = new WorkForm(K.Brand + (action.StartsWith("install") ? " - installing"
                               : action == "uninstall" ? " - uninstalling" : " - connection list"));
            bool ok = false;
            f.Shown += delegate {
                var th = new Thread(delegate () {
                    try
                    {
                        if (action == "install-hide" || action == "install") ok = Setup.Install(game, true, f.Log);
                        else if (action == "install-show") ok = Setup.Install(game, false, f.Log);
                        else if (action == "hide") { Setup.HideOthers(f.Log); ok = true; }
                        else if (action == "unhide") { if (Setup.UnhideOthers(f.Log) == 0) f.Log("Nothing was hidden"); ok = true; }
                        else ok = Setup.Uninstall(f.Log);
                    }
                    catch (Exception e) { f.Log("ERROR: " + e.Message); ok = false; }
                    f.Done(ok);
                });
                th.IsBackground = true;
                th.Start();
            };
            Application.Run(f);
            return ok ? 0 : 1;
        }

        static int DryRun(string dir, string game)
        {
            K.DryRun = true;
            K.InstallDir = Path.Combine(dir, "install");
            K.DryShortcutDir = Path.Combine(dir, "shortcuts");
            Directory.CreateDirectory(K.DryShortcutDir);
            var oldDir = Path.Combine(dir, "old-v0.1-install");
            Directory.CreateDirectory(Path.Combine(oldDir, "logs"));
            File.WriteAllText(Path.Combine(oldDir, "okplay.dll"), "old");
            File.WriteAllText(Path.Combine(oldDir, "uninstall.ps1"), "old");
            File.WriteAllText(Path.Combine(oldDir, @"logs\okplay-old.log"), "old log");
            K.OldInstallDirs = new[] { oldDir };
            var sb = new StringBuilder();
            Action<string> log = delegate (string l) { sb.AppendLine(l); };
            Setup.DryLog = log;
            // plant copies of Windows' own entries in the test key
            using (var root = Sys.Hklm32())
            {
                using (var k = root.CreateSubKey(K.SpKeyBase + "Internet TCP/IP Connection For DirectPlay"))
                {
                    k.SetValue("DescriptionA", @"@%SystemRoot%\system32\dpwsockx.dll,-3000", RegistryValueKind.ExpandString);
                    k.SetValue("Guid", "{36E95EE0-8577-11cf-960C-0080C7534E82}", RegistryValueKind.String);
                    k.SetValue("Path", @"C:\Windows\SysWOW64\dpwsockx.dll", RegistryValueKind.String);
                    k.SetValue("dwReserved1", 500, RegistryValueKind.DWord);
                    using (var at = k.CreateSubKey(@"Address Types\{C4A54DA0-E0AF-11cf-9C4E-00A0C905425E}")) { }
                }
                using (var k = root.CreateSubKey(K.SpKeyBase + "IPX Connection For DirectPlay"))
                {
                    k.SetValue("Guid", "{685BC400-9D2C-11cf-A9CD-00AA006886E3}", RegistryValueKind.String);
                    k.SetValue("Path", "dpwsockx.dll", RegistryValueKind.String);
                    k.SetValue("dwReserved1", 50, RegistryValueKind.DWord);
                }
            }
            var planted = DumpForeign();
            sb.AppendLine("PLANTED " + planted.Split('\n').Length + " lines");

            bool ok = false;
            try { ok = Setup.Install(game, true, log); } catch (Exception e) { sb.AppendLine("EXCEPTION " + e); }
            sb.AppendLine("INSTALL " + (ok ? "OK" : "FAILED"));
            sb.AppendLine("OLD_INSTALL files_left=" + Directory.GetFiles(oldDir).Length +
                          " log_kept=" + File.Exists(Path.Combine(oldDir, @"logs\okplay-old.log")));
            sb.AppendLine("VISIBLE_AFTER_INSTALL foreign=" + Setup.ForeignProviders().Count + " hidden=" + Setup.HiddenProviders().Count);

            // intro-video decoder: fresh install, someone else's working decoder, a broken registration
            string cfile;
            var cval = Sys.IndeoCodec(out cfile);
            sb.AppendLine("CODEC_FRESH " + (Sys.IsOurCodec(cval) && cfile != null &&
                SamePathish(cfile, Path.Combine(K.InstallDir, K.CodecDll)) ? "ours" : "WRONG: " + (cval ?? "(none)")));
            SetCodec("iccvid.dll");   // exists in SysWOW64: stands in for a real Intel Indeo install
            Setup.InstallVideoFix(log);
            sb.AppendLine("CODEC_KEEPS_WORKING_FOREIGN " + (Sys.IndeoCodec(out cfile) == "iccvid.dll" ? "yes" : "NO"));
            SetCodec("ir50_32.dll");  // registered but the file is gone
            Setup.InstallVideoFix(log);
            cval = Sys.IndeoCodec(out cfile);
            sb.AppendLine("CODEC_REPLACES_BROKEN " + (Sys.IsOurCodec(cval) && cfile != null ? "yes" : "NO: " + cval));

            // program folder: users may read but not add files; logs stay writable
            sb.AppendLine("USERS_CAN_WRITE_PROGRAM_DIR " + (UsersCanWrite(K.InstallDir) ? "YES (bad)" : "no"));
            sb.AppendLine("USERS_CAN_WRITE_LOGS " + (UsersCanWrite(Path.Combine(K.InstallDir, "logs")) ? "yes" : "NO (bad)"));
            var ctl = Path.Combine(dir, "acl-control");   // ProgramData's default: Users may create files
            Directory.CreateDirectory(ctl);
            var csec = Directory.GetAccessControl(ctl);
            csec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
                FileSystemRights.WriteData | FileSystemRights.AppendData, InheritanceFlags.ContainerInherit,
                PropagationFlags.None, AccessControlType.Allow));
            Directory.SetAccessControl(ctl, csec);
            sb.AppendLine("ACL_CHECKER_FIRES " + (UsersCanWrite(ctl) ? "yes" : "NO - check is blind"));

            Setup.UnhideOthers(log);
            var restored = DumpForeign();
            sb.AppendLine("UNHIDE_ROUNDTRIP " + (restored == planted ? "IDENTICAL" : "DIFFERENT\n--planted\n" + planted + "--restored\n" + restored));
            sb.AppendLine("VISIBLE_AFTER_UNHIDE foreign=" + Setup.ForeignProviders().Count + " hidden=" + Setup.HiddenProviders().Count);
            Setup.HideOthers(log);
            sb.AppendLine("VISIBLE_AFTER_REHIDE foreign=" + Setup.ForeignProviders().Count + " hidden=" + Setup.HiddenProviders().Count);
            using (var root = Sys.Hklm32())
            {
                foreach (var name in Sys.KeysWithGuid(K.OnlineGuid).Concat(Sys.KeysWithGuid(K.LanGuid)))
                    using (var k = root.OpenSubKey(K.SpKeyBase + name))
                    {
                        if (k == null) { sb.AppendLine("REG MISSING " + name); continue; }
                        foreach (var v in k.GetValueNames())
                            sb.AppendLine("REG " + name + " " + v + "=" + k.GetValue(v) + " (" + k.GetValueKind(v) + ")");
                    }
            }
            sb.AppendLine("PROVIDER_PATH online=" + Sys.ProviderPath(K.OnlineGuid));
            sb.AppendLine("PROVIDER_PATH lan=" + Sys.ProviderPath(K.LanGuid));

            // a self-hoster renames their connections and reinstalls: exactly one key per GUID, new names
            K.OnlineName = "MyClan Online Play";
            K.LanName = "MyClan LAN Play";
            try { Setup.Install(game, true, log); } catch (Exception e) { sb.AppendLine("EXCEPTION " + e); }
            sb.AppendLine("RENAMED online=" + string.Join("|", Sys.KeysWithGuid(K.OnlineGuid).ToArray()) +
                          " lan=" + string.Join("|", Sys.KeysWithGuid(K.LanGuid).ToArray()));
            foreach (var f in Directory.GetFiles(K.InstallDir)) sb.AppendLine("FILE " + Path.GetFileName(f) + " " + new FileInfo(f).Length);
            foreach (var f in Directory.GetFiles(K.DryShortcutDir)) sb.AppendLine("LNK " + Path.GetFileName(f));
            var acl = Directory.GetAccessControl(Path.Combine(K.InstallDir, "logs"));
            foreach (FileSystemAccessRule r in acl.GetAccessRules(true, false, typeof(SecurityIdentifier)))
                if (r.IdentityReference.Value == "S-1-5-32-545") sb.AppendLine("ACL logs Users " + r.FileSystemRights + " " + r.InheritanceFlags);
            bool uok = false;
            try { uok = Setup.Uninstall(log); } catch (Exception e) { sb.AppendLine("EXCEPTION " + e); }
            sb.AppendLine("UNINSTALL " + (uok ? "OK" : "FAILED"));
            var afterUninstall = DumpForeign();
            sb.AppendLine("UNINSTALL_RESTORES_WINDOWS_ENTRIES " + (afterUninstall == planted ? "IDENTICAL" : "DIFFERENT"));
            sb.AppendLine("VISIBLE_AFTER_UNINSTALL foreign=" + Setup.ForeignProviders().Count + " hidden=" + Setup.HiddenProviders().Count);
            // prove the comparison can fail: change one value the way a lossy copy would
            using (var root = Sys.Hklm32())
            using (var k = root.OpenSubKey(K.SpKeyBase + "Internet TCP/IP Connection For DirectPlay", true))
                k.SetValue("DescriptionA", @"@C:\Windows\system32\dpwsockx.dll,-3000", RegistryValueKind.String);
            sb.AppendLine("CHECKER_FIRES " + (DumpForeign() != planted ? "yes" : "NO - comparison is blind"));
            sb.AppendLine("AFTER_UNINSTALL online=" + (Sys.ProviderPath(K.OnlineGuid) ?? "(none)") +
                          " lan=" + (Sys.ProviderPath(K.LanGuid) ?? "(none)"));
            sb.AppendLine("CODEC_AFTER_UNINSTALL " + (Sys.IndeoCodec(out cfile) ?? "(none)"));
            SetCodec("iccvid.dll");
            Setup.RemoveVideoFix(log);
            sb.AppendLine("UNINSTALL_KEEPS_FOREIGN_CODEC " + (Sys.IndeoCodec(out cfile) == "iccvid.dll" ? "yes" : "NO"));
            foreach (var f in Directory.GetFiles(K.InstallDir)) sb.AppendLine("LEFT " + Path.GetFileName(f));
            try { Registry.CurrentUser.DeleteSubKeyTree(@"Software\OldkidPlayDryRun", false); } catch (Exception) { }
            File.WriteAllText(Path.Combine(dir, "dryrun.txt"), sb.ToString());
            return ok && uok ? 0 : 1;
        }

        static void SetCodec(string value)
        {
            using (var root = Sys.Hklm32())
            using (var k = root.CreateSubKey(K.Drivers32Key))
                k.SetValue(K.CodecValue, value, RegistryValueKind.String);
        }

        static bool SamePathish(string a, string b)
        {
            return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>Does any allow rule (own or inherited) let BUILTIN\Users create files here?</summary>
        static bool UsersCanWrite(string dir)
        {
            foreach (FileSystemAccessRule r in Directory.GetAccessControl(dir).GetAccessRules(true, true, typeof(SecurityIdentifier)))
                if (r.IdentityReference.Value == "S-1-5-32-545" && r.AccessControlType == AccessControlType.Allow &&
                    (r.FileSystemRights & (FileSystemRights.WriteData | FileSystemRights.AppendData)) != 0)
                    return true;
            return false;
        }

        /// <summary>Every value (name, kind, raw data) and sub-key of the non-Oldkid providers, sorted.</summary>
        static string DumpForeign()
        {
            var lines = new List<string>();
            using (var root = Sys.Hklm32())
                foreach (var name in Setup.ForeignProviders())
                    using (var k = root.OpenSubKey(K.SpKeyBase + name))
                        DumpKey(k, name, lines);
            lines.Sort(StringComparer.Ordinal);
            return string.Join("\n", lines.ToArray());
        }

        static void DumpKey(RegistryKey k, string path, List<string> lines)
        {
            foreach (var v in k.GetValueNames())
                lines.Add(path + " | " + v + " | " + k.GetValueKind(v) + " | " +
                          k.GetValue(v, null, RegistryValueOptions.DoNotExpandEnvironmentNames));
            foreach (var sub in k.GetSubKeyNames())
                using (var s = k.OpenSubKey(sub))
                {
                    lines.Add(path + "\\" + sub + " | (key)");
                    DumpKey(s, path + "\\" + sub, lines);
                }
        }

        /// <summary>Headless audit: checks + layout (no overlaps, nothing outside its parent, no clipped text).</summary>
        static int SelfTest(string outPath)
        {
            var sb = new StringBuilder();
            int problems = 0;
            sb.AppendLine("directplay_state=" + Sys.DirectPlayState());
            sb.AppendLine("brand=" + K.Brand + " online_name=" + K.OnlineName + " lan_name=" + K.LanName);
            sb.AppendLine("online_path=" + (Sys.ProviderPath(K.OnlineGuid) ?? "(none)"));
            sb.AppendLine("lan_path=" + (Sys.ProviderPath(K.LanGuid) ?? "(none)"));
            sb.AppendLine("bundled_dll=" + (Sys.BundledDll() ?? "(none)") + " v" + (Sys.BundledDll() != null ? Sys.FileVersion(Sys.BundledDll()) : "-"));
            foreach (var e in Sys.KnownGameExes()) sb.AppendLine("known_game=" + e);
            sb.AppendLine("server=" + Sys.ServerStatus());
            string codecFile;
            sb.AppendLine("indeo_codec=" + (Sys.IndeoCodec(out codecFile) ?? "(none)") + " file=" + (codecFile ?? "(none)"));

            var form = new MainForm();
            form.Opacity = 0;
            form.ShowInTaskbar = false;
            form.Show();
            form.Refresh2();
            Application.DoEvents();
            form.PerformLayout();
            Application.DoEvents();
            sb.AppendLine("form_client=" + form.ClientSize.Width + "x" + form.ClientSize.Height);
            problems += Audit(form, sb);
            form.Close();

            sb.AppendLine(problems == 0 ? "LAYOUT OK" : "LAYOUT PROBLEMS: " + problems);
            if (outPath != null) File.WriteAllText(outPath, sb.ToString());
            return problems == 0 ? 0 : 1;
        }

        // UTF-8 bytes read as Windows-1252 start with these pairs (built from char codes so this
        // check can't itself be garbled by a wrong source code page)
        static readonly string[] MojibakeMarks = {
            new string(new[] { (char)0xE2, (char)0x20AC }), new string(new[] { (char)0xE2, (char)0x0153 }),
            new string(new[] { (char)0xE2, (char)0x2020 }), new string(new[] { (char)0xE2, (char)0x2013 }),
            new string(new[] { (char)0xC2, (char)0xB7 }) };

        static bool Mojibake(string t)
        {
            foreach (var m in MojibakeMarks) if (t.Contains(m)) return true;
            return false;
        }

        static int Audit(Control parent, StringBuilder sb)
        {
            int bad = 0;
            var kids = new List<Control>();
            foreach (Control c in parent.Controls) if (c.Visible) kids.Add(c);
            foreach (var c in kids)
            {
                var r = c.Bounds;
                if (r.Left < 0 || r.Top < 0 || r.Right > parent.ClientSize.Width || r.Bottom > parent.ClientSize.Height)
                {
                    sb.AppendLine("OUTSIDE: " + Describe(c) + " " + r + " in " + Describe(parent) + " " + parent.ClientSize);
                    bad++;
                }
                if (c is Button)
                {
                    var need = c.GetPreferredSize(Size.Empty);
                    if (need.Width > c.Width + 1 || need.Height > c.Height + 1)
                    {
                        sb.AppendLine("CLIPPED: " + Describe(c) + " needs " + need + " has " + c.Size);
                        bad++;
                    }
                }
                else if (c is Label)
                {
                    var lbl = (Label)c;
                    if (!lbl.AutoEllipsis)
                    {
                        Size need;
                        if (lbl.AutoSize && lbl.MaximumSize.Width > 0)   // wraps: must fit its height
                            need = TextRenderer.MeasureText(c.Text, c.Font, new Size(lbl.MaximumSize.Width, 0),
                                                            TextFormatFlags.WordBreak);
                        else
                            need = TextRenderer.MeasureText(c.Text, c.Font);
                        if (need.Width > c.Width + 1 || need.Height > c.Height + 1)
                        {
                            sb.AppendLine("CLIPPED: " + Describe(c) + " needs " + need + " has " + c.Size);
                            bad++;
                        }
                    }
                }
                // UTF-8 read as Windows-1252 turns "✔ · → …" into "âœ” Â· â†’ â€¦"
                var txt = c.Text ?? "";
                if (Mojibake(txt))
                {
                    sb.AppendLine("GARBLED TEXT: " + Describe(c));
                    bad++;
                }
                bad += Audit(c, sb);
            }
            for (int i = 0; i < kids.Count; i++)
                for (int j = i + 1; j < kids.Count; j++)
                    if (kids[i].Bounds.IntersectsWith(kids[j].Bounds))
                    {
                        sb.AppendLine("OVERLAP: " + Describe(kids[i]) + " " + kids[i].Bounds + " x " + Describe(kids[j]) + " " + kids[j].Bounds);
                        bad++;
                    }
            return bad;
        }

        static string Describe(Control c)
        {
            var t = c.Text ?? "";
            if (t.Length > 24) t = t.Substring(0, 24) + "...";
            return c.GetType().Name + "(\"" + t + "\")";
        }
    }
}
