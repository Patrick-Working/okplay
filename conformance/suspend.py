"""Freeze a test process for a while, then let it go on (S3 'dead' scenario: a machine that went silent
long enough to be dropped comes back). Finds the process by a piece of its command line.

  python suspend.py <command-line-part> <seconds>
"""
import ctypes, subprocess, sys, time

part, secs = sys.argv[1], float(sys.argv[2])
out = subprocess.run(['powershell', '-NoProfile', '-Command',
                      "Get-CimInstance Win32_Process -Filter \"Name='dptest.exe'\" | "
                      "Where-Object { $_.CommandLine -like '*" + part + "*' } | ForEach-Object { $_.ProcessId }"],
                     capture_output=True, text=True).stdout.split()
if not out:
    print('suspend: no dptest.exe with', part)
    sys.exit(1)
pid = int(out[0])
PROCESS_SUSPEND_RESUME = 0x0800
h = ctypes.windll.kernel32.OpenProcess(PROCESS_SUSPEND_RESUME, False, pid)
ntdll = ctypes.windll.ntdll
print('suspend: pid %d frozen for %.0f s' % (pid, secs), flush=True)
ntdll.NtSuspendProcess(h)
time.sleep(secs)
ntdll.NtResumeProcess(h)
ctypes.windll.kernel32.CloseHandle(h)
print('suspend: pid %d resumed' % pid)
