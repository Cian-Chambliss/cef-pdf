param(
    [string]$ExePath
)

$ErrorActionPreference = "Stop"
$Utf8 = New-Object System.Text.UTF8Encoding($false, $true)
$Ascii = [System.Text.Encoding]::ASCII
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$OutputDir = Join-Path $RepoRoot "build\streamed-smoke\windows"
$Process = $null
$StderrTask = $null

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Resolve-CefPdf([string]$Requested) {
    if ($Requested) {
        $resolved = [System.IO.Path]::GetFullPath($Requested)
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "cef-pdf executable not found: $resolved"
        }
        return $resolved
    }

    $candidates = @(
        "build\src\Release\cef-pdf.exe",
        "src\Release\cef-pdf.exe",
        "build\Release\cef-pdf.exe",
        "Release\cef-pdf.exe",
        "build\src\cef-pdf.exe"
    )
    foreach ($candidate in $candidates) {
        $path = Join-Path $RepoRoot $candidate
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    throw "cef-pdf.exe was not found; pass its path as the first argument"
}

function Write-Frame([System.IO.Stream]$Stream, $Packet) {
    $json = $Packet | ConvertTo-Json -Depth 12 -Compress
    $body = $Utf8.GetBytes($json)
    $header = $Ascii.GetBytes("Content-Length: $($body.Length)`r`nContent-Type: application/json`r`n`r`n")
    $Stream.Write($header, 0, $header.Length)
    $Stream.Write($body, 0, $body.Length)
    $Stream.Flush()
}

function Read-Frames([byte[]]$Bytes) {
    $frames = New-Object System.Collections.ArrayList
    $offset = 0
    while ($offset -lt $Bytes.Length) {
        $headerEnd = -1
        for ($i = $offset; $i -le $Bytes.Length - 4; $i++) {
            if ($Bytes[$i] -eq 13 -and $Bytes[$i + 1] -eq 10 -and
                $Bytes[$i + 2] -eq 13 -and $Bytes[$i + 3] -eq 10) {
                $headerEnd = $i
                break
            }
            if ($i - $offset -gt 16384) { throw "response header exceeds 16 KiB" }
        }
        Assert-True ($headerEnd -ge 0) "response has an incomplete header"
        $header = $Ascii.GetString($Bytes, $offset, $headerEnd - $offset)
        $match = [regex]::Match($header, "(?im)^Content-Length:\s*([0-9]+)\s*$")
        Assert-True $match.Success "response has no valid Content-Length header"
        $length = [int]$match.Groups[1].Value
        $bodyStart = $headerEnd + 4
        Assert-True (($bodyStart + $length) -le $Bytes.Length) "response body is shorter than Content-Length"
        $json = $Utf8.GetString($Bytes, $bodyStart, $length)
        [void]$frames.Add(($json | ConvertFrom-Json))
        $offset = $bodyStart + $length
    }
    return ,$frames.ToArray()
}

function Read-U32BE([byte[]]$Bytes, [int]$Offset) {
    return ([uint32]$Bytes[$Offset] -shl 24) -bor
        ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
        ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
        [uint32]$Bytes[$Offset + 3]
}

function Get-PngDimensions([byte[]]$Bytes) {
    $signature = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
    Assert-True ($Bytes.Length -ge 24) "PNG output is too short"
    for ($i = 0; $i -lt $signature.Length; $i++) {
        Assert-True ($Bytes[$i] -eq $signature[$i]) "PNG signature is invalid"
    }
    return @((Read-U32BE $Bytes 16), (Read-U32BE $Bytes 20))
}

function Get-JpegDimensions([byte[]]$Bytes) {
    Assert-True ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xD8) "JPEG signature is invalid"
    Assert-True ($Bytes[$Bytes.Length - 2] -eq 0xFF -and $Bytes[$Bytes.Length - 1] -eq 0xD9) "JPEG end marker is invalid"
    $sof = @(0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF)
    $offset = 2
    while ($offset -lt $Bytes.Length) {
        while ($offset -lt $Bytes.Length -and $Bytes[$offset] -ne 0xFF) { $offset++ }
        while ($offset -lt $Bytes.Length -and $Bytes[$offset] -eq 0xFF) { $offset++ }
        if ($offset -ge $Bytes.Length) { break }
        $marker = $Bytes[$offset]
        $offset++
        if ($marker -eq 0xD8 -or $marker -eq 0xD9 -or ($marker -ge 0xD0 -and $marker -le 0xD7)) { continue }
        Assert-True (($offset + 1) -lt $Bytes.Length) "JPEG segment is truncated"
        $segmentLength = ([int]$Bytes[$offset] -shl 8) -bor [int]$Bytes[$offset + 1]
        Assert-True ($segmentLength -ge 2 -and ($offset + $segmentLength) -le $Bytes.Length) "JPEG segment length is invalid"
        if ($sof -contains $marker) {
            Assert-True ($segmentLength -ge 7) "JPEG SOF segment is too short"
            $height = ([int]$Bytes[$offset + 3] -shl 8) -bor [int]$Bytes[$offset + 4]
            $width = ([int]$Bytes[$offset + 5] -shl 8) -bor [int]$Bytes[$offset + 6]
            return @($width, $height)
        }
        $offset += $segmentLength
    }
    throw "JPEG has no supported SOF marker"
}

