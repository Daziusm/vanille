using System;
using System.Drawing;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;
using Chocola;

namespace WindowsFormsApp2
{
    partial class Form1
    {
        enum LoaderMode
        {
            Roblox,
            SourceCode
        }

        LoaderMode _mode = LoaderMode.Roblox;
        readonly LoaderSettings _settings = LoaderSettings.Load();

        Panel _gameListPanel;
        Panel _robloxRow;
        Panel _sourceRow;
        PictureBox _robloxIcon;
        PictureBox _folderIcon;
        Label _robloxTitle;
        Label _robloxPid;
        Label _sourceTitle;
        Label _sourceSubtitle;
        Panel _robloxIndicator;
        Panel _sourceIndicator;
        Label _sourcePathLabel;
        TextBox _sourcePathBox;
        Button _browseSourceButton;

        void SetupVanilleUi()
        {
            VanilleTheme.HideLegacyGradientBar(pictureBox1);
            VanilleTheme.ApplyRainbowStrip(this);
            BuildGameRows();
            BuildSourcePathControls();
            FixControlOrder();
            WireWindowDrag();
            UpdateModeUi();
            RefreshRobloxRow();
        }

        void BuildGameRows()
        {
            pictureBox4.Visible = false;
            pictureBox4.BackgroundImage = null;

            _gameListPanel = new Panel
            {
                Location = pictureBox4.Location,
                Size = pictureBox4.Size,
                BackColor = Color.Transparent
            };
            Controls.Add(_gameListPanel);

            var robloxImage = LoadRobloxIcon();

            _robloxRow = CreateRowPanel(6, 10, _gameListPanel.Width - 12, 48);
            _robloxIndicator = CreateIndicator(0, 6);
            _robloxIcon = new PictureBox
            {
                Location = new Point(8, 6),
                Size = new Size(32, 32),
                SizeMode = PictureBoxSizeMode.Zoom,
                Image = robloxImage,
                BackColor = Color.Transparent
            };
            _robloxTitle = CreateRowTitle("Roblox", 48, 8);
            _robloxPid = CreateRowSubtitle("PID —", 48, 26);
            _robloxRow.Controls.AddRange(new Control[] { _robloxIndicator, _robloxIcon, _robloxTitle, _robloxPid });
            WireRowSelection(_robloxRow, LoaderMode.Roblox);

            _sourceRow = CreateRowPanel(6, 62, _gameListPanel.Width - 12, 48);
            _sourceIndicator = CreateIndicator(0, 6);
            _folderIcon = new PictureBox
            {
                Location = new Point(8, 6),
                Size = new Size(32, 32),
                SizeMode = PictureBoxSizeMode.Zoom,
                Image = VanilleTheme.CreateFolderIcon(32),
                BackColor = Color.Transparent
            };
            _sourceTitle = CreateRowTitle("Source Code", 48, 8);
            _sourceSubtitle = CreateRowSubtitle("GitHub repository", 48, 26);
            _sourceRow.Controls.AddRange(new Control[] { _sourceIndicator, _folderIcon, _sourceTitle, _sourceSubtitle });
            WireRowSelection(_sourceRow, LoaderMode.SourceCode);

            _gameListPanel.Controls.Add(_sourceRow);
            _gameListPanel.Controls.Add(_robloxRow);
        }

        void BuildSourcePathControls()
        {
            _sourcePathLabel = new Label
            {
                AutoSize = true,
                Location = new Point(315, 142),
                Text = "Download to:",
                Font = new Font("Verdana", 6.5F),
                ForeColor = VanilleTheme.MutedText,
                BackColor = Color.Transparent,
                Visible = false
            };

            _sourcePathBox = new TextBox
            {
                Location = new Point(315, 156),
                Size = new Size(138, 20),
                BackColor = VanilleTheme.PanelDark,
                ForeColor = VanilleTheme.Accent,
                BorderStyle = BorderStyle.FixedSingle,
                Font = new Font("Verdana", 7F),
                Text = _settings.SourcePath,
                Visible = false
            };

            _browseSourceButton = new Button
            {
                Location = new Point(456, 155),
                Size = new Size(30, 22),
                Text = "...",
                FlatStyle = FlatStyle.Flat,
                BackColor = VanilleTheme.Panel,
                ForeColor = VanilleTheme.Accent,
                Visible = false
            };
            _browseSourceButton.FlatAppearance.BorderColor = VanilleTheme.AccentMuted;
            _browseSourceButton.Click += BrowseSourceButton_Click;

            Controls.Add(_sourcePathLabel);
            Controls.Add(_sourcePathBox);
            Controls.Add(_browseSourceButton);
        }

        void FixControlOrder()
        {
            panel1.SendToBack();
            _gameListPanel.BringToFront();
            pictureBox6.BringToFront();
            button2.BringToFront();
            button1.BringToFront();
            _sourcePathLabel.BringToFront();
            _sourcePathBox.BringToFront();
            _browseSourceButton.BringToFront();
            label5.BringToFront();
            label6.BringToFront();
            label7.BringToFront();
            label8.BringToFront();
            label9.BringToFront();
            SendRainbowBarBehindContent();
        }

        void SendRainbowBarBehindContent()
        {
            var bar = Controls["vanilleRainbow"];
            if (bar != null)
                bar.SendToBack();
        }

        void WireWindowDrag()
        {
            WireDrag(_gameListPanel);
            WireDrag(pictureBox6);
            WireDrag(pictureBox3);
        }

