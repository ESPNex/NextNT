/*
 * PROJECT:     LiberNT Wine Mono payload
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Windows Runtime projection for assemblies that reference the Windows metadata
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Reflection;
using System.Runtime.InteropServices;

[assembly: AssemblyVersion("255.255.255.255")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace Windows.Internal
{
    static class WinRT
    {
        const int CO_E_NOTINITIALIZED = unchecked((int)0x800401F0);
        const int RPC_E_CHANGED_MODE = unchecked((int)0x80010106);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        internal delegate int GetPointer(IntPtr self, out IntPtr value);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        internal delegate int GetInt32(IntPtr self, out int value);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        internal delegate int GetBoolean(IntPtr self, out byte value);

        [DllImport("combase.dll", ExactSpelling = true)]
        static extern int RoGetActivationFactory(IntPtr activatableClassId, ref Guid iid, out IntPtr factory);

        [DllImport("combase.dll", ExactSpelling = true)]
        static extern int RoInitialize(int initType);

        [DllImport("combase.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        static extern int WindowsCreateString(string sourceString, int length, out IntPtr hstring);

        [DllImport("combase.dll", ExactSpelling = true)]
        static extern int WindowsDeleteString(IntPtr hstring);

        internal static IntPtr GetActivationFactory(string className, Guid iid)
        {
            IntPtr hstring, factory;
            int hr;

            Check(WindowsCreateString(className, className.Length, out hstring));
            try
            {
                hr = RoGetActivationFactory(hstring, ref iid, out factory);
                if (hr == CO_E_NOTINITIALIZED)
                {
                    hr = RoInitialize(1);
                    if (hr < 0 && hr != RPC_E_CHANGED_MODE)
                        Check(hr);
                    hr = RoGetActivationFactory(hstring, ref iid, out factory);
                }
                Check(hr);
                return factory;
            }
            finally
            {
                WindowsDeleteString(hstring);
            }
        }

        internal static T Method<T>(IntPtr instance, int slot) where T : class
        {
            IntPtr vtable = Marshal.ReadIntPtr(instance);
            IntPtr function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
        }

        internal static void Check(int hr)
        {
            if (hr < 0)
                Marshal.ThrowExceptionForHR(hr);
        }
    }
}

namespace Windows.Networking.Connectivity
{
    using Windows.Internal;

    public enum NetworkCostType
    {
        Unknown = 0,
        Unrestricted = 1,
        Fixed = 2,
        Variable = 3,
    }

    public static class NetworkInformation
    {
        static readonly Guid IID_INetworkInformationStatics = new Guid("5074F851-950D-4165-9C15-365619481EEA");

        public static ConnectionProfile GetInternetConnectionProfile()
        {
            IntPtr statics = WinRT.GetActivationFactory("Windows.Networking.Connectivity.NetworkInformation", IID_INetworkInformationStatics);
            try
            {
                IntPtr profile;
                WinRT.Check(WinRT.Method<WinRT.GetPointer>(statics, 7)(statics, out profile));
                return profile == IntPtr.Zero ? null : new ConnectionProfile(profile);
            }
            finally
            {
                Marshal.Release(statics);
            }
        }
    }

    public sealed class ConnectionProfile
    {
        readonly IntPtr Abi;

        internal ConnectionProfile(IntPtr abi)
        {
            Abi = abi;
        }

        ~ConnectionProfile()
        {
            Marshal.Release(Abi);
        }

        public ConnectionCost GetConnectionCost()
        {
            IntPtr cost;
            WinRT.Check(WinRT.Method<WinRT.GetPointer>(Abi, 9)(Abi, out cost));
            return cost == IntPtr.Zero ? null : new ConnectionCost(cost);
        }
    }

    public sealed class ConnectionCost
    {
        readonly IntPtr Abi;

        internal ConnectionCost(IntPtr abi)
        {
            Abi = abi;
        }

        ~ConnectionCost()
        {
            Marshal.Release(Abi);
        }

        public NetworkCostType NetworkCostType
        {
            get
            {
                int value;
                WinRT.Check(WinRT.Method<WinRT.GetInt32>(Abi, 6)(Abi, out value));
                return (NetworkCostType)value;
            }
        }

        public bool Roaming
        {
            get { return GetBoolean(7); }
        }

        public bool OverDataLimit
        {
            get { return GetBoolean(8); }
        }

        public bool ApproachingDataLimit
        {
            get { return GetBoolean(9); }
        }

        bool GetBoolean(int slot)
        {
            byte value;
            WinRT.Check(WinRT.Method<WinRT.GetBoolean>(Abi, slot)(Abi, out value));
            return value != 0;
        }
    }
}