try {
    $ExePath = Resolve-CefPdf $ExePath
    if (Test-Path -LiteralPath $OutputDir) {
        Remove-Item -LiteralPath $OutputDir -Recurse -Force
    }
    [void](New-Item -ItemType Directory -Path $OutputDir -Force)

    $html = [System.IO.File]::ReadAllText((Join-Path $RepoRoot "tests\fixtures\streamed-rich.html"), $Utf8)
    $svg = [System.IO.File]::ReadAllText((Join-Path $RepoRoot "tests\fixtures\streamed-rich.svg"), $Utf8)
    $requests = @(
        [ordered]@{
            id = "pdf-1"; command = "render"
            input = [ordered]@{ type = "html"; content = $html }
            output = [ordered]@{ path = "build/streamed-smoke/windows/page.pdf"; format = "pdf" }
            options = [ordered]@{ size = "A4"; margin = "10"; backgrounds = $true }
        },
        [ordered]@{
            id = "png-1"; command = "render"
            input = [ordered]@{ type = "html"; content = $html }
            output = [ordered]@{ path = "build/streamed-smoke/windows/page.png"; format = "png" }
            options = [ordered]@{ capture = "viewport"; viewWidth = 360; viewHeight = 240 }
        },
        [ordered]@{
            id = "jpeg-1"; command = "render"
            input = [ordered]@{ type = "svg"; content = $svg }
            output = [ordered]@{ path = "build/streamed-smoke/windows/page.jpg"; format = "jpeg" }
            options = [ordered]@{ capture = "viewport"; viewWidth = 320; viewHeight = 180; quality = 82; imageBackground = "#ffffff" }
        },
        [ordered]@{ id = "quit-1"; command = "quit" }
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $ExePath
    $startInfo.Arguments = "--streamed --disable-gpu"
    $startInfo.WorkingDirectory = $RepoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $startInfo
    Assert-True ($Process.Start()) "failed to start cef-pdf"

    $stdout = New-Object System.IO.MemoryStream
    $stdoutTask = $Process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $StderrTask = $Process.StandardError.ReadToEndAsync()
    foreach ($request in $requests) { Write-Frame $Process.StandardInput.BaseStream $request }
    $Process.StandardInput.Close()

    if (-not $Process.WaitForExit(120000)) {
        $Process.Kill()
        throw "cef-pdf did not exit within 120 seconds"
    }
    Assert-True ($stdoutTask.Wait(5000)) "stdout did not close after cef-pdf exited"
    Assert-True ($StderrTask.Wait(5000)) "stderr did not close after cef-pdf exited"
    Assert-True ($Process.ExitCode -eq 0) "cef-pdf exited with code $($Process.ExitCode)`n$($StderrTask.Result)"

    $responses = Read-Frames $stdout.ToArray()
    Assert-True ($responses.Count -eq 4) "expected 4 responses, received $($responses.Count)"
    $expectedIds = @("pdf-1", "png-1", "jpeg-1", "quit-1")
    $seen = @{}
    foreach ($response in $responses) {
        $id = [string]$response.id
        Assert-True ($expectedIds -contains $id) "unexpected response id: $id"
        Assert-True (-not $seen.ContainsKey($id)) "duplicate response id: $id"
        Assert-True ($response.status -eq "success") "request $id returned status '$($response.status)': $($response.message)"
        $seen[$id] = $true
    }
    foreach ($id in $expectedIds) { Assert-True ($seen.ContainsKey($id)) "missing response id: $id" }
    Assert-True ([string]$responses[-1].id -eq "quit-1") "quit response was not last"

    $pdf = [System.IO.File]::ReadAllBytes((Join-Path $OutputDir "page.pdf"))
    Assert-True ($pdf.Length -ge 5 -and $Ascii.GetString($pdf, 0, 5) -eq "%PDF-") "PDF signature is invalid"
    $pdfEndLength = [Math]::Min(1024, $pdf.Length)
    $pdfEnd = $Ascii.GetString($pdf, $pdf.Length - $pdfEndLength, $pdfEndLength)
    Assert-True ($pdfEnd.Contains("%%EOF")) "PDF end marker is missing"
    $pngSize = Get-PngDimensions ([System.IO.File]::ReadAllBytes((Join-Path $OutputDir "page.png")))
    Assert-True ($pngSize[0] -eq 360 -and $pngSize[1] -eq 240) "PNG dimensions are $($pngSize[0])x$($pngSize[1]), expected 360x240"
    $jpegSize = Get-JpegDimensions ([System.IO.File]::ReadAllBytes((Join-Path $OutputDir "page.jpg")))
    Assert-True ($jpegSize[0] -eq 320 -and $jpegSize[1] -eq 180) "JPEG dimensions are $($jpegSize[0])x$($jpegSize[1]), expected 320x180"

    Write-Host "streamed smoke passed: PDF, PNG 360x240, JPEG 320x180"
    exit 0
} catch {
    if ($Process) {
        try { if (-not $Process.HasExited) { $Process.Kill() } } catch {}
    }
    [Console]::Error.WriteLine("streamed smoke failed: " + $_.Exception.Message)
    if ($StderrTask -and $StderrTask.IsCompleted -and $StderrTask.Result) {
        [Console]::Error.WriteLine($StderrTask.Result)
    }
    exit 1
}
