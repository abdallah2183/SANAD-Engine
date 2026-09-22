using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace NF.GameScript;

// NFUiDriver — the managed side of the runtime game UI contract (Game-Ready G3).
//
// A C# game script drives the SAME nf::ui::GameFlow the C++ sample and the Lua
// nf.ui.* bindings drive. The native side hands managed code a UiHostApi table
// (nf::scripting::UiHostApi, built by nf::scripting::make_ui_host_api — see
// Engine/Scripting/include/NF/Scripting/CSharpMarshal.hpp); every field is a
// Cdecl function pointer, and `User` is an opaque handle echoed back on every
// call. No GC handles, no strings and no exceptions cross the boundary:
// strings travel as (UTF-8 bytes, length) spans and a bad argument answers a
// sentinel instead of throwing.
//
// This file is deliberately separate from NFSandbox.cs: the G3 driver is an
// addition, and keeping it in its own translation unit keeps the diff small.
// The SDK's default source glob picks it up on the next managed build.
//
// Layout rule: the UiHostApi struct below must match the C++ struct field for
// field and in order. The C++ side asserts standard layout and a zero offset
// for User, so a mismatch here is a compile-time error there, not a silent
// wrong-argument call.
public static unsafe class NFUiDriver
{
    [StructLayout(LayoutKind.Sequential)]
    public struct UiHostApi
    {
        public void* User;
        public delegate* unmanaged[Cdecl]<void*, int, void> Handle;
        public delegate* unmanaged[Cdecl]<void*, int> Screen;
        public delegate* unmanaged[Cdecl]<void*, float, float, void> SetHealth;
        public delegate* unmanaged[Cdecl]<void*, int, int, void> SetAmmo;
        public delegate* unmanaged[Cdecl]<void*, byte*, int, float, void> ShowMessage;
        public delegate* unmanaged[Cdecl]<void*, void> NotifyGameOver;
        public delegate* unmanaged[Cdecl]<void*, float, void> Update;
        public delegate* unmanaged[Cdecl]<void*, byte*, int, float, int> SetVolume;
        public delegate* unmanaged[Cdecl]<void*, byte*, int, float> Volume;
    }

    // Drives the UI through a caller-supplied action sequence — the managed
    // twin of a C++ loop calling GameFlow::handle(). Returns the resulting
    // nf::ui::Screen value, or -1 when the table is null/incomplete. A null
    // action array or a negative count is a no-op returning -1 (never a throw).
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int DriveActions(nint api, int* actions, int count)
    {
        if (api == 0 || actions == null || count < 0)
        {
            return -1;
        }
        var ui = (UiHostApi*)api;
        if (ui->Handle == null)
        {
            return -1;
        }
        for (int i = 0; i < count; i++)
        {
            ui->Handle(ui->User, actions[i]);
        }
        return ui->Screen != null ? ui->Screen(ui->User) : -1;
    }

    // Reads the current screen without handling input.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UiScreen(nint api)
    {
        if (api == 0)
        {
            return -1;
        }
        var ui = (UiHostApi*)api;
        return ui->Screen != null ? ui->Screen(ui->User) : -1;
    }

    // Ends the run (health hit 0, timer out, ...) and returns the new screen.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UiGameOver(nint api)
    {
        if (api == 0)
        {
            return -1;
        }
        var ui = (UiHostApi*)api;
        if (ui->NotifyGameOver == null || ui->Screen == null)
        {
            return -1;
        }
        ui->NotifyGameOver(ui->User);
        return ui->Screen(ui->User);
    }

    // Sets the HUD health bar; returns 1 when the table has the entry, else 0.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UiSetHealth(nint api, float current, float max)
    {
        if (api == 0)
        {
            return 0;
        }
        var ui = (UiHostApi*)api;
        if (ui->SetHealth == null)
        {
            return 0;
        }
        ui->SetHealth(ui->User, current, max);
        return 1;
    }

    // Binds one audio bus ("master"/"music"/"sfx") to a value — the G5
    // settings contract, exercised from managed code. Returns 1 on success.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UiSetVolume(nint api, byte* bus, int len, float value)
    {
        if (api == 0 || bus == null || len < 0)
        {
            return 0;
        }
        var ui = (UiHostApi*)api;
        if (ui->SetVolume == null)
        {
            return 0;
        }
        return ui->SetVolume(ui->User, bus, len, value);
    }

    // Reads one audio bus value, or -1 for an unknown bus / missing entry.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static float UiVolume(nint api, byte* bus, int len)
    {
        if (api == 0 || bus == null || len < 0)
        {
            return -1.0f;
        }
        var ui = (UiHostApi*)api;
        return ui->Volume != null ? ui->Volume(ui->User, bus, len) : -1.0f;
    }
}
