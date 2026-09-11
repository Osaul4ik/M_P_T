using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using Forms = System.Windows.Forms;
using AmtPtpConfigGui.Native;
using Microsoft.Win32;
using Application = System.Windows.Application;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Color = System.Windows.Media.Color;
using Point = System.Windows.Point;
using FontFamily = System.Windows.Media.FontFamily;
using Orientation = System.Windows.Controls.Orientation;
using Button = System.Windows.Controls.Button;
using TextBox = System.Windows.Controls.TextBox;
using RadioButton = System.Windows.Controls.RadioButton;
using Rectangle = System.Windows.Shapes.Rectangle;
using CheckBox = System.Windows.Controls.CheckBox;
using ColorConverter = System.Windows.Media.ColorConverter;
using MessageBox = System.Windows.MessageBox;
using SaveFileDialog = Microsoft.Win32.SaveFileDialog;
using OpenFileDialog = Microsoft.Win32.OpenFileDialog;

namespace AmtPtpConfigGui
{
    public partial class MainWindow : Window
    {
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            IncludeFields = true
        };

        private static SolidColorBrush Frozen(byte a, byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(Color.FromArgb(a, r, g, b));
            brush.Freeze();
            return brush;
        }

        // Status-dot colors follow the active theme (Light/Dark) instead of a
        // fixed palette, so StatusDot/LiveDot never clash with the rest of
        // the themed chrome after a theme switch. Falls back to the frozen
        // light-theme tone only if the resource lookup fails (e.g. resource
        // dictionary not yet initialized).
        private static Brush ConnectedBrush =>
            System.Windows.Application.Current?.Resources["StatusOnlineBrush"] as Brush
                ?? Frozen(0xFF, 0x1D, 0x9A, 0x6C);
        private static Brush DisconnectedBrush =>
            System.Windows.Application.Current?.Resources["StatusOfflineBrush"] as Brush
                ?? Frozen(0xFF, 0xE5, 0x48, 0x4D);
        private static Brush LiveDotIdleBrush =>
            System.Windows.Application.Current?.Resources["StatusIdleBrush"] as Brush
                ?? Frozen(0xFF, 0x9A, 0xA1, 0xAC);
        private static readonly LinearGradientBrush GlassPadBrush = CreateGlassPadBrush();

