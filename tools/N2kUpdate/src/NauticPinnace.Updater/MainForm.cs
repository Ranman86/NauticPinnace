using System.Globalization;
using System.Text;
using NauticPinnace.N2kUpdate;
using NauticPinnace.N2kUpdate.Bus;
using NauticPinnace.N2kUpdate.Can;
using NauticPinnace.N2kUpdate.Engine;
using NauticPinnace.N2kUpdate.Hosting;
using NauticPinnace.N2kUpdate.Images;
using NauticPinnace.N2kUpdate.N2k;
using NauticPinnace.N2kUpdate.Protocol;
using NauticPinnace.N2kUpdate.Session;

namespace NauticPinnace.Updater;

/// <summary>
/// The updater window (12.7). Layout in code, no designer file: adapter, release,
/// display list with Identify, options, progress and log, top to bottom.
/// </summary>
internal sealed class MainForm : Form
{
    private sealed record PortItem(string Port, bool IsCanable, bool IsBridge = false)
    {
        public override string ToString() => Port + (IsCanable ? T.CanableSuffix : "");
    }

    /// <summary>
    /// For tests without hardware: N2KUPD_BRIDGE=tcp:127.0.0.1:5577 adds an slcan-over-TCP
    /// endpoint to the port list, e.g. the scripted display of "n2kupd sim-display".
    /// </summary>
    private static readonly string? Bridge = Environment.GetEnvironmentVariable("N2KUPD_BRIDGE");

    private readonly UpdaterSettings _settings;
    private readonly UpdaterLog _log;
    private N2kUpdateClient? _client;
    private WindowsHighResWaiter? _waiter;
    private ReleasePackage? _release;
    private FirmwareImage? _looseFw;
    private WebUiImage? _looseWeb;
    private CancellationTokenSource? _cts;
    private bool _running, _mustStay, _closeAfterRun;

