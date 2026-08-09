# Convert PPM frames produced by ekit_boids into PNG images and an animated GIF.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File render.ps1 [-Dir frames] [-Fps 30] [-Out boids.gif] [-PngDir png]

param(
    [string]$Dir = "frames",
    [int]$Fps = 30,
    [string]$Out = "boids.gif",
    [string]$PngDir = "png"
)

Add-Type -AssemblyName System.Drawing

function Read-PpmToken($reader) {
    $sb = [System.Text.StringBuilder]::new()
    while ($true) {
        $b = $reader.ReadByte()
        if ($b -lt 0) { break }
        $ch = [char]$b
        if ($ch -eq '#') {
            # skip to end of line
            while ($true) {
                $b2 = $reader.ReadByte()
                if ($b2 -lt 0 -or [char]$b2 -eq "`n") { break }
            }
            continue
        }
        if ($ch -match '\s') {
            if ($sb.Length -gt 0) { break }
            continue
        }
        [void]$sb.Append($ch)
    }
    return $sb.ToString()
}

function Read-Ppm([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        $magic = Read-PpmToken $reader
        if ($magic -ne "P6") { throw "unsupported PPM magic '$magic' in $Path" }
        $w = [int](Read-PpmToken $reader)
        $h = [int](Read-PpmToken $reader)
        $maxval = [int](Read-PpmToken $reader)
        if ($maxval -ne 255) { throw "unsupported maxval $maxval in $Path" }
        $data = $reader.ReadBytes($w * $h * 3)
        if ($data.Length -lt $w * $h * 3) { throw "truncated PPM data in $Path" }
        return @{ W = $w; H = $h; Data = $data }
    } finally {
        $stream.Dispose()
    }
}

function Ppm-ToBitmap($ppm) {
    $w = $ppm.W
    $h = $ppm.H
    $bmp = [System.Drawing.Bitmap]::new($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    try {
        $stride = $data.Stride
        $scan0 = $data.Scan0
        for ($y = 0; $y -lt $h; $y++) {
            $src = $y * $w * 3
            # GDI+ bitmaps store rows bottom-up, so flip vertically.
            $dst = $h - 1 - $y
            [System.Runtime.InteropServices.Marshal]::Copy($ppm.Data, $src,
                [IntPtr]::Add($scan0, $dst * $stride), $w * 3)
        }
    } finally {
        $bmp.UnlockBits($data)
    }
    return $bmp
}

$files = Get-ChildItem -Path $Dir -Filter *.ppm | Sort-Object Name
if ($files.Count -eq 0) { Write-Error "no .ppm files found in '$Dir'"; exit 1 }
Write-Host "found $($files.Count) frames"

if ($PngDir -and $PngDir.Length -gt 0) {
    New-Item -ItemType Directory -Force -Path $PngDir | Out-Null
}

$frames = [System.Collections.Generic.List[System.Drawing.Bitmap]]::new()
foreach ($f in $files) {
    $ppm = Read-Ppm $f.FullName
    $bmp = Ppm-ToBitmap $ppm
    if ($PngDir) {
        $pngPath = Join-Path $PngDir ([System.IO.Path]::GetFileNameWithoutExtension($f.Name) + ".png")
        $bmp.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $frames.Add($bmp)
}

# --- Assemble an animated GIF ---------------------------------------------
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
    Where-Object { $_.MimeType -eq 'image/gif' }
if (-not $codec) { Write-Error "GIF encoder not available"; exit 1 }

$w = $frames[0].Width
$h = $frames[0].Height
$delay = [int](100 / $Fps)   # hundredths of a second

$gif = [System.Drawing.Bitmap]::new($w, $h)
$g = [System.Drawing.Graphics]::FromImage($gif)
$g.Clear([System.Drawing.Color]::Black)
$g.DrawImage($frames[0], 0, 0, $w, $h)
$g.Dispose()

# Frame delay property (0x5100) so the GIF plays at the requested FPS.
# PropertyItem has no public constructor; create one via reflection.
$propCtor = [System.Drawing.Imaging.PropertyItem].GetConstructor(
    [System.Reflection.BindingFlags]'Instance,NonPublic', $null, [Type[]]@(), $null)
function New-FrameDelayProperty([int]$DelayMs) {
    $p = $propCtor.Invoke(@())
    $p.Id = 0x5100          # PropertyTagFrameDelay
    $p.Type = 4             # PropertyTagTypeLong
    $p.Len = 4
    $p.Value = [System.BitConverter]::GetBytes([int]$DelayMs)
    return $p
}
$gif.SetPropertyItem((New-FrameDelayProperty $delay))

$params = New-Object System.Drawing.Imaging.EncoderParameters(1)
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
    [System.Drawing.Imaging.Encoder]::SaveFlag, [long][System.Drawing.Imaging.EncoderValue]::MultiFrame)
$gif.Save($Out, $codec, $params)

for ($i = 1; $i -lt $frames.Count; $i++) {
    $frames[$i].SetPropertyItem((New-FrameDelayProperty $delay))
    $params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
        [System.Drawing.Imaging.Encoder]::SaveFlag, [long][System.Drawing.Imaging.EncoderValue]::FrameDimensionTime)
    $gif.SaveAdd($frames[$i], $params)
}
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
    [System.Drawing.Imaging.Encoder]::SaveFlag, [long][System.Drawing.Imaging.EncoderValue]::Flush)
$gif.SaveAdd($params)
$gif.Dispose()

foreach ($bmp in $frames) { $bmp.Dispose() }

Write-Host "wrote $Out ($($frames.Count) frames, ${Fps} fps)"
