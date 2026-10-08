// SPDX-License-Identifier: GPL-3.0-or-later
// ABI declarations checked against Microsoft's Windows SDK headers (see README).
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

namespace DvDirectShow;

internal static class Ds
{
    public static readonly Guid SystemDeviceEnum = new("62be5d10-60eb-11d0-bd3b-00a0c911ce86");
    public static readonly Guid VideoInputCategory = new("860bb310-5d01-11d0-bd3b-00a0c911ce86");
    public static readonly Guid FilterGraph = new("e436ebb3-524f-11ce-9f53-0020af0ba770");
    public static readonly Guid SampleGrabber = new("c1f400a0-3f08-11d3-9f0b-006008039e37");
    public static readonly Guid NullRenderer = new("c1f400a4-3f08-11d3-9f0b-006008039e37");
    public static readonly Guid Dvsd = new("64737664-0000-0010-8000-00aa00389b71");
    public static readonly Guid Video = new("73646976-0000-0010-8000-00aa00389b71");
    public static readonly Guid Interleaved = new("73766169-0000-0010-8000-00aa00389b71");
    public static readonly Guid PinPropertySet = new("9b00f101-1567-11d1-b3f1-00aa003761c5");
    public static readonly Guid CaptureCategory = new("fb6c4281-0353-11d1-905f-0000c0cc16ba");
    public const int EcComplete = 0x01, EcErrorAbort = 0x03, EcDeviceLost = 0x1f;
    // Xprtdefs.h: ED_BASE is 0x1000; these are playback modes, never recording.
    public const int ModePlay = 0x1000 + 200, ModeStop = 0x1000 + 201, ModeRewind = 0x1000 + 205;

    [DllImport("ole32.dll", ExactSpelling = true)]
    internal static extern int CreateBindCtx(uint reserved, out IBindCtx bindContext);

