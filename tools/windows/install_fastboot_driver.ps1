$ErrorActionPreference = 'Stop'

$code = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public class FastbootDriverInstaller {
    private const int DIGCF_PRESENT = 0x00000002;
    private const int DIGCF_ALLCLASSES = 0x00000004;
    private const int SPDIT_CLASSDRIVER = 0x00000001;
    private const uint DI_ENUMSINGLEINF = 0x00010000;
    private const uint DI_FLAGSEX_ALLOWEXCLUDEDDRVS = 0x00000800;
    private const int MAX_PATH = 260;
    private const int LINE_LEN = 256;

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    public struct SP_DEVINSTALL_PARAMS {
        public uint cbSize;
        public uint Flags;
        public uint FlagsEx;
        public IntPtr hwndParent;
        public IntPtr InstallMsgHandler;
        public IntPtr InstallMsgHandlerContext;
        public IntPtr FileQueue;
        public IntPtr ClassInstallReserved;
        public uint Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = MAX_PATH)]
        public string DriverPath;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    public struct SP_DRVINFO_DATA {
        public uint cbSize;
        public uint DriverType;
        public IntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = LINE_LEN)]
        public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = LINE_LEN)]
        public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = LINE_LEN)]
        public string ProviderName;
        public System.Runtime.InteropServices.ComTypes.FILETIME DriverDate;
        public ulong DriverVersion;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(IntPtr ClassGuid, string Enumerator, IntPtr hwndParent, int Flags);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiOpenDeviceInfo(IntPtr DeviceInfoSet, string DeviceInstanceId, IntPtr hwndParent, uint OpenFlags, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiGetDeviceInstallParams(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, ref SP_DEVINSTALL_PARAMS DeviceInstallParams);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiSetDeviceInstallParams(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, ref SP_DEVINSTALL_PARAMS DeviceInstallParams);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiBuildDriverInfoList(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int DriverType);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiEnumDriverInfo(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int DriverType, uint MemberIndex, ref SP_DRVINFO_DATA DriverInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiSetSelectedDriver(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, ref SP_DRVINFO_DATA DriverInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool SetupDiSetDeviceRegistryProperty(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, uint Property, byte[] PropertyBuffer, uint PropertyBufferSize);

    [DllImport("newdev.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern bool DiInstallDevice(IntPtr hwndParent, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, ref SP_DRVINFO_DATA DriverInfoData, uint Flags, out bool NeedReboot);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    public static void Install(string instanceId, string infPath) {
        IntPtr devSet = SetupDiGetClassDevs(IntPtr.Zero, null, IntPtr.Zero, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (devSet == new IntPtr(-1)) throw new Exception("SetupDiGetClassDevs failed: " + Marshal.GetLastWin32Error());
        try {
            SP_DEVINFO_DATA devData = new SP_DEVINFO_DATA();
            devData.cbSize = (uint)Marshal.SizeOf(devData);
            if (!SetupDiOpenDeviceInfo(devSet, instanceId, IntPtr.Zero, 0, ref devData))
                throw new Exception("SetupDiOpenDeviceInfo failed: " + Marshal.GetLastWin32Error());

            // Set ClassGUID to AndroidUsbDeviceClass {3f966bd9-fa04-4ec5-991c-d326973b5128} (SPDRP_CLASSGUID = 8)
            string classGuidStr = "{3f966bd9-fa04-4ec5-991c-d326973b5128}\0";
            byte[] guidBytes = Encoding.Unicode.GetBytes(classGuidStr);
            SetupDiSetDeviceRegistryProperty(devSet, ref devData, 8, guidBytes, (uint)guidBytes.Length);
            devData.ClassGuid = new Guid("3f966bd9-fa04-4ec5-991c-d326973b5128");

            SP_DEVINSTALL_PARAMS installParams = new SP_DEVINSTALL_PARAMS();
            installParams.cbSize = (uint)Marshal.SizeOf(installParams);
            if (!SetupDiGetDeviceInstallParams(devSet, ref devData, ref installParams))
                throw new Exception("SetupDiGetDeviceInstallParams failed: " + Marshal.GetLastWin32Error());

            installParams.Flags |= DI_ENUMSINGLEINF;
            installParams.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
            installParams.DriverPath = infPath;

            if (!SetupDiSetDeviceInstallParams(devSet, ref devData, ref installParams))
                throw new Exception("SetupDiSetDeviceInstallParams failed: " + Marshal.GetLastWin32Error());

            if (!SetupDiBuildDriverInfoList(devSet, ref devData, SPDIT_CLASSDRIVER))
                throw new Exception("SetupDiBuildDriverInfoList failed: " + Marshal.GetLastWin32Error());

            SP_DRVINFO_DATA drvData = new SP_DRVINFO_DATA();
            drvData.cbSize = (uint)Marshal.SizeOf(drvData);
            uint idx = 0;
            bool found = false;
            while (SetupDiEnumDriverInfo(devSet, ref devData, SPDIT_CLASSDRIVER, idx++, ref drvData)) {
                Console.WriteLine("Found driver: " + drvData.Description);
                if (drvData.Description.Contains("Bootloader") || idx == 1) {
                    found = true;
                    if (drvData.Description.Contains("Bootloader")) break;
                }
            }
            if (!found) throw new Exception("No driver found in INF: " + Marshal.GetLastWin32Error());

            if (!SetupDiSetSelectedDriver(devSet, ref devData, ref drvData))
                throw new Exception("SetupDiSetSelectedDriver failed: " + Marshal.GetLastWin32Error());

            bool reboot;
            if (!DiInstallDevice(IntPtr.Zero, devSet, ref devData, ref drvData, 0, out reboot))
                throw new Exception("DiInstallDevice failed: " + Marshal.GetLastWin32Error());

            Console.WriteLine("Installed successfully! Reboot needed: " + reboot);
        } finally {
            SetupDiDestroyDeviceInfoList(devSet);
        }
    }
}
"@

Add-Type -TypeDefinition $code
[FastbootDriverInstaller]::Install("USB\VID_0E8D&PID_201C\202512000191", "C:\Windows\INF\oem8.inf")
