using System;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Chocola;

namespace SimpleLoader
{
    public partial class Form2 : Form
    {
        public string question,
            info;

        readonly LoaderSettings _settings = LoaderSettings.Load();

        [System.Runtime.InteropServices.DllImport("Gdi32.dll", EntryPoint = "CreateRoundRectRgn")]
        private static extern IntPtr CreateRoundRectRgn(
            int nLeftRect,
            int nTopRect,
            int nRightRect,
            int nBottomRect,
            int nWidthEllipse,
            int nHeightEllipse
        );

        public Form2()
        {
            InitializeComponent();
            VanilleTheme.ApplyWindowIcon(this);
            VanilleTheme.ApplyRainbowStrip(this, "pictureBox2");
            Region = System.Drawing.Region.FromHrgn(CreateRoundRectRgn(0, 0, Width, Height, 7, 7));
this.Text = "Vanille";
        }

        private void label4_Click(object sender, EventArgs e) { }

        private void label4_Click_1(object sender, EventArgs e) { }

        private void Form2_Load(object sender, EventArgs e)
        {
            timer2.Start();
        }

        int r = 219;
        int g = 219;
        int b = 219;
        bool inv = true;
        private void timer1_Tick(object sender, EventArgs e)
        {
            if (r == 15 && g == 15 && b == 15)
                inv = false;
            if (r == 219 && g == 219 && b == 219)
                inv = true;
            if (inv)
            {
                r -= 2;
                g -= 2;
                b -= 2;
            }
            else
            {
                r += 2;
                g += 2;
                b += 2;
            }

            label4.ForeColor = Color.FromArgb(r, g, b);
        }

        private void label4_Click_2(object sender, EventArgs e) { }

        private void label4_MouseMove(object sender, MouseEventArgs e) { }

        private void next_Click(object sender, EventArgs e)
        {
            Application.Exit();
        }

        private void pictureBox1_Click(object sender, EventArgs e) { }

        private async void timer2_Tick(object sender, EventArgs e)
        {
            timer2.Stop();
            label4.Text = "Installing Vanille...";

            string error;
            if (!PayloadService.EnsureInstalled(_settings.InstallPath, msg => label4.Text = msg, out error))
            {
                MessageBox.Show(error ?? "Install failed.", "Vanille", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Application.Exit();
                return;
            }

            if (!OffsetUpdater.TryRefresh(_settings.InstallPath, _settings, msg => label4.Text = msg, out error))
            {
                MessageBox.Show(error ?? "Could not update offsets.", "Vanille", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Application.Exit();
                return;
            }

            if (!PayloadService.LaunchVanille(_settings.InstallPath, msg => label4.Text = msg, out error))
            {
                MessageBox.Show(error ?? "Launch failed.", "Vanille", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Application.Exit();
                return;
            }

            label4.ForeColor = VanilleTheme.Accent;
            label4.Text = "Vanille launched.";
            await Task.Delay(1200);
            Application.Exit();
        }
    }
}
