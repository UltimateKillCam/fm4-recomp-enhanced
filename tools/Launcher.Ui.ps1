# FM4 launcher look and control bindings. Modified 2026-10-20. GPL-3.0-only.
# Shared by the Wheel and Enhanced launchers (Windows PowerShell 5.1 and PowerShell 7).
# - A dark, Forza Motorsport 4 inspired shell: header, left navigation, borderless pages
#   and controller-style action prompts (original artwork only; no game assets).
# - A Controls page that binds every action from live input. Each axis, button and the
#   H-pattern shifter can come from a different device (wheelbase, rim, pedals, handbrake,
#   shifter, button box). Live input comes from fm4-input.exe, a read-only DirectInput
#   reader that enumerates devices exactly like the game, so bindings match what it reads.

Add-Type -AssemblyName System.Windows.Forms, System.Drawing

if (-not ('Fm4Ui.PageHost' -as [type])) {
    $fm4UiSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace Fm4Ui {
  public static class Palette {
    public static readonly Color Back = Color.FromArgb(11, 12, 14);
    public static readonly Color Panel = Color.FromArgb(21, 22, 25);
    public static readonly Color Raised = Color.FromArgb(31, 33, 37);
    public static readonly Color Field = Color.FromArgb(42, 44, 49);
    public static readonly Color Line = Color.FromArgb(62, 64, 70);
    public static readonly Color Text = Color.FromArgb(236, 236, 236);
    public static readonly Color Dim = Color.FromArgb(152, 155, 162);
    public static readonly Color Faint = Color.FromArgb(34, 36, 40);
    public static readonly Color Accent = Color.FromArgb(214, 28, 40);
    public static readonly Color AccentDark = Color.FromArgb(120, 14, 20);
    public static string Body = "Segoe UI";
    public static string Heading = "Segoe UI Semibold";
    public static Font Make(float size, bool heading) {
      // Small text in the condensed heading font loses all smoothing, so it uses Segoe UI Semibold.
      string family = heading ? (size < 13f ? "Segoe UI Semibold" : Heading) : Body;
      return new Font(family, size, FontStyle.Regular, GraphicsUnit.Point);
    }
    // GDI draws variable fonts such as Bahnschrift without any anti-aliasing, so headings
    // in those fonts are drawn with GDI+; everything else keeps GDI ClearType text.
    public static bool NeedsSmoothing(Font f) { return f != null && f.Name.StartsWith("Bahnschrift", StringComparison.OrdinalIgnoreCase); }
    public static void Draw(IDeviceContext dc, string s, Font f, Rectangle r, Color c, TextFormatFlags flags) {
      Graphics g = dc as Graphics;
      if (g == null || !NeedsSmoothing(f)) { TextRenderer.DrawText(dc, s, f, r, c, flags); return; }
      TextRenderingHint old = g.TextRenderingHint;
      g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
      using (SolidBrush b = new SolidBrush(c))
      using (StringFormat sf = new StringFormat(StringFormatFlags.NoWrap)) {
        sf.Alignment = (flags & TextFormatFlags.HorizontalCenter) != 0 ? StringAlignment.Center :
            (flags & TextFormatFlags.Right) != 0 ? StringAlignment.Far : StringAlignment.Near;
        sf.LineAlignment = (flags & TextFormatFlags.VerticalCenter) != 0 ? StringAlignment.Center :
            (flags & TextFormatFlags.Bottom) != 0 ? StringAlignment.Far : StringAlignment.Near;
        if ((flags & TextFormatFlags.EndEllipsis) != 0) sf.Trimming = StringTrimming.EllipsisCharacter;
        g.DrawString(s, f, b, r, sf);
      }
      g.TextRenderingHint = old;
    }
    public static void Draw(IDeviceContext dc, string s, Font f, Point at, Color c, TextFormatFlags flags) {
      Draw(dc, s, f, new Rectangle(at, new Size(4000, 400)), c, flags);
    }
  }

  // TabControl without its tab strip or border; navigation buttons select the pages.
  public class PageHost : TabControl {
    protected override void WndProc(ref Message m) {
      if (m.Msg == 0x1328 && !DesignMode) { m.Result = (IntPtr)1; return; }
      base.WndProc(ref m);
    }
  }

  public class NavButton : Control {
    bool selected, hover;
    public NavButton() {
      SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
               ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
      Cursor = Cursors.Hand; Font = Palette.Make(14f, true);
    }
    public bool Selected { get { return selected; } set { selected = value; Invalidate(); } }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e) {
      Graphics g = e.Graphics;
      Color back = selected ? Palette.Accent : (hover ? Palette.Field : Palette.Raised);
      using (SolidBrush b = new SolidBrush(back)) g.FillRectangle(b, ClientRectangle);
      if (selected) using (SolidBrush w = new SolidBrush(Color.White)) g.FillRectangle(w, 0, 0, 5, Height);
      Palette.Draw(g, (Text ?? "").ToUpperInvariant(), Font, new Rectangle(20, 0, Width - 26, Height),
          selected ? Color.White : (hover ? Palette.Text : Palette.Dim),
          TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
    }
  }

  public class Header : Control {
    public string Title = "FORZA MOTORSPORT 4";
    public string Subtitle = "";
    public string Badge = "";
    public Header() {
      SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
               ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
    }
    protected override void OnPaint(PaintEventArgs e) {
      Graphics g = e.Graphics;
      g.SmoothingMode = SmoothingMode.AntiAlias;
      using (LinearGradientBrush b = new LinearGradientBrush(ClientRectangle, Palette.Panel, Palette.Back, 90f))
        g.FillRectangle(b, ClientRectangle);
      // Faint diagonal speed lines on the right, drawn rather than copied from any game asset.
      using (Pen p = new Pen(Color.FromArgb(18, 255, 255, 255), 10f))
        for (int x = Width - 520; x < Width + 80; x += 34) g.DrawLine(p, x, Height, x + 60, 0);
      using (Font title = Palette.Make(26f, true))
        Palette.Draw(g, Title, title, new Point(26, 10), Palette.Text, TextFormatFlags.NoPadding);
      using (Font sub = Palette.Make(11f, false))
        Palette.Draw(g, Subtitle.ToUpperInvariant(), sub, new Point(29, 58), Palette.Dim, TextFormatFlags.NoPadding);
      using (SolidBrush red = new SolidBrush(Palette.Accent)) g.FillRectangle(red, 28, Height - 6, 96, 3);
      using (Pen line = new Pen(Palette.Line)) g.DrawLine(line, 0, Height - 1, Width, Height - 1);
      if (!string.IsNullOrEmpty(Badge)) {
        using (Font badge = Palette.Make(15f, true)) {
          Size size = TextRenderer.MeasureText(Badge.ToUpperInvariant(), badge);
          int w = size.Width + 44, x = Width - w - 24, y = 22, h = 40;
          Point[] shape = { new Point(x + 14, y), new Point(x + w, y), new Point(x + w, y + h), new Point(x, y + h) };
          using (SolidBrush red = new SolidBrush(Palette.Accent)) g.FillPolygon(red, shape);
          Palette.Draw(g, Badge.ToUpperInvariant(), badge, new Rectangle(x + 8, y, w - 8, h), Color.White,
              TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
        }
      }
    }
  }

  // Controller-style prompt: coloured face button glyph plus label.
  public class PromptButton : Control {
    public char Glyph = 'A';
    public Color GlyphColor = Color.FromArgb(96, 176, 56);
    bool hover, down;
    public PromptButton() {
      SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
               ControlStyles.UserPaint | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
      Cursor = Cursors.Hand; Font = Palette.Make(13f, true);
    }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnPaint(PaintEventArgs e) {
      Graphics g = e.Graphics;
      g.SmoothingMode = SmoothingMode.AntiAlias;
      Color back = down ? Palette.AccentDark : (hover ? Palette.Field : Palette.Raised);
      using (SolidBrush b = new SolidBrush(back)) g.FillRectangle(b, ClientRectangle);
      using (Pen p = new Pen(hover ? Palette.Accent : Palette.Line)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
      int d = Math.Min(26, Height - 10), cy = (Height - d) / 2;
      using (SolidBrush c = new SolidBrush(Enabled ? GlyphColor : Palette.Line)) g.FillEllipse(c, 12, cy, d, d);
      using (Font glyph = Palette.Make(11f, true))
        Palette.Draw(g, Glyph.ToString(), glyph, new Rectangle(12, cy, d, d), Color.White,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
      Palette.Draw(g, (Text ?? "").ToUpperInvariant(), Font, new Rectangle(20 + d, 0, Width - 26 - d, Height),
          Enabled ? Palette.Text : Palette.Dim, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
    }
  }

  // Live input meter: centred bar for steering, 0..1 bar for pedals, lamp for buttons.
  public class InputMeter : Control {
    float value; bool centered, isButton, pressed, available = true;
    public InputMeter() {
      SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
               ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
    }
    public float Value { get { return value; } set { float v = Math.Max(-1f, Math.Min(1f, value)); if (v != this.value) { this.value = v; Invalidate(); } } }
    public bool Centered { get { return centered; } set { centered = value; Invalidate(); } }
    public bool IsButton { get { return isButton; } set { isButton = value; Invalidate(); } }
    public bool Pressed { get { return pressed; } set { if (pressed != value) { pressed = value; Invalidate(); } } }
    public bool Available { get { return available; } set { if (available != value) { available = value; Invalidate(); } } }
    protected override void OnPaint(PaintEventArgs e) {
      Graphics g = e.Graphics;
      g.SmoothingMode = SmoothingMode.AntiAlias;
      using (SolidBrush b = new SolidBrush(Parent != null ? Parent.BackColor : Palette.Panel)) g.FillRectangle(b, ClientRectangle);
      if (isButton) {
        int d = Math.Min(Width, Height) - 6;
        Rectangle r = new Rectangle((Width - d) / 2, (Height - d) / 2, d, d);
        using (SolidBrush f = new SolidBrush(available && pressed ? Palette.Accent : Palette.Field)) g.FillEllipse(f, r);
        using (Pen p = new Pen(Palette.Line)) g.DrawEllipse(p, r);
        return;
      }
      Rectangle track = new Rectangle(0, Height / 2 - 6, Width - 1, 12);
      using (SolidBrush t = new SolidBrush(Palette.Field)) g.FillRectangle(t, track);
      if (available) {
        using (SolidBrush f = new SolidBrush(Palette.Accent)) {
          if (centered) {
            int mid = track.Width / 2, len = (int)Math.Round(Math.Abs(value) * mid);
            g.FillRectangle(f, value >= 0 ? mid : mid - len, track.Y, len, track.Height);
          } else {
            g.FillRectangle(f, track.X, track.Y, (int)Math.Round(Math.Max(0f, value) * track.Width), track.Height);
          }
        }
      }
      using (Pen p = new Pen(Palette.Line)) {
        g.DrawRectangle(p, track);
        if (centered) g.DrawLine(p, track.Width / 2, track.Y - 3, track.Width / 2, track.Bottom + 3);
      }
    }
  }

  // Dark styling for stock controls that WinForms cannot colour on its own.
  public static class Styler {
    static readonly List<object> styled = new List<object>();
    static bool Mark(object control) { if (styled.Contains(control)) return false; styled.Add(control); return true; }
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    public static void DarkTitleBar(Form form) {
      form.HandleCreated += delegate {
        int on = 1;
        try { DwmSetWindowAttribute(form.Handle, 20, ref on, 4); } catch (Exception) { }
      };
    }
    public static void CheckBox(CheckBox box) {
      if (!Mark(box)) return;
      // Move the label clear of the larger custom box.
      box.Padding = new Padding(6, 0, 0, 0);
      box.Paint += delegate(object sender, PaintEventArgs e) {
        CheckBox c = (CheckBox)sender;
        Graphics g = e.Graphics;
        int s = 16, y = (c.Height - s) / 2;
        Rectangle r = new Rectangle(0, y, s, s);
        Color back = c.Parent != null ? c.Parent.BackColor : Palette.Panel;
        using (SolidBrush clear = new SolidBrush(back)) g.FillRectangle(clear, 0, Math.Max(0, y - 2), s + 1, s + 4);
        // Unavailable options keep their value but read as greyed out.
        Color fill = !c.Enabled ? Palette.Raised : (c.Checked ? Palette.Accent : Palette.Field);
        using (SolidBrush b = new SolidBrush(fill)) g.FillRectangle(b, r);
        using (Pen p = new Pen(c.Checked && c.Enabled ? Palette.Accent : Palette.Line)) g.DrawRectangle(p, r.X, r.Y, r.Width - 1, r.Height - 1);
        if (c.Checked) {
          g.SmoothingMode = SmoothingMode.AntiAlias;
          using (Pen w = new Pen(c.Enabled ? Color.White : Palette.Dim, 2.2f))
            g.DrawLines(w, new Point[] { new Point(r.X + 3, r.Y + 8), new Point(r.X + 7, r.Y + 12), new Point(r.X + 13, r.Y + 4) });
        }
      };
    }
    public static void ComboBox(ComboBox combo) {
      if (!Mark(combo)) return;
      combo.DrawMode = DrawMode.OwnerDrawFixed;
      combo.ItemHeight = Math.Max(combo.ItemHeight, 22);
      combo.DrawItem += delegate(object sender, DrawItemEventArgs e) {
        ComboBox c = (ComboBox)sender;
        bool edit = (e.State & DrawItemState.ComboBoxEdit) != 0;
        bool selected = (e.State & DrawItemState.Selected) != 0 && !edit;
        using (SolidBrush b = new SolidBrush(selected ? Palette.Accent : (c.Enabled ? Palette.Field : Palette.Raised))) e.Graphics.FillRectangle(b, e.Bounds);
        string text = e.Index >= 0 ? c.GetItemText(c.Items[e.Index]) : "";
        Rectangle bounds = new Rectangle(e.Bounds.X + 4, e.Bounds.Y, e.Bounds.Width - 6, e.Bounds.Height);
        Palette.Draw(e.Graphics, text, c.Font, bounds, c.Enabled ? Palette.Text : Palette.Dim,
            TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
      };
    }
  }

  // Large rotated caption like the game's vertical section titles.
  public class SideCaption : Control {
    public SideCaption() {
      SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
               ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
    }
    protected override void OnPaint(PaintEventArgs e) {
      Graphics g = e.Graphics;
      using (SolidBrush b = new SolidBrush(Parent != null ? Parent.BackColor : Palette.Back)) g.FillRectangle(b, ClientRectangle);
      g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
      g.TranslateTransform(0, Height);
      g.RotateTransform(-90f);
      using (Font f = Palette.Make(40f, true))
      using (SolidBrush t = new SolidBrush(Palette.Faint))
        g.DrawString((Text ?? "").ToUpperInvariant(), f, t, 8f, -8f);
    }
  }
}
'@
    $fm4UiReferences = if ($PSVersionTable.PSEdition -eq 'Core') {
        # PowerShell 7 replaces its default references when any are given; list the facades.
        # Newer .NET builds also split WinForms/GDI+ into System.Private.Windows.* assemblies.
        $desktop = Split-Path ([Windows.Forms.Form].Assembly.Location)
        @('System.Windows.Forms', 'System.Windows.Forms.Primitives', 'System.Drawing', 'System.Drawing.Primitives', 'System.Drawing.Common',
          'System.ComponentModel.Primitives', 'System.Collections', 'System.Runtime', 'System.Runtime.InteropServices', 'System.Private.CoreLib') +
        @(Get-ChildItem -LiteralPath $desktop -Filter 'System.Private.Windows*.dll' -ErrorAction SilentlyContinue | ForEach-Object FullName)
    } else { @('System.Windows.Forms', 'System.Drawing') }
    Add-Type -TypeDefinition $fm4UiSource -ReferencedAssemblies $fm4UiReferences -WarningAction SilentlyContinue
}

function Initialize-Fm4Fonts {
    $installed = @([Drawing.Text.InstalledFontCollection]::new().Families | ForEach-Object Name)
    if ($installed -contains 'Bahnschrift SemiBold Condensed') { [Fm4Ui.Palette]::Heading = 'Bahnschrift SemiBold Condensed' }
    elseif ($installed -contains 'Bahnschrift SemiBold') { [Fm4Ui.Palette]::Heading = 'Bahnschrift SemiBold' }
}
Initialize-Fm4Fonts

function Set-Fm4Theme($control) {
    $p = [Fm4Ui.Palette]
    if ($control.GetType().Namespace -eq 'Fm4Ui') {
        foreach ($child in $control.Controls) { Set-Fm4Theme $child }
        return
    }
    switch ($control) {
        { $_ -is [Windows.Forms.TabPage] } { $_.UseVisualStyleBackColor = $false; $_.BackColor = $p::Panel; $_.ForeColor = $p::Text; break }
        { $_ -is [Windows.Forms.Label] } {
            if ($_.BackColor -eq [Drawing.SystemColors]::Control) { $_.BackColor = [Drawing.Color]::Transparent }
            if ($_.ForeColor -eq [Drawing.SystemColors]::ControlText) { $_.ForeColor = $p::Text }
            elseif ($_.ForeColor.GetBrightness() -lt 0.42) { $_.ForeColor = $p::Dim }
            break
        }
        { $_ -is [Windows.Forms.CheckBox] } {
            $_.FlatStyle = 'Flat'; $_.FlatAppearance.BorderColor = $p::Line
            $_.ForeColor = $p::Text; $_.BackColor = [Drawing.Color]::Transparent; [Fm4Ui.Styler]::CheckBox($_); break
        }
        { $_ -is [Windows.Forms.ComboBox] } { $_.FlatStyle = 'Flat'; $_.BackColor = $p::Field; $_.ForeColor = $p::Text; [Fm4Ui.Styler]::ComboBox($_); break }
        { $_ -is [Windows.Forms.TextBox] } { $_.BorderStyle = 'FixedSingle'; $_.BackColor = $p::Field; $_.ForeColor = $p::Text; break }
        { $_ -is [Windows.Forms.NumericUpDown] } { $_.BorderStyle = 'FixedSingle'; $_.BackColor = $p::Field; $_.ForeColor = $p::Text; break }
        { $_ -is [Windows.Forms.Button] } {
            $_.FlatStyle = 'Flat'; $_.BackColor = $p::Raised; $_.ForeColor = $p::Text; $_.UseVisualStyleBackColor = $false
            $_.FlatAppearance.BorderColor = $p::Line; $_.FlatAppearance.MouseOverBackColor = $p::Field
            $_.FlatAppearance.MouseDownBackColor = $p::AccentDark; $_.Cursor = [Windows.Forms.Cursors]::Hand; break
        }
        { $_ -is [Windows.Forms.GroupBox] } { $_.ForeColor = $p::Dim; break }
        { $_ -is [Windows.Forms.Form] } { $_.BackColor = $p::Back; $_.ForeColor = $p::Text; break }
    }
    foreach ($child in $control.Controls) { Set-Fm4Theme $child }
}

# Builds the window frame. Callers add TabPages to .Pages exactly as before, then call
# Complete-Fm4Shell to create the navigation and apply the theme.
function New-Fm4Shell {
    param([string]$Title, [string]$Subtitle, [string]$Badge, [int]$Width = 1120, [int]$Height = 740)
    $form = [Windows.Forms.Form]::new()
    $form.Text = $Title; $form.ClientSize = [Drawing.Size]::new($Width, $Height)
    $form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false
    $form.Font = [Fm4Ui.Palette]::Make(10.5, $false); $form.KeyPreview = $true
    $form.BackColor = [Fm4Ui.Palette]::Back; [Fm4Ui.Styler]::DarkTitleBar($form)
    $header = [Fm4Ui.Header]::new(); $header.Subtitle = $Subtitle; $header.Badge = $Badge
    $header.SetBounds(0, 0, $Width, 92); $form.Controls.Add($header)
    $nav = [Windows.Forms.Panel]::new(); $nav.SetBounds(0, 92, 236, $Height - 92 - 64); $nav.BackColor = [Fm4Ui.Palette]::Back
    $form.Controls.Add($nav)
    $caption = [Fm4Ui.SideCaption]::new(); $caption.Text = 'Settings'; $caption.SetBounds(0, $nav.Height - 300, 70, 300); $nav.Controls.Add($caption)
    $pages = [Fm4Ui.PageHost]::new(); $pages.SetBounds(248, 104, $Width - 260, $Height - 104 - 76)
    $form.Controls.Add($pages)
    $footer = [Windows.Forms.Panel]::new(); $footer.SetBounds(0, $Height - 64, $Width, 64); $footer.BackColor = [Fm4Ui.Palette]::Panel
    $form.Controls.Add($footer)
    $status = [Windows.Forms.Label]::new(); $status.SetBounds(24, 20, $Width - 640, 26); $status.ForeColor = [Fm4Ui.Palette]::Dim
    $status.AutoEllipsis = $true; $footer.Controls.Add($status)
    return @{ Form = $form; Header = $header; Nav = $nav; Pages = $pages; Footer = $footer; Status = $status; Prompts = @() }
}

function Add-Fm4Prompt {
    param($Shell, [char]$Glyph, [string]$Text, [int]$Width = 190)
    $colors = @{ 'A' = [Drawing.Color]::FromArgb(96, 176, 56); 'B' = [Drawing.Color]::FromArgb(206, 40, 40)
                 'X' = [Drawing.Color]::FromArgb(40, 118, 214); 'Y' = [Drawing.Color]::FromArgb(230, 176, 20) }
    $button = [Fm4Ui.PromptButton]::new(); $button.Glyph = $Glyph; $button.GlyphColor = $colors[[string]$Glyph]; $button.Text = $Text
    $right = $Shell.Form.ClientSize.Width - 20
    foreach ($existing in $Shell.Prompts) { $right = [Math]::Min($right, $existing.Left - 12) }
    $button.SetBounds($right - $Width, 12, $Width, 40); $Shell.Footer.Controls.Add($button)
    $Shell.Prompts += $button
    return $button
}

function Set-Fm4SmoothText($Control) {
    foreach ($child in $Control.Controls) {
        if ($child -is [Windows.Forms.Label] -or $child -is [Windows.Forms.ButtonBase]) {
            if ([Fm4Ui.Palette]::NeedsSmoothing($child.Font)) { $child.UseCompatibleTextRendering = $true }
        }
        if ($child.Controls.Count) { Set-Fm4SmoothText $child }
    }
}

function Complete-Fm4Shell {
    param($Shell)
    Set-Fm4SmoothText $Shell.Form
    $y = 18; $buttons = @()
    foreach ($page in $Shell.Pages.TabPages) {
        $page.AutoScroll = $true
        $nav = [Fm4Ui.NavButton]::new(); $nav.Text = $page.Text; $nav.Tag = $page
        $nav.SetBounds(16, $y, 204, 46); $Shell.Nav.Controls.Add($nav); $nav.BringToFront(); $y += 52
        $buttons += $nav
    }
    $Shell.NavButtons = $buttons
    foreach ($nav in $buttons) {
        $nav.Add_Click({
            param($sender, $eventArgs)
            foreach ($other in $sender.Parent.Controls) { if ($other -is [Fm4Ui.NavButton]) { $other.Selected = ($other -eq $sender) } }
            $sender.Tag.Parent.SelectedTab = $sender.Tag
        })
    }
    if ($buttons.Count) { $buttons[0].Selected = $true; $Shell.Pages.SelectedIndex = 0 }
    $Shell.Pages.Add_SelectedIndexChanged({
        param($sender, $eventArgs)
        $form = $sender.FindForm()
        foreach ($panel in $form.Controls) { foreach ($other in $panel.Controls) { if ($other -is [Fm4Ui.NavButton]) { $other.Selected = ($other.Tag -eq $sender.SelectedTab) } } }
    })
    Set-Fm4Theme $Shell.Form
}

# Settings-page layout helpers: titled sections, one-line explanations and greyed-out
# options. Add sections and hints before Complete-Fm4Shell; call Set-Fm4Available after it.
function Add-Fm4Section {
    param($Page, [string]$Title, [int]$Y, [int]$Width = 800)
    # Same caption style as the Controls page: upper-case title over a short red rule.
    $label = [Windows.Forms.Label]::new(); $label.Text = $Title.ToUpperInvariant(); $label.Font = [Fm4Ui.Palette]::Make(15, $true)
    $label.ForeColor = [Fm4Ui.Palette]::Text; $label.SetBounds(20, $Y - 2, $Width, 28); $Page.Controls.Add($label)
    $line = [Windows.Forms.Label]::new(); $line.BackColor = [Fm4Ui.Palette]::Accent; $line.SetBounds(22, $Y + 27, 60, 3); $Page.Controls.Add($line)
    return $label
}

function Add-Fm4Hint {
    param($Page, [string]$Text, [int]$X, [int]$Y, [int]$Width = 760, [int]$Height = 22)
    $label = [Windows.Forms.Label]::new(); $label.Text = $Text; $label.AutoSize = $false; $label.AutoEllipsis = $true
    $label.SetBounds($X, $Y, $Width, $Height); $label.ForeColor = [Fm4Ui.Palette]::Dim; $label.Font = [Fm4Ui.Palette]::Make(9.5, $false)
    $Page.Controls.Add($label)
    return $label
}

# Enables or greys out controls without changing their values. Labels dim; fields keep
# the dark palette instead of the light system "disabled" colours.
function Set-Fm4Available {
    param([object[]]$Controls, [bool]$Available)
    $p = [Fm4Ui.Palette]
    foreach ($control in $Controls) {
        if ($null -eq $control) { continue }
        if ($control -is [Windows.Forms.Label]) {
            if ($control.Tag -isnot [Drawing.Color]) {
                $control.Tag = if ($control.ForeColor -eq [Drawing.SystemColors]::ControlText) { $p::Text } else { $control.ForeColor }
            }
            $control.ForeColor = if ($Available) { $control.Tag } elseif ($control.Tag -eq $p::Text) { $p::Dim } else { $p::Line }
            continue
        }
        $control.Enabled = $Available
        if ($control -is [Windows.Forms.TextBox] -or $control -is [Windows.Forms.NumericUpDown]) {
            $control.BackColor = if ($Available) { $p::Field } else { $p::Raised }
            $control.ForeColor = if ($Available) { $p::Text } else { $p::Dim }
        }
        $control.Invalidate()
    }
}

# ---------------------------------------------------------------------------------------
# Control bindings

function Get-Fm4IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Fallback) {
    $block = [regex]::Match($Text, '(?ms)^\[' + [regex]::Escape($Section) + '\]\s*\r?\n(.*?)(?=^\[|\z)')
    if ($block.Success) {
        $entry = [regex]::Match($block.Groups[1].Value, '(?m)^' + [regex]::Escape($Key) + '[ \t]*=[ \t]*([^\r\n]*)')
        if ($entry.Success) { return $entry.Groups[1].Value.Trim() }
    }
    return $Fallback
}
function Set-Fm4IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    if ($Value -match '[\r\n]') { throw 'A setting cannot contain line breaks.' }
    $block = [regex]::Match($Text, '(?ms)^\[' + [regex]::Escape($Section) + '\]\s*\r?\n(.*?)(?=^\[|\z)')
    if (-not $block.Success) { return $Text.TrimEnd() + "`n`n[$Section]`n$Key=$Value`n" }
    $body = $block.Groups[1].Value; $entry = '(?m)^' + [regex]::Escape($Key) + '[ \t]*=[ \t]*[^\r\n]*'
    if ([regex]::IsMatch($body, $entry)) { $body = [regex]::Replace($body, $entry, [Text.RegularExpressions.MatchEvaluator] { param($m) "$Key=$Value" }.GetNewClosure()) }
    else { $body = $body.TrimEnd() + "`n$Key=$Value`n`n" }
    return $Text.Substring(0, $block.Groups[1].Index) + $body + $Text.Substring($block.Groups[1].Index + $block.Groups[1].Length)
}

$script:Fm4AxisActions = @(
    @{ Key = 'steering'; Label = 'Steering'; Centered = $true; Prompt = 'Centre the wheel, then turn right and back' }
    @{ Key = 'throttle'; Label = 'Throttle'; Centered = $false; Prompt = 'Fully press and release the throttle' }
    @{ Key = 'brake'; Label = 'Brake'; Centered = $false; Prompt = 'Fully press and release the brake' }
    @{ Key = 'clutch'; Label = 'Clutch'; Centered = $false; Prompt = 'Fully press and release the clutch' }
    @{ Key = 'handbrake'; Label = 'Handbrake'; Centered = $false; Prompt = 'Fully pull and release the handbrake' }
)
# Xbox 360 wheel button bits as FM4 reads them (wheel_driver.cpp report layout).
$script:Fm4ButtonActions = @(
    @{ Bit = 9; Label = 'Shift up'; Hint = 'RB' }
    @{ Bit = 8; Label = 'Shift down'; Hint = 'LB' }
    @{ Bit = 12; Label = 'A'; Hint = 'Select' }
    @{ Bit = 13; Label = 'B'; Hint = 'Back' }
    @{ Bit = 14; Label = 'X'; Hint = '' }
    @{ Bit = 15; Label = 'Y'; Hint = 'Rewind' }
    @{ Bit = 4; Label = 'Start'; Hint = 'Pause' }
    @{ Bit = 5; Label = 'Back'; Hint = '' }
    @{ Bit = 6; Label = 'Left stick click'; Hint = 'LS' }
    @{ Bit = 7; Label = 'Right stick click'; Hint = 'RS' }
    @{ Bit = 0; Label = 'D-pad up'; Hint = '' }
    @{ Bit = 1; Label = 'D-pad down'; Hint = '' }
    @{ Bit = 2; Label = 'D-pad left'; Hint = '' }
    @{ Bit = 3; Label = 'D-pad right'; Hint = '' }
)

function Find-Fm4InputHelper([string]$EditionRoot) {
    foreach ($relative in @('.local/work/fm4/out/build/win-amd64-release/fm4-input.exe', 'fm4/out/build/win-amd64-release/fm4-input.exe', 'fm4-input.exe')) {
        $candidate = Join-Path $EditionRoot $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return [IO.Path]::GetFullPath($candidate) }
    }
    return $null
}

function Get-Fm4Device($ui, [string]$Selector) {
    # Empty selector = the force-feedback wheel the game picks (wheel.device or automatic).
    if (-not $Selector) {
        $ffb = $ui.FfbDevice
        if ($ffb) { return Get-Fm4Device $ui $ffb }
        $best = $null; $rank = -1
        foreach ($device in $ui.Devices) {
            $actuator = @($device.axes | Where-Object { $_.actuator }).Count -gt 0
            $r = if ($device.ffb -and $device.driving) { 2 } elseif ($device.ffb -and $actuator) { 1 } else { 0 }
            if ($device.ffb -and $r -gt $rank) { $best = $device; $rank = $r }
        }
        return $best
    }
    foreach ($device in $ui.Devices) { if ($device.guid -ieq $Selector) { return $device } }
    foreach ($device in $ui.Devices) { if ($device.name -ieq $Selector) { return $device } }
    return $null
}

function Get-Fm4AxisIndex($device, [string]$Axis) {
    if (-not $device -or -not $Axis -or $Axis -ieq 'disabled') { return -1 }
    $axes = @($device.axes)
    for ($i = 0; $i -lt $axes.Count; $i++) { if ($axes[$i].name -ieq $Axis) { return $i } }
    if ($Axis -ieq 'Wheel Axis') { for ($i = 0; $i -lt $axes.Count; $i++) { if ($axes[$i].actuator) { return $i } } }
    $semantic = @{ 'X' = 'X'; 'Wheel Axis' = 'X'; 'Y' = 'Y'; 'Z' = 'Z'; 'RX' = 'RX'; 'RY' = 'RY'; 'RZ' = 'RZ' }[$Axis.ToUpperInvariant()]
    if ($semantic) { for ($i = 0; $i -lt $axes.Count; $i++) { if ($axes[$i].type -eq $semantic) { return $i } } }
    if ($Axis -match '^Slider([01])$') {
        $want = [int]$Matches[1]
        for ($i = 0; $i -lt $axes.Count; $i++) { if ($axes[$i].type -eq 'Slider') { if ($want -eq 0) { return $i }; $want-- } }
    }
    if ($Axis -match '^#(\d+)$') { $i = [int]$Matches[1]; if ($i -lt $axes.Count) { return $i } }
    return -1
}

function Get-Fm4DeviceLabel($ui, $device) {
    $same = @($ui.Devices | Where-Object { $_.name -eq $device.name })
    if ($same.Count -le 1) { return $device.name }
    return '{0} ({1})' -f $device.name, ([Array]::IndexOf(@($same), $device) + 1)
}

function Format-Fm4Binding($ui, $row) {
    if ($row.Kind -eq 'axis') {
        if (-not $row.Axis -or $row.Axis -ieq 'disabled') { return 'Not bound' }
        $device = Get-Fm4Device $ui $row.Device
        $deviceText = if ($device) { Get-Fm4DeviceLabel $ui $device } elseif ($row.Device) { 'Not connected' } else { 'Wheel' }
        $index = Get-Fm4AxisIndex $device $row.Axis
        $axisText = if ($index -ge 0) { '{0} (#{1})' -f $device.axes[$index].name, $index } else { $row.Axis }
        return '{0} · {1}' -f $deviceText, $axisText
    }
    if (-not $row.Button -or [int]$row.Button -le 0) { return 'Not bound' }
    $selector = if ($row.Kind -eq 'gear') { $ui.ShifterDevice } else { $row.Device }
    $device = Get-Fm4Device $ui $selector
    $deviceText = if ($device) { Get-Fm4DeviceLabel $ui $device } elseif ($selector) { 'Not connected' } else { 'Wheel' }
    return '{0} · Button {1}' -f $deviceText, $row.Button
}

function Get-Fm4AxisOwner($ui, $row) {
    # Physical identity of an axis binding (device guid or selector, plus axis index or name).
    $device = Get-Fm4Device $ui $row.Device
    $index = Get-Fm4AxisIndex $device $row.Axis
    if ($device -and $index -ge 0) { return '{0}#{1}' -f $device.guid, $index }
    return '{0}|{1}' -f $row.Device, $row.Axis
}

function Update-Fm4RowText($ui, $row) {
    $text = Format-Fm4Binding $ui $row
    $shared = @()
    if ($row.Kind -eq 'axis' -and $row.Axis -and $row.Axis -ine 'disabled') {
        $owner = Get-Fm4AxisOwner $ui $row
        foreach ($other in $ui.Rows) {
            if ($other -ne $row -and $other.Kind -eq 'axis' -and $other.Axis -and $other.Axis -ine 'disabled' -and (Get-Fm4AxisOwner $ui $other) -eq $owner) { $shared += $other.Label }
        }
    }
    if ($shared.Count) { $text += '  ' + [char]0x26A0 + ' also ' + ($shared -join ', ') }
    $row.BindingLabel.Text = $text
    $row.BindingLabel.ForeColor = if ($shared.Count) { [Fm4Ui.Palette]::Accent } elseif ($text -eq 'Not bound' -or $text -like 'Not connected*') { [Fm4Ui.Palette]::Dim } else { [Fm4Ui.Palette]::Text }
}

function Read-Fm4BindingsFromIni($ui, [string]$Text) {
    $ui.FfbDevice = Get-Fm4IniValue $Text wheel device ''
    foreach ($row in $ui.Rows) {
        switch ($row.Kind) {
            'axis' {
                $fallback = @{ steering = 'Wheel Axis'; throttle = 'Accelerator'; brake = 'Brake'; clutch = 'Clutch'; handbrake = 'disabled' }[$row.Key]
                $row.Device = Get-Fm4IniValue $Text $row.Key device ''
                $row.Axis = Get-Fm4IniValue $Text $row.Key axis $fallback
                $row.Minimum = [int](Get-Fm4IniValue $Text $row.Key minimum 0)
                $row.Center = [int](Get-Fm4IniValue $Text $row.Key center 32767)
                $row.Maximum = [int](Get-Fm4IniValue $Text $row.Key maximum 65535)
                $row.Invert.Checked = (Get-Fm4IniValue $Text $row.Key invert 0) -eq '1'
                $dz = [decimal]::Parse((Get-Fm4IniValue $Text $row.Key deadzone 0), [Globalization.CultureInfo]::InvariantCulture) * 100
                $row.Deadzone.Value = [Math]::Min([decimal]$row.Deadzone.Maximum, [Math]::Max([decimal]0, [decimal]$dz))
            }
            'button' {
                $defaults = @(0, 0, 0, 0, 10, 9, 12, 11, 6, 5, 0, 0, 2, 3, 1, 4)
                $row.Button = [int](Get-Fm4IniValue $Text buttons ('bit' + $row.Bit) $defaults[$row.Bit])
                $row.Device = Get-Fm4IniValue $Text buttons ('device' + $row.Bit) ''
            }
            'gear' {
                $key = if ($row.Gear -eq 0) { 'reverse' } else { 'gear' + $row.Gear }
                $row.Button = [int](Get-Fm4IniValue $Text shifter $key 0)
            }
        }
    }
    $ui.ShifterDevice = Get-Fm4IniValue $Text shifter device ''
    $ui.ShifterEnabled.Checked = (Get-Fm4IniValue $Text shifter enabled 0) -eq '1'
    $gears = [int](Get-Fm4IniValue $Text shifter gears 7); $ui.ShifterGears.Value = [Math]::Min(8, [Math]::Max(1, $gears))
    foreach ($row in $ui.Rows) { Update-Fm4RowText $ui $row }
    Update-Fm4FfbChoices $ui
}

function Write-Fm4BindingsToIni($ui, [string]$Text) {
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $Text = Set-Fm4IniValue $Text wheel device ([string]$ui.FfbDevice)
    foreach ($row in $ui.Rows) {
        switch ($row.Kind) {
            'axis' {
                $Text = Set-Fm4IniValue $Text $row.Key device ([string]$row.Device)
                $Text = Set-Fm4IniValue $Text $row.Key axis ([string]$(if ($row.Axis) { $row.Axis } else { 'disabled' }))
                $Text = Set-Fm4IniValue $Text $row.Key minimum ([string]$row.Minimum)
                $Text = Set-Fm4IniValue $Text $row.Key center ([string]$row.Center)
                $Text = Set-Fm4IniValue $Text $row.Key maximum ([string]$row.Maximum)
                $Text = Set-Fm4IniValue $Text $row.Key invert ([string][int]$row.Invert.Checked)
                $Text = Set-Fm4IniValue $Text $row.Key deadzone (([double]$row.Deadzone.Value / 100).ToString($culture))
            }
            'button' {
                $Text = Set-Fm4IniValue $Text buttons ('bit' + $row.Bit) ([string][int]$row.Button)
                $Text = Set-Fm4IniValue $Text buttons ('device' + $row.Bit) ([string]$row.Device)
            }
            'gear' {
                $key = if ($row.Gear -eq 0) { 'reverse' } else { 'gear' + $row.Gear }
                $Text = Set-Fm4IniValue $Text shifter $key ([string][int]$row.Button)
            }
        }
    }
    $Text = Set-Fm4IniValue $Text shifter enabled ([string][int]$ui.ShifterEnabled.Checked)
    $Text = Set-Fm4IniValue $Text shifter device ([string]$ui.ShifterDevice)
    $Text = Set-Fm4IniValue $Text shifter gears ([string][int]$ui.ShifterGears.Value)
    return $Text
}

function Update-Fm4FfbChoices($ui) {
    $combo = $ui.FfbCombo
    $combo.BeginUpdate()
    try {
        $combo.Items.Clear(); $ui.FfbValues = @('')
        [void]$combo.Items.Add('Automatic')
        foreach ($device in @($ui.Devices | Where-Object { $_.ffb })) {
            [void]$combo.Items.Add(('{0}  {1}' -f (Get-Fm4DeviceLabel $ui $device), $device.guid)); $ui.FfbValues += $device.guid
        }
        if ($ui.FfbDevice -and ($ui.FfbValues -notcontains $ui.FfbDevice)) {
            $match = @($ui.Devices | Where-Object { $_.name -ieq $ui.FfbDevice })
            if (-not $match.Count) { [void]$combo.Items.Add(('Not connected: {0}' -f $ui.FfbDevice)); $ui.FfbValues += $ui.FfbDevice }
        }
        $index = [Array]::IndexOf(@($ui.FfbValues), [string]$ui.FfbDevice)
        if ($index -lt 0) { $index = [Array]::IndexOf(@($ui.FfbValues), [string](Get-Fm4Device $ui $ui.FfbDevice).guid) }
        $ui.UpdatingFfb = $true; $combo.SelectedIndex = [Math]::Max(0, $index); $ui.UpdatingFfb = $false
    } finally { $combo.EndUpdate() }
}

function Set-Fm4FfbDevice($ui, [string]$Selector) {
    $ui.FfbDevice = $Selector; Update-Fm4FfbChoices $ui
    foreach ($row in $ui.Rows) { Update-Fm4RowText $ui $row }
}

function Show-Fm4DeviceChips($ui) {
    $panel = $ui.DevicePanel; $panel.SuspendLayout()
    try {
        $panel.Controls.Clear()
        if (-not $ui.HelperPath) {
            $note = [Windows.Forms.Label]::new(); $note.AutoSize = $false; $note.SetBounds(0, 0, 760, 44); $note.ForeColor = [Fm4Ui.Palette]::Dim
            $note.Text = 'Input helper missing; edit bindings in wheel.ini.'
            $panel.Controls.Add($note); return
        }
        if (-not @($ui.Devices).Count) {
            $note = [Windows.Forms.Label]::new(); $note.AutoSize = $false; $note.SetBounds(0, 0, 760, 30); $note.ForeColor = [Fm4Ui.Palette]::Dim
            $note.Text = 'Connect a wheel, pedals or controller.'
            $panel.Controls.Add($note); return
        }
        foreach ($device in $ui.Devices) {
            $chip = [Windows.Forms.Label]::new(); $chip.AutoSize = $false; $chip.Size = [Drawing.Size]::new(250, 44)
            $chip.Margin = [Windows.Forms.Padding]::new(0, 0, 10, 8); $chip.BackColor = [Fm4Ui.Palette]::Raised; $chip.ForeColor = [Fm4Ui.Palette]::Text
            $kind = if ($device.ffb) { 'Force feedback' } elseif ($device.driving) { 'Driving' } else { 'Controller' }
            $chip.Text = "$(Get-Fm4DeviceLabel $ui $device)`n$kind · $(@($device.axes).Count) axes · $($device.buttons) buttons"
            $chip.Padding = [Windows.Forms.Padding]::new(10, 3, 6, 3); $chip.AutoEllipsis = $true
            $panel.Controls.Add($chip)
        }
    } finally { $panel.ResumeLayout() }
}

function Invoke-Fm4InputLine($ui, [string]$Line) {
    if (-not $Line -or $Line[0] -ne '{') { return }
    try { $message = $Line | ConvertFrom-Json } catch { return }
    if ($message.type -eq 'devices') {
        $ui.Devices = @($message.devices)
        Show-Fm4DeviceChips $ui; Update-Fm4FfbChoices $ui
        foreach ($row in $ui.Rows) { Update-Fm4RowText $ui $row }
        return
    }
    if ($message.type -ne 'state') { return }
    $ui.Values[$message.guid] = @{ Axes = @($message.axes | ForEach-Object { [int]$_ }); Buttons = @($message.buttons | ForEach-Object { [int]$_ }) }
    $ui.LastInput = [DateTime]::UtcNow
    if ($ui.Capture) { Step-Fm4Capture $ui }
    Update-Fm4Meters $ui
}

function Get-Fm4CalibratedValue($row, [int]$Raw) {
    $min = [double]$row.Minimum; $max = [double]$row.Maximum; $center = [double]$row.Center
    $raw = [Math]::Min($max, [Math]::Max($min, [double]$Raw))
    if ($row.Centered) {
        if ($center -le $min -or $center -ge $max) { return 0 }
        $value = if ($raw -lt $center) { ($raw - $center) / ($center - $min) } else { ($raw - $center) / ($max - $center) }
        if ($row.Invert.Checked) { $value = -$value }
    } else {
        if ($max -le $min) { return 0 }
        $value = ($raw - $min) / ($max - $min)
        if ($row.Invert.Checked) { $value = 1 - $value }
    }
    $deadzone = [double]$row.Deadzone.Value / 100
    $magnitude = [Math]::Max(0, ([Math]::Abs($value) - $deadzone) / [Math]::Max(0.01, 1 - $deadzone))
    return [Math]::Sign($value) * [Math]::Min(1, $magnitude)
}

function Update-Fm4Meters($ui) {
    foreach ($row in $ui.Rows) {
        $meter = $row.Meter
        if ($row.Kind -eq 'axis') {
            $device = Get-Fm4Device $ui $row.Device; $index = Get-Fm4AxisIndex $device $row.Axis
            $values = if ($device) { $ui.Values[$device.guid] } else { $null }
            if ($values -and $index -ge 0 -and $index -lt $values.Axes.Count) { $meter.Available = $true; $meter.Value = [float](Get-Fm4CalibratedValue $row $values.Axes[$index]) }
            else { $meter.Available = $false }
        } else {
            $selector = if ($row.Kind -eq 'gear') { $ui.ShifterDevice } else { $row.Device }
            $device = Get-Fm4Device $ui $selector
            $values = if ($device) { $ui.Values[$device.guid] } else { $null }
            $meter.Available = [bool]($values -and [int]$row.Button -gt 0)
            $meter.Pressed = [bool]($values -and $values.Buttons -contains [int]$row.Button)
        }
    }
}

function Set-Fm4Banner($ui, [string]$Text, [bool]$Alert = $false) {
    $ui.Banner.Text = $Text
    $ui.Banner.BackColor = if ($Alert) { [Fm4Ui.Palette]::Accent } else { [Fm4Ui.Palette]::Raised }
    $ui.Banner.ForeColor = if ($Alert) { [Drawing.Color]::White } else { [Fm4Ui.Palette]::Dim }
}

function Start-Fm4Capture($ui, $row) {
    if (-not @($ui.Devices).Count) { Set-Fm4Banner $ui 'Connect an input device first.' $true; return }
    $baseline = @{}
    foreach ($key in @($ui.Values.Keys)) { $baseline[$key] = @{ Axes = @($ui.Values[$key].Axes); Buttons = @($ui.Values[$key].Buttons) } }
    $ui.Capture = @{ Row = $row; Started = [DateTime]::UtcNow; Baseline = $baseline; Phase = 'detect' }
    $prompt = if ($row.Kind -eq 'axis') { $row.Prompt } elseif ($row.Kind -eq 'gear') { 'Select {0} on your shifter' -f $row.Label.ToUpperInvariant() } else { 'Press the button for {0}' -f $row.Label.ToUpperInvariant() }
    Set-Fm4Banner $ui ('{0}   ·   Esc to cancel' -f $prompt) $true
    $row.BindButton.Text = '…'
}

function Stop-Fm4Capture($ui, [string]$Message) {
    if ($ui.Capture) { $ui.Capture.Row.BindButton.Text = 'Bind' }
    $ui.Capture = $null
    Set-Fm4Banner $ui $Message $false
}

function Step-Fm4Capture($ui) {
    $capture = $ui.Capture; $row = $capture.Row; $now = [DateTime]::UtcNow
    if ($capture.Phase -eq 'detect' -and ($now - $capture.Started).TotalSeconds -gt 12) { Stop-Fm4Capture $ui 'Nothing detected; binding unchanged.'; return }
    if ($row.Kind -eq 'axis') {
        if ($capture.Phase -eq 'detect') {
            $best = 0; $pick = $null
            foreach ($guid in @($ui.Values.Keys)) {
                $base = $capture.Baseline[$guid]; if (-not $base) { continue }
                $axes = $ui.Values[$guid].Axes
                for ($i = 0; $i -lt [Math]::Min($axes.Count, $base.Axes.Count); $i++) {
                    $delta = [Math]::Abs($axes[$i] - $base.Axes[$i])
                    if ($delta -gt $best) { $best = $delta; $pick = @{ Guid = $guid; Index = $i; Rest = $base.Axes[$i]; Direction = [Math]::Sign($axes[$i] - $base.Axes[$i]) } }
                }
            }
            if ($best -gt 0.3 * 65535) {
                $capture.Pick = $pick; $capture.Phase = 'sweep'; $capture.Detected = $now
                $capture.Low = [Math]::Min($pick.Rest, $ui.Values[$pick.Guid].Axes[$pick.Index]); $capture.High = [Math]::Max($pick.Rest, $ui.Values[$pick.Guid].Axes[$pick.Index])
                Set-Fm4Banner $ui ('Detected. {0}   ·   release to finish' -f $row.Prompt) $true
            }
            return
        }
        $pick = $capture.Pick; $value = $ui.Values[$pick.Guid].Axes[$pick.Index]
        $capture.Low = [Math]::Min($capture.Low, $value); $capture.High = [Math]::Max($capture.High, $value)
        $elapsed = ($now - $capture.Detected).TotalSeconds
        $returned = [Math]::Abs($value - $pick.Rest) -lt 0.08 * 65535
        if (($elapsed -gt 1.0 -and $returned) -or $elapsed -gt 5) { Complete-Fm4AxisCapture $ui }
        return
    }
    # Buttons and gears: the first button that was not held when binding started.
    foreach ($guid in @($ui.Values.Keys)) {
        $base = $capture.Baseline[$guid]; $held = if ($base) { $base.Buttons } else { @() }
        foreach ($button in $ui.Values[$guid].Buttons) {
            if ($held -notcontains $button) { Complete-Fm4ButtonCapture $ui $guid $button; return }
        }
    }
}

function Complete-Fm4AxisCapture($ui) {
    $capture = $ui.Capture; $row = $capture.Row; $pick = $capture.Pick
    $notes = @()
    # One physical axis drives one action. Free any other action that used it.
    foreach ($other in $ui.Rows) {
        if ($other -eq $row -or $other.Kind -ne 'axis') { continue }
        $device = Get-Fm4Device $ui $other.Device
        if ($device -and $device.guid -eq $pick.Guid -and (Get-Fm4AxisIndex $device $other.Axis) -eq $pick.Index) {
            $other.Axis = 'disabled'; $other.Device = ''; Update-Fm4RowText $ui $other; $notes += "$($other.Label) was unbound (same axis)"
        }
    }
    $row.Device = $pick.Guid; $row.Axis = '#' + $pick.Index
    if ($row.Centered) {
        $row.Minimum = 0; $row.Maximum = 65535; $row.Center = [Math]::Min(65534, [Math]::Max(1, [int]$pick.Rest))
        $row.Invert.Checked = $pick.Direction -lt 0
    } else {
        $low = [int]$capture.Low; $high = [int]$capture.High
        if ($high - $low -ge 0.2 * 65535) { $row.Minimum = $low; $row.Maximum = $high } else { $row.Minimum = 0; $row.Maximum = 65535; $notes += 'short travel, kept full range' }
        $row.Center = 32767
        $row.Invert.Checked = $pick.Rest -gt ($row.Minimum + $row.Maximum) / 2
    }
    foreach ($each in $ui.Rows) { Update-Fm4RowText $ui $each }
    $message = '{0} bound to {1}' -f $row.Label, (Format-Fm4Binding $ui $row)
    if ($notes.Count) { $message += ' - ' + ($notes -join '; ') }
    Stop-Fm4Capture $ui $message
    Update-Fm4Meters $ui
}

function Complete-Fm4ButtonCapture($ui, [string]$Guid, [int]$Button) {
    $row = $ui.Capture.Row; $notes = @()
    if ($row.Kind -eq 'gear' -and $ui.ShifterDevice -and $ui.ShifterDevice -ine $Guid) {
        $previous = Get-Fm4Device $ui $ui.ShifterDevice
        if (-not $previous -or $previous.guid -ne $Guid) {
            foreach ($gear in @($ui.Rows | Where-Object { $_.Kind -eq 'gear' -and $_ -ne $row -and [int]$_.Button -gt 0 })) { $gear.Button = 0; Update-Fm4RowText $ui $gear }
            $notes += 'other gears were cleared (all gears use one shifter device)'
        }
    }
    foreach ($other in $ui.Rows) {
        if ($other -eq $row -or $other.Kind -eq 'axis' -or [int]$other.Button -ne $Button) { continue }
        $selector = if ($other.Kind -eq 'gear') { $ui.ShifterDevice } else { $other.Device }
        $device = Get-Fm4Device $ui $selector
        $targetDevice = if ($row.Kind -eq 'gear' -or $other.Kind -eq 'gear') { $Guid } else { $Guid }
        if ($device -and $device.guid -eq $targetDevice) { $other.Button = 0; Update-Fm4RowText $ui $other; $notes += "$($other.Label) was unbound" }
    }
    if ($row.Kind -eq 'gear') { $ui.ShifterDevice = $Guid; $ui.ShifterEnabled.Checked = $true } else { $row.Device = $Guid }
    $row.Button = $Button
    foreach ($each in $ui.Rows) { Update-Fm4RowText $ui $each }
    $message = '{0} bound to {1}' -f $row.Label, (Format-Fm4Binding $ui $row)
    if ($notes.Count) { $message += ' - ' + ($notes -join '; ') }
    Stop-Fm4Capture $ui $message
    Update-Fm4Meters $ui
}

function Clear-Fm4Binding($ui, $row) {
    if ($row.Kind -eq 'axis') { $row.Axis = 'disabled'; $row.Device = '' } else { $row.Button = 0; if ($row.Kind -eq 'button') { $row.Device = '' } }
    foreach ($each in $ui.Rows) { Update-Fm4RowText $ui $each }; Update-Fm4Meters $ui
    Set-Fm4Banner $ui ('{0} cleared.' -f $row.Label) $false
}

function Start-Fm4InputHelper($ui) {
    if ($ui.Process -or -not $ui.HelperPath -or $ui.NoLiveInput) { return }
    $info = [Diagnostics.ProcessStartInfo]::new($ui.HelperPath)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardInput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    try { $ui.Process = [Diagnostics.Process]::Start($info); $ui.Pending = $ui.Process.StandardOutput.ReadLineAsync() }
    catch { $ui.Process = $null; Set-Fm4Banner $ui ('Live input unavailable: ' + $_.Exception.Message) $true; return }
    $ui.Timer.Start()
}

function Stop-Fm4InputHelper($ui) {
    if ($ui.Timer) { $ui.Timer.Stop() }
    if ($ui.Process) {
        try { $ui.Process.StandardInput.Close() } catch {}
        try { if (-not $ui.Process.WaitForExit(1500)) { $ui.Process.Kill() } } catch {}
        $ui.Process.Dispose(); $ui.Process = $null
    }
}

function Receive-Fm4InputLines($ui) {
    for ($n = 0; $n -lt 200 -and $ui.Pending -and $ui.Pending.IsCompleted; $n++) {
        $line = $null
        try { $line = $ui.Pending.Result } catch {}
        if ($null -eq $line) { $ui.Pending = $null; Set-Fm4Banner $ui 'Live input stopped.' $false; break }
        Invoke-Fm4InputLine $ui $line
        $ui.Pending = $ui.Process.StandardOutput.ReadLineAsync()
    }
    if ($ui.Capture) { Step-Fm4Capture $ui }
}

function New-Fm4ControlsPage {
    param([Parameter(Mandatory)]$Pages, [Parameter(Mandatory)][string]$EditionRoot, [Parameter(Mandatory)][string]$WheelText, [string]$WheelPath = '', [switch]$NoLiveInput)
    $tab = [Windows.Forms.TabPage]::new('Controls'); $tab.AutoScroll = $true; [void]$Pages.TabPages.Add($tab)
    $script:Fm4ControlsUi = $ui = @{
        Tab = $tab; Rows = @(); Devices = @(); Values = @{}; Capture = $null; Process = $null; Pending = $null
        HelperPath = (Find-Fm4InputHelper $EditionRoot); FfbDevice = ''; ShifterDevice = ''; FfbValues = @(''); UpdatingFfb = $false
        WheelPath = $WheelPath; NoLiveInput = [bool]$NoLiveInput
    }
    $p = [Fm4Ui.Palette]
    function Add-Caption([string]$text, [int]$y) {
        $c = [Windows.Forms.Label]::new(); $c.Text = $text.ToUpperInvariant(); $c.Font = $p::Make(15, $true); $c.ForeColor = $p::Text
        $c.SetBounds(20, $y, 600, 30); $tab.Controls.Add($c)
        $line = [Windows.Forms.Label]::new(); $line.BackColor = $p::Accent; $line.SetBounds(22, $y + 31, 60, 3); $tab.Controls.Add($line)
    }
    $intro = [Windows.Forms.Label]::new(); $intro.ForeColor = $p::Dim; $intro.SetBounds(20, 12, 800, 40)
    $intro.Text = 'Select Bind, then press or move the control.'
    $tab.Controls.Add($intro)
    $ui.Banner = [Windows.Forms.Label]::new(); $ui.Banner.SetBounds(20, 58, 800, 38); $ui.Banner.TextAlign = 'MiddleLeft'
    $ui.Banner.Padding = [Windows.Forms.Padding]::new(12, 0, 0, 0); $ui.Banner.Font = $p::Make(12.5, $true); $tab.Controls.Add($ui.Banner)
    Add-Caption 'Devices' 108
    $ui.DevicePanel = [Windows.Forms.FlowLayoutPanel]::new(); $ui.DevicePanel.SetBounds(20, 148, 800, 104); $ui.DevicePanel.AutoScroll = $true
    $tab.Controls.Add($ui.DevicePanel)
    $ffbLabel = [Windows.Forms.Label]::new(); $ffbLabel.Text = 'Force feedback device'; $ffbLabel.SetBounds(20, 262, 220, 28); $tab.Controls.Add($ffbLabel)
    $ui.FfbCombo = [Windows.Forms.ComboBox]::new(); $ui.FfbCombo.DropDownStyle = 'DropDownList'; $ui.FfbCombo.SetBounds(250, 260, 570, 29)
    $tab.Controls.Add($ui.FfbCombo)
    $ui.FfbCombo.Add_SelectedIndexChanged({
        $state = $script:Fm4ControlsUi
        if ($state.UpdatingFfb -or $state.FfbCombo.SelectedIndex -lt 0) { return }
        $state.FfbDevice = $state.FfbValues[$state.FfbCombo.SelectedIndex]
        foreach ($row in $state.Rows) { Update-Fm4RowText $state $row }
    })

    function Add-Row([hashtable]$row, [int]$y) {
        $label = [Windows.Forms.Label]::new(); $label.Text = $row.Label; $label.SetBounds(20, $y + 4, 150, 26); $label.Font = $p::Make(12, $true); $tab.Controls.Add($label)
        if ($row.Hint) { $hint = [Windows.Forms.Label]::new(); $hint.Text = $row.Hint; $hint.ForeColor = $p::Dim; $hint.SetBounds(150, $y + 6, 54, 24); $tab.Controls.Add($hint) }
        $row.BindingLabel = [Windows.Forms.Label]::new(); $row.BindingLabel.SetBounds(206, $y + 6, 268, 24); $row.BindingLabel.AutoEllipsis = $true; $tab.Controls.Add($row.BindingLabel)
        $row.Meter = [Fm4Ui.InputMeter]::new(); $row.Meter.Centered = [bool]$row.Centered; $row.Meter.IsButton = $row.Kind -ne 'axis'
        if ($row.Kind -eq 'axis') { $row.Meter.SetBounds(480, $y + 4, 110, 26) } else { $row.Meter.SetBounds(480, $y + 4, 26, 26) }
        $tab.Controls.Add($row.Meter)
        if ($row.Kind -eq 'axis') {
            $row.Invert = [Windows.Forms.CheckBox]::new(); $row.Invert.Text = 'Invert'; $row.Invert.SetBounds(598, $y + 4, 72, 28); $tab.Controls.Add($row.Invert)
            $row.Deadzone = [Windows.Forms.NumericUpDown]::new(); $row.Deadzone.DecimalPlaces = 1; $row.Deadzone.Increment = 0.5; $row.Deadzone.Maximum = 30
            $row.Deadzone.SetBounds(672, $y + 4, 50, 28); $tab.Controls.Add($row.Deadzone)
            $tip = [Windows.Forms.ToolTip]::new(); $tip.SetToolTip($row.Deadzone, 'Deadzone (%)')
        }
        $row.BindButton = [Windows.Forms.Button]::new(); $row.BindButton.Text = 'Bind'; $row.BindButton.SetBounds(728, $y, 60, 32); $row.BindButton.Tag = $row; $tab.Controls.Add($row.BindButton)
        $clear = [Windows.Forms.Button]::new(); $clear.Text = [string][char]0x2715; $clear.SetBounds(792, $y, 32, 32); $clear.Tag = $row; $tab.Controls.Add($clear)
        $row.ClearButton = $clear
        $ui.Rows += $row
        $row.BindButton.Add_Click({ param($sender, $eventArgs)
            $state = $script:Fm4ControlsUi
            if ($state.Capture) { Stop-Fm4Capture $state 'Binding cancelled.' } else { Start-Fm4Capture $state $sender.Tag } })
        $clear.Add_Click({ param($sender, $eventArgs) Clear-Fm4Binding $script:Fm4ControlsUi $sender.Tag })
    }
    Add-Caption 'Axes' 304
    $header = [Windows.Forms.Label]::new(); $header.Text = 'INVERT    DEADZONE %'; $header.ForeColor = $p::Dim; $header.SetBounds(598, 316, 160, 20); $tab.Controls.Add($header)
    $y = 346
    foreach ($action in $script:Fm4AxisActions) {
        Add-Row @{ Kind = 'axis'; Key = $action.Key; Label = $action.Label; Centered = $action.Centered; Prompt = $action.Prompt; Device = ''; Axis = 'disabled'; Minimum = 0; Center = 32767; Maximum = 65535 } $y
        $y += 40
    }
    Add-Caption 'Buttons' ($y + 14); $y += 56
    foreach ($action in $script:Fm4ButtonActions) {
        Add-Row @{ Kind = 'button'; Bit = $action.Bit; Label = $action.Label; Hint = $action.Hint; Device = ''; Button = 0 } $y
        $y += 40
    }
    Add-Caption 'H-pattern shifter' ($y + 14); $y += 56
    $ui.ShifterEnabled = [Windows.Forms.CheckBox]::new(); $ui.ShifterEnabled.Text = 'Use an H-pattern shifter'; $ui.ShifterEnabled.SetBounds(20, $y, 300, 30); $tab.Controls.Add($ui.ShifterEnabled)
    $gearsLabel = [Windows.Forms.Label]::new(); $gearsLabel.Text = 'Forward gears'; $gearsLabel.SetBounds(400, $y + 2, 130, 26); $tab.Controls.Add($gearsLabel)
    $ui.ShifterGears = [Windows.Forms.NumericUpDown]::new(); $ui.ShifterGears.Minimum = 1; $ui.ShifterGears.Maximum = 8; $ui.ShifterGears.Value = 7
    $ui.ShifterGears.SetBounds(540, $y, 60, 28); $tab.Controls.Add($ui.ShifterGears)
    $y += 44
    for ($gear = 0; $gear -le 8; $gear++) {
        $label = if ($gear -eq 0) { 'Reverse' } else { "Gear $gear" }
        Add-Row @{ Kind = 'gear'; Gear = $gear; Label = $label; Hint = ''; Button = 0 } $y
        $y += 40
    }
    $raw = [Windows.Forms.Button]::new(); $raw.Text = 'Edit wheel.ini…'; $raw.SetBounds(20, $y + 16, 260, 34); $tab.Controls.Add($raw)
    $ui.RawButton = $raw
    $spacer = [Windows.Forms.Label]::new(); $spacer.SetBounds(20, $y + 60, 10, 20); $tab.Controls.Add($spacer)

    $ui.Timer = [Windows.Forms.Timer]::new(); $ui.Timer.Interval = 25
    $ui.Timer.Add_Tick({ Receive-Fm4InputLines $script:Fm4ControlsUi })
    Read-Fm4BindingsFromIni $ui $WheelText
    Show-Fm4DeviceChips $ui
    $message = if ($ui.HelperPath) { 'Ready to bind controls.' } else { 'Input helper missing; binding unavailable.' }
    Set-Fm4Banner $ui $message $false
    return $ui
}

# Starts live input when the Controls page is shown, stops it when the window closes,
# and lets Esc cancel a capture.
function Register-Fm4ControlsPage($ui, $Form, $Pages) {
    $script:Fm4ControlsUi = $ui
    $Pages.Add_SelectedIndexChanged({ param($sender, $eventArgs) if ($sender.SelectedTab -eq $script:Fm4ControlsUi.Tab) { Start-Fm4InputHelper $script:Fm4ControlsUi } })
    $Form.Add_FormClosed({ Stop-Fm4InputHelper $script:Fm4ControlsUi })
    $Form.Add_KeyDown({ param($sender, $eventArgs)
        if ($eventArgs.KeyCode -eq 'Escape' -and $script:Fm4ControlsUi.Capture) { Stop-Fm4Capture $script:Fm4ControlsUi 'Binding cancelled.'; $eventArgs.Handled = $true } })
}

# CPU-only regression for the binding logic with synthetic devices (no hardware, no window).
function Test-Fm4ControlsPage($ui) {
    $wheelGuid = '{11111111-0000-0000-0000-000000000001}'; $pedalGuid = '{22222222-0000-0000-0000-000000000002}'; $boxGuid = '{33333333-0000-0000-0000-000000000003}'
    $devices = @{ type = 'devices'; devices = @(
        @{ guid = $wheelGuid; name = 'Test Wheelbase'; driving = $true; ffb = $true; axes = @(@{ name = 'Wheel Axis'; type = 'X'; actuator = $true }, @{ name = 'Dial'; type = 'Slider'; actuator = $false }); buttons = 32; povs = 1 }
        @{ guid = $pedalGuid; name = 'Test Pedals'; driving = $false; ffb = $false; axes = @(@{ name = 'Accelerator'; type = 'Y'; actuator = $false }, @{ name = 'Brake'; type = 'RZ'; actuator = $false }, @{ name = 'Clutch'; type = 'Slider'; actuator = $false }); buttons = 0; povs = 0 }
        @{ guid = $boxGuid; name = 'Test Handbrake'; driving = $false; ffb = $false; axes = @(@{ name = 'X Axis'; type = 'X'; actuator = $false }); buttons = 12; povs = 0 }
    ) } | ConvertTo-Json -Depth 6 -Compress
    function Send($guid, $axes, $buttons) { Invoke-Fm4InputLine $ui (@{ type = 'state'; guid = $guid; axes = $axes; buttons = $buttons; povs = @() } | ConvertTo-Json -Compress) }
    Invoke-Fm4InputLine $ui $devices
    if (@($ui.Devices).Count -ne 3 -or $ui.FfbCombo.Items.Count -lt 2) { throw 'Controls page did not list synthetic devices.' }
    Send $wheelGuid @(32768, 0) @(); Send $pedalGuid @(65535, 65535, 65535) @(); Send $boxGuid @(0) @()
    $row = { param($key) @($ui.Rows | Where-Object { $_.Key -eq $key })[0] }
    # Throttle on the pedal set, released high (inverted) and pressed to 3000.
    Start-Fm4Capture $ui (& $row 'throttle')
    Send $pedalGuid @(20000, 65535, 65535) @(); Send $pedalGuid @(3000, 65535, 65535) @()
    $ui.Capture.Detected = [DateTime]::UtcNow.AddSeconds(-2); Send $pedalGuid @(65000, 65535, 65535) @()
    $throttle = & $row 'throttle'
    if ($ui.Capture -or $throttle.Device -ne $pedalGuid -or $throttle.Axis -ne '#0' -or -not $throttle.Invert.Checked -or $throttle.Minimum -ne 3000 -or $throttle.Maximum -ne 65535) {
        throw "Throttle capture failed: $($throttle.Device) $($throttle.Axis) inv=$($throttle.Invert.Checked) $($throttle.Minimum)..$($throttle.Maximum)"
    }
    # Handbrake on its own device, not inverted.
    Start-Fm4Capture $ui (& $row 'handbrake')
    Send $boxGuid @(60000) @(); $ui.Capture.Detected = [DateTime]::UtcNow.AddSeconds(-2); Send $boxGuid @(500) @()
    $handbrake = & $row 'handbrake'
    if ($handbrake.Device -ne $boxGuid -or $handbrake.Axis -ne '#0' -or $handbrake.Invert.Checked) { throw 'Handbrake capture failed.' }
    # Steering turned right with a decreasing value means inverted.
    Start-Fm4Capture $ui (& $row 'steering')
    Send $wheelGuid @(5000, 0) @(); $ui.Capture.Detected = [DateTime]::UtcNow.AddSeconds(-2); Send $wheelGuid @(32800, 0) @()
    $steering = & $row 'steering'
    if ($steering.Device -ne $wheelGuid -or $steering.Axis -ne '#0' -or -not $steering.Invert.Checked -or $steering.Center -ne 32768) { throw 'Steering capture failed.' }
    # Brake captured on the throttle's axis frees the throttle.
    Start-Fm4Capture $ui (& $row 'brake')
    Send $pedalGuid @(10000, 65535, 65535) @(); $ui.Capture.Detected = [DateTime]::UtcNow.AddSeconds(-2); Send $pedalGuid @(65535, 65535, 65535) @()
    if ((& $row 'throttle').Axis -ne 'disabled' -or (& $row 'brake').Axis -ne '#0') { throw 'Axis conflict was not resolved.' }
    # Buttons: shift up on the wheel, then the same button for shift down frees shift up.
    $up = @($ui.Rows | Where-Object { $_.Kind -eq 'button' -and $_.Bit -eq 9 })[0]; $down = @($ui.Rows | Where-Object { $_.Kind -eq 'button' -and $_.Bit -eq 8 })[0]
    Start-Fm4Capture $ui $up; Send $wheelGuid @(32768, 0) @(5)
    if ($up.Device -ne $wheelGuid -or $up.Button -ne 5) { throw 'Button capture failed.' }
    Send $wheelGuid @(32768, 0) @()
    Start-Fm4Capture $ui $down; Send $wheelGuid @(32768, 0) @(5)
    if ($up.Button -ne 0 -or $down.Button -ne 5) { throw 'Button conflict was not resolved.' }
    # Gears go to the handbrake device's buttons (a shifter on a separate box).
    $first = @($ui.Rows | Where-Object { $_.Kind -eq 'gear' -and $_.Gear -eq 1 })[0]
    Start-Fm4Capture $ui $first; Send $boxGuid @(500) @(3)
    if ($ui.ShifterDevice -ne $boxGuid -or $first.Button -ne 3 -or -not $ui.ShifterEnabled.Checked) { throw 'Shifter capture failed.' }
    Set-Fm4FfbDevice $ui $wheelGuid
    $text = Write-Fm4BindingsToIni $ui "[wheel]`nenabled=1`n[custom]`nkeep=1`n"
    foreach ($check in @(@('wheel', 'device', $wheelGuid), @('steering', 'axis', '#0'), @('steering', 'invert', '1'), @('brake', 'device', $pedalGuid),
                         @('throttle', 'axis', 'disabled'), @('handbrake', 'device', $boxGuid), @('buttons', 'bit8', '5'), @('buttons', 'device8', $wheelGuid),
                         @('buttons', 'bit9', '0'), @('shifter', 'device', $boxGuid), @('shifter', 'gear1', '3'), @('shifter', 'enabled', '1'), @('custom', 'keep', '1'))) {
        if ((Get-Fm4IniValue $text $check[0] $check[1] '<missing>') -ne $check[2]) { throw "Bindings INI mismatch: [$($check[0])] $($check[1])" }
    }
    # Round trip back into the page.
    Read-Fm4BindingsFromIni $ui $text
    if ((& $row 'brake').Device -ne $pedalGuid -or (& $row 'steering').Invert.Checked -ne $true -or $first.Button -ne 3) { throw 'Bindings did not round-trip.' }
    # A timed-out capture leaves bindings unchanged.
    Start-Fm4Capture $ui (& $row 'clutch'); $ui.Capture.Started = [DateTime]::UtcNow.AddSeconds(-20); Step-Fm4Capture $ui
    if ($ui.Capture) { throw 'Capture timeout did not cancel cleanly.' }
}

# Renders the window to PNG for self-test previews. Native combo boxes do not draw into
# DrawToBitmap, so their current text is painted on top in the launcher's field style.
function Save-Fm4Preview($Form, [string]$Path) {
    function Initialize-Handles($control) { $null = $control.Handle; foreach ($child in $control.Controls) { Initialize-Handles $child } }
    Initialize-Handles $Form; $Form.PerformLayout()
    $bitmap = [Drawing.Bitmap]::new($Form.Width, $Form.Height)
    try {
        $Form.DrawToBitmap($bitmap, [Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height))
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $offset = $Form.PointToScreen([Drawing.Point]::Empty)
            $origin = [Drawing.Point]::new($offset.X - $Form.Left, $offset.Y - $Form.Top)
            function Paint-Combos($control) {
                foreach ($child in $control.Controls) {
                    if ($child -is [Windows.Forms.ComboBox]) {
                        $screen = $child.Parent.PointToScreen($child.Location); $client = $Form.PointToClient($screen)
                        $visible = $true; $parent = $child.Parent; $page = $null
                        while ($parent -and $parent -ne $Form) { if ($parent -is [Windows.Forms.TabPage]) { $page = $parent; if ($parent.Parent.SelectedTab -ne $parent) { $visible = $false } }; $parent = $parent.Parent }
                        if (-not $visible) { continue }
                        $box = [Drawing.Rectangle]::new($client.X + $origin.X, $client.Y + $origin.Y, $child.Width, $child.Height)
                        # Combos below a page's visible area must not paint over the footer.
                        if ($page) { $pageClient = $Form.PointToClient($page.PointToScreen([Drawing.Point]::Empty)); $graphics.SetClip([Drawing.Rectangle]::new($pageClient.X + $origin.X, $pageClient.Y + $origin.Y, $page.ClientSize.Width, $page.ClientSize.Height)) }
                        else { $graphics.ResetClip() }
                        $fill = [Drawing.SolidBrush]::new($(if ($child.Enabled) { [Fm4Ui.Palette]::Field } else { [Fm4Ui.Palette]::Raised })); $pen = [Drawing.Pen]::new([Fm4Ui.Palette]::Line)
                        try { $graphics.FillRectangle($fill, $box); $graphics.DrawRectangle($pen, $box.X, $box.Y, $box.Width - 1, $box.Height - 1) } finally { $fill.Dispose(); $pen.Dispose() }
                        $text = [Drawing.Rectangle]::new($box.X + 6, $box.Y, $box.Width - 30, $box.Height)
                        [Windows.Forms.TextRenderer]::DrawText($graphics, $child.Text, $child.Font, $text, $(if ($child.Enabled) { [Fm4Ui.Palette]::Text } else { [Fm4Ui.Palette]::Dim }),
                            [Windows.Forms.TextFormatFlags]::VerticalCenter -bor [Windows.Forms.TextFormatFlags]::EndEllipsis -bor [Windows.Forms.TextFormatFlags]::PreserveGraphicsClipping)
                        [Windows.Forms.TextRenderer]::DrawText($graphics, [string][char]0x25BE, $child.Font, [Drawing.Rectangle]::new($box.Right - 24, $box.Y, 20, $box.Height), [Fm4Ui.Palette]::Dim,
                            [Windows.Forms.TextFormatFlags]::VerticalCenter -bor [Windows.Forms.TextFormatFlags]::HorizontalCenter -bor [Windows.Forms.TextFormatFlags]::PreserveGraphicsClipping)
                    } elseif ($child.Controls.Count) { Paint-Combos $child }
                }
            }
            Paint-Combos $Form
        } finally { $graphics.Dispose() }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}

# Saves one preview per page, selecting each page through its navigation button state.
function Save-Fm4Previews($Shell, [string]$Directory) {
    $folder = [IO.Path]::GetFullPath($Directory); New-Item -ItemType Directory -Path $folder -Force | Out-Null
    foreach ($page in $Shell.Pages.TabPages) {
        $Shell.Pages.SelectedTab = $page
        foreach ($nav in $Shell.NavButtons) { $nav.Selected = ($nav.Tag -eq $page) }
        Save-Fm4Preview $Shell.Form (Join-Path $folder ($page.Text.Replace(' ', '-') + '.png'))
    }
    $Shell.Pages.SelectedIndex = 0
}