    // Controls.
    private readonly GroupBox _gAdapter = new(), _gRelease = new(), _gDisplays = new(), _gOptions = new(), _gProgress = new(), _gLog = new();
    private readonly Label _lPort = new(), _lAdapterState = new(), _lRelease = new(), _lShare = new(), _lApply = new(), _lWebVer = new();
    private readonly ComboBox _cbPort = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 170 };
    private readonly Button _bRefresh = new(), _bConnect = new(), _bFolder = new(), _bZip = new(), _bFiles = new();
    private readonly ListView _lv = new() { View = View.Details, CheckBoxes = true, FullRowSelect = true, HideSelection = false, MultiSelect = false, Dock = DockStyle.Fill };
    private readonly Button _bDiscover = new(), _bIdentify = new(), _bInstall = new(), _bCancel = new();
    private readonly WholeNumberUpDown _nShare = new() { Minimum = 10, Maximum = 60, Increment = 5, Width = 55 };
    private readonly CheckBox _cAdaptive = new() { AutoSize = true }, _cFw = new() { AutoSize = true, Checked = true }, _cWeb = new() { AutoSize = true, Checked = true },
                              _cCompress = new() { AutoSize = true, Checked = true }, _cForce = new() { AutoSize = true };
    private readonly ComboBox _cbApply = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 280 };
    private readonly TextBox _tWebVer = new() { Width = 90 };
    private readonly LinkLabel _lLang = new() { AutoSize = true };
    private readonly ProgressBar _progress = new() { Dock = DockStyle.Top, Height = 22, Maximum = 1000 };
    private readonly Label _lPhase = new() { Dock = DockStyle.Top, AutoSize = false, Height = 22 };
    private readonly Label _lStay = new()
    {
        Dock = DockStyle.Top, AutoSize = false, Height = 30, Visible = false, TextAlign = ContentAlignment.MiddleLeft,
        BackColor = Color.FromArgb(255, 236, 160), ForeColor = Color.FromArgb(90, 60, 0), Font = new Font("Segoe UI", 10f, FontStyle.Bold),
    };
    private readonly TextBox _tLog = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font("Consolas", 9f) };

    public MainForm()
    {
        _settings = UpdaterSettings.Load();
        T.German = !_settings.Language.Equals("en", StringComparison.OrdinalIgnoreCase);
        _log = new UpdaterLog(UpdaterSettings.LogFolder);
        BuildLayout();
        ApplyTexts();
        _nShare.Value = Math.Clamp(_settings.BusSharePercent, 10, 60);
        _cAdaptive.Checked = _settings.AdaptiveBackoff;
        _cCompress.Checked = _settings.Compression;
        _cbApply.SelectedIndex = _settings.ApplyMode switch { "NowNoWait" => 1, "NextRestart" => 2, "None" => 3, _ => 0 };
        RefreshPorts();
        if (_settings.LastRelease is { } last && (Directory.Exists(last) || File.Exists(last))) LoadRelease(last);
        UpdateButtons();
        Log("NauticPinnace updater " + typeof(MainForm).Assembly.GetName().Version?.ToString(3) + ", log file " + _log.Path);
    }

    // ── Layout ────────────────────────────────────────────────────────────────
    private void BuildLayout()
    {
        Width = 1000;
        Height = 760;
        MinimumSize = new Size(820, 600);
        StartPosition = FormStartPosition.CenterScreen;

        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 6, Padding = new Padding(8) };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 55));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 45));
        Controls.Add(root);

        // Adapter.
        var fa = Flow();
        _lPort.AutoSize = true;
        _lPort.Margin = new Padding(3, 7, 3, 3);
        _lAdapterState.AutoSize = true;
        _lAdapterState.Margin = new Padding(12, 7, 3, 3);
        fa.Controls.AddRange([_lPort, _cbPort, _bRefresh, _bConnect, _lAdapterState]);
        _gAdapter.Controls.Add(fa);
        AddGroup(root, _gAdapter, 0);

        // Release.
        var fr = Flow();
        _lRelease.AutoSize = true;
        _lRelease.Margin = new Padding(12, 7, 3, 3);
        fr.Controls.AddRange([_bFolder, _bZip, _bFiles, _lRelease]);
        _gRelease.Controls.Add(fr);
        AddGroup(root, _gRelease, 1);

        // Displays.
        var dl = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        dl.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        dl.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        dl.Controls.Add(_lv, 0, 0);
        var fb = Flow();
        fb.Controls.AddRange([_bDiscover, _bIdentify, _bInstall, _bCancel]);
        dl.Controls.Add(fb, 0, 1);
        _gDisplays.Controls.Add(dl);
        _gDisplays.Dock = DockStyle.Fill;
        root.Controls.Add(_gDisplays, 0, 2);
        foreach (var w in new[] { 45, 70, 140, 90, 95, 65, 100, 70, 330 }) _lv.Columns.Add("", w);

        // Options: three fixed lines; a wrapping flow panel would not grow its group box.
        _lShare.AutoSize = _lApply.AutoSize = _lWebVer.AutoSize = true;
        var opts = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, RowCount = 3 };
        Control[][] lines =
        [
            [_cFw, _cWeb, _cCompress, _lApply, _cbApply],
            [_lShare, _nShare, _cAdaptive, _lLang],
            [_lWebVer, _tWebVer, _cForce],
        ];
        foreach (var line in lines)
        {
            var f = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Dock = DockStyle.Fill, Margin = Padding.Empty };
            f.Controls.AddRange(line);
            foreach (Control c in line)
                c.Margin = c is CheckBox or LinkLabel or Label ? new Padding(c == line[0] ? 3 : 12, 6, 3, 3) : new Padding(3);
            opts.Controls.Add(f);
        }
        _gOptions.Controls.Add(opts);
        AddGroup(root, _gOptions, 3);

        // Progress.
        var pp = new Panel { Dock = DockStyle.Fill, Height = 76 };
        pp.Controls.Add(_lStay);
        pp.Controls.Add(_lPhase);
        pp.Controls.Add(_progress);
        _gProgress.Controls.Add(pp);
        _gProgress.Height = 100;
        _gProgress.Dock = DockStyle.Fill;
        root.Controls.Add(_gProgress, 0, 4);

        // Log.
        _gLog.Controls.Add(_tLog);
        _gLog.Dock = DockStyle.Fill;
        root.Controls.Add(_gLog, 0, 5);

        _bRefresh.Click += (_, _) => RefreshPorts();
        _bConnect.Click += async (_, _) => await ToggleConnect();
        _bFolder.Click += (_, _) => PickFolder();
        _bZip.Click += (_, _) => PickZip();
        _bFiles.Click += (_, _) => PickFiles();
        _bDiscover.Click += async (_, _) => await DiscoverAsync();
        _bIdentify.Click += async (_, _) => await IdentifyAsync();
        _bInstall.Click += async (_, _) => await InstallAsync();
        _bCancel.Click += (_, _) => CancelRun();
        _lLang.LinkClicked += (_, _) =>
        {
            T.German = !T.German;
            _settings.Language = T.German ? "de" : "en";
            _settings.Save();
            ApplyTexts();
        };
        _lv.ItemCheck += (_, e) => { if (_lv.Items[e.Index].Tag is not DisplayDevice) e.NewValue = CheckState.Unchecked; };
        _lv.SelectedIndexChanged += (_, _) => UpdateButtons();
        // The status column takes the rest of the width.
        _lv.Resize += (_, _) => { if (_lv.Columns.Count > 0) _lv.Columns[^1].Width = -2; };
        _cWeb.CheckedChanged += (_, _) => UpdateButtons();
    }

    private static FlowLayoutPanel Flow() => new() { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true, FlowDirection = FlowDirection.LeftToRight };

    private static void AddGroup(TableLayoutPanel root, GroupBox g, int row)
    {
        g.Dock = DockStyle.Fill;
        g.AutoSize = true;
        g.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        root.Controls.Add(g, 0, row);
    }

    private void ApplyTexts()
    {
        Text = T.Title;
        _gAdapter.Text = T.GroupAdapter; _gRelease.Text = T.GroupRelease; _gDisplays.Text = T.GroupDisplays;
        _gOptions.Text = T.GroupOptions; _gProgress.Text = T.GroupProgress; _gLog.Text = T.GroupLog;
        _lPort.Text = T.Port; _bRefresh.Text = T.RefreshPorts;
        _bConnect.Text = _client is null ? T.Connect : T.Disconnect;
        if (_client is null) _lAdapterState.Text = T.NotConnected;
        _bFolder.Text = T.ChooseFolder; _bZip.Text = T.ChooseZip; _bFiles.Text = T.ChooseFiles;
        if (_release is null && _looseFw is null && _looseWeb is null) _lRelease.Text = T.NoRelease;
        string[] cols = [T.ColAddress, T.ColBoard, T.ColLabel, T.ColVersion, T.ColState, T.ColAccept, T.ColWebUi, T.ColUid, T.ColStatus];
        for (int i = 0; i < cols.Length; i++) _lv.Columns[i].Text = cols[i];
        _bDiscover.Text = T.Discover; _bIdentify.Text = T.Identify; _bInstall.Text = T.Install; _bCancel.Text = T.Cancel;
        foreach (var b in new[] { _bRefresh, _bConnect, _bFolder, _bZip, _bFiles, _bDiscover, _bIdentify, _bInstall, _bCancel }) { b.AutoSize = true; b.MinimumSize = new Size(90, 28); }
        _lShare.Text = T.BusShare; _cAdaptive.Text = T.Adaptive; _cFw.Text = T.InstallFirmware; _cWeb.Text = T.InstallWebUi;
        _cCompress.Text = T.Compress; _lApply.Text = T.ApplyLabel; _lWebVer.Text = T.WebUiVersion; _cForce.Text = T.ForceVersion;
        int apply = _cbApply.SelectedIndex;
        _cbApply.Items.Clear();
        _cbApply.Items.AddRange([T.ApplyNow, T.ApplyNowNoWait, T.ApplyNext, T.ApplyNone]);
        _cbApply.SelectedIndex = apply < 0 ? 0 : apply;
        _lLang.Text = T.Language;
        _lStay.Text = "  ⚠  " + T.StayOpen;
        if (!_running) _lPhase.Text = T.Idle;
        if (_release is not null) ShowRelease();
    }

    private void UpdateButtons()
    {
        bool connected = _client is not null;
        _bDiscover.Enabled = connected && !_running;
        _bIdentify.Enabled = connected && !_running && _lv.SelectedItems.Count == 1 && _lv.SelectedItems[0].Tag is DisplayDevice;
        _bInstall.Enabled = connected && !_running && (_release is not null || _looseFw is not null || _looseWeb is not null);
        _bCancel.Enabled = _running;
        _bConnect.Enabled = !_running;
        _cbPort.Enabled = _bRefresh.Enabled = !connected;
        foreach (var c in new Control[] { _bFolder, _bZip, _bFiles, _nShare, _cAdaptive, _cFw, _cWeb, _cCompress, _cbApply }) c.Enabled = !_running;
        bool looseWebOnly = _release is null && _looseWeb is not null && _looseFw is null;
        _tWebVer.Enabled = _cForce.Enabled = !_running && looseWebOnly;
    }

    // ── Logging ───────────────────────────────────────────────────────────────
    private void Log(string msg)
    {
        string line = $"{DateTime.Now:HH:mm:ss}  {msg}";
        _log.Write(line);
        if (InvokeRequired) { BeginInvoke(() => AppendLog(line)); return; }
        AppendLog(line);
    }

    private void AppendLog(string line)
    {
        if (_tLog.TextLength > 400_000) _tLog.Text = _tLog.Text[^200_000..];
        _tLog.AppendText(line + Environment.NewLine);
    }

    // ── Questions and notes ───────────────────────────────────────────────────
    // A MessageBox takes its button captions from the Windows language, which can
    // differ from the updater's; a TaskDialog gets them from T.
    private bool Ask(string text, string title, TaskDialogIcon icon, bool defaultYes)
    {
        var yes = new TaskDialogButton(T.Yes);
        var no = new TaskDialogButton(T.No);
        var page = new TaskDialogPage
        {
            Caption = title, Text = text, Icon = icon,
            Buttons = { yes, no }, DefaultButton = defaultYes ? yes : no,
            AllowCancel = true,
        };
        return TaskDialog.ShowDialog(this, page) == yes;
    }

    private void Tell(string text, string title, TaskDialogIcon icon)
    {
        var page = new TaskDialogPage { Caption = title, Text = text, Icon = icon, Buttons = { new TaskDialogButton(T.Ok) }, AllowCancel = true };
        TaskDialog.ShowDialog(this, page);
    }

    // ── Adapter ───────────────────────────────────────────────────────────────
    private void RefreshPorts()
    {
        var canables = CanablePorts.FindAll().ToHashSet(StringComparer.OrdinalIgnoreCase);
        _cbPort.Items.Clear();
        if (Bridge is { Length: > 0 } && Bridge.StartsWith("tcp:", StringComparison.Ordinal)) _cbPort.Items.Add(new PortItem(Bridge, false, IsBridge: true));
        foreach (var p in CanablePorts.AllPorts()) _cbPort.Items.Add(new PortItem(p, canables.Contains(p)));
        var pick = _cbPort.Items.Cast<PortItem>().FirstOrDefault(p => p.IsBridge)
                ?? _cbPort.Items.Cast<PortItem>().FirstOrDefault(p => p.IsCanable)
                ?? _cbPort.Items.Cast<PortItem>().FirstOrDefault(p => p.Port == _settings.LastPort)
                ?? _cbPort.Items.Cast<PortItem>().FirstOrDefault();
        if (pick is not null) _cbPort.SelectedItem = pick;
    }

    private async Task ToggleConnect()
    {
        if (_client is not null) { Disconnect(); return; }
        if (_cbPort.SelectedItem is not PortItem port) { Tell(T.NoCanableFound, T.Title, TaskDialogIcon.Information); return; }
        if (!port.IsCanable && !port.IsBridge && !Ask(T.NotACanable(port.Port), T.Title, TaskDialogIcon.Warning, defaultYes: false))
            return;
        _lAdapterState.Text = T.Connecting;
        _bConnect.Enabled = false;
        N2kUpdateClient? client = null;
        try
        {
            string fixedPort = port.Port;
            int colon = fixedPort.LastIndexOf(':');
            // A CANable is looked up again at every reopen, because its port wanders (5.8).
            // Only the first open may fall back to the port chosen here: on a reopen that
            // name may belong to another device by now - a display's ESP32-S3, which the
            // DTR/RTS of the open would reset into its bootloader.
            bool firstOpen = true;
            var link = port.IsBridge ? SlcanFrameLink.ForTcp(fixedPort[4..colon], int.Parse(fixedPort[(colon + 1)..]))
                     : port.IsCanable ? new SlcanFrameLink(() =>
                       {
                           string? found = CanablePorts.Find();
                           if (firstOpen) { firstOpen = false; return found ?? fixedPort; }
                           return found;
                       })
                     : new SlcanFrameLink(fixedPort) { AssertDtrRts = false };
            _waiter = new WindowsHighResWaiter();
            var opt = new EngineOptions
            {
                UniqueNumber = _settings.UniqueNumber,
                PreferredAddress = _settings.PreferredAddress,
                ProductInfo = new ProductInfo { ProductCode = 2046, ModelId = "NauticPinnace Updater", SoftwareVersion = Application.ProductVersion.Split('+')[0], SerialCode = _settings.UniqueNumber.ToString() },
                BusLoad = BusOptions(),
            };
            client = new N2kUpdateClient(link, opt, _waiter);
            client.Log += Log;
            client.BusSampled += s => BeginInvoke(() => ShowBus(s));
            client.LinkStateChanged += up => BeginInvoke(() => ShowLinkState(up));
            await client.StartAsync();
            _client = client;
            _settings.LastPort = fixedPort;
            _settings.Save();
            _lAdapterState.Text = T.ConnectedAs(client.SourceAddress);
            Log($"{fixedPort}: {(_waiter.HighResolution ? "high-resolution timer" : "timeBeginPeriod(1)")} for pacing");
            UpdateButtons();
            _bConnect.Text = T.Disconnect;
            await DiscoverAsync();
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            // The client's engine thread already runs and waits through _waiter: stop it
            // before the waiter goes, or it would wait on a disposed timer.
            if (_client is null) client?.Dispose();
            _lAdapterState.Text = T.ConnectFailed(ex.Message);
            Log(T.ConnectFailed(ex.Message));
            Disconnect();
        }
        finally
        {
            _bConnect.Enabled = !_running;
        }
    }

    private void Disconnect()
    {
        _client?.Dispose();
        _client = null;
        _waiter?.Dispose();
        _waiter = null;
        _bConnect.Text = T.Connect;
        _lAdapterState.Text = T.NotConnected;
        UpdateButtons();
    }

    private void ShowBus(BusLoadSample s)
    {
        if (_client is null || _client.Engine.LinkLost) return;
        _lAdapterState.Text = T.ConnectedAs(_client.SourceAddress) + "   ·   " + T.BusLoad(s.OwnLoad, s.TotalLoad, s.EffectiveShare);
    }

    // The adapter went away (unplugged) or came back. Outside an update nothing
    // reopens it until the next search or identify, which try once a second.
    private void ShowLinkState(bool up)
    {
        if (_client is null) return;
        _lAdapterState.Text = up ? T.ConnectedAs(_client.SourceAddress) : T.AdapterLost;
        Log(up ? "CAN adapter usable again" : T.AdapterLost);
    }

    private BusLoadOptions BusOptions() => new()
    {
        BusShare = (double)_nShare.Value / 100.0,
        AdaptiveBackoff = _cAdaptive.Checked,
    };

    // ── Release ───────────────────────────────────────────────────────────────
    private void PickFolder()
    {
        using var d = new FolderBrowserDialog { Description = T.FolderPrompt, UseDescriptionForTitle = true };
        if (_settings.LastRelease is { } l && Directory.Exists(l)) d.InitialDirectory = l;
        if (d.ShowDialog(this) == DialogResult.OK) LoadRelease(d.SelectedPath);
    }

    private void PickZip()
    {
        using var d = new OpenFileDialog { Filter = T.ZipFilter };
        if (d.ShowDialog(this) == DialogResult.OK) LoadRelease(d.FileName);
    }

    private void PickFiles()
    {
        using var d = new OpenFileDialog { Filter = T.FilesFilter, Multiselect = true };
        if (d.ShowDialog(this) != DialogResult.OK) return;
        try
        {
            FirmwareImage? fw = null;
            WebUiImage? web = null;
            foreach (var f in d.FileNames)
            {
                byte[] head = new byte[4];
                using (var s = File.OpenRead(f)) _ = s.Read(head, 0, 4);
                if (head[0] == 0xE9) fw = FirmwareImage.Load(f);
                else web = ReleasePackage.LoadFiles(null, f).WebUi;
            }
            _release = null;
            _looseFw = fw;
            _looseWeb = web;
            var sb = new StringBuilder();
            if (fw is not null) sb.Append($"{Path.GetFileName(fw.Source)}: {Boards.DisplayName(fw.Board)} {fw.Version}" + (fw.IsLegacy ? $" ({T.LegacyFirmware})" : ""));
            // Decimal point in every language (owner's decision), not the Windows culture's mark.
            if (web is not null) sb.Append((sb.Length > 0 ? "  ·  " : "") + string.Create(CultureInfo.InvariantCulture, $"{T.WebUiOnly} {web.Size / 1024.0:F1} KB"));
            _lRelease.Text = sb.ToString();
            if (fw is null && web is not null && _tWebVer.Text.Length == 0) _tWebVer.Text = "";
            Log(_lRelease.Text);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            _lRelease.Text = T.LoadFailed(ex.Message);
        }
        UpdateButtons();
    }

    private void LoadRelease(string path)
    {
        try
        {
            _release = ReleasePackage.Load(path);
            _looseFw = null;
            _looseWeb = null;
            _settings.LastRelease = path;
            _settings.Save();
            ShowRelease();
            foreach (var n in _release.Notes) Log("  " + n);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            _release = null;
            _lRelease.Text = T.LoadFailed(ex.Message);
        }
        UpdateButtons();
    }

    private void ShowRelease()
    {
        if (_release is null) return;
        var parts = new List<string>();
        foreach (var (board, bp) in _release.Boards.OrderBy(b => b.Key))
        {
            string s = board == BoardId.Any ? T.WebUiOnly : Boards.DisplayName(board) + " " + (bp.Firmware?.Version is { Length: > 0 } v ? v : "?");
            if (bp.Firmware is { IsLegacy: true }) s += $" ({T.LegacyFirmware})";
            if (bp.WebUi is not null && board != BoardId.Any) s += " + " + T.WebUiOnly;
            parts.Add(s);
        }
        _lRelease.Text = Path.GetFileName(_release.Source.TrimEnd('\\', '/')) + ":  " + string.Join("  ·  ", parts);
        Log($"release {_release.Source}");
        foreach (var bp in _release.Boards.Values.Where(b => b.WebUiOrigin is not null))
            Log($"  {Boards.DisplayName(bp.Board)}: {T.WebUiFrom(bp.WebUiOrigin!)}");
    }

    // ── Displays ──────────────────────────────────────────────────────────────
    private async Task DiscoverAsync()
    {
        if (_client is null) return;
        _bDiscover.Enabled = false;
        _lPhase.Text = T.Searching;
        try
        {
            var r = await _client.DiscoverAsync();
            FillList(r);
            _lPhase.Text = r.AdapterLost ? T.AdapterLost : r.Displays.Count + r.Legacy.Count == 0 ? T.NoneFound : T.Idle;
            if (r.AdapterLost) _lAdapterState.Text = T.AdapterLost;
        }
        finally { UpdateButtons(); }
    }

    private void FillList(DiscoveryResult r)
    {
        var keep = _lv.Items.Cast<ListViewItem>().Where(i => i.Tag is DisplayDevice).ToDictionary(i => ((DisplayDevice)i.Tag!).Uid, i => i.SubItems[8].Text);
        _lv.BeginUpdate();
        _lv.Items.Clear();
        foreach (var d in r.Displays)
        {
            var i = d.Info;
            var item = new ListViewItem([
                d.Address.ToString(), Boards.DisplayName(i.Board), i.Label, i.RunningVersion,
                i.OtaState switch { OtaState.Valid => T.OtaValid, OtaState.PendingVerify => T.OtaPending, _ => "–" },
                (i.Flags & StatusFlags.AcceptUpdates) != 0 ? T.On : T.Off, i.WebUiVersion.Length > 0 ? i.WebUiVersion : "?",
                i.Uid.LastSixHex, keep.GetValueOrDefault(i.Uid, ""),
            ]) { Tag = d };
            _lv.Items.Add(item);
        }
        foreach (var l in r.Legacy)
        {
            var item = new ListViewItem([l.Address.ToString(), "?", "", l.SoftwareVersion, "", "", "", "", T.LegacyHint(l.SoftwareVersion)])
            { Tag = l, ForeColor = SystemColors.GrayText };
            _lv.Items.Add(item);
        }
        if (r.Displays.Count == 1) _lv.Items[0].Checked = true;
        _lv.EndUpdate();
        Log($"{r.Displays.Count} display(s), {r.Legacy.Count} without update support");
    }

    private async Task IdentifyAsync()
    {
        if (_client is null || _lv.SelectedItems.Count != 1 || _lv.SelectedItems[0].Tag is not DisplayDevice d) return;
        var item = _lv.SelectedItems[0];
        bool sent = await _client.IdentifyAsync(d, 10);
        item.SubItems[8].Text = sent ? T.IdentifySent : T.IdentifyFailed;
        if (!sent) _lAdapterState.Text = T.AdapterLost;
    }

    // ── Install ───────────────────────────────────────────────────────────────
    private UpdateRequest? RequestFor(DisplayDevice d)
    {
        if (_release is not null)
        {
            var bp = _release.For(d.Board) ?? _release.For(BoardId.Any);
            if (bp is null) return null;
            return new UpdateRequest { Firmware = bp.Firmware, WebUi = bp.WebUi, WebUiVersion = bp.WebUiVersion };
        }
        if (_looseFw is null && _looseWeb is null) return null;
        // 12.4: for a loose web UI file the user enters the firmware version it belongs
        // to, or skips the version check. Without either the library refuses it - taking
        // the display's running version instead would make the display's check
        // (VERSION_MISMATCH) pass for any file. Only with the check skipped (the OFFER then
        // carries ALLOW_VERSION_MISMATCH) does the running version fill the field.
        string? typed = _tWebVer.Text.Trim() is { Length: > 0 } t ? t : null;
        string? ver = _looseFw?.Descriptor?.FwVersion ?? typed ?? (ForceVersion ? d.Info.RunningVersion : null);
        return new UpdateRequest { Firmware = _looseFw, WebUi = _looseWeb, WebUiVersion = ver };
    }

    private bool ForceVersion => _cForce.Checked && _cForce.Enabled;

    private UpdateOptions Options() => new()
    {
        BusLoad = BusOptions(),
        Compression = _cCompress.Checked ? CompressionMode.Auto : CompressionMode.Off,
        ApplyMode = _cbApply.SelectedIndex switch { 1 => ApplyMode.NowNoWait, 2 => ApplyMode.NextRestart, 3 => ApplyMode.None, _ => ApplyMode.Now },
        InstallFirmware = _cFw.Checked,
        InstallWebUi = _cWeb.Checked,
        IncludeCompanionWebUi = true,
        AllowWebUiVersionMismatch = ForceVersion,
        SenderName = "PC Updater",
        SenderKind = SenderKind.StandaloneUpdater,
        CacheDirectory = UpdaterSettings.CacheFolder,
    };

    private async Task InstallAsync()
    {
        if (_client is null) return;
        var items = _lv.CheckedItems.Cast<ListViewItem>().Where(i => i.Tag is DisplayDevice).ToList();
        if (items.Count == 0 && _lv.SelectedItems.Count == 1 && _lv.SelectedItems[0].Tag is DisplayDevice) items.Add(_lv.SelectedItems[0]);
        if (items.Count == 0) { Tell(T.SelectDisplay, T.Title, TaskDialogIcon.Information); return; }

        var plan = new List<(ListViewItem Item, DisplayDevice Dev, UpdateRequest Req)>();
        var sb = new StringBuilder(T.ConfirmIntro + "\n\n");
        foreach (var it in items)
        {
            var d = (DisplayDevice)it.Tag!;
            var req = RequestFor(d);
            string who = $"{Boards.DisplayName(d.Board)} \"{d.Info.Label}\" ({d.Info.Uid.LastSixHex}), {d.Info.RunningVersion}";
            if (req is null) { sb.Append($"•  {who}: {T.NothingForDisplay}\n"); continue; }
            var what = new List<string>();
            if (_cFw.Checked && req.Firmware is not null) what.Add($"{T.InstallFirmware} {req.Firmware.Version}");
            if (_cWeb.Checked && req.WebUi is not null) what.Add(T.InstallWebUi);
            sb.Append($"•  {who}:  {string.Join(" + ", what)}\n");
            plan.Add((it, d, req));
        }
        sb.Append('\n').Append(T.ConfirmOutro);
        if (plan.Count == 0 || !Ask(sb.ToString(), T.ConfirmTitle, TaskDialogIcon.Information, defaultYes: true))
            return;

        _settings.BusSharePercent = (int)_nShare.Value;
        _settings.AdaptiveBackoff = _cAdaptive.Checked;
        _settings.Compression = _cCompress.Checked;
        _settings.ApplyMode = Options().ApplyMode.ToString();
        _settings.Save();

        _running = true;
        _cts = new CancellationTokenSource();
        UpdateButtons();
        var results = new StringBuilder();
        // Keeps the laptop awake; disposed on this (UI) thread after the loop (6.6).
        using (WindowsHighResWaiter.KeepAwake())
        {
            foreach (var (item, dev, req) in plan)
            {
                if (_cts.IsCancellationRequested) break;
                _lv.Items.Cast<ListViewItem>().ToList().ForEach(i => i.BackColor = SystemColors.Window);
                item.BackColor = Color.FromArgb(225, 238, 255);
                var progress = new Progress<UpdateProgress>(p => OnProgress(item, p));
                UpdateReport? rep = null;
                try
                {
                    rep = await _client.UpdateAsync(dev, req, Options(), progress, _cts.Token);
                }
                catch (OperationCanceledException)
                {
                    Log("cancelled");
                }
                catch (Exception ex) when (ex is IOException or InvalidOperationException or InvalidDataException or UnauthorizedAccessException)
                {
                    Log("error: " + ex.Message);
                }
                string fwText = rep?.Firmware is { } f ? $"{T.InstallFirmware}: {T.Outcome(f)}" : "";
                string webText = rep?.WebUi is { } w ? $"{T.InstallWebUi}: {T.Outcome(w)}" : "";
                string line = string.Join("  ·  ", new[] { fwText, webText }.Where(s => s.Length > 0));
                item.SubItems[8].Text = line;
                item.BackColor = rep?.IsGood == true ? Color.FromArgb(220, 245, 220) : Color.FromArgb(255, 225, 225);
                results.Append($"{Boards.DisplayName(dev.Board)} \"{dev.Info.Label}\": {line}\n");
                Log($"{dev}: {line}");
            }
        }
        _running = false;
        _mustStay = false;
        _lStay.Visible = false;
        _progress.Value = 0;
        _lPhase.Text = T.Idle;
        UpdateButtons();
        if (_closeAfterRun) { Close(); return; }
        Tell(results.ToString(), T.Summary, TaskDialogIcon.Information);
        await DiscoverAsync();
    }

    private void OnProgress(ListViewItem item, UpdateProgress p)
    {
        _mustStay = p.MustStayConnected;
        _lStay.Visible = p.MustStayConnected;
        string phase = $"{T.Target(p.Target)}: {T.Phase(p)}";
        _lPhase.Text = phase + (p.Phase == SenderPhase.Transferring ? "   ·   " + T.BusLoad(p.OwnLoad, p.TotalLoad, p.EffectiveShare) : "");
        _progress.Value = p.Phase switch
        {
            SenderPhase.Transferring or SenderPhase.PausedForeignTransfer => (int)Math.Clamp(p.Fraction * 1000, 0, 1000),
            SenderPhase.Verifying or SenderPhase.Applying or SenderPhase.WaitingForValidation or SenderPhase.CheckingWebUi or SenderPhase.Done => 1000,
            _ => 0,
        };
        item.SubItems[8].Text = phase;
    }

    private void CancelRun()
    {
        if (!_running || _cts is null) return;
        if (_mustStay && !Ask(T.CloseDuringCheck, T.CloseDuringCheckTitle, TaskDialogIcon.Warning, defaultYes: false))
            return;
        _cts.Cancel();
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (_running && !_closeAfterRun)
        {
            // H5: while the display validates, closing makes it roll back (12.7).
            string q = _mustStay ? T.CloseDuringCheck : T.CloseDuringTransfer;
            if (!Ask(q, _mustStay ? T.CloseDuringCheckTitle : T.Title, TaskDialogIcon.Warning, defaultYes: false))
            {
                e.Cancel = true;
                return;
            }
            // Let the ABORT go out first; the install loop closes the window when done.
            _closeAfterRun = true;
            e.Cancel = true;
            _cts?.Cancel();
            return;
        }
        Disconnect();
        _log.Dispose();
        base.OnFormClosing(e);
    }
}

/// <summary>The log file of 12.7: one file per day in %LOCALAPPDATA%\NauticPinnaceUpdater\logs.</summary>
internal sealed class UpdaterLog : IDisposable
{
    private readonly StreamWriter? _w;
    private readonly object _lock = new();

    public UpdaterLog(string folder)
    {
        Path = System.IO.Path.Combine(folder, $"updater-{DateTime.Now:yyyyMMdd}.log");
        try
        {
            Directory.CreateDirectory(folder);
            _w = new StreamWriter(Path, append: true, new UTF8Encoding(false)) { AutoFlush = true };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _w = null;
        }
    }

    public string Path { get; }

    public void Write(string line)
    {
        lock (_lock)
        {
            try { _w?.WriteLine(line); }
            catch (IOException) { }
        }
    }

    public void Dispose()
    {
        lock (_lock) _w?.Dispose();
    }
}
