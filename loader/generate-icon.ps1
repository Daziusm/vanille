$ErrorActionPreference = "Stop"

$loaderRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vanilleRoot = Join-Path (Split-Path -Parent $loaderRoot) "Vanille"
$src = Join-Path $vanilleRoot "assets\loader_icon.png"
if (-not (Test-Path $src)) {
    $src = Join-Path $vanilleRoot "assets\logo_icon.png"
}
$dst = Join-Path $loaderRoot "ChocolaLoader\icon.ico"

if (-not (Test-Path $src)) {
    throw "Loader icon source not found: $src"
}

Add-Type -AssemblyName System.Drawing

function Get-CroppedSquareBitmap([System.Drawing.Image]$image) {
    $side = [Math]::Min($image.Width, $image.Height)
    $x = [int](($image.Width - $side) / 2)
    if ($image.Height -gt $image.Width) {
        $y = [int](($image.Height - $side) * 0.12)
    }
    else {
        $y = [int](($image.Height - $side) / 2)
    }
    $y = [Math]::Max(0, [Math]::Min($y, $image.Height - $side))

    $bmp = New-Object System.Drawing.Bitmap $side, $side
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($image, 0, 0, (New-Object System.Drawing.Rectangle $x, $y, $side, $side), [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $g.Dispose()
    }
    return $bmp
}

$srcImg = [System.Drawing.Image]::FromFile($src)
try {
    $cropped = Get-CroppedSquareBitmap $srcImg
    try {
        $size = 256
        $bmp = New-Object System.Drawing.Bitmap $size, $size
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $g.Clear([System.Drawing.Color]::Transparent)
                $g.DrawImage($cropped, 0, 0, $size, $size)
            }
            finally {
                $g.Dispose()
            }

            $hIcon = $bmp.GetHicon()
            $icon = [System.Drawing.Icon]::FromHandle($hIcon)
            try {
                $fs = [System.IO.File]::Create($dst)
                try {
                    $icon.Save($fs)
                }
                finally {
                    $fs.Close()
                }
            }
            finally {
                $icon.Dispose()
            }
        }
        finally {
            $bmp.Dispose()
        }
    }
    finally {
        $cropped.Dispose()
    }
}
finally {
    $srcImg.Dispose()
}

Write-Host "Generated loader icon: $dst"
