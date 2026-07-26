using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace Chocola
{
    internal static class VanilleTheme
    {
        public static readonly Color Accent = Color.FromArgb(255, 250, 238);
        public static readonly Color AccentMuted = Color.FromArgb(210, 188, 140);
        public static readonly Color Panel = Color.FromArgb(35, 35, 35);
        public static readonly Color PanelDark = Color.FromArgb(26, 26, 26);
        public static readonly Color Background = Color.FromArgb(21, 21, 21);
        public static readonly Color MutedText = Color.FromArgb(180, 180, 180);

        // Original gamesense: pictureBox2 at (6, 6), width = clientWidth - 12, height = 4.
        public const int BorderInset = 6;
        public const int RainbowTop = 6;
        public const int RainbowHeight = 4;

        static readonly SolidBrush AccentBrush = new SolidBrush(Accent);

        public static void ApplyRainbowStrip(Form form, string existingBarName = null)
        {
            if (form == null)
                return;

            PatchBackgroundRainbow(form);

            var bar = FindRainbowBar(form, existingBarName);
            if (bar == null)
            {
                bar = new PictureBox { Name = "vanilleRainbow" };
                form.Controls.Add(bar);
            }

            ConfigureRainbowBar(bar, form);
            form.Resize -= OnFormResize;
            form.Resize += OnFormResize;
            form.Tag = form.Tag ?? bar;
        }

        static void OnFormResize(object sender, EventArgs e)
        {
            var form = sender as Form;
            if (form == null)
                return;

            var bar = form.Controls["vanilleRainbow"] as PictureBox
                ?? form.Controls["pictureBox2"] as PictureBox;
            if (bar != null)
                ConfigureRainbowBar(bar, form);
        }

        static PictureBox FindRainbowBar(Form form, string existingBarName)
        {
            if (!string.IsNullOrEmpty(existingBarName))
            {
                var existing = form.Controls[existingBarName] as PictureBox;
                if (existing != null)
                    return existing;
            }

            return form.Controls["vanilleRainbow"] as PictureBox;
        }

        static void ConfigureRainbowBar(PictureBox bar, Form form)
        {
            bar.BackgroundImage = null;
            bar.Image = null;
            bar.BackColor = Accent;
            bar.Location = new Point(BorderInset, RainbowTop);
            bar.Size = new Size(Math.Max(1, form.ClientSize.Width - BorderInset * 2), RainbowHeight);
            bar.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            bar.Visible = true;
        }

        static void PatchBackgroundRainbow(Form form)
        {
            if (form.BackgroundImage == null)
                return;

            var patched = new Bitmap(form.BackgroundImage);
            var rect = RainbowRectInBitmap(form, patched);
            using (var graphics = Graphics.FromImage(patched))
            {
                graphics.FillRectangle(AccentBrush, rect);
            }

            form.BackgroundImageLayout = ImageLayout.None;
            form.BackgroundImage = patched;
        }

        static Rectangle RainbowRectInBitmap(Form form, Bitmap bitmap)
        {
            var clientRect = new Rectangle(
                BorderInset,
                RainbowTop,
                Math.Max(1, form.ClientSize.Width - BorderInset * 2),
                RainbowHeight);

            float scaleX = (float)bitmap.Width / Math.Max(1, form.ClientSize.Width);
            float scaleY = (float)bitmap.Height / Math.Max(1, form.ClientSize.Height);

            var x = (int)Math.Round(clientRect.X * scaleX);
            var y = (int)Math.Round(clientRect.Y * scaleY);
            var w = (int)Math.Round(clientRect.Width * scaleX);
            var h = (int)Math.Round(clientRect.Height * scaleY);

            w = Math.Max(1, Math.Min(w, bitmap.Width - x));
            h = Math.Max(1, Math.Min(h, bitmap.Height - y));

            return new Rectangle(x, y, w, h);
        }

        public static void HideLegacyGradientBar(Control control)
        {
            if (control == null)
                return;
            control.Visible = false;
            control.BackgroundImage = null;
            if (control is PictureBox pictureBox)
                pictureBox.Image = null;
        }

        static Icon _windowIcon;

        public static void ApplyWindowIcon(Form form)
        {
            if (form == null)
                return;

            try
            {
                if (_windowIcon == null)
                    _windowIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
                if (_windowIcon != null)
                    form.Icon = (Icon)_windowIcon.Clone();
            }
            catch
            {
                // Keep designer/default icon if extraction fails.
            }
        }

        public static Bitmap CreateFolderIcon(int size)
        {
            var bitmap = new Bitmap(size, size);
            using (var g = Graphics.FromImage(bitmap))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);
                using (var brush = new SolidBrush(AccentMuted))
                {
                    g.FillRectangle(brush, size / 5, size / 3, size - size / 4, size - size / 3);
                    g.FillRectangle(brush, size / 5, size / 4, size / 2, size / 6);
                }
            }
            return bitmap;
        }
    }
}
