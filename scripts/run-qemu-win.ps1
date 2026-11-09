# Boot the QNX SDP 8.0 quickstart image with QEMU for Windows, using the
# Windows Hypervisor Platform (WHPX) for hardware acceleration.
#
# Why this exists: inside WSL2 the CPU virtualization flag is not exposed
# (Windows VBS is active), so QEMU there falls back to TCG emulation, which is
# slow and makes timing measurements meaningless. QEMU on Windows can use WHPX.
#
# Image files (made once from the WSL image, see README):
#   $VmDir\ifs.bin           - QNX boot image (kernel + startup)
#   $VmDir\disk-qemu.qcow2   - root disk, converted from output/disk-qemu
#
# Guest SSH is forwarded to 127.0.0.1:$SshPort. With WSL networkingMode=mirrored,
# WSL reaches it at the same address.
#
# Usage: powershell -ExecutionPolicy Bypass -File run-qemu-win.ps1 [start|stop|status|log] [-Tcg]
param(
    [ValidateSet('start', 'stop', 'status', 'log')]
    [string]$Action = 'start',
    [switch]$Tcg,                       # force software emulation (for comparison)
    [switch]$UserIrqchip,               # WHPX with kernel-irqchip=off (fallback if default fails)
    [string]$VmDir = 'D:\qnx-vm',
    [int]$SshPort = 2223,               # 2222 stays free for the WSL/TCG VM
    # 2 vCPUs leave host cores free, so Windows has less reason to preempt
    # the VM's virtual CPUs (which shows up as millisecond stalls in the guest).
    [int]$Cpus = 2,
    # Windows priority class of the QEMU process. High reduces host-side
    # preemption of the VM; RealTime needs an elevated shell and can starve
    # the host, so it is not the default.
    [ValidateSet('Normal', 'AboveNormal', 'High', 'RealTime')]
    [string]$HostPriority = 'High',
    [string]$Ram = '1G'
)
$ErrorActionPreference = 'Stop'

$Qemu    = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$PidFile = Join-Path $VmDir 'qemu.pid'
$Log     = Join-Path $VmDir 'qemu.out'

function Get-VmProcess {
    if (-not (Test-Path $PidFile)) { return $null }
    $p = Get-Process -Id (Get-Content $PidFile) -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -like 'qemu-system*') { return $p }
    return $null
}

function Start-Vm {
    if (Get-VmProcess) { Write-Host "QEMU already running (pid $(Get-Content $PidFile))"; return }
    if (-not (Test-Path $Qemu)) { throw "QEMU not found at $Qemu (winget install SoftwareFreedomConservancy.QEMU)" }

    if ($Tcg) {
        $accel = @('-accel', 'tcg', '-cpu', 'max')
        Write-Host 'Using TCG emulation (forced)'
    } else {
        # Default: the interrupt controller is emulated by the Windows
        # hypervisor (fast). -UserIrqchip moves it into QEMU, which some
        # guests need, but it slows timer interrupts badly.
        $irq = if ($UserIrqchip) { 'off' } else { 'on' }
        $accel = @('-accel', "whpx,kernel-irqchip=$irq", '-cpu', 'max')
        Write-Host "Using WHPX acceleration (kernel-irqchip=$irq)"
    }

    Remove-Item $Log -ErrorAction SilentlyContinue
    # Forward slashes keep QEMU's option parser happy with Windows paths.
    $disk = (Join-Path $VmDir 'disk-qemu.qcow2') -replace '\\', '/'
    # WHPX needs the boot image with the corrected timer frequency (made by
    # tools/patch_ifs_timer.py); with the original one the guest clock runs
    # five times too slow. TCG uses the original image.
    $ifsName = if ($Tcg -or $UserIrqchip) { 'ifs.bin' } else { 'ifs-whpx.bin' }
    $ifs  = (Join-Path $VmDir $ifsName) -replace '\\', '/'
    if (-not (Test-Path (Join-Path $VmDir $ifsName))) {
        throw "$ifsName not found in $VmDir; run tools/patch_ifs_timer.py first"
    }
    $logq = $Log -replace '\\', '/'

    $qemuArgs = @('-smp', $Cpus) + $accel + @(
        '-m', $Ram,
        '-drive', "file=$disk,if=ide,id=drv0,format=qcow2",
        '-netdev', "user,id=net0,hostfwd=tcp:127.0.0.1:${SshPort}-:22",
        '-device', 'virtio-net-pci,netdev=net0',
        '-device', 'virtio-rng-pci',
        '-kernel', $ifs,
        '-serial', "file:$logq",
        '-display', 'none', '-monitor', 'none'
    )
    $started = Get-Date
    $p = Start-Process -FilePath $Qemu -ArgumentList $qemuArgs -WindowStyle Hidden -PassThru
    $p.Id | Set-Content $PidFile
    try {
        $p.PriorityClass = $HostPriority
        Write-Host "QEMU host priority: $HostPriority, vCPUs: $Cpus"
    } catch {
        Write-Host "Could not set host priority $HostPriority ($($_.Exception.Message)); continuing"
    }

    # Wait for a login prompt or shell on the serial console.
    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { throw "QEMU exited with code $($p.ExitCode); see $Log" }
        if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern 'login:|^# ?$' -Quiet)) {
            $secs = [int]((Get-Date) - $started).TotalSeconds
            # Windows' OpenSSH default cipher fails against this guest
            # ("Corrupted MAC on input"); aes128-ctr works.
            Write-Host "QNX booted in ${secs}s. SSH: ssh -c aes128-ctr -p $SshPort qnxuser@127.0.0.1"
            return
        }
    }
    Write-Host 'Timed out waiting for boot; last console lines:'
    Get-Content $Log -Tail 20 -ErrorAction SilentlyContinue
}

function Stop-Vm {
    $p = Get-VmProcess
    if ($p) { Stop-Process -Id $p.Id -Force; Write-Host 'QEMU stopped' } else { Write-Host 'QEMU not running' }
    Remove-Item $PidFile -ErrorAction SilentlyContinue
}

switch ($Action) {
    'start'  { Start-Vm }
    'stop'   { Stop-Vm }
    'status' { if (Get-VmProcess) { "running (pid $(Get-Content $PidFile))" } else { 'stopped' } }
    'log'    { Get-Content $Log -Tail 40 }
}
