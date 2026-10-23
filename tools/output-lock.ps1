# Hold one writer for a shared output directory. Dispose the returned handle.
# The file remains after release; the open handle, not its contents, owns it.
function Enter-OutputLock {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [string]$Label = 'output writer'
    )
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    [void][IO.Directory]::CreateDirectory($directoryPath)
    $lockPath = Join-Path $directoryPath '.output.lock'
    try {
        $handle = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    } catch [IO.IOException] {
        $code = $_.Exception.HResult -band 0xffff
        if ($code -eq 32 -or $code -eq 33) {
            throw "Another $Label owns $directoryPath. Wait for it to finish before retrying; do not delete the lock file."
        }
        throw
    }
    try {
        $record = [ordered]@{
            pid = $PID
            started_utc = [DateTime]::UtcNow.ToString('o')
            label = $Label
            directory = $directoryPath
        } | ConvertTo-Json -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($record + [Environment]::NewLine)
        $handle.SetLength(0)
        $handle.Write($bytes, 0, $bytes.Length)
        $handle.Flush()
        return $handle
    } catch {
        $handle.Dispose()
        throw
    }
}
