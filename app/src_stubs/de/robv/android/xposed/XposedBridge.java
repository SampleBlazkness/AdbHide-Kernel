package de.robv.android.xposed;

import java.util.Set;

/**
 * Compile-time stub only - signature must match the real XposedBridge from
 * LSPosed, because the bytecode calls these exact descriptors at runtime.
 */
public final class XposedBridge {

    private XposedBridge() {
    }

    public static void log(String text) {
    }

    public static void log(Throwable t) {
    }

    public static Set<XC_MethodHook.Unhook> hookAllMethods(
            Class<?> hookClass, String methodName, XC_MethodHook callback) {
        return null;
    }

    public static void hookAllConstructors(Class<?> hookClass, XC_MethodHook callback) {
    }
}
