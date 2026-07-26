using System;
using System.Drawing;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.Runtime.InteropServices;
using Chocola;
using WindowsFormsApp2;

namespace SimpleLoader
{
    public partial class Form4 : Form
    {
        const int SplashDurationMs = 1800;

        readonly Timer _showTimer;

        readonly PictureBox _sprite;
        readonly Label _loadingLabel;
        readonly Timer _loadingTimer;
        int _loadingPhase;
        float _pulse;

        [DllImport("Gdi32.dll", EntryPoint = "CreateRoundRectRgn")]
        static extern IntPtr CreateRoundRectRgn(
            int nLeftRect,
            int nTopRect,
            int nRightRect,
            int nBottomRect,
            int nWidthEllipse,
            int nHeightEllipse);

        public Form4()
        {
            InitializeComponent();
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            UpdateStyles();

            VanilleTheme.ApplyWindowIcon(this);
            Text = "Vanille";
            BackColor = Color.FromArgb(14, 14, 14);
            Opacity = 0.94;
            ClientSize = new Size(640, 180);
            StartPosition = FormStartPosition.CenterScreen;
            TopMost = true;
            Region = Region.FromHrgn(CreateRoundRectRgn(0, 0, Width, Height, 12, 12));

            _showTimer = new Timer { Interval = 10 };
            _showTimer.Tick += (_, __) => { Opacity = Math.Min(0.94, Opacity + 0.08); };

            _sprite = new PictureBox
            {
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.Transparent,
                Size = new Size(112, 112),
                Location = new Point(36, (ClientSize.Height - 112) / 2)
            };
            TryLoadSprite(_sprite);

            _loadingLabel = new Label
            {
                AutoSize = false,
                TextAlign = ContentAlignment.MiddleLeft,
                Font = new Font("Segoe UI Semibold", 18f, FontStyle.Regular),
                ForeColor = VanilleTheme.Accent,
                BackColor = Color.Transparent,
                Location = new Point(176, 0),
                Size = new Size(ClientSize.Width - 196, ClientSize.Height),
                Text = "Loading"
            };

            _loadingTimer = new Timer { Interval = 140 };
            _loadingTimer.Tick += OnLoadingTick;

            Controls.Add(_sprite);
            Controls.Add(_loadingLabel);
        }

        static void TryLoadSprite(PictureBox target)
        {
            try
            {
                var baseDir = AppDomain.CurrentDomain.BaseDirectory;
                var candidates = new[]
                {
                    Path.Combine(baseDir, "Resources", "splash_sprite.png"),
                    Path.Combine(baseDir, "splash_sprite.png")
                };

                foreach (var path in candidates)
                {
                    if (!File.Exists(path))
                        continue;

                    using (var stream = File.OpenRead(path))
                    {
                        target.Image = Image.FromStream(stream);
                    }
                    return;
                }
            }
            catch
            {
                // Splash still works without the sprite.
            }
        }

        void OnLoadingTick(object sender, EventArgs e)
        {
            _loadingPhase = (_loadingPhase + 1) % 4;
            _pulse += 0.14f;
            if (_pulse > 1f)
                _pulse = 0f;

            var dots = new string('.', _loadingPhase);
            var accentStrength = 0.55f + (0.45f * (0.5f + 0.5f * Math.Sin(_pulse * Math.PI * 2f)));
            var accent = VanilleTheme.Accent;
            var r = (int)(accent.R * accentStrength + VanilleTheme.MutedText.R * (1f - accentStrength));
            var g = (int)(accent.G * accentStrength + VanilleTheme.MutedText.G * (1f - accentStrength));
            var b = (int)(accent.B * accentStrength + VanilleTheme.MutedText.B * (1f - accentStrength));
            _loadingLabel.ForeColor = Color.FromArgb(255, r, g, b);
            _loadingLabel.Text = "Loading" + dots;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var lineY = ClientSize.Height - 1;
            using (var pen = new Pen(Color.FromArgb(50, VanilleTheme.Accent), 1f))
            {
                e.Graphics.DrawLine(pen, 20, lineY, ClientSize.Width - 20, lineY);
            }
        }

        async void Form4_Load(object sender, EventArgs e)
        {
            Opacity = 0.55;
            _showTimer.Start();
            _loadingTimer.Start();
            await Task.Delay(SplashDurationMs);
            _loadingTimer.Stop();
            _showTimer.Stop();

            Hide();
            var main = new Form1();
            main.Closed += (_, __) => Close();
            main.Show();
        }
    }
}
