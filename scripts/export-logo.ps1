$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$imageFolder = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\images'
New-Item -ItemType Directory -Force $imageFolder | Out-Null
$bitmap = New-Object System.Drawing.Bitmap(512, 512)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$font = New-Object System.Drawing.Font('Segoe UI', 400, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(250,250,250))
$format = New-Object System.Drawing.StringFormat
try {
    $graphics.Clear([System.Drawing.Color]::FromArgb(22,22,22))
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $bounds = New-Object System.Drawing.RectangleF(0,0,512,512)
    $graphics.DrawString('A', $font, $brush, $bounds, $format)
    $bitmap.Save((Join-Path $imageFolder 'logo.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $format.Dispose(); $brush.Dispose(); $font.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