        void WireDrag(Control control)
        {
            control.MouseDown += panel1_MouseDown;
            control.MouseMove += panel1_MouseMove;
            control.MouseUp += panel1_MouseUp;
        }

        void WireRowSelection(Control row, LoaderMode mode)
        {
            row.Click += (_, __) => SelectMode(mode);
            row.Cursor = Cursors.Hand;
            foreach (Control child in row.Controls)
            {
                child.Click += (_, __) => SelectMode(mode);
                child.Cursor = Cursors.Hand;
            }
        }

        static Panel CreateRowPanel(int x, int y, int w, int h)
        {
            return new Panel
            {
                Location = new Point(x, y),
                Size = new Size(w, h),
                BackColor = Color.Transparent
            };
        }

        static Panel CreateIndicator(int x, int y)
        {
            return new Panel
            {
                Location = new Point(x, y),
                Size = new Size(3, 32),
                BackColor = Color.Transparent
            };
        }

        static Label CreateRowTitle(string text, int x, int y)
        {
            return new Label
            {
                AutoSize = true,
                Location = new Point(x, y),
                Text = text,
                Font = new Font("Verdana", 8.25F, FontStyle.Bold),
                ForeColor = Color.Gainsboro,
                BackColor = Color.Transparent
            };
        }

        static Label CreateRowSubtitle(string text, int x, int y)
        {
            return new Label
            {
                AutoSize = true,
                Location = new Point(x, y),
                Text = text,
                Font = new Font("Verdana", 7F),
                ForeColor = VanilleTheme.MutedText,
                BackColor = Color.Transparent
            };
        }

        static Image LoadRobloxIcon()
        {
            var paths = new[]
            {
                Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Resources", "roblox.png"),
                Path.Combine(Application.StartupPath, "Resources", "roblox.png")
            };

            foreach (var path in paths)
            {
                if (!File.Exists(path))
                    continue;
                return Image.FromFile(path);
            }

            return VanilleTheme.CreateFolderIcon(32);
        }

        void SelectMode(LoaderMode mode)
        {
            _mode = mode;
            UpdateModeUi();
            RefreshStatusLabels();
        }

        void UpdateModeUi()
        {
            var robloxSelected = _mode == LoaderMode.Roblox;
            _robloxIndicator.BackColor = robloxSelected ? VanilleTheme.Accent : Color.Transparent;
            _sourceIndicator.BackColor = robloxSelected ? Color.Transparent : VanilleTheme.Accent;
            _robloxTitle.ForeColor = robloxSelected ? VanilleTheme.Accent : Color.Gainsboro;
            _sourceTitle.ForeColor = robloxSelected ? Color.Gainsboro : VanilleTheme.Accent;

            var showSource = !robloxSelected;
            _sourcePathLabel.Visible = showSource;
            _sourcePathBox.Visible = showSource;
            _browseSourceButton.Visible = showSource;
        }

        void RefreshRobloxRow()
        {
            var pid = RobloxService.GetPrimaryPid();
            _robloxPid.Text = pid.HasValue ? "PID " + pid.Value : "Not running";
        }

        void RefreshStatusLabels()
        {
            RefreshRobloxRow();

            if (_mode == LoaderMode.Roblox)
            {
                label7.Text = RobloxService.IsRobloxRunning() ? "Added Roblox" : "Waiting for Roblox...";
                label8.Text = RobloxService.IsRobloxRunning()
                    ? "Ready to load"
                    : "Warning: Open Roblox before loading";
            }
            else
            {
                label7.Text = "Source install selected";
                label8.Text = "Download to: " + ShortenPath(_sourcePathBox.Text);
            }

            label9.Text = "Vanille loader";
        }

        static string ShortenPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return "(pick a folder)";
            return path.Length <= 42 ? path : "..." + path.Substring(path.Length - 39);
        }

        void BrowseSourceButton_Click(object sender, EventArgs e)
        {
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = "Choose where to download source code";
                dialog.SelectedPath = Directory.Exists(_sourcePathBox.Text)
                    ? _sourcePathBox.Text
                    : LoaderSettings.DefaultSourcePath();
                if (dialog.ShowDialog(this) == DialogResult.OK)
                {
                    _sourcePathBox.Text = dialog.SelectedPath;
                    _settings.SourcePath = dialog.SelectedPath;
                    _settings.Save();
                    RefreshStatusLabels();
                }
            }
        }

        async Task DownloadSourcesAsync()
        {
            var destination = _sourcePathBox.Text.Trim();
            if (string.IsNullOrWhiteSpace(destination))
            {
                BrowseSourceButton_Click(this, EventArgs.Empty);
                destination = _sourcePathBox.Text.Trim();
                if (string.IsNullOrWhiteSpace(destination))
                    return;
            }

            _settings.SourcePath = destination;
            _settings.Save();

            label8.Text = "Downloading sources...";
            label8.ForeColor = VanilleTheme.Accent;

            var result = await SourceDownloader.InstallAsync(
                _settings.RepoUrl,
                _settings.Branch,
                destination,
                msg => label8.Text = msg);

            if (!result.Item1)
            {
                MessageBox.Show(result.Item2 ?? "Download failed.", "Vanille", MessageBoxButtons.OK, MessageBoxIcon.Error);
                label8.Text = "Source download failed";
                return;
            }

            label8.Text = "Sources installed to " + ShortenPath(destination);
            label7.Text = "Source Code ready";
        }
    }
}
