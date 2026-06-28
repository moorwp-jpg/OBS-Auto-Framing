param(
    [string]$Command
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$cmakeBin = "C:\Program Files\CMake\bin"
$cmakeExe = Join-Path $cmakeBin "cmake.exe"

if (-not (Test-Path -LiteralPath $cmakeExe -PathType Leaf)) {
    throw "CMake was not found at $cmakeExe. Update scripts/start_clean_project_shell.ps1 if it is installed elsewhere."
}

# Codex and some terminal hosts can inherit PATH and Path as separate keys.
# MSBuild rejects that environment, so launch the child shell with one canonical Path key.
if (-not ("ObsAutoFraming.CleanEnvironmentProcess" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace ObsAutoFraming
{
    public static class CleanEnvironmentProcess
    {
        private const uint CreateUnicodeEnvironment = 0x00000400;
        private const uint Infinite = 0xFFFFFFFF;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int cb;
            public string lpReserved;
            public string lpDesktop;
            public string lpTitle;
            public int dwX;
            public int dwY;
            public int dwXSize;
            public int dwYSize;
            public int dwXCountChars;
            public int dwYCountChars;
            public int dwFillAttribute;
            public int dwFlags;
            public short wShowWindow;
            public short cbReserved2;
            public IntPtr lpReserved2;
            public IntPtr hStdInput;
            public IntPtr hStdOutput;
            public IntPtr hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            public IntPtr hProcess;
            public IntPtr hThread;
            public int dwProcessId;
            public int dwThreadId;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcess(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string currentDirectory,
            ref StartupInfo startupInfo,
            out ProcessInformation processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        public static int Run(string fileName, string arguments, string workingDirectory, string cmakeBin)
        {
            var environmentEntries = new List<string>();
            var seenKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string inheritedPath = null;

            foreach (DictionaryEntry entry in Environment.GetEnvironmentVariables()) {
                var key = (string)entry.Key;
                var value = (string)entry.Value;
                if (!seenKeys.Add(key))
                    continue;

                if (string.Equals(key, "Path", StringComparison.OrdinalIgnoreCase)) {
                    if (inheritedPath == null)
                        inheritedPath = value;
                    continue;
                }

                environmentEntries.Add(key + "=" + value);
            }

            if (string.IsNullOrWhiteSpace(inheritedPath))
                throw new InvalidOperationException("The inherited environment does not contain a usable PATH value.");

            environmentEntries.Add("Path=" + cmakeBin + ";" + inheritedPath);
            var environmentBlock = string.Join("\0", environmentEntries.ToArray()) + "\0\0";
            var environmentPointer = Marshal.StringToHGlobalUni(environmentBlock);
            var startupInfo = new StartupInfo { cb = Marshal.SizeOf(typeof(StartupInfo)) };
            var commandLine = new StringBuilder("\"" + fileName + "\" " + arguments);
            ProcessInformation processInformation;

            try {
                if (!CreateProcess(
                        fileName,
                        commandLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        false,
                        CreateUnicodeEnvironment,
                        environmentPointer,
                        workingDirectory,
                        ref startupInfo,
                        out processInformation)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not start the clean project shell.");
                }

                try {
                    WaitForSingleObject(processInformation.hProcess, Infinite);
                    uint exitCode;
                    if (!GetExitCodeProcess(processInformation.hProcess, out exitCode))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not read the clean project shell exit code.");
                    return unchecked((int)exitCode);
                }
                finally {
                    CloseHandle(processInformation.hThread);
                    CloseHandle(processInformation.hProcess);
                }
            }
            finally {
                Marshal.FreeHGlobal(environmentPointer);
            }
        }
    }
}
'@
}

$shell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
$startupCommand = @"
`$ProgressPreference = 'SilentlyContinue'
Set-Location -LiteralPath '$($projectRoot.Replace("'", "''"))'
[Console]::WriteLine('Project environment active.')
[Console]::WriteLine('CMake: $cmakeExe')
cmake --version
"@
if (-not [string]::IsNullOrWhiteSpace($Command)) {
    $startupCommand += "`r`n$Command"
}
$encodedStartupCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($startupCommand))

if ([string]::IsNullOrWhiteSpace($Command)) {
    $shellArguments = "-NoExit -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encodedStartupCommand"
} else {
    $shellArguments = "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encodedStartupCommand"
}
exit [ObsAutoFraming.CleanEnvironmentProcess]::Run($shell, $shellArguments, $projectRoot, $cmakeBin)
