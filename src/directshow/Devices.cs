// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace DvDirectShow;

internal sealed class RawPin : IDisposable
{
    public IPin Pin { get; }
    public AmMediaType MediaType;
    public string Id { get; }
    public RawPin(IPin pin, AmMediaType mediaType, string id) => (Pin, MediaType, Id) = (pin, mediaType, id);
    public void Dispose() { Ds.FreeType(ref MediaType); Ds.Release(Pin); }
}

internal static class Devices
{
    internal static bool MatchesTarget(string id, string name, string devicePath)
    {
        string paths = id + " " + devicePath;
        bool exactUsb = paths.Contains("vid_04da", StringComparison.OrdinalIgnoreCase) &&
                        paths.Contains("pid_231a", StringComparison.OrdinalIgnoreCase);
        bool namedPanasonic = name.Contains("Panasonic", StringComparison.OrdinalIgnoreCase) ||
                              name.Contains("Matsushita", StringComparison.OrdinalIgnoreCase);
        return exactUsb || namedPanasonic;
    }

    private static string ReadProperty(IPropertyBag bag, string property) =>
        bag.Read(property, out object value, IntPtr.Zero) >= 0 ? Convert.ToString(value) ?? "" : "";

    // Binding and inspecting pins does not run the graph or issue tape-control writes.
    private static void Enumerate(Action<string, string, string, IMoniker, IBindCtx> visit)
    {
        object devEnumObject = Ds.Create(Ds.SystemDeviceEnum);
        IEnumMoniker? enumerator = null;
        IBindCtx? bind = null;
        try
        {
            Ds.Check(Ds.CreateBindCtx(0, out bind), "CreateBindCtx");
            Guid category = Ds.VideoInputCategory;
            int hr = ((ICreateDevEnum)devEnumObject).CreateClassEnumerator(ref category, out enumerator, 0);
            if (hr == 1 || enumerator is null) return;
            Ds.Check(hr, "Enumerate video devices");
            var monikers = new IMoniker[1];
            while (enumerator.Next(1, monikers, IntPtr.Zero) == 0)
            {
                IMoniker moniker = monikers[0];
                object? bagObject = null;
                try
                {
                    moniker.GetDisplayName(bind, null, out string id);
                    Guid iid = typeof(IPropertyBag).GUID;
                    moniker.BindToStorage(bind, null, ref iid, out bagObject);
                    var bag = (IPropertyBag)bagObject;
                    visit(id, ReadProperty(bag, "FriendlyName"), ReadProperty(bag, "DevicePath"), moniker, bind);
                }
                finally { Ds.Release(bagObject); Ds.Release(moniker); }
            }
        }
        finally { Ds.Release(enumerator); Ds.Release(bind); Ds.Release(devEnumObject); }
    }

    private static IBaseFilter Bind(IMoniker moniker, IBindCtx context)
    {
        Guid iid = typeof(IBaseFilter).GUID;
        moniker.BindToObject(context, null, ref iid, out object source);
        return (IBaseFilter)source;
    }

    public static int List()
    {
        int count = 0;
        Enumerate((id, name, path, moniker, bind) =>
        {
            if (!MatchesTarget(id, name, path)) return;
            IBaseFilter? source = null;
            try
            {
                source = Bind(moniker, bind);
                using RawPin? raw = FindRawCapturePin(source);
                if (raw is null)
                {
                    JsonOutput.Write(new { type = "info", backend = "directshow", id, name,
                        supported = false, reason = "no_raw_dvsd_capture_pin" });
                    return;
                }
                JsonOutput.Write(new { type = "device", backend = "directshow", id, name,
                    supported = true, pin_id = raw.Id, subtype = "dvsd", native_driver = true,
                    transport = source is IAMExtTransport });
                count++;
            }
            catch (Exception ex)
            {
                JsonOutput.Write(new { type = "info", backend = "directshow", id, name,
                    supported = false, reason = "device_probe_failed", message = ex.Message });
            }
            finally { Ds.Release(source); }
        });
        JsonOutput.Write(new { type = "list_done", backend = "directshow", devices = count });
        return 0;
    }

    public static IBaseFilter Open(string selectedId)
    {
        IBaseFilter? result = null;
        Enumerate((id, name, path, moniker, bind) =>
        {
            if (id != selectedId || !MatchesTarget(id, name, path)) return;
            if (result is null) result = Bind(moniker, bind);
        });
        return result ?? throw new InvalidOperationException("The selected Panasonic MotionDV device is no longer available.");
    }

    private static bool IsCapturePin(IPin pin)
    {
        if (pin is not IKsPropertySet properties) return false;
        IntPtr data = Marshal.AllocCoTaskMem(16);
        try
        {
            Guid propertySet = Ds.PinPropertySet;
            int hr = properties.Get(ref propertySet, 0, IntPtr.Zero, 0, data, 16, out uint returned);
            return hr >= 0 && returned == 16 && Marshal.PtrToStructure<Guid>(data) == Ds.CaptureCategory;
        }
        finally { Marshal.FreeCoTaskMem(data); }
    }

    public static RawPin? FindRawCapturePin(IBaseFilter filter)
    {
        Ds.Check(filter.EnumPins(out IEnumPins enumerator), "EnumPins");
        try
        {
            var pins = new IPin[1];
            while (enumerator.Next(1, pins, IntPtr.Zero) == 0)
            {
                IPin pin = pins[0];
                bool retained = false;
                try
                {
                    if (pin.QueryDirection(out int direction) < 0 || direction != 1 || !IsCapturePin(pin)) continue;
                    if (pin.EnumMediaTypes(out IEnumMediaTypes mediaEnumerator) < 0) continue;
                    try
                    {
                        var pointers = new IntPtr[1];
                        while (mediaEnumerator.Next(1, pointers, IntPtr.Zero) == 0)
                        {
                            IntPtr pointer = pointers[0];
                            var media = Marshal.PtrToStructure<AmMediaType>(pointer);
                            Marshal.FreeCoTaskMem(pointer);
                            if (Ds.IsDv(media))
                            {
                                pin.QueryId(out string pinId);
                                retained = true;
                                return new RawPin(pin, media, pinId ?? "capture");
                            }
                            Ds.FreeType(ref media);
                        }
                    }
                    finally { Ds.Release(mediaEnumerator); }
                }
                finally { if (!retained) Ds.Release(pin); }
            }
            return null;
        }
        finally { Ds.Release(enumerator); }
    }

    public static IPin GetPin(IBaseFilter filter, int direction)
    {
        Ds.Check(filter.EnumPins(out IEnumPins enumerator), "EnumPins");
        try
        {
            var pins = new IPin[1];
            while (enumerator.Next(1, pins, IntPtr.Zero) == 0)
            {
                IPin pin = pins[0];
                if (pin.QueryDirection(out int actual) >= 0 && actual == direction) return pin;
                Ds.Release(pin);
            }
            throw new InvalidOperationException("The required DirectShow filter pin was not available.");
        }
        finally { Ds.Release(enumerator); }
    }
}
