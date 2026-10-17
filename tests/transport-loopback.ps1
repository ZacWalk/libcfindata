#requires -Version 7.4
param([Parameter(Mandatory)][string]$TestExecutable)
$ErrorActionPreference = 'Stop'

# The listener stays in this process; only the test executable is a child.
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$process = [Diagnostics.Process]::new()
$started = $false
try {
    $listener.Start()
    $process.StartInfo.FileName = (Resolve-Path -LiteralPath $TestExecutable).Path
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.Environment['FG_TEST_HTTP_BASE'] = "http://127.0.0.1:$($listener.LocalEndpoint.Port)"
    $started = $process.Start()
    if (-not $started) { throw 'Cannot start native transport test.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $expected = @('/ok', '/post', '/seed', '/cookie', '/cookie', '/cookie', '/redirect', '/large', '/slow')
    $count = 0
    while (-not $process.HasExited) {
        if ($timer.Elapsed.TotalSeconds -gt 60) { throw 'Native transport test timed out.' }
        if (-not $listener.Pending()) {
            Start-Sleep -Milliseconds 10
            continue
        }
        $connection = $listener.AcceptTcpClient()
        try {
            $stream = $connection.GetStream()
            $stream.ReadTimeout = 5000
            $stream.WriteTimeout = 5000
            $headerBytes = [Collections.Generic.List[byte]]::new()
            do {
                $b = $stream.ReadByte()
                if ($b -lt 0) { throw 'Incomplete loopback request headers.' }
                $headerBytes.Add([byte]$b)
                $n = $headerBytes.Count
                if ($n -gt 65536) { throw 'Loopback request headers too large.' }
            } until ($n -ge 4 -and $headerBytes[$n-4] -eq 13 -and $headerBytes[$n-3] -eq 10 -and
                     $headerBytes[$n-2] -eq 13 -and $headerBytes[$n-1] -eq 10)
            $lines = [Text.Encoding]::ASCII.GetString($headerBytes.ToArray()) -split "`r`n"
            $request = $lines[0].Split(' ')
            if ($request.Count -ne 3 -or $count -ge $expected.Count -or
                $request[1] -cne $expected[$count]) { throw 'Unexpected loopback request.' }
            $headers = @{}
            foreach ($line in $lines[1..($lines.Count-1)]) {
                if (-not $line) { continue }
                $parts = $line.Split(':', 2)
                if ($parts.Count -ne 2 -or $headers.ContainsKey($parts[0])) {
                    throw 'Invalid or duplicate loopback request header.'
                }
                $headers[$parts[0]] = $parts[1].Trim()
            }
            if ($headers['User-Agent'] -cne 'offline-loopback-test') { throw 'Missing caller user agent.' }
            if ($count -eq 4) {
                if ($headers['Cookie'] -cne 'seed=retained') { throw 'Explicit Cookie was not sent exactly.' }
            } elseif ($headers.ContainsKey('Cookie')) { throw 'Native transport sent an implicit Cookie.' }
            if ($request[1] -in @('/ok', '/redirect')) {
                if (-not $headers['Authorization']) { throw 'Missing explicit authorization.' }
            } elseif ($headers.ContainsKey('Authorization')) { throw 'Native transport retained authorization.' }
            $method = if ($request[1] -eq '/post') { 'POST' } else { 'GET' }
            if ($request[0] -cne $method) { throw 'Unexpected HTTP method.' }
            $length = 0
            if ($headers.ContainsKey('Content-Length')) {
                if (-not [int]::TryParse($headers['Content-Length'], [ref]$length) -or
                    $length -lt 0 -or $length -gt 1024) { throw 'Invalid loopback content length.' }
            }
            $body = [byte[]]::new($length)
            for ($offset = 0; $offset -lt $length;) {
                $read = $stream.Read($body, $offset, $length - $offset)
                if (-not $read) { throw 'Incomplete loopback request body.' }
                $offset += $read
            }
            $status = '200 OK'
            $extra = ''
            $text = ''
            switch ($request[1]) {
                '/ok' { $text = 'success' }
                '/post' {
                    if ($length -ne 3 -or $body[0] -ne 97 -or $body[1] -ne 0 -or $body[2] -ne 98) {
                        throw 'Binary POST body did not round-trip.'
                    }
                }
                '/seed' {
                    $status = '404 Not Found'
                    $extra = "Set-Cookie: seed=retained; Path=/`r`n"
                    $text = 'cookie seed'
                }
                '/cookie' { $text = if ($count -eq 4) { 'cookie accepted' } else { 'no cookie' } }
                '/redirect' {
                    $status = '302 Found'
                    $extra = "Location: http://127.0.0.1:$($listener.LocalEndpoint.Port)/unexpected`r`n"
                    $text = 'not followed'
                }
                '/large' { $text = 'x' * 1024 }
                '/slow' { Start-Sleep -Milliseconds 400 }
            }
            ++$count
            if ($request[1] -ne '/slow') {
                if ($request[1] -ne '/post') { $body = [Text.Encoding]::ASCII.GetBytes($text) }
                $reply = [Text.Encoding]::ASCII.GetBytes(
                    "HTTP/1.1 $status`r`n${extra}Content-Length: $($body.Length)`r`nConnection: close`r`n`r`n")
                $stream.Write($reply, 0, $reply.Length)
                $stream.Write($body, 0, $body.Length)
            }
        } finally {
            $connection.Dispose()
        }
    }
    $process.WaitForExit()
    $output = $stdout.GetAwaiter().GetResult()
    $errors = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0 -or $count -ne $expected.Count -or
        $output -notmatch 'native loopback tests passed' -or $errors) {
        throw "Native transport regression failed ($count requests, exit $($process.ExitCode)).`n$output`n$errors"
    }
    Write-Output $output.TrimEnd()
} finally {
    $listener.Stop()
    if ($started -and -not $process.HasExited) {
        $process.Kill($true)
        $process.WaitForExit()
    }
    $process.Dispose()
}
