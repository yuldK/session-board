#requires -Version 7.2
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$asset_directory = Join-Path $PSScriptRoot '../assets'
[xml]$source = Get-Content -LiteralPath (Join-Path $asset_directory 'session-board.svg') -Raw

function New-IconBitmap([int]$size) {
    # 작은 아이콘도 모서리와 상태 점을 살리도록 크게 그린 뒤 줄인다.
    $render_size = [Math]::Max(1024, $size)
    $canvas = [System.Drawing.Bitmap]::new($render_size, $render_size)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $bitmap = [System.Drawing.Bitmap]::new($size, $size)
    $output = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.ScaleTransform($render_size / 256.0, $render_size / 256.0)
        foreach ($shape in $source.DocumentElement.ChildNodes) {
            if ($shape.LocalName -in 'title', 'desc') {
                continue
            }
            if ($shape.LocalName -notin 'rect', 'circle') {
                throw "Unsupported SVG shape: $($shape.LocalName)"
            }
            $brush = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml($shape.GetAttribute('fill')))
            $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
            try {
                if ($shape.LocalName -eq 'circle') {
                    $radius = [float]$shape.GetAttribute('r')
                    $graphics.FillEllipse($brush, [float]$shape.GetAttribute('cx') - $radius, [float]$shape.GetAttribute('cy') - $radius, 2 * $radius, 2 * $radius)
                }
                else {
                    $x = [float]$shape.GetAttribute('x')
                    $y = [float]$shape.GetAttribute('y')
                    $width = [float]$shape.GetAttribute('width')
                    $height = [float]$shape.GetAttribute('height')
                    $diameter = 2 * [float]$shape.GetAttribute('rx')
                    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
                    $path.AddArc($x + $width - $diameter, $y, $diameter, $diameter, 270, 90)
                    $path.AddArc($x + $width - $diameter, $y + $height - $diameter, $diameter, $diameter, 0, 90)
                    $path.AddArc($x, $y + $height - $diameter, $diameter, $diameter, 90, 90)
                    $path.CloseFigure()
                    $graphics.FillPath($brush, $path)
                }
            }
            finally {
                $path.Dispose()
                $brush.Dispose()
            }
        }
        $output.Clear([System.Drawing.Color]::Transparent)
        $output.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $output.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $output.DrawImage($canvas, [System.Drawing.Rectangle]::new(0, 0, $size, $size))
        return ,$bitmap
    }
    catch {
        $bitmap.Dispose()
        throw
    }
    finally {
        $output.Dispose()
        $graphics.Dispose()
        $canvas.Dispose()
    }
}

$preview = New-IconBitmap 512
try {
    $preview.Save((Join-Path $asset_directory 'session-board.png'), [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $preview.Dispose()
}

# Windows의 배율별 크기를 각각 담는다. 256px 항목의 디렉터리 크기 값은 0이다.
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = foreach ($size in $sizes) {
    $bitmap = New-IconBitmap $size
    $stream = [System.IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        [pscustomobject]@{ size = $size; data = $stream.ToArray() }
    }
    finally {
        $stream.Dispose()
        $bitmap.Dispose()
    }
}
$file = [System.IO.File]::Create((Join-Path $asset_directory 'session-board.ico'))
$writer = [System.IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.size -eq 256) { 0 } else { $frame.size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.data.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.data.Length
    }
    foreach ($frame in $frames) {
        $writer.Write([byte[]]$frame.data)
    }
}
finally {
    $writer.Dispose()
}
Write-Output 'Generated assets/session-board.png and session-board.ico from session-board.svg.'
