# C# binding

The managed binding will keep raw `DllImport` declarations in
`NativeMethods.cs` and expose ownership-safe wrappers separately. Native
contexts should be wrapped by `SafeHandle`.

The native library base name is `libsoc`. Unity iOS builds may map the same entry
points to `__Internal`; desktop and Android builds load the platform-specific
`libsoc` binary.
