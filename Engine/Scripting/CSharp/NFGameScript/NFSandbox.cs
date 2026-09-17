using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NF.GameScript;

// Blittable mirror of nf::scripting::NetVec3 (3 x float, sequential).
[StructLayout(LayoutKind.Sequential)]
public struct Vec3f
{
    public float X;
    public float Y;
    public float Z;
}

// Managed gameplay sandbox for the SANAD C# hosting tests.
//
// Every entry point is a static UnmanagedCallersOnly method with blittable
// types only (ints, floats, raw pointers, the long entity id): no GC handles,
// no strings, no exceptions cross the boundary — a fault returns a sentinel
// (-1 / no-op) instead of crashing the host. Multi-threading note: the host
// calls these on the game thread; the per-entity state below is locked so a
// future worker-thread tick cannot corrupt it.
public static unsafe class NFSandbox
{
    private static readonly Dictionary<long, int> s_counters = new();
    private static readonly object s_lock = new();

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Add(int a, int b)
    {
        return a + b;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static float Scale(float x, float factor)
    {
        return x * factor;
    }

    // Copies UTF-8 input into the caller's buffer. Returns bytes written, or
    // -1 when a pointer is null, a length is negative, or it does not fit.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Echo(byte* input, int inputLen, byte* output, int outputCap)
    {
        if (input == null || output == null || inputLen < 0 || outputCap < 0)
        {
            return -1;
        }
        if (inputLen > outputCap)
        {
            return -1;
        }
        new Span<byte>(input, inputLen).CopyTo(new Span<byte>(output, outputCap));
        return inputLen;
    }

    // Integrates position by velocity * dt (null pointers are a no-op).
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void StepVec3(Vec3f* pos, Vec3f* vel, float dt)
    {
        if (pos == null || vel == null)
        {
            return;
        }
        pos->X += vel->X * dt;
        pos->Y += vel->Y * dt;
        pos->Z += vel->Z * dt;
    }

    // Calls back into the host: the first field of *api is a Cdecl function
    // pointer void(int level, byte* msg, int len). Exercises the managed ->
    // native direction of the nf.* binding table.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void CallLog(nint api, int level, byte* message, int messageLen)
    {
        if (api == 0 || message == null || messageLen < 0)
        {
            return;
        }
        var log = (delegate* unmanaged[Cdecl]<int, byte*, int, void>)(void*)Marshal.ReadIntPtr(api);
        if (log == null)
        {
            return;
        }
        log(level, message, messageLen);
    }

    // Per-entity script lifecycle (mirrors Lua's update(dt) convention with an
    // explicit entity id, since managed methods are static): Start resets the
    // counter, Update advances it once per tick, Counter reads it back.
    // Update on an unknown entity starts it at 1 (no Start required).
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Start(long entity)
    {
        lock (s_lock)
        {
            s_counters[entity] = 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Update(long entity, float dt)
    {
        if (dt <= 0.0f)
        {
            return;
        }
        lock (s_lock)
        {
            s_counters[entity] = GetOrZero(entity) + 1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Counter(long entity)
    {
        lock (s_lock)
        {
            return GetOrZero(entity);
        }
    }

    private static int GetOrZero(long entity)
    {
        return s_counters.TryGetValue(entity, out var c) ? c : 0;
    }
}
