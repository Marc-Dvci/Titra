# Boots Titra's image under QEMU for Windows with the Windows Hypervisor Platform, headless. No window opens.
#
#   powershell -File qnx/image/run_whpx.ps1 -Image DIR [-Models FILE] [-Qemu PATH]
#
# DIR holds ifs.bin, disk-qemu.vmdk and disk-qemu, from an image built with TITRA_STARTUP_OPTS="-f 0,0,200M".
#
# QNX 8.0's startup needs the always-running APIC timer bit in CPUID leaf 6, which Hyper-V hides from its guests. QEMU
# answers that leaf itself, so "+arat,x-force-features=on" puts the bit back; the timer does keep running in a VM.
# AVX-512 stays off: the model is built for AVX2, the instruction set of the boards this could run on.
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$Models = "",
    [string]$Qemu = "qemu-system-x86_64.exe"
)
$noAvx512 = "-avx512f,-avx512dq,-avx512ifma,-avx512cd,-avx512bw,-avx512vl,-avx512vbmi,-avx512vbmi2,-avx512vnni,-avx512bitalg,-avx512-vpopcntdq,-avx512-bf16"
Remove-Item (Join-Path $Image "console.log") -ErrorAction SilentlyContinue
$a = @("-accel", "whpx", "-machine", "pc", "-cpu", "host,+arat,x-force-features=on,$noAvx512", "-smp", "4", "-m", "8G",
    "-drive", "file=disk-qemu.vmdk,if=ide,id=drv0",
    "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:8080",
    "-device", "virtio-net-pci,netdev=net0", "-object", "rng-builtin,id=rng0", "-device", "virtio-rng-pci,rng=rng0",
    "-kernel", "ifs.bin", "-display", "none", "-vga", "none",
    "-chardev", "socket,id=s0,host=127.0.0.1,port=5500,server=on,wait=off,logfile=console.log", "-serial", "chardev:s0",
    "-pidfile", "qemu.pid")
if ($Models) { $a += @("-drive", "file=$Models,if=ide,index=1,media=disk,format=raw,snapshot=on") }
$p = Start-Process -FilePath $Qemu -WorkingDirectory $Image -ArgumentList $a -PassThru -WindowStyle Hidden `
    -RedirectStandardError (Join-Path $Image "qemu.err") -RedirectStandardOutput (Join-Path $Image "qemu.out")
"started pid $($p.Id); ssh on 127.0.0.1:2222, station on http://127.0.0.1:8080, console on 127.0.0.1:5500"