        private static LinearGradientBrush CreateGlassPadBrush()
        {
            // Brushed-aluminum silver, like the MacBook's own trackpad glass
            // set into its aluminum body, instead of the previous dark
            // "glass device" look.
            var brush = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1),
            };
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xF3, 0xF3, 0xF4), 0.0));
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xE2, 0xE3, 0xE5), 0.55));
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xD3, 0xD4, 0xD7), 1.0));
            brush.Freeze();
            return brush;
        }

        private static readonly SolidColorBrush GlassPadStrokeBrush = Frozen(0xFF, 0xB7, 0xB9, 0xBD);
        private static readonly SolidColorBrush CrosshairBrush = Frozen(0x50, 0x00, 0x00, 0x00);
        private static readonly SolidColorBrush EdgeZoneBrush = Frozen(60, 0xFF, 0x6D, 0x2E);
        private static readonly SolidColorBrush EdgeZoneLabelBrush = Frozen(0xFF, 0xC4, 0x45, 0x0B);

        // Test-touch classification fill - the same semantic accepted/
        // soft-reject/hard-reject colors, tuned to stay legible (with a
        // dark stroke, see DrawPreview/AddCross) on the silver pad surface.
        private static readonly SolidColorBrush PalmNoneBrush = Frozen(225, 0x1D, 0x9A, 0x6C);
        private static readonly SolidColorBrush PalmLocalBrush = Frozen(225, 0xE0, 0x9A, 0x0A);
        private static readonly SolidColorBrush PalmLargeBrush = Frozen(225, 0xE5, 0x48, 0x4D);

        // Live-overlay outline colors reuse WPF's own static Brushes.* -
        // those are already immutable singletons, no allocation there -
        // except the palm-suspect outline, which is unified with
        // LiveFillPalm's exact hue below instead of the mismatched
        // built-in Brushes.Firebrick previously used only for the outline.
        private static readonly SolidColorBrush LivePalmOutline = Frozen(0xFF, 0xE8, 0x11, 0x23);
        private static readonly SolidColorBrush LiveFillDown = Frozen(90, Colors.LimeGreen.R, Colors.LimeGreen.G, Colors.LimeGreen.B);
        private static readonly SolidColorBrush LiveFillUp = Frozen(90, Colors.Orange.R, Colors.Orange.G, Colors.Orange.B);
        private static readonly SolidColorBrush LiveFillMove = Frozen(90, Colors.DeepSkyBlue.R, Colors.DeepSkyBlue.G, Colors.DeepSkyBlue.B);
        private static readonly SolidColorBrush LiveFillPalm = Frozen(150, 0xE8, 0x11, 0x23);
        private static readonly SolidColorBrush LiveTagBgBrush = Frozen(230, 0x14, 0x16, 0x1B);

        private readonly DeviceIo _device = new DeviceIo();
        private readonly List<string> _diagnosticLog = new();
        private PadGeometry _geometry = PadGeometry.Fallback;
        private bool _suppressEvents;
        private bool _uiReady;
        private bool _profilesReady;
        private readonly List<GuiProfile> _profiles = new();
        private int _activeProfileIndex = -1;

        private readonly AppSettings _appSettings;
        private readonly Forms.NotifyIcon _trayIcon;
        private bool _allowWindowClose;
        private bool _settingsDialogOpen;

        // Cached tray menu Font/Renderer. Both used to be recreated on every
        // RefreshTrayMenu() call (every tray icon right-click, plus every
        // tray toggle) without disposing the instance they replaced -
        // System.Drawing.Font wraps a native GDI font handle, so that leaked
        // one real OS handle per refresh for as long as the app stayed
        // parked in the tray. Resolving/building once and reusing removes
        // both the leak and the repeated FontFamily.Families enumeration.
        private System.Drawing.Font? _trayFont;
        private string? _trayRendererThemeId;

        private readonly DispatcherTimer _liveRenderTimer;
        private CancellationTokenSource? _liveCts;
        private Task? _livePollTask;
        private int _livePollGeneration;
        private readonly object _liveFrameLock = new();
        private LiveFrame _latestLiveFrame;
        private bool _hasLatestLiveFrame;
        private bool _liveEnabled;
        // Set when Live polling/rendering is paused for a close-to-tray
        // Hide() (see MainWindow_Closing / ResumeLiveAfterShow). Distinct
        // from _liveEnabled/ChkLive.IsChecked, which stay true the whole
        // time - this only tracks "should ShowFromTray restart the pump."
        private bool _liveWasEnabledBeforeHide;
        // Whether the detailed live touch-contact list (LiveCoordPanel) is
        // expanded. Reset to false every time Live is (re)enabled so the
        // panel always starts collapsed by default.
        private bool _liveDetailsExpanded;
        private uint _lastLiveSequence;
        private int _liveTelemetryTickCounter;
        private bool _liveShadowsSuppressed;

        private const int LiveOverlaySlots = 5;
        private const double LiveGeometrySmoothAlpha = 0.25;

        private sealed class CornerExtrema
        {
            public int Samples;

            public short MinRawX = short.MaxValue;
            public short MaxRawX = short.MinValue;
            public short MinRawY = short.MaxValue;
            public short MaxRawY = short.MinValue;

            public ushort MinNormX = ushort.MaxValue;
            public ushort MaxNormX = 0;
            public ushort MinNormY = ushort.MaxValue;
            public ushort MaxNormY = 0;

            public void Update(short rawX, short rawY, ushort normX, ushort normY)
            {
                Samples++;

                if (rawX < MinRawX) MinRawX = rawX;
                if (rawX > MaxRawX) MaxRawX = rawX;
                if (rawY < MinRawY) MinRawY = rawY;
                if (rawY > MaxRawY) MaxRawY = rawY;

                if (normX < MinNormX) MinNormX = normX;
                if (normX > MaxNormX) MaxNormX = normX;
                if (normY < MinNormY) MinNormY = normY;
                if (normY > MaxNormY) MaxNormY = normY;
            }

            public string ToText()
            {
                if (Samples == 0)
                    return "samples=0";

                return
                    $"samples={Samples}; " +
                    $"RawX=[{MinRawX}..{MaxRawX}] " +
                    $"RawY=[{MinRawY}..{MaxRawY}] " +
                    $"NormX=[{MinNormX}..{MaxNormX}] " +
                    $"NormY=[{MinNormY}..{MaxNormY}]";
            }
        }

        private readonly CornerExtrema _topLeft = new();
        private readonly CornerExtrema _topRight = new();
        private readonly CornerExtrema _bottomLeft = new();
        private readonly CornerExtrema _bottomRight = new();
        private int _liveCornerSamples;

        public MainWindow()
        {
            _appSettings = AppSettingsStore.Load();

            InitializeComponent();
            AddHandler(UIElement.PreviewMouseWheelEvent, new MouseWheelEventHandler(SlowScrollWheel), true);
            ChkPalmEdgeRejection.IsChecked = _appSettings.PalmEdgeRejectionEnabled;
            _appSettings.Theme = ThemeManager.Get(_appSettings.Theme).Id;
            ThemeManager.Apply(_appSettings.Theme, Resources);
            InitializeProfiles();
            UpdateProModeVisibility();
            _uiReady = true;

            var appIcon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath ?? string.Empty);
            _trayIcon = new Forms.NotifyIcon
            {
                Icon = appIcon ?? System.Drawing.SystemIcons.Application,
                Visible = true,
                Text = "Wellspring Control Center"
            };
            _trayIcon.DoubleClick += (_, _) => ShowFromTray();
            _trayIcon.ContextMenuStrip = BuildTrayMenu();
            _trayIcon.ContextMenuStrip.Opening += (_, _) => RefreshTrayMenu();

            Closing += MainWindow_Closing;

            _liveRenderTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(42)
            };
            _liveRenderTimer.Tick += LiveRenderTimer_Tick;

            Loaded += (_, _) =>
            {
                if (_appSettings.StartWithWindows)
                    UpdateStartupRegistration(true);
                Reconnect();
                RefreshTrayMenu();
            };
        }

        // ---------------------------------------------------------------
        // Application / tray settings
        // ---------------------------------------------------------------

        private void AppSettings_Click(object sender, RoutedEventArgs e)
        {
            ShowAppSettingsDialog();
        }

        private void ShowAppSettingsDialog()
        {
            if (_settingsDialogOpen) return;
            _settingsDialogOpen = true;

            string originalTheme = ThemeManager.CurrentThemeId;
            string selectedTheme = _appSettings.Theme;
            bool settingsCommitted = false;

            var dialog = new Window
            {
                Title = "Wellspring Control Center Settings",
                Width = 540,
                SizeToContent = SizeToContent.Height,
                MinWidth = 500,
                MaxHeight = 640,
                ResizeMode = ResizeMode.NoResize,
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Owner = this,
                WindowStyle = WindowStyle.ToolWindow,
                ShowInTaskbar = false,
                FontFamily = new FontFamily("Segoe UI Variable Text, Segoe UI"),
                FontSize = 13
            };
            // This dialog is a standalone Window, not part of MainWindow's visual
            // tree, so DynamicResource/StaticResource lookups (AndroidToggle,
            // PageBrush, CardBrush, ThemePreviewButton, ...) would otherwise fall
            // through to the (empty) Application resources and silently render
            // with default WPF chrome - e.g. the tray/startup toggles falling back
            // to plain checkboxes instead of the app's switch style. Merging
            // MainWindow's resource dictionary in fixes every lookup below at once.
            dialog.Resources.MergedDictionaries.Add(Resources);
            dialog.SetResourceReference(System.Windows.Controls.Control.BackgroundProperty, "PageBrush");

            var root = new Grid { Margin = new Thickness(22) };
            // Was a "*" row: with a fixed-height window that made the ScrollViewer
            // stretch to fill unused space, showing an empty gap below the cards.
            // Now the window sizes to content (SizeToContent above) and the row
            // sizes to content too, so the dialog is only as tall as it needs to be.
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var scroll = new ScrollViewer
            {
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                MaxHeight = 520
            };
            var content = new StackPanel();
            scroll.Content = content;
            Grid.SetRow(scroll, 0);
            root.Children.Add(scroll);

            Border MakeCard(string title, string caption)
            {
                var card = new Border
                {
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(12),
                    Padding = new Thickness(16),
                    Margin = new Thickness(0, 0, 0, 12)
                };
                var panel = new StackPanel();
                var titleText = new TextBlock
                {
                    Text = title,
                    FontSize = 14,
                    FontWeight = FontWeights.SemiBold
                };
                titleText.SetResourceReference(TextBlock.ForegroundProperty, "TextPrimaryBrush");
                panel.Children.Add(titleText);
                var captionText = new TextBlock
                {
                    Text = caption,
                    FontSize = 11,
                    Margin = new Thickness(0, 2, 0, 12),
                    TextWrapping = TextWrapping.Wrap
                };
                captionText.SetResourceReference(TextBlock.ForegroundProperty, "TextTertiaryBrush");
                panel.Children.Add(captionText);
                card.SetResourceReference(Border.BackgroundProperty, "CardBrush");
                card.SetResourceReference(Border.BorderBrushProperty, "CardBorderBrush");
                card.Child = panel;
                card.Tag = panel;
                return card;
            }

            var behaviorCard = MakeCard(
                "Behavior",
                "Control how the application stays available and starts with Windows.");
            var behaviorPanel = (StackPanel)behaviorCard.Tag!;
            var behaviorRows = new Grid();
            behaviorRows.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            behaviorRows.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

            var closeToTray = new System.Windows.Controls.Primitives.ToggleButton
            {
                Content = "Minimize to tray",
                IsChecked = _appSettings.CloseToTray,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Left,
                VerticalAlignment = System.Windows.VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 8, 0)
            };
            closeToTray.SetResourceReference(System.Windows.Controls.Control.StyleProperty, "AndroidToggle");
            closeToTray.SetResourceReference(System.Windows.Controls.Control.ForegroundProperty, "TextPrimaryBrush");
            Grid.SetColumn(closeToTray, 0);

            var startup = new System.Windows.Controls.Primitives.ToggleButton
            {
                Content = "Start the GUI with Windows",
                IsChecked = _appSettings.StartWithWindows,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Left,
                VerticalAlignment = System.Windows.VerticalAlignment.Center,
                Margin = new Thickness(8, 0, 0, 0)
            };
            startup.SetResourceReference(System.Windows.Controls.Control.StyleProperty, "AndroidToggle");
            startup.SetResourceReference(System.Windows.Controls.Control.ForegroundProperty, "TextPrimaryBrush");
            Grid.SetColumn(startup, 1);

            behaviorRows.Children.Add(closeToTray);
            behaviorRows.Children.Add(startup);
            behaviorPanel.Children.Add(behaviorRows);
            content.Children.Add(behaviorCard);

            var themeCard = MakeCard(
                "Appearance",
                "Choose the palette preview. The selected theme is kept when you press Save.");
            var themePanel = (StackPanel)themeCard.Tag!;
            var themeButtons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                Margin = new Thickness(0, 4, 0, 0)
            };

            void UpdateThemePreview()
            {
                themeButtons.Children.Clear();
                foreach (var theme in ThemeManager.Themes)
                {
                    var borderColor = (Color)ColorConverter.ConvertFromString(theme.Outline)!;
                    var bg = (Color)ColorConverter.ConvertFromString(theme.Bg)!;
                    var surface = (Color)ColorConverter.ConvertFromString(theme.Surface)!;
                    var surfaceHigh = (Color)ColorConverter.ConvertFromString(theme.SurfaceHigh)!;
                    var primary = (Color)ColorConverter.ConvertFromString(theme.Primary)!;

                    var button = new Button
                    {
                        Width = 210,
                        Height = 88,
                        Margin = new Thickness(0, 0, theme.Id == "light" ? 12 : 0, 0),
                        Padding = new Thickness(0),
                        ToolTip = theme.Name,
                        Style = (Style)FindResource("ThemePreviewButton"),
                        HorizontalContentAlignment = System.Windows.HorizontalAlignment.Stretch,
                        VerticalContentAlignment = System.Windows.VerticalAlignment.Stretch
                    };

                    var preview = new Border
                    {
                        Background = new SolidColorBrush(bg),
                        BorderBrush = new SolidColorBrush(borderColor),
                        BorderThickness = string.Equals(theme.Id, selectedTheme, StringComparison.OrdinalIgnoreCase) ? new Thickness(2) : new Thickness(1),
                        CornerRadius = new CornerRadius(10),
                        Padding = new Thickness(10),
                        ClipToBounds = true
                    };
                    var previewGrid = new Grid();
                    previewGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(12) });
                    previewGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

                    var topBar = new Border
                    {
                        Background = new SolidColorBrush(surface),
                        CornerRadius = new CornerRadius(7, 7, 0, 0),
                        Margin = new Thickness(1)
                    };
                    Grid.SetRow(topBar, 0);
                    previewGrid.Children.Add(topBar);

                    var miniCard = new Border
                    {
                        Width = 164,
                        Height = 52,
                        HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                        VerticalAlignment = System.Windows.VerticalAlignment.Center,
                        Background = new SolidColorBrush(surfaceHigh),
                        BorderBrush = new SolidColorBrush(borderColor),
                        BorderThickness = new Thickness(1),
                        CornerRadius = new CornerRadius(6),
                        Margin = new Thickness(0, 10, 0, 0)
                    };
                    Grid.SetRow(miniCard, 1);
                    previewGrid.Children.Add(miniCard);

                    var accent = new Border
                    {
                        Width = 44,
                        Height = 7,
                        HorizontalAlignment = System.Windows.HorizontalAlignment.Left,
                        VerticalAlignment = System.Windows.VerticalAlignment.Top,
                        Margin = new Thickness(14, 12, 0, 0),
                        Background = new SolidColorBrush(primary),
                        CornerRadius = new CornerRadius(3)
                    };
                    Grid.SetRow(accent, 1);
                    previewGrid.Children.Add(accent);

                    var accent2 = new Border
                    {
                        Width = 28,
                        Height = 7,
                        HorizontalAlignment = System.Windows.HorizontalAlignment.Left,
                        VerticalAlignment = System.Windows.VerticalAlignment.Top,
                        Margin = new Thickness(68, 12, 0, 0),
                        Background = new SolidColorBrush(primary),
                        CornerRadius = new CornerRadius(3)
                    };
                    Grid.SetRow(accent2, 1);
                    previewGrid.Children.Add(accent2);

                    var accent3 = new Border
                    {
                        Width = 38,
                        Height = 7,
                        HorizontalAlignment = System.Windows.HorizontalAlignment.Left,
                        VerticalAlignment = System.Windows.VerticalAlignment.Top,
                        Margin = new Thickness(14, 30, 0, 0),
                        Background = new SolidColorBrush(primary),
                        CornerRadius = new CornerRadius(3)
                    };
                    Grid.SetRow(accent3, 1);
                    previewGrid.Children.Add(accent3);
                    preview.Child = previewGrid;
                    button.Content = preview;

                    button.Click += (_, _) =>
                    {
                        selectedTheme = theme.Id;
                        ThemeManager.Apply(selectedTheme, Resources);
                        dialog.SetResourceReference(System.Windows.Controls.Control.BackgroundProperty, "PageBrush");
                        UpdateThemePreview();
                    };
                    themeButtons.Children.Add(button);
                }
            }

            UpdateThemePreview();
            themePanel.Children.Add(themeButtons);
            content.Children.Add(themeCard);

            var backupCard = MakeCard(
                "Backup & restore",
                "Export all profiles and application settings to one file, or restore a previous backup.");
            var backupPanel = (StackPanel)backupCard.Tag!;
            var backupButtons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = System.Windows.HorizontalAlignment.Center };
            var exportBackup = new Button
            {
                Content = "Export backup...",
                Style = (Style)FindResource("SecondaryButton"),
                Width = 132,
                Height = 36,
                Margin = new Thickness(0, 0, 8, 0)
            };
            var restoreBackup = new Button
            {
                Content = "Restore backup...",
                Style = (Style)FindResource("SecondaryButton"),
                Width = 132,
                Height = 36
            };
            exportBackup.Click += (_, _) => ExportBackup();
            restoreBackup.Click += (_, _) => RestoreBackup();
            backupButtons.Children.Add(exportBackup);
            backupButtons.Children.Add(restoreBackup);
            backupPanel.Children.Add(backupButtons);
            content.Children.Add(backupCard);

            var footer = new Grid { Margin = new Thickness(0, 12, 0, 0) };
            footer.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            footer.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var footerHint = new TextBlock
            {
                Text = "Changes are committed when you press Save; Cancel restores the previous theme.",
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                TextAlignment = TextAlignment.Center,
                FontSize = 11,
                Margin = new Thickness(0, 0, 0, 10)
            };
            footerHint.SetResourceReference(TextBlock.ForegroundProperty, "TextTertiaryBrush");
            Grid.SetRow(footerHint, 0);
            footer.Children.Add(footerHint);

            var footerButtons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Right
            };
            var cancel = new Button
            {
                Content = "Cancel",
                Style = (Style)FindResource("GhostButton"),
                Width = 96,
                Height = 34,
                Margin = new Thickness(0, 0, 8, 0)
            };
            var save = new Button
            {
                Content = "Save",
                Style = (Style)FindResource("AccentButton"),
                Width = 96,
                Height = 34
            };
            footerButtons.Children.Add(cancel);
            footerButtons.Children.Add(save);
            Grid.SetRow(footerButtons, 1);
            footer.Children.Add(footerButtons);

            Grid.SetRow(footer, 1);
            root.Children.Add(footer);

            cancel.Click += (_, _) =>
            {
                settingsCommitted = true;
                ThemeManager.Apply(originalTheme, Resources);
                RefreshTrayMenu();
                dialog.Close();
            };

            save.Click += (_, _) =>
            {
                settingsCommitted = true;
                _appSettings.CloseToTray = closeToTray.IsChecked == true;
                _appSettings.StartWithWindows = startup.IsChecked == true;
                _appSettings.Theme = ThemeManager.Get(selectedTheme).Id;
                TrySaveAppSettings();
                UpdateStartupRegistration(_appSettings.StartWithWindows);

                RefreshTrayMenu();
                SetBottomStatus("Application settings saved.");
                dialog.Close();
            };

            dialog.Content = root;
            dialog.Closed += (_, _) =>
            {
                if (!settingsCommitted)
                {
                    ThemeManager.Apply(originalTheme, Resources);
                    RefreshTrayMenu();
                }
                _settingsDialogOpen = false;
            };
            dialog.ShowDialog();
        }

        private void ExportBackup()
        {
            var dlg = new SaveFileDialog
            {
                Filter = "Wellspring Control Center backup (*.wspbackup.json)|*.wspbackup.json|JSON (*.json)|*.json",
                FileName = $"WellspringPTP-Backup-{DateTime.Now:yyyy-MM-dd}.wspbackup.json",
                Title = "Export backup"
            };
            if (dlg.ShowDialog() != true)
                return;

            try
            {
                // Capture the values currently visible in the GUI as the active profile,
                // so the backup exactly represents what the user sees before pressing Save.
                var active = ActiveProfile;
                if (active != null)
                {
                    active.Palm = ReadConfigFromSliders();
                    active.Pointer = ReadPointerConfigFromControls();
                    active.Scroll = ReadScrollConfigFromControls();
                }

                var backup = new WellspringBackup
                {
                    Version = 2,
                    AppSettings = new AppSettings
                    {
                        CloseToTray = _appSettings.CloseToTray,
                        StartWithWindows = _appSettings.StartWithWindows,
                        PalmEdgeRejectionEnabled = _appSettings.PalmEdgeRejectionEnabled,
                        Theme = _appSettings.Theme
                    },
                    Profiles = _profiles.Select(CloneProfile).ToList(),
                    ActiveProfileIndex = _activeProfileIndex
                };

                var json = JsonSerializer.Serialize(backup, JsonOptions);
                File.WriteAllText(dlg.FileName, json);
                SetBottomStatus($"Backup saved: {dlg.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Failed to create backup.\n\n{ex.Message}", "Backup", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void RestoreBackup()
        {
            var dlg = new OpenFileDialog
            {
                Filter = "Wellspring Control Center backup (*.wspbackup.json)|*.wspbackup.json|JSON (*.json)|*.json",
                Title = "Restore backup"
            };
            if (dlg.ShowDialog() != true)
                return;

            try
            {
                var json = File.ReadAllText(dlg.FileName);
                var backup = JsonSerializer.Deserialize<WellspringBackup>(json, JsonOptions);
                if (backup == null || backup.Profiles == null || backup.Profiles.Count == 0)
                    throw new InvalidDataException("The file does not contain a valid profile list.");

                if (backup.Version < 1)
                    throw new InvalidDataException("Unsupported backup version.");

                var restoredProfiles = backup.Profiles.Select(CloneProfile).ToList();
                foreach (var profile in restoredProfiles)
                {
                    profile.Name = string.IsNullOrWhiteSpace(profile.Name) ? "Profile" : profile.Name.Trim();
                    profile.Palm = profile.Palm.StructVersion == 0 ? PalmConfig.Default : profile.Palm.Clamped();
                    profile.Pointer = NormalizePointerConfig(profile.Pointer);
                    profile.Scroll = profile.Scroll.StructVersion == 0 ? ScrollConfig.Default : profile.Scroll.Clamped();
                }

                _profilesReady = false;
                _profiles.Clear();
                _profiles.AddRange(restoredProfiles);
                _activeProfileIndex = Math.Clamp(backup.ActiveProfileIndex, 0, _profiles.Count - 1);
                ProfileCombo.ItemsSource = null;
                ProfileCombo.ItemsSource = _profiles;
                ProfileCombo.SelectedIndex = _activeProfileIndex;
                _profilesReady = true;

                _appSettings.CloseToTray = backup.AppSettings?.CloseToTray ?? true;
                _appSettings.StartWithWindows = backup.AppSettings?.StartWithWindows ?? false;
                var oldPalmEdges = _appSettings.PalmEdgeRejectionEnabled;
                _appSettings.PalmEdgeRejectionEnabled = backup.AppSettings?.PalmEdgeRejectionEnabled ?? true;
                ChkPalmEdgeRejection.IsChecked = _appSettings.PalmEdgeRejectionEnabled;
                _appSettings.Theme = ThemeManager.Get(backup.AppSettings?.Theme).Id;
                ThemeManager.Apply(_appSettings.Theme, Resources);

                TrySaveProfiles();
                TrySaveAppSettings();
                UpdateStartupRegistration(_appSettings.StartWithWindows);

                var active = ActiveProfile;
                if (active != null)
                {
                    LoadConfigIntoSliders(active.Palm);
                    LoadPointerConfigIntoControls(active.Pointer);
                    LoadScrollConfigIntoControls(active.Scroll);
                    DrawPreview();
                }

                if (oldPalmEdges != _appSettings.PalmEdgeRejectionEnabled)
                    ApplyPalmEdgeToggle(_appSettings.PalmEdgeRejectionEnabled);

                RefreshTrayMenu();
                SetBottomStatus($"Backup restored: {dlg.FileName}. Click “Save” to apply the profile to the driver.");
                MessageBox.Show(this, "Backup restored successfully.\n\nProfiles and application settings have been loaded.", "Backup", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Failed to restore backup.\n\n{ex.Message}", "Backup", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private static PointerConfig NormalizePointerConfig(PointerConfig cfg)
        {
            var defaults = PointerConfig.Default;

            if (cfg.StructVersion == 0)
                return defaults;

            // Older files used smaller PointerConfig layouts. Fields added
            // after the serialized version must be filled from current defaults
            // because the missing tail arrives as zero.
            if (cfg.StructVersion < PointerConfig.CurrentVersion)
            {
                if (cfg.StructVersion < 4)
                {
                    cfg.ForceTouchEnabled = defaults.ForceTouchEnabled;
                    cfg.RequirePressureToActivate = defaults.RequirePressureToActivate;
                }

                if (cfg.StructVersion < 5)
                    cfg.SmallContactRejectionEnabled = defaults.SmallContactRejectionEnabled;

                if (cfg.StructVersion < 6)
                    cfg.SmallContactRejectionStrict = defaults.SmallContactRejectionStrict;

                if (cfg.StructVersion < 7)
                    cfg.RequirePressureContinuously = defaults.RequirePressureContinuously;

                if (cfg.StructVersion < 8)
                {
                    cfg.ForceTouchEmulationEnabled = defaults.ForceTouchEmulationEnabled;
                    cfg.ForceTouchEmulationAction = defaults.ForceTouchEmulationAction;
                    cfg.ForceTouchEmulationHoldMs = defaults.ForceTouchEmulationHoldMs;
                }

                if (cfg.StructVersion < 9)
                {
                    cfg.ForceTapDragLockoutDistance = defaults.ForceTapDragLockoutDistance;
                    cfg.ForceTouchEmulationDragLockoutDistance = defaults.ForceTouchEmulationDragLockoutDistance;
                }
            }

            return cfg.Clamped();
        }

        private static GuiProfile CloneProfile(GuiProfile source)
        {
            return new GuiProfile
            {
                Name = source.Name,
                Palm = source.Palm,
                Pointer = source.Pointer,
                Scroll = source.Scroll
            };
        }

        private void UpdateStartupRegistration(bool enabled)
        {
            const string runKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
            const string valueName = "WellspringPTP";
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(runKeyPath, writable: true)
                    ?? Registry.CurrentUser.CreateSubKey(runKeyPath, writable: true);
                if (key == null) return;

                if (enabled)
                {
                    var exe = Environment.ProcessPath;
                    // "-tray" tells App.OnStartup (see App.xaml.cs) to keep
                    // MainWindow parked in the tray on launch instead of
                    // showing it - without this, every Windows sign-in would
                    // pop the full config window on screen even though the
                    // user only asked for the app to be *running* at
                    // startup, not front-and-center.
                    if (!string.IsNullOrWhiteSpace(exe))
                        key.SetValue(valueName, $"\"{exe}\" -tray");
                }
                else
                {
                    key.DeleteValue(valueName, throwOnMissingValue: false);
                }
            }
            catch
            {
                SetBottomStatus("Failed to update Windows startup.");
            }
        }

        // System.Drawing.Font does NOT understand WPF-style comma-separated
        // fallback lists ("Segoe UI Variable Text, Segoe UI") - GDI+ treats
        // the whole string as a single family name. When that exact combined
        // name isn't a registered family (always, since no such family
        // exists) it silently substitutes a generic default font with
        // different metrics, which is what made the tray menu's text and
        // row spacing look off. Resolve to a real installed family instead,
        // preferring the Windows 11 variable font but always falling back to
        // plain "Segoe UI", which ships on every supported Windows version.
        private static System.Drawing.Font ResolveTrayFont(float size, System.Drawing.FontStyle style = System.Drawing.FontStyle.Regular)
        {
            const string preferred = "Segoe UI Variable Text";
            const string fallback = "Segoe UI";

            bool preferredInstalled = System.Drawing.FontFamily.Families
                .Any(f => string.Equals(f.Name, preferred, StringComparison.OrdinalIgnoreCase));

            return new System.Drawing.Font(preferredInstalled ? preferred : fallback, size, style);
        }

        // Single cached instance of the 9pt tray font, reused across every
        // menu rebuild instead of allocating (and leaking) a new native GDI
        // Font handle each time. Disposed alongside the tray icon on exit.
        private System.Drawing.Font TrayFont => _trayFont ??= ResolveTrayFont(9F);

        private Forms.ContextMenuStrip BuildTrayMenu()
        {
            var menu = new RoundedContextMenuStrip
            {
                ShowImageMargin = false,
                ShowCheckMargin = true,
                AutoClose = true,
                Padding = new System.Windows.Forms.Padding(8, 8, 8, 8),
                Font = TrayFont,
                BackColor = System.Drawing.Color.FromArgb(250, 251, 253),
                ForeColor = System.Drawing.Color.FromArgb(30, 34, 40),
                Renderer = new ModernTrayRenderer(ThemeManager.CurrentThemeId),
                DropShadowEnabled = true,
                ShowItemToolTips = true
            };
            _trayRendererThemeId = ThemeManager.CurrentThemeId;

            RefreshTrayMenu(menu);
            return menu;
        }

        private void RefreshTrayMenu()
        {
            if (_trayIcon.ContextMenuStrip != null)
                RefreshTrayMenu(_trayIcon.ContextMenuStrip);
        }

        private void RefreshTrayMenu(Forms.ContextMenuStrip menu)
        {
            menu.SuspendLayout();
            try
            {
                var trayColors = ThemeManager.TrayColors(ThemeManager.CurrentThemeId);
                menu.BackColor = trayColors.Back;
                menu.ForeColor = trayColors.Fore;

                // Only rebuild the renderer when the theme actually changed
                // (it bakes ThemeManager.TrayColors into an immutable Colors
                // table at construction time). Recreating it on every open
                // was pure churn - the old one was still managed-only, but
                // there is no reason to allocate and discard it dozens of
                // times a session.
                if (_trayRendererThemeId != ThemeManager.CurrentThemeId)
                {
                    menu.Renderer = new ModernTrayRenderer(ThemeManager.CurrentThemeId);
                    _trayRendererThemeId = ThemeManager.CurrentThemeId;
                }
                menu.Font = TrayFont;

                // Dispose the outgoing items instead of just Items.Clear()-ing
                // them. Clear() only detaches items from the collection; it
                // does not call Dispose(). "Profile" and "Force Tap action"
                // are RoundedMenuItems whose flyout is created lazily
                // (CreateDefaultDropDown) the first time the user hovers
                // them, which means it owns a real native popup window
                // handle once opened. Without disposing the old top-level
                // item, that flyout's HWND was never destroyed - it just
                // stayed alive, unreachable, for as long as the process ran.
                // ToolStripDropDownItem.Dispose() cascades into any created
                // DropDown, so this releases it. Same reasoning applies to
                // any menu Font we assigned directly on an item.
                var outgoingItems = new Forms.ToolStripItem[menu.Items.Count];
                menu.Items.CopyTo(outgoingItems, 0);
                menu.Items.Clear();
                foreach (var outgoing in outgoingItems)
                    outgoing.Dispose();

                var profilesItem = new RoundedMenuItem("Profile") { ToolTipText = "Select active profile", Padding = new System.Windows.Forms.Padding(10, 7, 10, 7), Font = TrayFont };
                for (int i = 0; i < _profiles.Count; i++)
                {
                    int index = i;
                    var item = new Forms.ToolStripMenuItem(_profiles[i].Name)
                    {
                        Checked = i == _activeProfileIndex,
                        CheckOnClick = false,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7)
                    };
                    item.Click += (_, _) => Dispatcher.BeginInvoke(new Action(() => ActivateProfileFromTray(index)));
                    profilesItem.DropDownItems.Add(item);
                }
                if (_profiles.Count == 0)
                    profilesItem.DropDownItems.Add(new Forms.ToolStripMenuItem("No profiles") { Enabled = false, Padding = new System.Windows.Forms.Padding(10, 7, 10, 7) });

                var palmEdges = new Forms.ToolStripMenuItem("Palm rejection at edges")
                {
                    Checked = _appSettings.PalmEdgeRejectionEnabled,
                    Padding = new System.Windows.Forms.Padding(10, 7, 10, 7)
                };
                palmEdges.Click += (_, _) => Dispatcher.BeginInvoke(new Action(TogglePalmEdgesFromTray));

                var pointerCfg = ReadPointerConfigFromControls();
                bool forceTouchOn = pointerCfg.ForceTouchEnabled != 0;
                bool smallContactRejectionOn = pointerCfg.SmallContactRejectionEnabled != 0;

                menu.Items.Add(profilesItem);
                menu.Items.Add(new Forms.ToolStripSeparator());
                menu.Items.Add(palmEdges);

                // On non-Force-Touch trackpads expose the small-contact filter in
                // the tray exactly like the Force Touch toggle. The driver ignores
                // this setting on Force Touch-capable hardware.
                if (!_forceTouchSupported)
                {
                    var smallReject = new Forms.ToolStripMenuItem("Small-contact rejection")
                    {
                        Checked = smallContactRejectionOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7),
                        ToolTipText = "Reject tiny contacts on trackpads without Force Touch until M:80/60 is reached."
                    };
                    smallReject.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ToggleSmallContactRejectionFromTray));
                    menu.Items.Add(smallReject);

                    if (smallContactRejectionOn)
                    {
                        var requireM = new Forms.ToolStripMenuItem("Require M:50/30 continuously")
                        {
                            Checked = pointerCfg.SmallContactRejectionStrict != 0,
                            Padding = new System.Windows.Forms.Padding(28, 7, 10, 7),
                            ToolTipText = "Ignore a non-Force-Touch contact on every frame unless Major is at least 50 and Minor is at least 30."
                        };
                        requireM.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ToggleSmallContactStrictFromTray));
                        menu.Items.Add(requireM);
                    }

                    // Force Touch emulation: same idea as the hardware Force Touch
                    // block below, but for trackpads with no pressure sensor - hold
                    // a Hard Tap for the configured duration instead of a pressure
                    // threshold. Mutually exclusive with the hardware block since
                    // both are gated on _forceTouchSupported / !_forceTouchSupported.
                    bool forceTouchEmulationOn = pointerCfg.ForceTouchEmulationEnabled != 0;

                    var forceTouchEmulation = new Forms.ToolStripMenuItem("Force Touch emulation")
                    {
                        Checked = forceTouchEmulationOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7),
                        ToolTipText = "On trackpads with no pressure sensor, hold a Hard Tap for the configured duration to trigger a Force Touch action."
                    };
                    forceTouchEmulation.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ToggleForceTouchEmulationFromTray));

                    var emulationActionMenu = new RoundedMenuItem("Force Touch emulation action")
                    {
                        Enabled = forceTouchEmulationOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7),
                        ToolTipText = "Choose what a held Hard Tap sends: context menu, middle click, or double click."
                    };
                    AddTrayEmulationActionItem(emulationActionMenu, "Context menu", PointerConfig.ActionContextMenu, pointerCfg.ForceTouchEmulationAction);
                    AddTrayEmulationActionItem(emulationActionMenu, "Middle mouse button", PointerConfig.ActionMiddleClick, pointerCfg.ForceTouchEmulationAction);
                    AddTrayEmulationActionItem(emulationActionMenu, "Double click", PointerConfig.ActionDoubleClick, pointerCfg.ForceTouchEmulationAction);

                    menu.Items.Add(forceTouchEmulation);
                    menu.Items.Add(emulationActionMenu);
                }

                // Force Touch controls now live directly in the main menu instead of
                // being buried inside a nested "Force Touch" submenu - but only on
                // trackpads that actually have Force Touch hardware. On older/non-
                // Force-Touch models these items are omitted.
                if (_forceTouchSupported)
                {
                    var forceTouch = new Forms.ToolStripMenuItem("Force Touch")
                    {
                        Checked = forceTouchOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7),
                        ToolTipText = "Enable Force Touch click arbitration on trackpads that support it."
                    };
                    forceTouch.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ToggleForceTouchFromTray));

                    var requirePressure = new Forms.ToolStripMenuItem("Require pressure to activate contact")
                    {
                        Checked = pointerCfg.RequirePressureToActivate != 0,
                        Enabled = forceTouchOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7)
                    };
                    requirePressure.Click += (_, _) => Dispatcher.BeginInvoke(new Action(TogglePressureGateFromTray));

                    // Dedicated item that expands into the list of Force Tap button types.
                    var actionMenu = new RoundedMenuItem("Force Tap action")
                    {
                        Enabled = forceTouchOn,
                        Padding = new System.Windows.Forms.Padding(10, 7, 10, 7),
                        ToolTipText = "Choose what a hard press sends: context menu, middle click, or double click."
                    };
                    AddTrayActionItem(actionMenu, "Context menu", PointerConfig.ActionContextMenu, pointerCfg.ForceTapAction);
                    AddTrayActionItem(actionMenu, "Middle mouse button", PointerConfig.ActionMiddleClick, pointerCfg.ForceTapAction);
                    AddTrayActionItem(actionMenu, "Double click", PointerConfig.ActionDoubleClick, pointerCfg.ForceTapAction);

                    menu.Items.Add(forceTouch);
                    menu.Items.Add(requirePressure);

                    if (forceTouchOn && pointerCfg.RequirePressureToActivate != 0)
                    {
                        var requirePressureContinuous = new Forms.ToolStripMenuItem("Require pressure continuously")
                        {
                            Checked = pointerCfg.RequirePressureContinuously != 0,
                            Padding = new System.Windows.Forms.Padding(28, 7, 10, 7),
                            ToolTipText = "Ignore a Force-Touch contact on any frame where pressure is zero."
                        };
                        requirePressureContinuous.Click += (_, _) => Dispatcher.BeginInvoke(new Action(TogglePressureContinuousFromTray));
                        menu.Items.Add(requirePressureContinuous);
                    }

                    menu.Items.Add(actionMenu);
                }

                var open = new Forms.ToolStripMenuItem("Open application") { Padding = new System.Windows.Forms.Padding(10, 7, 10, 7) };
                open.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ShowFromTray));

                var settings = new Forms.ToolStripMenuItem("Settings") { Padding = new System.Windows.Forms.Padding(10, 7, 10, 7) };
                settings.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ShowAppSettingsDialog));

                var exit = new Forms.ToolStripMenuItem("Exit") { Padding = new System.Windows.Forms.Padding(10, 7, 10, 7) };
                exit.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ExitApplication));

                menu.Items.Add(new Forms.ToolStripSeparator());
                menu.Items.Add(open);
                menu.Items.Add(settings);
                menu.Items.Add(new Forms.ToolStripSeparator());
                menu.Items.Add(exit);
            }
            finally
            {
                menu.ResumeLayout();
            }
        }

        private void AddTrayActionItem(Forms.ToolStripMenuItem parent, string text, uint action, uint current)
        {
            var item = new Forms.ToolStripMenuItem(text)
            {
                Checked = current == action,
                Padding = new System.Windows.Forms.Padding(10, 7, 10, 7)
            };
            item.Click += (_, _) => Dispatcher.BeginInvoke(new Action(() => SetForceTapActionFromTray(action)));
            parent.DropDownItems.Add(item);
        }

        private void AddTrayEmulationActionItem(Forms.ToolStripMenuItem parent, string text, uint action, uint current)
        {
            var item = new Forms.ToolStripMenuItem(text)
            {
                Checked = current == action,
                Padding = new System.Windows.Forms.Padding(10, 7, 10, 7)
            };
            item.Click += (_, _) => Dispatcher.BeginInvoke(new Action(() => SetForceTouchEmulationActionFromTray(action)));
            parent.DropDownItems.Add(item);
        }

        private void ToggleForceTouchFromTray()
        {
            ChkForceTouchEnabled.IsChecked = ChkForceTouchEnabled.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void TogglePressureGateFromTray()
        {
            ChkRequirePressure.IsChecked = ChkRequirePressure.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void TogglePressureContinuousFromTray()
        {
            ChkRequirePressureContinuously.IsChecked = ChkRequirePressureContinuously.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void ToggleSmallContactRejectionFromTray()
        {
            ChkSmallContactRejection.IsChecked = ChkSmallContactRejection.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void ToggleSmallContactStrictFromTray()
        {
            ChkSmallContactRejectionStrict.IsChecked = ChkSmallContactRejectionStrict.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void ToggleForceTouchEmulationFromTray()
        {
            ChkForceTouchEmulationEnabled.IsChecked = ChkForceTouchEmulationEnabled.IsChecked != true;
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private bool TrySaveProfiles()
        {
            try
            {
                ProfileStore.Save(_profiles);
                return true;
            }
            catch (Exception ex)
            {
                SetBottomStatus($"Could not save profiles: {ex.Message}");
                return false;
            }
        }

        private bool TrySaveAppSettings()
        {
            try
            {
                AppSettingsStore.Save(_appSettings);
                return true;
            }
            catch (Exception ex)
            {
                SetBottomStatus($"Could not save application settings: {ex.Message}");
                return false;
            }
        }

        private void SetForceTapActionFromTray(uint action)
        {
            _suppressPointerEvents = true;
            try
            {
                RbActionContextMenu.IsChecked = action == PointerConfig.ActionContextMenu;
                RbActionMiddleClick.IsChecked = action == PointerConfig.ActionMiddleClick;
                RbActionDoubleClick.IsChecked = action == PointerConfig.ActionDoubleClick;
            }
            finally
            {
                _suppressPointerEvents = false;
            }
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void SetForceTouchEmulationActionFromTray(uint action)
        {
            _suppressPointerEvents = true;
            try
            {
                RbEmulationActionContextMenu.IsChecked = action == PointerConfig.ActionContextMenu;
                RbEmulationActionMiddleClick.IsChecked = action == PointerConfig.ActionMiddleClick;
                RbEmulationActionDoubleClick.IsChecked = action == PointerConfig.ActionDoubleClick;
            }
            finally
            {
                _suppressPointerEvents = false;
            }
            CommitCurrentConfigToProfile();
            RefreshTrayMenu();
        }

        private void ActivateProfileFromTray(int index)
        {
            if (index < 0 || index >= _profiles.Count) return;
            ProfileCombo.SelectedIndex = index;
            CommitCurrentConfigToProfile();
            ShowFromTray();
        }

        private void TogglePalmEdgesFromTray()
        {
            _appSettings.PalmEdgeRejectionEnabled = !_appSettings.PalmEdgeRejectionEnabled;
            ChkPalmEdgeRejection.IsChecked = _appSettings.PalmEdgeRejectionEnabled;
            TrySaveAppSettings();
            ApplyPalmEdgeToggle(_appSettings.PalmEdgeRejectionEnabled);
            RefreshTrayMenu();
            if (!IsVisible)
                TrayMemoryTrimmer.TrimAfterHide();
        }

        private void PalmEdgeRejection_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady)
                return;

            bool enabled = ChkPalmEdgeRejection.IsChecked == true;
            _appSettings.PalmEdgeRejectionEnabled = enabled;
            TrySaveAppSettings();
            ApplyPalmEdgeToggle(enabled);
            RefreshTrayMenu();
        }

        private void ApplyPalmEdgeToggle(bool enabled)
        {
            var current = ReadConfigFromSliders();
            if (enabled)
            {
                var active = ActiveProfile;
                if (active != null)
                {
                    current.EdgePermilleTop = active.Palm.EdgePermilleTop;
                    current.EdgePermilleLeft = active.Palm.EdgePermilleLeft;
                    current.EdgePermilleRight = active.Palm.EdgePermilleRight;
                    current.EdgePermilleBottom = active.Palm.EdgePermilleBottom;
                    LoadConfigIntoSliders(current);
                }
            }
            else
            {
                current.EdgePermilleTop = 0;
                current.EdgePermilleLeft = 0;
                current.EdgePermilleRight = 0;
                current.EdgePermilleBottom = 0;
                LoadConfigIntoSliders(current);
            }

            if (_device.IsConnected)
            {
                _device.TrySetPalmConfig(current, out var applied);
                LoadConfigIntoSliders(applied);
            }
            DrawPreview();
        }

        private void ShowFromTray()
        {
            Show();
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            Activate();
            Topmost = true;
            Topmost = false;
            Focus();

            if (_liveWasEnabledBeforeHide)
                ResumeLiveAfterShow();
        }

        // Entry point for App's named-pipe activation server: a second
        // `.exe` launch attempt was detected and handed off here instead of
        // starting its own process. Same effect as double-clicking the tray
        // icon.
        public void ActivateFromExternalLaunch() => ShowFromTray();

        private void ExitApplication()
        {
            _allowWindowClose = true;
            Close();
        }

        private void MainWindow_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
        {
            if (!_allowWindowClose && _appSettings.CloseToTray)
            {
                e.Cancel = true;
                Hide();

                // Live overlay has nothing to render while hidden - the
                // 30 Hz DeviceIoControl poll task and the ~24 FPS
                // DispatcherTimer were previously left running against a
                // hidden window, which is why the tray-parked process used
                // the same CPU/memory footprint as the open one. Pause both
                // here; SetLiveEnabled(false) also tells the driver side to
                // stop assembling live frames for us. ChkLive.IsChecked and
                // _liveEnabled are deliberately left alone so the checkbox
                // still reflects the user's actual choice on next Show().
                if (_liveEnabled)
                {
                    _liveWasEnabledBeforeHide = true;
                    StopLivePolling();
                    _liveRenderTimer.Stop();
                    _device.SetLiveEnabled(false);
                }

                // Stopping the live poll/render loop above only stops the
                // process from doing more work while parked in tray - it
                // does not give back memory the process already touched
                // (window chrome, live overlay, config/profile data built up
                // while the window was open). Ask the OS to page that back
                // out now so "Memory" in Task Manager for the tray-parked
                // process reads close to the CLR/WPF baseline instead of
                // whatever peak the visible window reached. See
                // TrayMemoryTrimmer for why this is safe (fully reversible,
                // no behavior change - just what's resident vs paged out).
                TrayMemoryTrimmer.TrimAfterHide();

                SetBottomStatus("Wellspring Control Center is running in the system tray.");
                return;
            }

            StopLivePolling();
            _liveRenderTimer.Stop();
            _device.Disconnect();
            _trayIcon.Visible = false;
            // Disposing the NotifyIcon's ContextMenuStrip cascades into every
            // item currently in it (and their lazily-created flyout popups),
            // releasing those native handles. The cached tray Font is ours
            // to release too - it isn't owned by any single item, so nothing
            // else will dispose it.
            _trayIcon.ContextMenuStrip?.Dispose();
            _trayIcon.Dispose();
            _trayFont?.Dispose();
        }

        // ---------------------------------------------------------------
        // Profiles / Pro mode
        // ---------------------------------------------------------------

        private void InitializeProfiles()
        {
            _profiles.Clear();
            _profiles.AddRange(ProfileStore.Load());

            _profilesReady = false;
            ProfileCombo.ItemsSource = null;
            ProfileCombo.ItemsSource = _profiles;
            if (_profiles.Count > 0)
            {
                _activeProfileIndex = 0;
                ProfileCombo.SelectedIndex = 0;
            }
            _profilesReady = true;
        }

        private GuiProfile? ActiveProfile =>
            _activeProfileIndex >= 0 && _activeProfileIndex < _profiles.Count
                ? _profiles[_activeProfileIndex]
                : null;

        private void ProfileCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (!_profilesReady || ProfileCombo.SelectedIndex < 0 || ProfileCombo.SelectedIndex >= _profiles.Count)
                return;

            _activeProfileIndex = ProfileCombo.SelectedIndex;
            var profile = _profiles[_activeProfileIndex];
            LoadConfigIntoSliders(profile.Palm.Clamped());
            LoadPointerConfigIntoControls(profile.Pointer.Clamped());
            LoadScrollConfigIntoControls(profile.Scroll.StructVersion == 0 ? ScrollConfig.Default : profile.Scroll.Clamped());
            DrawPreview();
            SetBottomStatus($"Loaded profile “{profile.Name}”. Click “Save” to apply it to the driver.");
        }

        private void NewProfile_Click(object sender, RoutedEventArgs e)
        {
            string name = GetNextProfileName();
            var profile = new GuiProfile
            {
                Name = name,
                Palm = ReadConfigFromSliders(),
                Pointer = ReadPointerConfigFromControls(),
                Scroll = ReadScrollConfigFromControls(),
            };

            _profilesReady = false;
            _profiles.Add(profile);
            ProfileCombo.ItemsSource = null;
            ProfileCombo.ItemsSource = _profiles;
            ProfileCombo.SelectedIndex = _profiles.Count - 1;
            _activeProfileIndex = _profiles.Count - 1;
            _profilesReady = true;

            TrySaveProfiles();
            SetBottomStatus($"Created “{name}”. Click “Save” to save it as the current profile and apply it to the driver.");
        }

        private void RenameProfile_Click(object sender, RoutedEventArgs e)
        {
            var profile = ActiveProfile;
            if (profile == null)
                return;

            string? name = PromptText("Rename profile", "Profile name:", profile.Name);
            if (string.IsNullOrWhiteSpace(name))
                return;

            name = name.Trim();
            if (_profiles.Any(p => !ReferenceEquals(p, profile) && string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)))
            {
                MessageBox.Show(this, "A profile with this name already exists.", "Profiles", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            profile.Name = name;
            ProfileCombo.Items.Refresh();
            TrySaveProfiles();
            SetBottomStatus($"Profile renamed to “{name}”.");
        }

        private void DeleteProfile_Click(object sender, RoutedEventArgs e)
        {
            if (_profiles.Count <= 1)
            {
                MessageBox.Show(this, "At least one profile must remain.", "Profiles", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var profile = ActiveProfile;
            if (profile == null)
                return;

            var result = MessageBox.Show(
                this,
                $"Delete profile “{profile.Name}”?",
                "Delete profile",
                MessageBoxButton.YesNo,
                MessageBoxImage.Question);
            if (result != MessageBoxResult.Yes)
                return;

            int nextIndex = Math.Min(_activeProfileIndex, _profiles.Count - 2);
            _profilesReady = false;
            _profiles.RemoveAt(_activeProfileIndex);
            ProfileCombo.ItemsSource = null;
            ProfileCombo.ItemsSource = _profiles;
            ProfileCombo.SelectedIndex = nextIndex;
            _activeProfileIndex = nextIndex;
            _profilesReady = true;

            TrySaveProfiles();
            var current = ActiveProfile;
            if (current != null)
            {
                LoadConfigIntoSliders(current.Palm.Clamped());
                LoadPointerConfigIntoControls(current.Pointer.Clamped());
                LoadScrollConfigIntoControls(current.Scroll.StructVersion == 0 ? ScrollConfig.Default : current.Scroll.Clamped());
                DrawPreview();
            }
            SetBottomStatus("Profile deleted.");
        }

        private string GetNextProfileName()
        {
            int i = 1;
            while (_profiles.Any(p => string.Equals(p.Name, $"Profile {i}", StringComparison.OrdinalIgnoreCase)))
                i++;
            return $"Profile {i}";
        }

        private static string? PromptText(string title, string caption, string initialValue)
        {
            var dialog = new Window
            {
                Title = title,
                Width = 380,
                Height = 165,
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize,
                WindowStyle = WindowStyle.ToolWindow,
                ShowInTaskbar = false,
            };

            var root = new Grid { Margin = new Thickness(18) };
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            root.Children.Add(new TextBlock { Text = caption, Margin = new Thickness(0, 0, 0, 8), FontWeight = FontWeights.SemiBold });
            var text = new TextBox { Text = initialValue, Height = 32 };
            Grid.SetRow(text, 1);
            root.Children.Add(text);

            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = System.Windows.HorizontalAlignment.Right, Margin = new Thickness(0, 14, 0, 0) };
            var cancel = new Button { Content = "Cancel", Padding = new Thickness(14, 7, 14, 7), Margin = new Thickness(0, 0, 8, 0), IsCancel = true };
            var ok = new Button { Content = "OK", Padding = new Thickness(18, 7, 18, 7), IsDefault = true };
            ok.Click += (_, _) => { dialog.DialogResult = true; dialog.Close(); };
            buttons.Children.Add(cancel);
            buttons.Children.Add(ok);
            Grid.SetRow(buttons, 3);
            root.Children.Add(buttons);

            dialog.Content = root;
            dialog.Owner = Application.Current?.MainWindow;
            text.SelectAll();
            text.Focus();

            return dialog.ShowDialog() == true ? text.Text : null;
        }

        // ---------------------------------------------------------------
        // Force Touch availability
        // ---------------------------------------------------------------

        // Whether the connected trackpad supports Force Touch, per the last
        // Reconnect()/GetDeviceModelDisplay() result. Defaults to true
        // (controls shown) so the Force Touch group and tray menu items
        // aren't hidden before the first successful device query - only a
        // confirmed "false" from the driver/SMBIOS table hides them.
        private bool _forceTouchSupported = true;

        private void UpdateForceTouchAvailability(bool? supportsForceTouch)
        {
            _forceTouchSupported = supportsForceTouch != false;

            if (ForceTouchGroup != null)
                ForceTouchGroup.Visibility = _forceTouchSupported ? Visibility.Visible : Visibility.Collapsed;
            if (ForceTouchSmallContactRejectionGroup != null)
                ForceTouchSmallContactRejectionGroup.Visibility = _forceTouchSupported ? Visibility.Visible : Visibility.Collapsed;
            if (ForceTouchEmulationGroup != null)
                ForceTouchEmulationGroup.Visibility = _forceTouchSupported ? Visibility.Collapsed : Visibility.Visible;
            if (SmallContactRejectionGroup != null)
                SmallContactRejectionGroup.Visibility = _forceTouchSupported ? Visibility.Collapsed : Visibility.Visible;
            if (ChkSmallContactRejectionStrict != null)
                ChkSmallContactRejectionStrict.Visibility =
                    (!_forceTouchSupported && ChkSmallContactRejection?.IsChecked == true)
                        ? Visibility.Visible
                        : Visibility.Collapsed;

            // The tray context menu is rebuilt on demand from RefreshTrayMenu,
            // but if it's already open/cached it should reflect the change
            // immediately rather than waiting for the next open.
            RefreshTrayMenu();
        }

        private void ProMode_Changed(object sender, RoutedEventArgs e)
        {
            UpdateProModeVisibility();
        }

        private void UpdateProModeVisibility()
        {
            if (ScrollAdvancedPanel == null || PointerAdvancedPanel == null || ChkProMode == null)
                return;

            var visibility = ChkProMode.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
            ScrollAdvancedPanel.Visibility = visibility;
            PointerAdvancedPanel.Visibility = visibility;
            if (ChkDebugMode != null)
                ChkDebugMode.Visibility = visibility;
        }

        // Guards ChkDebugMode.Checked/Unchecked while we're populating the
        // checkbox from a driver read (Reconnect) rather than from the user
        // clicking it - same pattern as _suppressEvents/_suppressPointerEvents
        // for the Palm/Pointer sliders.
        private bool _suppressDebugModeEvent;

        private void DebugMode_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressDebugModeEvent)
                return;

            bool enabled = ChkDebugMode?.IsChecked == true;
            if (!_device.SetDebugMode(enabled))
            {
                // Revert the checkbox rather than show a state the driver
                // never actually applied (e.g. device unplugged mid-click).
                _suppressDebugModeEvent = true;
                try
                {
                    if (ChkDebugMode != null)
                        ChkDebugMode.IsChecked = !enabled;
                }
                finally
                {
                    _suppressDebugModeEvent = false;
                }
            }
        }

        private void LoadDebugModeIntoControl(bool enabled)
        {
            if (ChkDebugMode == null)
                return;

            _suppressDebugModeEvent = true;
            try
            {
                ChkDebugMode.IsChecked = enabled;
            }
            finally
            {
                _suppressDebugModeEvent = false;
            }
        }

        // ---------------------------------------------------------------
        // Connection handling
        // ---------------------------------------------------------------

        private void Reconnect_Click(object sender, RoutedEventArgs e) => Reconnect();

        private void SaveErrors_Click(object sender, RoutedEventArgs e)
        {
            if (_diagnosticLog.Count == 0 && _liveCornerSamples == 0)
            {
                MessageBox.Show(
                    "No diagnostic or live calibration data is available yet.",
                    "No data", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var dialog = new SaveFileDialog
            {
                Title = "Save diagnostic log",
                Filter = "Text files (*.txt)|*.txt|All files (*.*)|*.*",
                FileName = $"AmtPtpConfigGui_errors_{DateTime.Now:yyyyMMdd_HHmmss}.txt"
            };

            if (dialog.ShowDialog(this) != true)
                return;

            try
            {
                var lines = new List<string>(_diagnosticLog);

                lines.Add("");
                lines.Add("===== Live Touch Calibration / Corner Extrema =====");
                lines.Add($"Timestamp: {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
                lines.Add($"Live samples total: {_liveCornerSamples}");
                lines.Add("");
                lines.Add($"TOP-LEFT     {_topLeft.ToText()}");
                lines.Add($"TOP-RIGHT    {_topRight.ToText()}");
                lines.Add($"BOTTOM-LEFT  {_bottomLeft.ToText()}");
                lines.Add($"BOTTOM-RIGHT {_bottomRight.ToText()}");

                File.WriteAllLines(dialog.FileName, lines);
                SetBottomStatus($"Diagnostic log saved: {dialog.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Failed to save file:\n{ex.Message}",
                    "Save error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void Reconnect()
        {
            // Invalidate the current polling session before replacing the
            // device handle. This prevents a stale worker from touching the
            // newly connected handle after reconnect.
            StopLivePolling();

            if (_liveEnabled)
            {
                _device.SetLiveEnabled(false);
                _liveEnabled = false;
                _liveRenderTimer.Stop();
                if (ChkLive != null)
                    ChkLive.IsChecked = false;
                HideAllLiveOverlayElements();
            }

            bool connected = _device.TryConnect();

            // Opening the Wellspring control-device only proves that the
            // control device exists. It does not prove that an active
            // Wellspring HID target is currently attached to it. This can
            // happen when the Apple/default HID driver is servicing the
            // touchpad instead of Wellspring. Require authoritative device
            // info from the driver before showing Connected or a model.
            if (connected)
            {
                if (!_device.TryGetDeviceInfo(out var activeInfo) ||
                    activeInfo.StructVersion != 1 ||
                    activeInfo.ProductId == 0)
                {
                    _device.Disconnect();
                    connected = false;
                }
            }

            if (connected)
            {
                BtnReconnect.Visibility = Visibility.Collapsed;
                StatusDot.Fill = ConnectedBrush;
                StatusText.Text = "Connected";
                DeviceModelText.Text = _device.GetDeviceModelDisplay(out var supportsForceTouch);
                UpdateForceTouchAvailability(supportsForceTouch);

                if (_device.TryGetPalmConfig(out var cfg))
                {
                    LoadConfigIntoSliders(cfg);
                }
                else
                {
                    LoadConfigIntoSliders(PalmConfig.Default);
                    SetBottomStatus("Device found, but its configuration could not be read — showing defaults.");
                }

                if (_device.TryGetPointerConfig(out var pointerCfg))
                    LoadPointerConfigIntoControls(pointerCfg);
                else
                    LoadPointerConfigIntoControls(PointerConfig.Default);

                if (_device.TryGetScrollConfig(out var scrollCfg))
                    LoadScrollConfigIntoControls(scrollCfg);
                else
                    LoadScrollConfigIntoControls(ScrollConfig.Default);

                if (_device.TryGetPadGeometry(out var geo))
                {
                    _geometry = geo;
                }
                else
                {
                    _geometry = PadGeometry.Fallback;
                }

                // Best-effort: an unreadable DebugMode just leaves the
                // checkbox at its previous/default (off) state - same
                // "never fatal to the rest of Reconnect" treatment as the
                // geometry/config reads above.
                if (_device.TryGetDebugMode(out var debugEnabled))
                    LoadDebugModeIntoControl(debugEnabled);
                else
                    LoadDebugModeIntoControl(false);
            }
            else
            {
                BtnReconnect.Visibility = Visibility.Visible;
                StatusDot.Fill = DisconnectedBrush;
                StatusText.Text = "Disconnected";
                DeviceModelText.Text = "No device detected";
                // No device to ask, so we don't actually know whether Force
                // Touch is supported - default to showing the controls
                // rather than hiding a feature the user might have.
                UpdateForceTouchAvailability(supportsForceTouch: null);
                LoadConfigIntoSliders(PalmConfig.Default);
                LoadPointerConfigIntoControls(PointerConfig.Default);
                LoadScrollConfigIntoControls(ScrollConfig.Default);
                _geometry = PadGeometry.Fallback;
                LoadDebugModeIntoControl(false);

                // Surface exactly which SetupAPI/CreateFile step failed and
                // why, right in the GUI - no debugger or Event Viewer needed.
                SetBottomStatus(string.IsNullOrEmpty(_device.LastErrorMessage)
                    ? ""
                    : $"Diagnostics: {_device.LastErrorMessage}");

                if (!string.IsNullOrEmpty(_device.LastErrorMessage))
                {
                    _diagnosticLog.Add($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {_device.LastErrorMessage}");
                }
            }


            DrawPreview();
        }

        private static ScrollViewer? FindParentScrollViewer(DependencyObject source)
        {
            DependencyObject current = source;
            while (current != null)
            {
                if (current is ScrollViewer sv)
                    return sv;
                current = VisualTreeHelper.GetParent(current);
            }
            return null;
        }

        private void Window_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
        {
            var viewer = FindParentScrollViewer(e.OriginalSource as DependencyObject ?? this);
            if (viewer == null || viewer.ScrollableHeight <= 0)
                return;

            double step = 28.0;
            double delta = e.Delta > 0 ? -step : step;
            viewer.ScrollToVerticalOffset(viewer.VerticalOffset + delta);
            e.Handled = true;
        }

        // ---------------------------------------------------------------
        // Live touch monitor
        // ---------------------------------------------------------------

        private void ResetCornerExtrema()
        {
            ResetCorner(_topLeft);
            ResetCorner(_topRight);
            ResetCorner(_bottomLeft);
            ResetCorner(_bottomRight);
            _liveCornerSamples = 0;
        }

        private static void ResetCorner(CornerExtrema c)
        {
            c.Samples = 0;
            c.MinRawX = short.MaxValue;
            c.MaxRawX = short.MinValue;
            c.MinRawY = short.MaxValue;
            c.MaxRawY = short.MinValue;
            c.MinNormX = ushort.MaxValue;
            c.MaxNormX = 0;
            c.MinNormY = ushort.MaxValue;
            c.MaxNormY = 0;
        }

        private static CornerExtrema GetCorner(
            CornerExtrema tl,
            CornerExtrema tr,
            CornerExtrema bl,
            CornerExtrema br,
            ushort normX,
            ushort normY,
            double xRange,
            double yRange)
        {
            double px = xRange > 0 ? normX / xRange : 0.0;
            double py = yRange > 0 ? normY / yRange : 0.0;

            // Divide the pad into four quadrants. Each sample belongs to exactly
            // one corner; this is intentionally simple for calibration passes.
            if (py < 0.5)
                return px < 0.5 ? tl : tr;
            else
                return px < 0.5 ? bl : br;
        }

        private void AccumulateCornerExtrema(LiveFrame frame)
        {
            if (frame.Contacts == null || frame.ContactCount == 0)
                return;

            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;

            if (xRange <= 0 || yRange <= 0)
                return;

            for (int i = 0; i < frame.ContactCount && i < frame.Contacts.Length; i++)
            {
                var c = frame.Contacts[i];
                var target = GetCorner(
                    _topLeft,
                    _topRight,
                    _bottomLeft,
                    _bottomRight,
                    c.X,
                    c.Y,
                    xRange,
                    yRange);

                target.Update(c.RawX, c.RawY, c.X, c.Y);
                _liveCornerSamples++;
            }
        }

        private void Live_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady)
                return;

            bool enabled = ChkLive.IsChecked == true;

            if (!_device.IsConnected)
            {
                _liveEnabled = false;
                if (BtnExportLog != null) BtnExportLog.Visibility = Visibility.Collapsed;
                _liveRenderTimer.Stop();
                LiveStatusText.Text = "Live: device not connected";
                if (LiveCoordPanel != null) LiveCoordPanel.Visibility = Visibility.Collapsed;
                if (LiveCornerText != null) LiveCornerText.Visibility = Visibility.Collapsed;
                SetLiveDetailsToggleVisible(false);
                SetLiveDot(active: false);
                HideAllLiveOverlayElements();
                return;
            }

            if (!_device.SetLiveEnabled(enabled))
            {
                _liveEnabled = false;
                if (BtnExportLog != null) BtnExportLog.Visibility = Visibility.Collapsed;
                _liveRenderTimer.Stop();
                if (ChkLive.IsChecked == true)
                    ChkLive.IsChecked = false;
                LiveStatusText.Text = "Live: error";
                if (LiveCoordPanel != null) LiveCoordPanel.Visibility = Visibility.Collapsed;
                if (LiveCornerText != null) LiveCornerText.Visibility = Visibility.Collapsed;
                LiveCoordText.Text = "Live: coordinates —";
                if (LiveCornerText != null) LiveCornerText.Text = "Corners: TL 0 | TR 0 | BL 0 | BR 0";
                SetLiveDetailsToggleVisible(false);
                SetLiveDot(active: null); // error - solid red, no pulse
                HideAllLiveOverlayElements();
                return;
            }

            _liveEnabled = enabled;
            _lastLiveSequence = 0;
            if (BtnExportLog != null) BtnExportLog.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;

            if (enabled)
            {
                ResetCornerExtrema();
                _hasLatestLiveFrame = false;
                StartLivePolling();
                _liveRenderTimer.Start();
                LiveStatusText.Text = "Live: waiting… | corners: collecting";
                // The contact-detail list always starts collapsed when Live
                // is (re)enabled; the toggle button next to "Export log
                // .txt" is what reveals it.
                _liveDetailsExpanded = false;
                if (LiveCoordPanel != null) LiveCoordPanel.Visibility = Visibility.Collapsed;
                if (LiveCornerText != null) LiveCornerText.Visibility = Visibility.Visible;
                SetLiveDetailsToggleVisible(true);
                SetLiveDot(active: true);
            }
            else
            {
                StopLivePolling();
                _liveRenderTimer.Stop();
                LiveStatusText.Text = "Live: disabled";
                if (LiveCoordPanel != null) LiveCoordPanel.Visibility = Visibility.Collapsed;
                if (LiveCornerText != null) LiveCornerText.Visibility = Visibility.Collapsed;
                LiveCoordText.Text = "Live: coordinates —";
                if (LiveCornerText != null) LiveCornerText.Text = "Corners: TL 0 | TR 0 | BL 0 | BR 0";
                SetLiveDetailsToggleVisible(false);
                SetLiveDot(active: false);
                HideAllLiveOverlayElements();
                DrawPreview();
            }
        }

        private void SetLiveDetailsToggleVisible(bool visible)
        {
            if (BtnToggleLiveDetails == null) return;
            BtnToggleLiveDetails.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (!visible) UpdateLiveDetailsToggleIcon();
        }

        private void UpdateLiveDetailsToggleIcon()
        {
            if (BtnToggleLiveDetailsIcon == null) return;
            // ▾ collapsed (click to expand) / ▴ expanded (click to collapse).
            BtnToggleLiveDetailsIcon.Text = _liveDetailsExpanded ? "\u25B4" : "\u25BE";
        }

        private void ToggleLiveDetails_Click(object sender, RoutedEventArgs e)
        {
            _liveDetailsExpanded = !_liveDetailsExpanded;
            if (LiveCoordPanel != null)
                LiveCoordPanel.Visibility = _liveDetailsExpanded ? Visibility.Visible : Visibility.Collapsed;
            UpdateLiveDetailsToggleIcon();
        }

        // Drives the small dot next to "Live: ..." in the header toolbar:
        // a soft pulsing green while frames are actively streaming, a
        // steady muted gray when idle, and a steady red (no pulse - a
        // pulse would suggest "working", which an error state isn't) if
        // enabling live mode failed.
        private void SetLiveDot(bool? active)
        {
            if (LiveDot == null) return;

            var pulse = (Storyboard)FindResource("LivePulseStoryboard");
            pulse.Stop(this);
            LiveDot.Opacity = 1.0;

            switch (active)
            {
                case true:
                    LiveDot.Fill = ConnectedBrush;
                    pulse.Begin(this, isControllable: true);
                    break;
                case false:
                    LiveDot.Fill = LiveDotIdleBrush;
                    break;
                case null:
                    LiveDot.Fill = DisconnectedBrush;
                    break;
            }
        }

        private void LiveRenderTimer_Tick(object? sender, EventArgs e)
        {
            if (!_liveEnabled || !_device.IsConnected)
                return;

            LiveFrame frame;
            lock (_liveFrameLock)
            {
                if (!_hasLatestLiveFrame)
                    return;

                frame = _latestLiveFrame;
            }

            if (frame.Sequence == 0 || frame.Sequence == _lastLiveSequence)
                return;

            _lastLiveSequence = frame.Sequence;
            AccumulateCornerExtrema(frame);
            DrawLiveOverlay(frame);

            int count = frame.ContactCount;

            // The live contact count is part of the same snapshot as the
            // overlay. It must therefore update for every new frame;
            // throttling it independently can leave "Live: 1/2/3 contacts"
            // visible after the preview has already cleared on lift.
            LiveStatusText.Text =
                $"Live: {count} contacts | seq {frame.Sequence}" +
                (frame.ButtonDown != 0 ? " | BUTTON" : "") +
                (frame.LargePalmBlanked != 0 ? " | PALM" : "");

            // Detailed text/corner telemetry is secondary and can remain
            // throttled to reduce string-building/layout work. Force an
            // immediate refresh on a zero-contact frame so lift is reflected
            // everywhere in the UI in the same render tick.
            if (++_liveTelemetryTickCounter >= 3 || count == 0)
            {
                _liveTelemetryTickCounter = 0;
                LiveCoordText.Text = BuildLiveContactText(frame, count);

                LiveCornerText.Text =
                    $"Corners: TL {_topLeft.Samples} | TR {_topRight.Samples} | " +
                    $"BL {_bottomLeft.Samples} | BR {_bottomRight.Samples}";
            }
        }

        private static string BuildLiveContactText(in LiveFrame frame, int count)
        {
            if (count <= 0 || frame.Contacts == null)
                return "Live: no active contacts";

            int slots = Math.Min(count, Math.Min(frame.Contacts.Length, LiveOverlaySlots));
            var sb = new System.Text.StringBuilder(slots * 42);

            for (int i = 0; i < slots; i++)
            {
                var c = frame.Contacts[i];
                if (i > 0)
                    sb.AppendLine();

                sb.Append('C').Append(i + 1)
                  .Append("  ")
                  .Append(c.X).Append(',').Append(c.Y)
                  .Append("  P:").Append(c.Pressure)
                  .Append("  M:").Append(c.Major).Append('/').Append(c.Minor);
            }

            return sb.ToString();
        }

        private void StartLivePolling()
        {
            StopLivePolling();

            int generation = Interlocked.Increment(ref _livePollGeneration);
            var cts = new CancellationTokenSource();
            _liveCts = cts;
            var token = cts.Token;

            _livePollTask = Task.Run(() =>
            {
                try
                {
                    while (!token.IsCancellationRequested &&
                           generation == Volatile.Read(ref _livePollGeneration) &&
                           Volatile.Read(ref _liveEnabled))
                    {
                        if (_device.IsConnected &&
                            _device.TryGetLiveFrame(out var frame) &&
                            generation == Volatile.Read(ref _livePollGeneration))
                        {
                            lock (_liveFrameLock)
                            {
                                _latestLiveFrame = frame;
                                _hasLatestLiveFrame = true;
                            }
                        }

                        // The render timer is ~24 FPS; 30 Hz polling is enough
                        // for smooth telemetry while cutting DeviceIoControl
                        // and marshaling/GC work substantially versus 8 ms.
                        token.WaitHandle.WaitOne(33);
                    }
                }
                catch (ObjectDisposedException)
                {
                    // StopLivePolling can invalidate a session while the
                    // worker is between cancellation and its wait handle.
                }
            }, token);
        }

        // Symmetric counterpart to the pause block in MainWindow_Closing.
        // Re-enables live on the driver side and restarts both the poll
        // task and render timer, but only if Live was actually running when
        // we hid (a user who unchecked Live while parked in the tray - via
        // the tray's own Force Touch/palm toggles, none of which touch
        // Live - shouldn't have it silently turn back on).
        private void ResumeLiveAfterShow()
        {
            _liveWasEnabledBeforeHide = false;

            if (!_liveEnabled || !_device.IsConnected)
                return;

            if (!_device.SetLiveEnabled(true))
            {
                _liveEnabled = false;
                if (ChkLive.IsChecked == true)
                    ChkLive.IsChecked = false;
                return;
            }

            _hasLatestLiveFrame = false;
            StartLivePolling();
            _liveRenderTimer.Start();
        }

        private void StopLivePolling()
        {
            Interlocked.Increment(ref _livePollGeneration);

            var cts = _liveCts;
            var task = _livePollTask;
            _liveCts = null;
            _livePollTask = null;

            if (cts == null)
                return;

            try
            {
                cts.Cancel();
                task?.GetAwaiter().GetResult();
            }
            catch (OperationCanceledException)
            {
            }
            finally
            {
                cts.Dispose();
            }
        }

        private void SlowScrollViewer_MouseWheel(
            object sender,
            System.Windows.Input.MouseWheelEventArgs e)
        {
            if (sender is not System.Windows.Controls.ScrollViewer scrollViewer)
                return;

            // Increased from 18.0: the previous rate felt noticeably slower than a
            // native scroll, so it's bumped by 50% to keep pace with the mouse wheel.
            const double pixelsPerNotch = 27.0;

            double delta =
                e.Delta / 120.0 * pixelsPerNotch;

            double targetOffset =
                scrollViewer.VerticalOffset - delta;

            targetOffset = Math.Max(
                0,
                Math.Min(
                    targetOffset,
                    scrollViewer.ScrollableHeight));

            scrollViewer.ScrollToVerticalOffset(
                targetOffset);

            e.Handled = true;
        }

        private void SetLivePerformanceMode(bool enabled)
        {
            if (_liveShadowsSuppressed == enabled)
                return;

            _liveShadowsSuppressed = enabled;

            // DropShadowEffect is expensive during window movement and redraw.
            // Keep the normal visual style when Live is off, but remove the
            // expensive effects while Live is active.
            if (enabled)
            {
                if (ProfileBarCard != null) ProfileBarCard.Effect = null;
                if (LiveToolbarCard != null) LiveToolbarCard.Effect = null;
                if (BottomActionCard != null) BottomActionCard.Effect = null;
                if (PreviewCard != null) PreviewCard.Effect = null;
                }
            else
            {
                if (ProfileBarCard != null) ProfileBarCard.Effect = (Effect)FindResource("CardShadow");
                if (LiveToolbarCard != null) LiveToolbarCard.Effect = (Effect)FindResource("CardShadow");
                if (BottomActionCard != null) BottomActionCard.Effect = (Effect)FindResource("CardShadow");
                if (PreviewCard != null) PreviewCard.Effect = (Effect)FindResource("CardShadow");
            }
        }

        private void EnsureLiveOverlayElements()
        {
            // Overlay drawing is now handled by LiveOverlayControl as one WPF
            // visual. Keep this method for source compatibility with older
            // callers; there are no per-contact child elements anymore.
        }

        private void HideAllLiveOverlayElements()
        {
            if (LiveOverlayCanvas != null)
                LiveOverlayCanvas.Clear();
        }

        private void DrawLiveOverlay(LiveFrame frame)
        {
            if (LiveOverlayCanvas == null)
                return;

            if (!_liveEnabled || frame.Contacts == null)
            {
                LiveOverlayCanvas.Clear();
                return;
            }

            LiveOverlayCanvas.SetFrame(frame, _geometry, LiveGeometrySmoothAlpha);
        }

        // ---------------------------------------------------------------
        // Slider <-> PalmConfig plumbing
        // ---------------------------------------------------------------

        private void LoadConfigIntoSliders(PalmConfig cfg)
        {
            _suppressEvents = true;
            try
            {
                SlEdgeTop.Value = cfg.EdgePermilleTop;
                SlEdgeLeft.Value = cfg.EdgePermilleLeft;
                SlEdgeRight.Value = cfg.EdgePermilleRight;
                SlEdgeBottom.Value = cfg.EdgePermilleBottom;

                SlLargeMajor.Value = cfg.PalmLargeMajor;
                SlLargeRatio.Value = cfg.PalmLargeRatio;
                SlScoreThresh.Value = cfg.PalmScoreThresh;
                SlMinMajor.Value = cfg.PalmMinMajor;
                SlMinMinor.Value = cfg.PalmMinMinor;
            }
            finally
            {
                _suppressEvents = false;
            }
            UpdateAllLabels();
        }

        private PalmConfig ReadConfigFromSliders()
        {
            var c = PalmConfig.Default;
            c.EdgePermilleTop = (uint)SlEdgeTop.Value;
            c.EdgePermilleLeft = (uint)SlEdgeLeft.Value;
            c.EdgePermilleRight = (uint)SlEdgeRight.Value;
            c.EdgePermilleBottom = (uint)SlEdgeBottom.Value;
            c.PalmLargeMajor = (uint)SlLargeMajor.Value;
            c.PalmLargeRatio = (uint)SlLargeRatio.Value;
            c.PalmScoreThresh = (uint)SlScoreThresh.Value;
            c.PalmMinMajor = (uint)SlMinMajor.Value;
            c.PalmMinMinor = (uint)SlMinMinor.Value;
            return c.Clamped();
        }

        private void UpdateAllLabels()
        {
            LblEdgeTop.Text = FormatPermille(SlEdgeTop.Value);
            LblEdgeLeft.Text = FormatPermille(SlEdgeLeft.Value);
            LblEdgeRight.Text = FormatPermille(SlEdgeRight.Value);
            LblEdgeBottom.Text = FormatPermille(SlEdgeBottom.Value);

            LblLargeMajor.Text = $"{SlLargeMajor.Value:0}";
            LblLargeRatio.Text = $"{SlLargeRatio.Value / 100.0:0.00}×";
            LblScoreThresh.Text = $"{SlScoreThresh.Value:0}";
            LblMinMajor.Text = $"{SlMinMajor.Value:0}";
            LblMinMinor.Text = $"{SlMinMinor.Value:0}";

        }

        private static string FormatPermille(double permille) => $"{permille / 10.0:0.0}%";

        // ---------------------------------------------------------------
        // Pointer tab <-> PointerConfig plumbing
        private bool _suppressPointerEvents;

        private void LoadPointerConfigIntoControls(PointerConfig cfg)
        {
            _suppressPointerEvents = true;
            try
            {
                cfg = cfg.Clamped();
                SlForceTapThreshold.Value = cfg.ForceTapThreshold;
                SlForceTapDragLockoutDistance.Value = cfg.ForceTapDragLockoutDistance;
                ChkForceTouchEnabled.IsChecked = cfg.ForceTouchEnabled != 0;
                ChkRequirePressure.IsChecked = cfg.RequirePressureToActivate != 0;
                ChkRequirePressureContinuously.IsChecked = cfg.RequirePressureContinuously != 0;
                ChkSmallContactRejection.IsChecked = cfg.SmallContactRejectionEnabled != 0;
                ChkSmallContactRejectionStrict.IsChecked = cfg.SmallContactRejectionStrict != 0;
                SlSmallContactMajorThreshold.Value = cfg.SmallContactMajorThreshold;
                ChkForceTouchSmallContactRejection.IsChecked = cfg.ForceTouchSmallContactRejectionEnabled != 0;
                ChkForceTouchSmallContactRejectionStrict.IsChecked = cfg.ForceTouchSmallContactRejectionStrict != 0;
                SlForceTouchSmallContactMajorThreshold.Value = cfg.ForceTouchSmallContactMajorThreshold;
                ChkRequirePressure.IsEnabled = cfg.ForceTouchEnabled != 0;
                ChkRequirePressureContinuously.Visibility = cfg.ForceTouchEnabled != 0 ? Visibility.Visible : Visibility.Collapsed;
                ChkRequirePressureContinuously.IsEnabled = cfg.ForceTouchEnabled != 0;
                ChkSmallContactRejectionStrict.Visibility =
                    cfg.SmallContactRejectionEnabled != 0
                        ? Visibility.Visible
                        : Visibility.Collapsed;
                SmallContactMajorRow.Visibility =
                    (cfg.SmallContactRejectionEnabled != 0 && cfg.SmallContactRejectionStrict != 0)
                        ? Visibility.Visible
                        : Visibility.Collapsed;
                SlSmallContactMajorThreshold.Visibility = SmallContactMajorRow.Visibility;
                ChkForceTouchSmallContactRejectionStrict.Visibility =
                    cfg.ForceTouchSmallContactRejectionEnabled != 0
                        ? Visibility.Visible
                        : Visibility.Collapsed;
                ForceTouchSmallContactMajorRow.Visibility =
                    (cfg.ForceTouchSmallContactRejectionEnabled != 0 && cfg.ForceTouchSmallContactRejectionStrict != 0)
                        ? Visibility.Visible
                        : Visibility.Collapsed;
                SlForceTouchSmallContactMajorThreshold.Visibility = ForceTouchSmallContactMajorRow.Visibility;
                SlCursorSmoothing.Value = cfg.CursorSmoothingPercent;
                SlCursorSpeed.Value = cfg.CursorSpeedPercent;
                SlCursorDeadzone.Value = cfg.CursorDeadzone;
                SlCursorDeadzoneSlow.Value = cfg.CursorDeadzoneSlow;
                SlCursorDeadzoneFast.Value = cfg.CursorDeadzoneFast;
                SlCursorSlowVelocity.Value = cfg.CursorSlowVelocity;
                SlCursorFastVelocity.Value = cfg.CursorFastVelocity;
                SlSmoothingAlphaDen.Value = cfg.SmoothingAlphaDen;
                SlSmoothingAlphaNumSlow.Value = cfg.SmoothingAlphaNumSlow;

                RadioButton selected = cfg.ForceTapAction switch
                {
                    PointerConfig.ActionMiddleClick => RbActionMiddleClick,
                    PointerConfig.ActionDoubleClick => RbActionDoubleClick,
                    _ => RbActionContextMenu,
                };
                RbActionContextMenu.IsChecked = ReferenceEquals(selected, RbActionContextMenu);
                RbActionMiddleClick.IsChecked = ReferenceEquals(selected, RbActionMiddleClick);
                RbActionDoubleClick.IsChecked = ReferenceEquals(selected, RbActionDoubleClick);

                ChkForceTouchEmulationEnabled.IsChecked = cfg.ForceTouchEmulationEnabled != 0;
                SlForceTouchEmulationHoldMs.Value = cfg.ForceTouchEmulationHoldMs;
                SlForceTouchEmulationDragLockoutDistance.Value = cfg.ForceTouchEmulationDragLockoutDistance;
                RadioButton selectedEmulation = cfg.ForceTouchEmulationAction switch
                {
                    PointerConfig.ActionMiddleClick => RbEmulationActionMiddleClick,
                    PointerConfig.ActionDoubleClick => RbEmulationActionDoubleClick,
                    _ => RbEmulationActionContextMenu,
                };
                RbEmulationActionContextMenu.IsChecked = ReferenceEquals(selectedEmulation, RbEmulationActionContextMenu);
                RbEmulationActionMiddleClick.IsChecked = ReferenceEquals(selectedEmulation, RbEmulationActionMiddleClick);
                RbEmulationActionDoubleClick.IsChecked = ReferenceEquals(selectedEmulation, RbEmulationActionDoubleClick);
                bool emulationEnabled = cfg.ForceTouchEmulationEnabled != 0;
                ForceTouchEmulationActionPanel.IsEnabled = emulationEnabled;
                ForceTouchEmulationHoldRow.IsEnabled = emulationEnabled;
                SlForceTouchEmulationHoldMs.IsEnabled = emulationEnabled;
            }
            finally { _suppressPointerEvents = false; }
            UpdatePointerLabels();
        }

        private PointerConfig ReadPointerConfigFromControls()
        {
            var c = PointerConfig.Default;
            c.ForceTapThreshold = (uint)SlForceTapThreshold.Value;
            c.ForceTapDragLockoutDistance = (uint)SlForceTapDragLockoutDistance.Value;
            c.ForceTouchEnabled = ChkForceTouchEnabled.IsChecked == true ? 1u : 0u;
            c.RequirePressureToActivate = ChkRequirePressure.IsChecked == true ? 1u : 0u;
            c.RequirePressureContinuously =
                ChkRequirePressureContinuously.IsChecked == true ? 1u : 0u;
            c.SmallContactRejectionEnabled = ChkSmallContactRejection.IsChecked == true ? 1u : 0u;
            c.SmallContactRejectionStrict =
                (ChkSmallContactRejection.IsChecked == true &&
                 ChkSmallContactRejectionStrict.IsChecked == true) ? 1u : 0u;
            c.SmallContactMajorThreshold = (uint)SlSmallContactMajorThreshold.Value;
            c.ForceTouchSmallContactRejectionEnabled = ChkForceTouchSmallContactRejection.IsChecked == true ? 1u : 0u;
            c.ForceTouchSmallContactRejectionStrict =
                (ChkForceTouchSmallContactRejection.IsChecked == true &&
                 ChkForceTouchSmallContactRejectionStrict.IsChecked == true) ? 1u : 0u;
            c.ForceTouchSmallContactMajorThreshold = (uint)SlForceTouchSmallContactMajorThreshold.Value;
            c.CursorSmoothingPercent = (uint)SlCursorSmoothing.Value;
            c.CursorSpeedPercent = (uint)SlCursorSpeed.Value;
            c.CursorDeadzone = (uint)SlCursorDeadzone.Value;
            c.CursorDeadzoneSlow = (uint)SlCursorDeadzoneSlow.Value;
            c.CursorDeadzoneFast = (uint)SlCursorDeadzoneFast.Value;
            c.CursorSlowVelocity = (uint)SlCursorSlowVelocity.Value;
            c.CursorFastVelocity = (uint)SlCursorFastVelocity.Value;
            c.SmoothingAlphaDen = (uint)SlSmoothingAlphaDen.Value;
            c.SmoothingAlphaNumSlow = (uint)SlSmoothingAlphaNumSlow.Value;
            c.ForceTapAction =
                RbActionMiddleClick.IsChecked == true ? PointerConfig.ActionMiddleClick :
                RbActionDoubleClick.IsChecked == true ? PointerConfig.ActionDoubleClick :
                PointerConfig.ActionContextMenu;
            c.ForceTouchEmulationEnabled = ChkForceTouchEmulationEnabled.IsChecked == true ? 1u : 0u;
            c.ForceTouchEmulationHoldMs = (uint)SlForceTouchEmulationHoldMs.Value;
            c.ForceTouchEmulationDragLockoutDistance = (uint)SlForceTouchEmulationDragLockoutDistance.Value;
            c.ForceTouchEmulationAction =
                RbEmulationActionMiddleClick.IsChecked == true ? PointerConfig.ActionMiddleClick :
                RbEmulationActionDoubleClick.IsChecked == true ? PointerConfig.ActionDoubleClick :
                PointerConfig.ActionContextMenu;
            return c.Clamped();
        }

        private void UpdatePointerLabels()
        {
            if (LblForceTapThreshold == null) return;
            LblForceTapThreshold.Text = $"{SlForceTapThreshold.Value:0}";
            LblForceTapDragLockoutDistance.Text = $"{SlForceTapDragLockoutDistance.Value:0}";
            LblCursorSmoothing.Text = $"{SlCursorSmoothing.Value:0}%";
            LblCursorSpeed.Text = $"{SlCursorSpeed.Value:0}%";
            LblCursorDeadzone.Text = $"{SlCursorDeadzone.Value:0}";
            LblCursorDeadzoneSlow.Text = $"{SlCursorDeadzoneSlow.Value:0}";
            LblCursorDeadzoneFast.Text = $"{SlCursorDeadzoneFast.Value:0}";
            LblCursorSlowVelocity.Text = $"{SlCursorSlowVelocity.Value:0}";
            LblCursorFastVelocity.Text = $"{SlCursorFastVelocity.Value:0}";
            LblSmoothingAlphaDen.Text = $"{SlSmoothingAlphaDen.Value:0}";
            LblSmoothingAlphaNumSlow.Text = $"{SlSmoothingAlphaNumSlow.Value:0}";
            LblSmallContactMajorThreshold.Text = $"{SlSmallContactMajorThreshold.Value:0}";
            LblForceTouchSmallContactMajorThreshold.Text = $"{SlForceTouchSmallContactMajorThreshold.Value:0}";
            LblForceTouchEmulationHoldMs.Text = $"{SlForceTouchEmulationHoldMs.Value / 1000.0:0.00} s";
            LblForceTouchEmulationDragLockoutDistance.Text = $"{SlForceTouchEmulationDragLockoutDistance.Value:0}";
        }

        private void PointerSlider_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressPointerEvents) return;
            UpdatePointerLabels();
        }

        private void PointerAction_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressPointerEvents) return;
        }

        private void ForceTouchOption_Changed(object sender, RoutedEventArgs e)
        {
            if (ChkForceTouchEnabled == null ||
                ChkRequirePressure == null ||
                ChkRequirePressureContinuously == null)
                return;

            bool enabled = ChkForceTouchEnabled.IsChecked == true;
            ChkRequirePressure.IsEnabled = enabled;
            ChkRequirePressureContinuously.IsEnabled = enabled;
            ChkRequirePressureContinuously.Visibility =
                enabled ? Visibility.Visible : Visibility.Collapsed;
        }

        private void ForceTouchEmulationOption_Changed(object sender, RoutedEventArgs e)
        {
            if (ChkForceTouchEmulationEnabled == null ||
                ForceTouchEmulationActionPanel == null ||
                ForceTouchEmulationHoldRow == null ||
                SlForceTouchEmulationHoldMs == null)
                return;

            bool enabled = ChkForceTouchEmulationEnabled.IsChecked == true;
            ForceTouchEmulationActionPanel.IsEnabled = enabled;
            ForceTouchEmulationHoldRow.IsEnabled = enabled;
            SlForceTouchEmulationHoldMs.IsEnabled = enabled;
        }

        private void SmallContactRejectionOption_Changed(object sender, RoutedEventArgs e)
        {
            if (ChkSmallContactRejection == null || ChkSmallContactRejectionStrict == null ||
                SmallContactMajorRow == null || SlSmallContactMajorThreshold == null)
                return;

            bool enabled = ChkSmallContactRejection.IsChecked == true;
            ChkSmallContactRejectionStrict.Visibility =
                enabled ? Visibility.Visible : Visibility.Collapsed;

            if (!enabled)
                ChkSmallContactRejectionStrict.IsChecked = false;

            bool strict = enabled && ChkSmallContactRejectionStrict.IsChecked == true;
            SmallContactMajorRow.Visibility = strict ? Visibility.Visible : Visibility.Collapsed;
            SlSmallContactMajorThreshold.Visibility = SmallContactMajorRow.Visibility;
        }

        private void ForceTouchSmallContactRejectionOption_Changed(object sender, RoutedEventArgs e)
        {
            if (ChkForceTouchSmallContactRejection == null || ChkForceTouchSmallContactRejectionStrict == null ||
                ForceTouchSmallContactMajorRow == null || SlForceTouchSmallContactMajorThreshold == null)
                return;

            bool enabled = ChkForceTouchSmallContactRejection.IsChecked == true;
            ChkForceTouchSmallContactRejectionStrict.Visibility =
                enabled ? Visibility.Visible : Visibility.Collapsed;

            if (!enabled)
                ChkForceTouchSmallContactRejectionStrict.IsChecked = false;

            bool strict = enabled && ChkForceTouchSmallContactRejectionStrict.IsChecked == true;
            ForceTouchSmallContactMajorRow.Visibility = strict ? Visibility.Visible : Visibility.Collapsed;
            SlForceTouchSmallContactMajorThreshold.Visibility = ForceTouchSmallContactMajorRow.Visibility;
        }

        // Scroll tab <-> ScrollConfig plumbing
        private bool _suppressScrollEvents;

        private void LoadScrollConfigIntoControls(ScrollConfig cfg)
        {
            _suppressScrollEvents = true;
            try
            {
                cfg = cfg.Clamped();
                SlScrollSpeed.Value = cfg.SpeedPercent;
                SlScrollFastSpeed.Value = cfg.FastSpeedPercent;
                SlScrollSmoothing.Value = cfg.SmoothingPercent;
                SlScrollDeadzone.Value = cfg.Deadzone;
                SlScrollFastVelocity.Value = cfg.FastVelocity;
                SlScrollScaleNum.Value = cfg.ScaleNum;
                SlScrollScaleDen.Value = cfg.ScaleDen;
                SlScrollScaleNumFast.Value = cfg.ScaleNumFast;
                SlScrollScaleDenFast.Value = cfg.ScaleDenFast;
            }
            finally { _suppressScrollEvents = false; }
            UpdateScrollLabels();
        }

        private ScrollConfig ReadScrollConfigFromControls()
        {
            var c = ScrollConfig.Default;
            c.SpeedPercent = (uint)SlScrollSpeed.Value;
            c.FastSpeedPercent = (uint)SlScrollFastSpeed.Value;
            c.SmoothingPercent = (uint)SlScrollSmoothing.Value;
            c.Deadzone = (uint)SlScrollDeadzone.Value;
            c.FastVelocity = (uint)SlScrollFastVelocity.Value;
            c.ScaleNum = (uint)SlScrollScaleNum.Value;
            c.ScaleDen = (uint)SlScrollScaleDen.Value;
            c.ScaleNumFast = (uint)SlScrollScaleNumFast.Value;
            c.ScaleDenFast = (uint)SlScrollScaleDenFast.Value;
            return c.Clamped();
        }

        private void UpdateScrollLabels()
        {
            if (LblScrollSpeed == null) return;
            LblScrollSpeed.Text = $"{SlScrollSpeed.Value:0}%";
            LblScrollFastSpeed.Text = $"{SlScrollFastSpeed.Value:0}%";
            LblScrollSmoothing.Text = $"{SlScrollSmoothing.Value:0}%";
            LblScrollDeadzone.Text = $"{SlScrollDeadzone.Value:0}";
            LblScrollFastVelocity.Text = $"{SlScrollFastVelocity.Value:0}";
            LblScrollScaleNum.Text = $"{SlScrollScaleNum.Value:0}";
            LblScrollScaleDen.Text = $"{SlScrollScaleDen.Value:0}";
            LblScrollScaleNumFast.Text = $"{SlScrollScaleNumFast.Value:0}";
            LblScrollScaleDenFast.Text = $"{SlScrollScaleDenFast.Value:0}";
        }

        private void ScrollSlider_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressScrollEvents) return;
            UpdateScrollLabels();
        }

        private void Slider_Changed(object sender, RoutedEventArgs e)
        {
            // Setting Minimum/Maximum on sliders declared earlier in the XAML
            // can coerce Value and fire this handler while InitializeComponent()
            // is still parsing later controls (e.g. SlLargeMajor's Min/Max is
            // set before SlLargeRatio exists) — those fields are still null then.
            if (!_uiReady || _suppressEvents) return;
            UpdateAllLabels();
            DrawPreview();
        }

        // ---------------------------------------------------------------
        // Live preview canvas
        //
        // Draws the raw touchpad surface and its configured edge zones.
        // Live contacts are rendered separately by LiveOverlayControl.
        // ---------------------------------------------------------------

        private Rectangle? _previewPadRect;
        private readonly Rectangle[] _previewEdgeZones = new Rectangle[4];
        private Line? _previewCenterVertical;
        private Line? _previewCenterHorizontal;

        private void EnsurePreviewVisuals()
        {
            if (PreviewCanvas == null || _previewPadRect != null)
                return;

            _previewPadRect = new Rectangle
            {
                Stroke = GlassPadStrokeBrush,
                StrokeThickness = 1.5,
                Fill = GlassPadBrush,
                RadiusX = 14,
                RadiusY = 14
            };
            PreviewCanvas.Children.Add(_previewPadRect);

            _previewCenterVertical = new Line { Stroke = CrosshairBrush, StrokeThickness = 1 };
            _previewCenterHorizontal = new Line { Stroke = CrosshairBrush, StrokeThickness = 1 };
            PreviewCanvas.Children.Add(_previewCenterVertical);
            PreviewCanvas.Children.Add(_previewCenterHorizontal);

            for (int i = 0; i < _previewEdgeZones.Length; i++)
            {
                _previewEdgeZones[i] = new Rectangle { Fill = EdgeZoneBrush };
                PreviewCanvas.Children.Add(_previewEdgeZones[i]);
            }
        }

        private void DrawPreview()
        {
            if (PreviewCanvas == null) return;
            EnsurePreviewVisuals();
            if (_previewPadRect == null ||
                _previewCenterVertical == null || _previewCenterHorizontal == null) return;

            double w = PreviewCanvas.Width;
            double h = PreviewCanvas.Height;
            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;
            if (xRange <= 0 || yRange <= 0) return;

            var cfg = ReadConfigFromSliders();

            _previewPadRect.Width = w;
            _previewPadRect.Height = h;

            _previewCenterVertical.X1 = _previewCenterVertical.X2 = w / 2;
            _previewCenterVertical.Y1 = 0;
            _previewCenterVertical.Y2 = h;
            _previewCenterHorizontal.X1 = 0;
            _previewCenterHorizontal.X2 = w;
            _previewCenterHorizontal.Y1 = _previewCenterHorizontal.Y2 = h / 2;

            double edgeTopPx = h * (cfg.EdgePermilleTop / 1000.0);
            double edgeBottomPx = h * (cfg.EdgePermilleBottom / 1000.0);
            double edgeLeftPx = w * (cfg.EdgePermilleLeft / 1000.0);
            double edgeRightPx = w * (cfg.EdgePermilleRight / 1000.0);

            UpdatePreviewZone(_previewEdgeZones[0], 0, 0, w, edgeTopPx);
            UpdatePreviewZone(_previewEdgeZones[1], 0, h - edgeBottomPx, w, edgeBottomPx);
            UpdatePreviewZone(_previewEdgeZones[2], 0, 0, edgeLeftPx, h);
            UpdatePreviewZone(_previewEdgeZones[3], w - edgeRightPx, 0, edgeRightPx, h);


        }

        private static void UpdatePreviewZone(Rectangle zone, double x, double y, double width, double height)
        {
            bool visible = width > 0 && height > 0;
            zone.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (!visible)
                return;

            zone.Width = width;
            zone.Height = height;
            Canvas.SetLeft(zone, x);
            Canvas.SetTop(zone, y);
        }

        private void AddZoneRect(double x, double y, double w, double h, System.Windows.Media.Brush brush)
        {
            if (w <= 0 || h <= 0) return;
            var r = new Rectangle { Width = w, Height = h, Fill = brush };
            Canvas.SetLeft(r, x);
            Canvas.SetTop(r, y);
            PreviewCanvas.Children.Add(r);
        }

        private void AddLabel(string text, double x, double y, System.Windows.Media.Brush brush, double size)
        {
            var t = new TextBlock
            {
                Text = text,
                Foreground = brush,
                FontSize = Math.Max(size + 1.5, 12.5),
                FontWeight = FontWeights.Bold,
                // Letter-spacing-ish caption treatment (small caps feel) for
                // the "EDGE ZONE" tag - a technical/instrument detail rather
                // than a plain inline label competing with the readout text.
                FontFamily = new FontFamily("Segoe UI Semibold, Segoe UI"),
            };
            Canvas.SetLeft(t, x);
            Canvas.SetTop(t, y);
            PreviewCanvas.Children.Add(t);
        }

        // Faint, thin guide line across the pad glass - purely a visual
        // calibration aid (center crosshair), not tied to any threshold.
        private void AddGuideLine(double x1, double y1, double x2, double y2)
        {
            var line = new Line
            {
                X1 = x1,
                Y1 = y1,
                X2 = x2,
                Y2 = y2,
                Stroke = CrosshairBrush,
                StrokeThickness = 1,
            };
            PreviewCanvas.Children.Add(line);
        }

        private void AddCross(double x, double y)
        {
            const double s = 8;
            var l1 = new Line { X1 = x - s, Y1 = y, X2 = x + s, Y2 = y, Stroke = Brushes.Black, StrokeThickness = 1.5, Opacity = 0.55 };
            var l2 = new Line { X1 = x, Y1 = y - s, X2 = x, Y2 = y + s, Stroke = Brushes.Black, StrokeThickness = 1.5, Opacity = 0.55 };
            PreviewCanvas.Children.Add(l1);
            PreviewCanvas.Children.Add(l2);
        }

        private static double Clamp(double v, double min, double max) => v < min ? min : (v > max ? max : v);

        // ---------------------------------------------------------------
        // Apply / Reset / Save / Load
        // ---------------------------------------------------------------

        // NOTE: this is the GLOBAL bottom-bar Apply/Reset - it is always
        // visible regardless of which tab (Palm/Scroll/Pointer) is
        // currently selected. It must commit BOTH PalmConfig and
        // PointerConfig, or a user who edits the Pointer tab and hits this
        // button (rather than the Pointer tab's own local Apply button)
        // would see their Force Tap changes silently discarded - the
        // sliders would keep showing the new values locally, but nothing
        // would actually reach the driver/registry, so they'd revert to
        // the old/default values the next time the GUI reconnects.
        //
        // This is only the XAML Click handler now - the actual commit logic
        // lives in CommitCurrentConfigToProfile() below, which every tray
        // menu action calls directly. Tray actions used to call
        // `Save_Click(this, new RoutedEventArgs())`: a fake event invocation
        // used purely to reach this method's body, with a RoutedEventArgs
        // that meant nothing and a `this` that wasn't really the event
        // source. Any future change to this handler's signature (or to what
        // it does with `sender`/`e`) would have silently broken every tray
        // toggle. Calling the real method directly removes that coupling.
        private void Save_Click(object sender, RoutedEventArgs e) => CommitCurrentConfigToProfile();

        private void CommitCurrentConfigToProfile()
        {
            var requestedPalm = ReadConfigFromSliders();
            var profileBeforeEdgeOverride = ActiveProfile;
            if (!_appSettings.PalmEdgeRejectionEnabled && profileBeforeEdgeOverride != null)
            {
                // Preserve configured edge widths in the profile; only the
                // effective driver config is disabled while the global
                // tray toggle is off.
                requestedPalm.EdgePermilleTop = 0;
                requestedPalm.EdgePermilleLeft = 0;
                requestedPalm.EdgePermilleRight = 0;
                requestedPalm.EdgePermilleBottom = 0;
            }
            var requestedPointer = ReadPointerConfigFromControls();
            var requestedScroll = ReadScrollConfigFromControls();

            bool palmOk = false, pointerOk = false, scrollOk = false;
            if (_device.IsConnected)
            {
                palmOk = _device.TrySetPalmConfig(requestedPalm, out var appliedPalm);
                if (palmOk)
                {
                    requestedPalm = appliedPalm;
                    LoadConfigIntoSliders(appliedPalm);
                    DrawPreview();
                }

                pointerOk = _device.TrySetPointerConfig(requestedPointer, out var appliedPointer);
                if (pointerOk)
                {
                    requestedPointer = appliedPointer;
                    LoadPointerConfigIntoControls(appliedPointer);
                }

                scrollOk = _device.TrySetScrollConfig(requestedScroll, out var appliedScroll);
                if (scrollOk)
                {
                    requestedScroll = appliedScroll;
                    LoadScrollConfigIntoControls(appliedScroll);
                }
            }

            var profile = ActiveProfile;
            if (profile != null)
            {
                var savedPalm = ReadConfigFromSliders();
                if (!_appSettings.PalmEdgeRejectionEnabled)
                {
                    // The UI/driver use zero as the effective edge config
                    // while disabled, but the profile must retain the user's
                    // configured edge widths for the next enable.
                    var storedPalm = profile.Palm;
                    savedPalm.EdgePermilleTop = storedPalm.EdgePermilleTop;
                    savedPalm.EdgePermilleLeft = storedPalm.EdgePermilleLeft;
                    savedPalm.EdgePermilleRight = storedPalm.EdgePermilleRight;
                    savedPalm.EdgePermilleBottom = storedPalm.EdgePermilleBottom;
                }

                profile.Palm = savedPalm.Clamped();
                profile.Pointer = requestedPointer.Clamped();
                profile.Scroll = requestedScroll.StructVersion == 0 ? ScrollConfig.Default : requestedScroll.Clamped();
                TrySaveProfiles();
            }

            if (!_device.IsConnected)
            {
                SetBottomStatus("Profile saved locally. Device is not connected, so the driver was not updated.");
            }
            else if (palmOk && pointerOk && scrollOk)
            {
                SetBottomStatus($"“{profile?.Name ?? "Current profile"}” saved and applied to the driver.");
            }
            else if (!palmOk && !pointerOk && !scrollOk)
            {
                SetBottomStatus("Profile saved locally, but DeviceIoControl did not allow writing it to the driver.");
            }
            else
            {
                SetBottomStatus("Profile saved locally. Some settings could not be written to the driver.");
            }

            // Every tray toggle/profile switch routes through here while the
            // window is still hidden. Each call allocates a fresh PalmConfig/
            // PointerConfig/ScrollConfig, runs DeviceIoControl round-trips,
            // and rebuilds the tray menu - all of which touches pages that
            // TrayMemoryTrimmer.TrimAfterHide() already paged out when we
            // first hid to tray. Re-trim so a tray-parked instance that gets
            // toggled a few times doesn't creep back up to open-window
            // memory and stay there. No-ops (cheaply) when the window is
            // actually visible/open, since this is also reached from the
            // real Save button.
            if (!IsVisible)
                TrayMemoryTrimmer.TrimAfterHide();
        }

        private void ResetDefaults_Click(object sender, RoutedEventArgs e)
        {
            var defaultPalm = PalmConfig.Default;
            var driverPalm = defaultPalm;
            if (!_appSettings.PalmEdgeRejectionEnabled)
            {
                driverPalm.EdgePermilleTop = 0;
                driverPalm.EdgePermilleLeft = 0;
                driverPalm.EdgePermilleRight = 0;
                driverPalm.EdgePermilleBottom = 0;
            }

            var appliedPalm = driverPalm;
            bool palmOk = false;
            if (_device.IsConnected)
            {
                palmOk = _appSettings.PalmEdgeRejectionEnabled
                    ? _device.TryResetPalmConfig(out appliedPalm)
                    : _device.TrySetPalmConfig(driverPalm, out appliedPalm);
            }
            // GUI keeps the configured default edge widths visible even when
            // the global edge-rejection switch is currently off.
            LoadConfigIntoSliders(defaultPalm);

            var appliedPointer = PointerConfig.Default;
            bool pointerOk = _device.IsConnected && _device.TryResetPointerConfig(out appliedPointer);
            LoadPointerConfigIntoControls(pointerOk ? appliedPointer : PointerConfig.Default);

            var appliedScroll = ScrollConfig.Default;
            bool scrollOk = _device.IsConnected && _device.TryResetScrollConfig(out appliedScroll);
            LoadScrollConfigIntoControls(scrollOk ? appliedScroll : ScrollConfig.Default);

            if (palmOk && pointerOk && scrollOk)
                SetBottomStatus("Reset to defaults (Palm + Scroll + Pointer).");
            else if (_device.IsConnected)
                SetBottomStatus("Could not fully reset driver settings — showing defaults.");
            else
                SetBottomStatus("Showing defaults (device not connected).");

            DrawPreview();
            var active = ActiveProfile;
            if (active != null)
            {
                active.Palm = ReadConfigFromSliders();
                active.Pointer = ReadPointerConfigFromControls();
                active.Scroll = ReadScrollConfigFromControls();
                TrySaveProfiles();
            }
        }

        private void SaveProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter = "AmtPtp profile (*.json)|*.json",
                FileName = "AmtPtpProfile.json",
            };
            if (dlg.ShowDialog() != true) return;

            var profile = AmtPtpProfile.FromCurrent(
                ReadConfigFromSliders(),
                ReadPointerConfigFromControls(),
                ReadScrollConfigFromControls());

            var json = JsonSerializer.Serialize(profile, JsonOptions);
            File.WriteAllText(dlg.FileName, json);
            SetBottomStatus($"Profile (Palm + Scroll + Pointer) saved: {dlg.FileName}");
        }

        private void LoadProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog { Filter = "AmtPtp profile (*.json)|*.json" };
            if (dlg.ShowDialog() != true) return;

            try
            {
                var json = File.ReadAllText(dlg.FileName);

                // New-format files ({"Palm": {...}, "Pointer": {...}})
                // round-trip both configs. Files saved by an older GUI
                // build (before Pointer support existed) are just a bare
                // PalmConfig object with no wrapper - detect which shape
                // this file is and fall back so those old files still
                // load instead of throwing.
                bool isProfileFormat;
                using (var doc = JsonDocument.Parse(json))
                {
                    isProfileFormat = doc.RootElement.TryGetProperty("Palm", out _);
                }

                if (isProfileFormat)
                {
                    var profile = JsonSerializer.Deserialize<AmtPtpProfile>(json, JsonOptions);
                    if (profile == null)
                        throw new InvalidDataException("Empty or invalid profile file.");

                    LoadConfigIntoSliders(profile.Palm.StructVersion == 0 ? PalmConfig.Default : profile.Palm.Clamped());
                    LoadPointerConfigIntoControls(NormalizePointerConfig(profile.Pointer));
                    LoadScrollConfigIntoControls(profile.Scroll.StructVersion == 0 ? ScrollConfig.Default : profile.Scroll.Clamped());
                    DrawPreview();
                    SetBottomStatus($"Profile (Palm + Scroll + Pointer) loaded: {dlg.FileName}. Click “Save” to write it to the driver.");
                }
                else
                {
                    // Legacy flat-PalmConfig file - it never had Pointer
                    // data, so the Pointer tab's current values are left
                    // untouched rather than being reset to something the
                    // file never actually contained.
                    var cfg = JsonSerializer.Deserialize<PalmConfig>(json, JsonOptions);
                    LoadConfigIntoSliders(cfg.StructVersion == 0 ? PalmConfig.Default : cfg.Clamped());
                    DrawPreview();
                    SetBottomStatus($"Profile (legacy Palm-only format) loaded: {dlg.FileName}. Click “Save” to write it to the driver.");
                }
            }
            catch (Exception ex)
            {
                SetBottomStatus($"Profile load error: {ex.Message}");
            }
        }

        private static void SlowScrollWheel(object? sender, MouseWheelEventArgs e)
        {
            if (e.Delta == 0)
                return;

            DependencyObject? source = e.OriginalSource as DependencyObject;
            ScrollViewer? viewer = null;

            while (source != null)
            {
                if (source is ScrollViewer sv)
                {
                    viewer = sv;
                    break;
                }

                source = System.Windows.Media.VisualTreeHelper.GetParent(source);
            }

            if (viewer == null)
                return;

            // Keep the normal mouse-wheel rate unchanged, but make smooth/high-resolution
            // trackpad wheel events 30% faster. Precision touchpads typically report
            // fractional/sub-notch deltas, while a conventional mouse reports 120 per notch.
            const double pixelsPerNotch = 27.0;
            bool highResolutionInput = Math.Abs(e.Delta) < System.Windows.Input.Mouse.MouseWheelDeltaForOneLine;
            double multiplier = highResolutionInput ? 1.30 : 1.0;
            double notches = e.Delta / (double)System.Windows.Input.Mouse.MouseWheelDeltaForOneLine;
            viewer.ScrollToVerticalOffset(viewer.VerticalOffset - notches * pixelsPerNotch * multiplier);
            e.Handled = true;
        }

        private void SetBottomStatus(string text) => BottomStatusText.Text = text;

        protected override void OnClosed(EventArgs e)
        {
            // Single teardown path (see the note in the constructor): tell
            // the driver to stop streaming live frames, stop the UI poll
            // timer, then release the device handle.
            if (_liveEnabled)
            {
                _device.SetLiveEnabled(false);
                _liveEnabled = false;
            }
            StopLivePolling();
            _liveRenderTimer.Stop();
            _device.Dispose();
            _trayIcon.Visible = false;
            _trayIcon.Dispose();
            base.OnClosed(e);
        }
    }
    // A ToolStripMenuItem whose flyout ("Profile", "Force Tap action")
    // reuses the rounded strip and the theme-accurate renderer/colors of
    // whichever ContextMenuStrip it lives in. Left as the default
    // ToolStripMenuItem, WinForms auto-creates a plain ToolStripDropDownMenu
    // for the flyout that falls back to the system-default renderer - a
    // square, unthemed popup with the stock checkmark glyph next to the
    // rounded, custom-rendered top-level tray menu it opens from. That
    // mismatch between the top-level menu and its own submenus was a big
    // part of why the context menu read as unfinished.
    internal sealed class RoundedMenuItem : Forms.ToolStripMenuItem
    {
        public RoundedMenuItem(string text) : base(text) { }

        protected override Forms.ToolStripDropDown CreateDefaultDropDown()
        {
            var dropDown = new RoundedContextMenuStrip
            {
                ShowImageMargin = false,
                ShowCheckMargin = true,
                Padding = new System.Windows.Forms.Padding(2, 4, 2, 4)
            };
            SyncChrome(dropDown);
            dropDown.Opening += (_, _) => SyncChrome(dropDown);
            dropDown.Opened += (_, _) => PositionDropDown(dropDown);
            return dropDown;
        }

        private void SyncChrome(Forms.ToolStripDropDown dropDown)
        {
            if (Owner == null)
                return;
            dropDown.Font = Owner.Font;
            dropDown.BackColor = Owner.BackColor;
            dropDown.ForeColor = Owner.ForeColor;
            dropDown.Renderer = Owner.Renderer;
        }

        private void PositionDropDown(Forms.ToolStripDropDown dropDown)
        {
            if (Owner == null || !dropDown.IsHandleCreated)
                return;

            var screenPoint = Owner.PointToScreen(
                new System.Drawing.Point(Bounds.Right - 1, Bounds.Top));
            dropDown.Location = screenPoint;
        }
    }

    internal sealed class RoundedContextMenuStrip : Forms.ContextMenuStrip
    {
        [DllImport("gdi32.dll")]
        private static extern IntPtr CreateRoundRectRgn(int x1, int y1, int x2, int y2, int cx, int cy);

        [DllImport("user32.dll")]
        private static extern int SetWindowRgn(IntPtr hWnd, IntPtr hRgn, bool redraw);

        // Corner radius is authored at 96 DPI and scaled to the strip's
        // actual DPI so the corner reads the same size relative to the
        // padding/font at 100%, 150%, 200% etc. A fixed pixel radius looked
        // proportionally too small/flat on scaled displays, which is what
        // read as "crooked" - the rounding didn't match the rest of the
        // chrome (buttons, cards) that already scale with DPI via WPF.
        private const int BaseRadiusAt96Dpi = 12;

        // Corner radius used for each item's own selection/press highlight.
        // Kept in sync with what ModernTrayRenderer draws so the highlight
        // never shows square corners inside the rounded strip.
        internal const int ItemHighlightRadius = 6;

        private int CurrentRadius => Math.Max(4, (int)Math.Round(BaseRadiusAt96Dpi * DeviceDpi / 96.0));

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            ApplyRoundedRegion();
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            if (IsHandleCreated)
                ApplyRoundedRegion();
        }

        protected override void OnDpiChangedAfterParent(EventArgs e)
        {
            base.OnDpiChangedAfterParent(e);
            if (IsHandleCreated)
                ApplyRoundedRegion();
        }

        private void ApplyRoundedRegion()
        {
            if (!IsHandleCreated || Width <= 0 || Height <= 0)
                return;

            int radius = CurrentRadius;
            IntPtr region = CreateRoundRectRgn(1, 1, Math.Max(1, Width - 1), Math.Max(1, Height - 1), radius, radius);
            if (region != IntPtr.Zero)
                SetWindowRgn(Handle, region, true);
        }
    }

    internal sealed class ModernTrayRenderer : Forms.ToolStripProfessionalRenderer
    {
        private sealed class Colors : Forms.ProfessionalColorTable
        {
            public Colors(string themeId)
            {
                var palette = ThemeManager.TrayColors(themeId);
                MenuBack = palette.Back;
                Fore = palette.Fore;
                Selected = palette.Selected;
                Border = palette.Border;
                Accent = palette.Accent;
            }

            public System.Drawing.Color MenuBack { get; }
            public System.Drawing.Color Fore { get; }
            public System.Drawing.Color Selected { get; }
            public System.Drawing.Color Border { get; }
            public System.Drawing.Color Accent { get; }

            public override System.Drawing.Color MenuBorder => Border;
            public override System.Drawing.Color MenuItemBorder => Border;
            public override System.Drawing.Color MenuItemSelected => Selected;
            public override System.Drawing.Color MenuItemSelectedGradientBegin => Selected;
            public override System.Drawing.Color MenuItemSelectedGradientEnd => Selected;
            public override System.Drawing.Color ToolStripDropDownBackground => MenuBack;
            public override System.Drawing.Color ImageMarginGradientBegin => MenuBack;
            public override System.Drawing.Color ImageMarginGradientMiddle => MenuBack;
            public override System.Drawing.Color ImageMarginGradientEnd => MenuBack;
            public override System.Drawing.Color SeparatorDark => Border;
            public override System.Drawing.Color SeparatorLight => MenuBack;
        }

        private readonly System.Drawing.Color _accent;

        public ModernTrayRenderer(string themeId) : base(new Colors(themeId))
        {
            RoundedEdges = true;
            _accent = ThemeManager.TrayColors(themeId).Accent;
        }

        // The base renderer paints a flat, square-cornered rectangle for the
        // hovered/pressed item. Inside RoundedContextMenuStrip that square
        // block visibly clashes with the strip's rounded outline - most
        // obviously on the first/last item, where the highlight's sharp
        // corner sits right next to the strip's rounded corner. Draw the
        // highlight as a rounded rect instead (Windows 11 menu style) so it
        // matches the container everywhere, not just at the edges.
        protected override void OnRenderMenuItemBackground(Forms.ToolStripItemRenderEventArgs e)
        {
            if (e.Item is not Forms.ToolStripMenuItem { Enabled: true } item || (!item.Selected && !item.Pressed))
            {
                base.OnRenderMenuItemBackground(e);
                return;
            }

            var bounds = new System.Drawing.Rectangle(System.Drawing.Point.Empty, e.Item.Size);
            bounds.Inflate(-4, -1);
            if (bounds.Width <= 0 || bounds.Height <= 0)
                return;

            var g = e.Graphics;
            var oldHint = g.SmoothingMode;
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

            var colors = (Colors)ColorTable;
            using (var brush = new System.Drawing.SolidBrush(colors.Selected))
            using (var path = RoundedRect(bounds, RoundedContextMenuStrip.ItemHighlightRadius))
                g.FillPath(brush, path);

            g.SmoothingMode = oldHint;
        }

        // ShowCheckMargin reserves space for the checkmark, but the default
        // renderer draws almost nothing visible against a flat, image-margin-less
        // menu. Paint an explicit, theme-accented checkmark so toggled items
        // (active profile, Force Touch, Require pressure, Force Tap action) are
        // actually distinguishable from unchecked ones.
        protected override void OnRenderItemCheck(Forms.ToolStripItemImageRenderEventArgs e)
        {
            if (e.Item is not Forms.ToolStripMenuItem { Checked: true })
                return;

            var checkBounds = e.ImageRectangle;
            if (checkBounds.Width <= 0)
                checkBounds = new System.Drawing.Rectangle(0, 0, 20, e.Item.Height);

            // ToolStripItemImageRenderEventArgs has no TextRectangle.
            // ImageRectangle is the reserved check/image column, so keep the
            // checkmark at its right edge (closest to the label) and center it
            // against the actual menu row height.
            // Keep the checkmark comfortably larger than the old 14px cap.
            // WinForms scales the menu row itself with DPI, while this glyph is
            // rendered directly by GDI+, so use the row height as the primary
            // constraint and allow a larger 18px target on normal menu rows.
            int size = Math.Min(18, Math.Max(12, e.Item.Height - 6));
            const int rightGap = 2;
            int x = Math.Max(checkBounds.X, checkBounds.Right - rightGap - size);
            int y = Math.Max(0, (e.Item.Height - size) / 2);
            var box = new System.Drawing.Rectangle(x, y, size, size);

            using var accentBrush = new System.Drawing.SolidBrush(_accent);
            using var pen = new System.Drawing.Pen(System.Drawing.Color.White, 2.0f)
            {
                StartCap = System.Drawing.Drawing2D.LineCap.Round,
                EndCap = System.Drawing.Drawing2D.LineCap.Round,
                LineJoin = System.Drawing.Drawing2D.LineJoin.Round
            };

            var g = e.Graphics;
            var oldHint = g.SmoothingMode;
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            using (var path = RoundedRect(box, 4))
                g.FillPath(accentBrush, path);

            var p1 = new System.Drawing.PointF(box.X + box.Width * 0.22f, box.Y + box.Height * 0.55f);
            var p2 = new System.Drawing.PointF(box.X + box.Width * 0.42f, box.Y + box.Height * 0.75f);
            var p3 = new System.Drawing.PointF(box.X + box.Width * 0.80f, box.Y + box.Height * 0.28f);
            g.DrawLines(pen, new[] { p1, p2, p3 });
            g.SmoothingMode = oldHint;
        }

        // The base ToolStripProfessionalRenderer computes its text rectangle
        // from the item's Height and Padding, but with ShowCheckMargin
        // enabled and no image margin, that computed rectangle ends up
        // shorter than the item's actual row height - so the default
        // vertical-center flag centers the text within a rectangle that
        // doesn't reach the row's true bottom edge, and the text reads as
        // pinned toward the top instead of centered. Draw it ourselves
        // against the full item height so it centers correctly every time.
        protected override void OnRenderItemText(Forms.ToolStripItemTextRenderEventArgs e)
        {
            var bounds = new System.Drawing.Rectangle(
                e.TextRectangle.X, 0,
                e.TextRectangle.Width, e.Item.Height);

            var flags = Forms.TextFormatFlags.Left
                        | Forms.TextFormatFlags.VerticalCenter
                        | Forms.TextFormatFlags.EndEllipsis
                        | Forms.TextFormatFlags.SingleLine;

            Forms.TextRenderer.DrawText(e.Graphics, e.Text, e.TextFont, bounds, e.TextColor, flags);
        }

        private static System.Drawing.Drawing2D.GraphicsPath RoundedRect(System.Drawing.Rectangle bounds, int radius)
        {
            int d = radius * 2;
            var path = new System.Drawing.Drawing2D.GraphicsPath();
            path.AddArc(bounds.X, bounds.Y, d, d, 180, 90);
            path.AddArc(bounds.Right - d, bounds.Y, d, d, 270, 90);
            path.AddArc(bounds.Right - d, bounds.Bottom - d, d, d, 0, 90);
            path.AddArc(bounds.X, bounds.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }
    }

}