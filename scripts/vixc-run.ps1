param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Command
)

$ErrorActionPreference = 'Stop'
if ($Command.Count -eq 0) {
    [Console]::Error.WriteLine('vixc-run: missing compiler command')
    exit 125
}

$asLimit = if ($env:VIXC_AS_LIMIT) { [UInt64]$env:VIXC_AS_LIMIT } else { 0 }
$stackLimit = if ($env:VIXC_STACK_LIMIT) { [UInt64]$env:VIXC_STACK_LIMIT } else { 0 }
if ($stackLimit -ne 0) {
    [Console]::Error.WriteLine('vixc-run: VIXC_STACK_LIMIT cannot be changed safely for an existing Windows PE image')
    exit 125
}

if ($asLimit -ne 0) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class VixJob {
    [StructLayout(LayoutKind.Sequential)]
    public struct BasicLimits {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct IoCounters {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ExtendedLimits {
        public BasicLimits BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll")]
    public static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref ExtendedLimits info, uint length);
    [DllImport("kernel32.dll")]
    public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $Command[0]
$startInfo.UseShellExecute = $false
if ($startInfo.PSObject.Properties.Name -notcontains 'ArgumentList') {
    [Console]::Error.WriteLine('vixc-run: this PowerShell/.NET version cannot preserve argv safely; PowerShell 7 is required')
    exit 125
}
for ($i = 1; $i -lt $Command.Count; $i++) { [void]$startInfo.ArgumentList.Add($Command[$i]) }

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
[void]$process.Start()
$job = [IntPtr]::Zero
try {
    if ($asLimit -ne 0) {
        $job = [VixJob]::CreateJobObject([IntPtr]::Zero, $null)
        if ($job -eq [IntPtr]::Zero) { throw 'CreateJobObject failed' }
        $limits = [VixJob+ExtendedLimits]::new()
        $limits.BasicLimitInformation.LimitFlags = 0x100 # JOB_OBJECT_LIMIT_PROCESS_MEMORY
        $limits.ProcessMemoryLimit = [UIntPtr]::new($asLimit)
        $size = [Runtime.InteropServices.Marshal]::SizeOf($limits)
        if (-not [VixJob]::SetInformationJobObject($job, 9, [ref]$limits, $size)) { throw 'SetInformationJobObject failed' }
        if (-not [VixJob]::AssignProcessToJobObject($job, $process.Handle)) { throw 'AssignProcessToJobObject failed' }
    }
    $process.WaitForExit()
    $status = $process.ExitCode
    if ($env:VIXC_MEMORY_LOG) {
        "peak_working_set=$($process.PeakWorkingSet64)B exit=$status" | Set-Content -LiteralPath $env:VIXC_MEMORY_LOG
    }
    if ($status -ne 0 -and $asLimit -ne 0) {
        [Console]::Error.WriteLine("vixc-run: memory limit exceeded or child failed under configured Job Object limit; exit=$status VIXC_AS_LIMIT=$asLimit")
    }
    exit $status
}
catch {
    if (-not $process.HasExited) { $process.Kill() }
    [Console]::Error.WriteLine("vixc-run: Windows Job Object setup failed: $($_.Exception.Message)")
    exit 125
}
finally {
    if ($job -ne [IntPtr]::Zero) { [void][VixJob]::CloseHandle($job) }
    $process.Dispose()
}
