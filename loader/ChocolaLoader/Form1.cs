using System;
using System.Data;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Chocola;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace WindowsFormsApp2
{
    public partial class Form1 : Form
    {
        public string question,
            info;

        [DllImport("Gdi32.dll", EntryPoint = "CreateRoundRectRgn")]
        private static extern IntPtr CreateRoundRectRgn(
            int nLeftRect,
            int nTopRect,
            int nRightRect,
            int nBottomRect,
            int nWidthEllipse,
            int nHeightEllipse
        );

        public Form1()
        {
            InitializeComponent();
            VanilleTheme.ApplyWindowIcon(this);
            Region = System.Drawing.Region.FromHrgn(CreateRoundRectRgn(0, 0, Width, Height, 7, 7));
this.Text = "Vanille";
            SetupVanilleUi();
        }

        private void Form1_Load(object sender, EventArgs e)
        {
            label5.Text = "Connected";
            label6.Text = "Welcome back";
            RefreshStatusLabels();
            ApplyAccent(label7);
            FixControlOrder();
        }

        static void ApplyVanilleGradient(PictureBox box)
        {
            VanilleTheme.HideLegacyGradientBar(box);
        }

        private void ApplyAccent(Label label)
        {
            label.BackColor = VanilleTheme.PanelDark;
            label.ForeColor = VanilleTheme.Accent;
            label.Font = new Font("Verdana", 6.25F, FontStyle.Bold);
        }

        private void checkonline() { }

        private void pictureBox3_Click(object sender, EventArgs e) { }

        private void pictureBox2_Click(object sender, EventArgs e) { }

        private void pictureBox2_Click_1(object sender, EventArgs e) { }

        private void groupBox1_Enter(object sender, EventArgs e) { }

        private void label3_Click(object sender, EventArgs e) { }

        private void label4_Click(object sender, EventArgs e) { }

        private void pictureBox4_Click(object sender, EventArgs e) { }

        private void pictureBox5_Click(object sender, EventArgs e) { }

        private void pictureBox6_Click(object sender, EventArgs e) { }

        private async void button2_Click(object sender, EventArgs e)
        {
            if (_mode == LoaderMode.SourceCode)
            {
                await DownloadSourcesAsync();
                return;
            }

            if (!RobloxService.IsRobloxRunning())
            {
                MessageBox.Show(
                    "Roblox not found. Open Roblox before loading.",
                    "Vanille",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning);
                return;
            }

            this.Hide();
            var loading = new SimpleLoader.Form5();
            loading.Closed += (_, __) => this.Close();
            loading.Show();
        }

        private void button1_Click(object sender, EventArgs e) { }

        private void label8_Click_1(object sender, EventArgs e) { }

        private void pictureBox8_Click(object sender, EventArgs e) { }

        private void label7_Click_1(object sender, EventArgs e) { }

        private void label6_Click_1(object sender, EventArgs e) { }

        private void label5_Click_2(object sender, EventArgs e) { }

        private void button1_Click_1(object sender, EventArgs e)
        {
            Application.Exit();
        }

        private void pictureBox3_Click_1(object sender, EventArgs e) { }

        private void label1_Click(object sender, EventArgs e) { }

        private void label2_Click(object sender, EventArgs e) { }

        private void textBox1_TextChanged(object sender, EventArgs e) { }

        private void pictureBox1_Click(object sender, EventArgs e) { }

        private void pictureBox7_Click(object sender, EventArgs e) { }

        private void timer1_Tick_1(object sender, EventArgs e)
        {
            RefreshStatusLabels();
        }

        private void label9_Click(object sender, EventArgs e) { }

        private void button3_Click(object sender, EventArgs e)
        {
            this.Hide();
            var form1 = new SimpleLoader.Form5();
            form1.Closed += (s, args) => this.Close();
            form1.Show();
        }

        private void label5_Click(object sender, EventArgs e)
        {
            // label5
            this.pictureBox8.Location = new System.Drawing.Point(41, 177);
            this.label5.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(26)))),
                ((int)(((byte)(26)))),
                ((int)(((byte)(26))))
            );
            this.label5.ForeColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(255)))),
                ((int)(((byte)(250)))),
                ((int)(((byte)(238))))
            );
            this.label5.Font = new System.Drawing.Font(
                "Verdana",
                6.25F,
                System.Drawing.FontStyle.Bold
            );
            // lb7
            this.label7.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label7.ForeColor = System.Drawing.Color.Gainsboro;
            this.label7.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb6
            this.label6.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label6.ForeColor = System.Drawing.Color.Gainsboro;
            this.label6.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb8
            this.label8.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label8.ForeColor = System.Drawing.Color.Gainsboro;
            this.label8.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb9
            this.label9.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label9.ForeColor = System.Drawing.Color.Gainsboro;
            this.label9.Font = new System.Drawing.Font("Verdana", 6.25F);
        }

        private void label6_Click(object sender, EventArgs e)
        {
            // label6
            this.pictureBox8.Location = new System.Drawing.Point(41, 192);
            this.label6.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(26)))),
                ((int)(((byte)(26)))),
                ((int)(((byte)(26))))
            );
            this.label6.ForeColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(255)))), ((int)(((byte)(250)))), ((int)(((byte)(238))))
            );
            this.label6.Font = new System.Drawing.Font(
                "Verdana",
                6.25F,
                System.Drawing.FontStyle.Bold
            );
            // lb7
            this.label7.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label7.ForeColor = System.Drawing.Color.Gainsboro;
            this.label7.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb5
            this.label5.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label5.ForeColor = System.Drawing.Color.Gainsboro;
            this.label5.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb8
            this.label8.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label8.ForeColor = System.Drawing.Color.Gainsboro;
            this.label8.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb9
            this.label9.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label9.ForeColor = System.Drawing.Color.Gainsboro;
            this.label9.Font = new System.Drawing.Font("Verdana", 6.25F);
        }

        private void label7_Click(object sender, EventArgs e)
        {
            // label7
            this.pictureBox8.Location = new System.Drawing.Point(41, 207);
            this.label7.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(26)))),
                ((int)(((byte)(26)))),
                ((int)(((byte)(26))))
            );
            this.label7.ForeColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(255)))), ((int)(((byte)(250)))), ((int)(((byte)(238))))
            );
            this.label7.Font = new System.Drawing.Font(
                "Verdana",
                6.25F,
                System.Drawing.FontStyle.Bold
            );
            // lb8
            this.label8.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label8.ForeColor = System.Drawing.Color.Gainsboro;
            this.label8.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb6
            this.label6.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label6.ForeColor = System.Drawing.Color.Gainsboro;
            this.label6.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb5
            this.label5.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label5.ForeColor = System.Drawing.Color.Gainsboro;
            this.label5.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb9
            this.label9.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label9.ForeColor = System.Drawing.Color.Gainsboro;
            this.label9.Font = new System.Drawing.Font("Verdana", 6.25F);
        }

        private void pictureBox8_Click_1(object sender, EventArgs e) { }

        private void label9_Click_1(object sender, EventArgs e)
        {
            this.pictureBox8.Location = new System.Drawing.Point(41, 222);
            this.label9.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(26)))),
                ((int)(((byte)(26)))),
                ((int)(((byte)(26))))
            );
            this.label9.ForeColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(255)))), ((int)(((byte)(250)))), ((int)(((byte)(238))))
            );
            this.label9.Font = new System.Drawing.Font(
                "Verdana",
                6.25F,
                System.Drawing.FontStyle.Bold
            );
            // lb7
            this.label7.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label7.ForeColor = System.Drawing.Color.Gainsboro;
            this.label7.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb6
            this.label6.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label6.ForeColor = System.Drawing.Color.Gainsboro;
            this.label6.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb5
            this.label5.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label5.ForeColor = System.Drawing.Color.Gainsboro;
            this.label5.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb8
            this.label8.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label8.ForeColor = System.Drawing.Color.Gainsboro;
            this.label8.Font = new System.Drawing.Font("Verdana", 6.25F);
        }

        private void label8_Click(object sender, EventArgs e)
        {
            // label8
            this.pictureBox8.Location = new System.Drawing.Point(41, 238);
            this.label8.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(26)))),
                ((int)(((byte)(26)))),
                ((int)(((byte)(26))))
            );
            this.label8.ForeColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(255)))), ((int)(((byte)(250)))), ((int)(((byte)(238))))
            );
            this.label8.Font = new System.Drawing.Font(
                "Verdana",
                6.25F,
                System.Drawing.FontStyle.Bold
            );
            // lb7
            this.label7.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label7.ForeColor = System.Drawing.Color.Gainsboro;
            this.label7.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb6
            this.label6.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label6.ForeColor = System.Drawing.Color.Gainsboro;
            this.label6.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb5
            this.label5.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label5.ForeColor = System.Drawing.Color.Gainsboro;
            this.label5.Font = new System.Drawing.Font("Verdana", 6.25F);
            // lb9
            this.label9.BackColor = System.Drawing.Color.FromArgb(
                ((int)(((byte)(35)))),
                ((int)(((byte)(35)))),
                ((int)(((byte)(35))))
            );
            this.label9.ForeColor = System.Drawing.Color.Gainsboro;
            this.label9.Font = new System.Drawing.Font("Verdana", 6.25F);
        }

        private void button4_Click(object sender, EventArgs e)
        {
            this.Hide();
            Form1 frm = new Form1();
            frm.Show();
        }

        int mouseX = 0,
            mouseY = 0;
        bool mouseDown;

        private void panel1_MouseMove(object sender, MouseEventArgs e)
        {
            if (mouseDown)
            {
                mouseX = MousePosition.X - 200;
                mouseY = MousePosition.Y - 40;
                this.SetDesktopLocation(mouseX, mouseY);
            }
        }

        private void panel1_MouseUp(object sender, MouseEventArgs e)
        {
            mouseDown = false;
        }

        private void panel1_MouseDown(object sender, MouseEventArgs e)
        {
            mouseDown = true;
        }

        private void button3_Click_1(object sender, EventArgs e)
        {
            this.Hide();
            Form1 frm = new Form1();
            frm.Show();
        }
    }
}