    public static object Create(Guid clsid) => Activator.CreateInstance(Type.GetTypeFromCLSID(clsid, true)!)!;
    public static void Check(int hr, string operation)
    {
        if (hr < 0) throw new COMException($"{operation}: 0x{hr:x8}", hr);
    }
    public static bool IsDv(AmMediaType mt) => mt.Subtype == Dvsd && (mt.MajorType == Video || mt.MajorType == Interleaved);
    public static void Release(object? obj)
    {
        if (obj is not null && Marshal.IsComObject(obj)) Marshal.ReleaseComObject(obj);
    }
    public static void FreeType(ref AmMediaType mt)
    {
        if (mt.FormatPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(mt.FormatPtr);
        if (mt.Unknown != IntPtr.Zero) Marshal.Release(mt.Unknown);
        mt.FormatPtr = mt.Unknown = IntPtr.Zero;
        mt.FormatSize = 0;
    }
}

[StructLayout(LayoutKind.Sequential)]
internal struct AmMediaType
{
    public Guid MajorType, Subtype;
    public int FixedSizeSamples, TemporalCompression;
    public uint SampleSize;
    public Guid FormatType;
    public IntPtr Unknown;
    public uint FormatSize;
    public IntPtr FormatPtr;
}

[ComImport, Guid("29840822-5b84-11d0-bd3b-00a0c911ce86"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface ICreateDevEnum
{
    [PreserveSig] int CreateClassEnumerator(ref Guid category, out IEnumMoniker? enumerator, int flags);
}

[ComImport, Guid("55272a00-42cb-11ce-8135-00aa004bb851"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IPropertyBag
{
    [PreserveSig] int Read([MarshalAs(UnmanagedType.LPWStr)] string name, [MarshalAs(UnmanagedType.Struct)] out object value, IntPtr errorLog);
    [PreserveSig] int Write([MarshalAs(UnmanagedType.LPWStr)] string name, [MarshalAs(UnmanagedType.Struct)] ref object value);
}

// Prefixes include every inherited slot through the last method used.
[ComImport, Guid("56a86895-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IBaseFilter
{
    [PreserveSig] int GetClassID(out Guid clsid);
    [PreserveSig] int Stop();
    [PreserveSig] int Pause();
    [PreserveSig] int Run(long start);
    [PreserveSig] int GetState(int timeout, out int state);
    [PreserveSig] int SetSyncSource(IntPtr clock);
    [PreserveSig] int GetSyncSource(out IntPtr clock);
    [PreserveSig] int EnumPins(out IEnumPins enumerator);
    [PreserveSig] int FindPin([MarshalAs(UnmanagedType.LPWStr)] string id, out IPin pin);
}

[ComImport, Guid("56a86899-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMediaFilter
{
    [PreserveSig] int GetClassID(out Guid clsid);
    [PreserveSig] int Stop();
    [PreserveSig] int Pause();
    [PreserveSig] int Run(long start);
    [PreserveSig] int GetState(int timeout, out int state);
    [PreserveSig] int SetSyncSource(IntPtr clock);
    [PreserveSig] int GetSyncSource(out IntPtr clock);
}

[ComImport, Guid("56a8689f-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IFilterGraph
{
    [PreserveSig] int AddFilter(IBaseFilter filter, [MarshalAs(UnmanagedType.LPWStr)] string name);
    [PreserveSig] int RemoveFilter(IBaseFilter filter);
    [PreserveSig] int EnumFilters(out IntPtr enumerator);
    [PreserveSig] int FindFilterByName([MarshalAs(UnmanagedType.LPWStr)] string name, out IBaseFilter filter);
    [PreserveSig] int ConnectDirect(IPin output, IPin input, IntPtr mediaType);
    [PreserveSig] int Reconnect(IPin pin);
    [PreserveSig] int Disconnect(IPin pin);
    [PreserveSig] int SetDefaultSyncSource();
}

[ComImport, Guid("56a86892-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IEnumPins
{
    [PreserveSig] int Next(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] IPin[] pins, IntPtr fetched);
    [PreserveSig] int Skip(uint count);
    [PreserveSig] int Reset();
    [PreserveSig] int Clone(out IEnumPins enumerator);
}

[ComImport, Guid("56a86891-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IPin
{
    [PreserveSig] int Connect(IPin receivePin, IntPtr mediaType);
    [PreserveSig] int ReceiveConnection(IPin connector, IntPtr mediaType);
    [PreserveSig] int Disconnect();
    [PreserveSig] int ConnectedTo(out IPin pin);
    [PreserveSig] int ConnectionMediaType(out AmMediaType mediaType);
    [PreserveSig] int QueryPinInfo(IntPtr info);
    [PreserveSig] int QueryDirection(out int direction);
    [PreserveSig] int QueryId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int QueryAccept(IntPtr mediaType);
    [PreserveSig] int EnumMediaTypes(out IEnumMediaTypes enumerator);
}

[ComImport, Guid("89c31040-846b-11ce-97d3-00aa0055595a"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IEnumMediaTypes
{
    [PreserveSig] int Next(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] IntPtr[] mediaTypes, IntPtr fetched);
    [PreserveSig] int Skip(uint count);
    [PreserveSig] int Reset();
    [PreserveSig] int Clone(out IEnumMediaTypes enumerator);
}

[ComImport, Guid("31efac30-515c-11d0-a9aa-00aa0061be93"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IKsPropertySet
{
    [PreserveSig] int Set(ref Guid propertySet, uint id, IntPtr instance, uint instanceSize, IntPtr data, uint dataSize);
    [PreserveSig] int Get(ref Guid propertySet, uint id, IntPtr instance, uint instanceSize, IntPtr data, uint dataSize, out uint returned);
    [PreserveSig] int QuerySupported(ref Guid propertySet, uint id, out uint support);
}

[ComImport, Guid("6b652fff-11fe-4fce-92ad-0266b5d7c78f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface ISampleGrabber
{
    [PreserveSig] int SetOneShot([MarshalAs(UnmanagedType.Bool)] bool oneShot);
    [PreserveSig] int SetMediaType(ref AmMediaType mediaType);
    [PreserveSig] int GetConnectedMediaType(out AmMediaType mediaType);
    [PreserveSig] int SetBufferSamples([MarshalAs(UnmanagedType.Bool)] bool buffer);
    [PreserveSig] int GetCurrentBuffer(ref int length, IntPtr buffer);
    [PreserveSig] int GetCurrentSample(out IntPtr sample);
    [PreserveSig] int SetCallback(ISampleGrabberCB? callback, int method);
}

[ComVisible(true), Guid("0579154a-2b53-4994-b0d0-e773148eff85"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ISampleGrabberCB
{
    [PreserveSig] int SampleCB(double time, IntPtr sample);
    [PreserveSig] int BufferCB(double time, IntPtr buffer, int length);
}

[ComImport, Guid("56a868b6-0ad4-11ce-b03a-0020af0ba770"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
internal interface IMediaEvent
{
    [PreserveSig] int GetEventHandle(out IntPtr handle);
    [PreserveSig] int GetEvent(out int code, out IntPtr param1, out IntPtr param2, int timeout);
    [PreserveSig] int WaitForCompletion(int timeout, out int code);
    [PreserveSig] int CancelDefaultHandling(int code);
    [PreserveSig] int RestoreDefaultHandling(int code);
    [PreserveSig] int FreeEventParams(int code, IntPtr param1, IntPtr param2);
}

[ComImport, Guid("a03cd5f0-3045-11cf-8c44-00aa006b6814"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAMExtTransport
{
    [PreserveSig] int GetCapability(int capability, out int value, out double doubleValue);
    [PreserveSig] int put_MediaState(int state);
    [PreserveSig] int get_MediaState(out int state);
    [PreserveSig] int put_LocalControl(int state);
    [PreserveSig] int get_LocalControl(out int state);
    [PreserveSig] int GetStatus(int item, out int value);
    [PreserveSig] int GetTransportBasicParameters(int param, ref int value, ref IntPtr data);
    [PreserveSig] int SetTransportBasicParameters(int param, int value, [MarshalAs(UnmanagedType.LPWStr)] string data);
    [PreserveSig] int GetTransportVideoParameters(int param, out int value);
    [PreserveSig] int SetTransportVideoParameters(int param, int value);
    [PreserveSig] int GetTransportAudioParameters(int param, out int value);
    [PreserveSig] int SetTransportAudioParameters(int param, int value);
    [PreserveSig] int put_Mode(int mode);
    [PreserveSig] int get_Mode(out int mode);
}
