package com.blazkness.adbhide;

/**
 * Xposed-free holder for cross-class flags. MainHook cannot be referenced from
 * the panel: it implements an Xposed interface, so loading it in a normal app
 * process throws NoClassDefFoundError before any field can be read.
 */
public final class HookState {

    public static volatile boolean appHooked = false;

    private HookState() {
    }
}
