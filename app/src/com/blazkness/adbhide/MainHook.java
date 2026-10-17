package com.blazkness.adbhide;

import android.os.Binder;
import android.os.Bundle;

import java.util.HashMap;
import java.util.Map;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * Two independent layers.
 *
 * system_server: SettingsProvider.call() answers "0" for the adb-related
 * global settings, for every caller that is a normal app. This is on by
 * default, matching the original AdbHide behaviour - no per-app setup needed.
 *
 * any injected app: SystemProperties is rewritten so Java-level reads see the
 * same values the kernel module will report. Which apps get this layer is
 * decided by the LSPosed scope, exactly as before.
 */
public class MainHook implements IXposedHookLoadPackage {

    private static final String TAG = "AdbHide";
    private static final String SYSTEM_PKG = "android";
    private static final String SPOOF_VALUE = "0";

    /** set when this process got the SystemProperties layer */
    public static volatile boolean appHooked = false;

    private static final String[] SETTINGS_KEYS = {
            "adb_enabled",
            "development_settings_enabled",
            "adb_wifi_enabled",
    };

    private static final Map<String, String> STRING_PROPS = new HashMap<>();
    private static final Map<String, String> INT_PROPS = new HashMap<>();

    static {
        STRING_PROPS.put("persist.sys.usb.config", "mtp");
        STRING_PROPS.put("sys.usb.config", "mtp");
        STRING_PROPS.put("sys.usb.state", "mtp");
        STRING_PROPS.put("init.svc.adbd", "stopped");
        STRING_PROPS.put("service.adb.tcp.port", "");
        STRING_PROPS.put("persist.adb.tcp.port", "");

        INT_PROPS.put("ro.debuggable", "0");
        INT_PROPS.put("ro.secure", "1");
        INT_PROPS.put("ro.adb.secure", "1");
    }

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (lp == null || lp.packageName == null) {
            return;
        }
        try {
            if (SYSTEM_PKG.equals(lp.packageName)) {
                hookSettingsProvider(lp);
            } else {
                hookSystemProperties(lp);
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": load " + lp.packageName + " failed: " + t);
        }
    }

    private boolean isSpoofedKey(String key) {
        for (String k : SETTINGS_KEYS) {
            if (k.equals(key)) {
                return true;
            }
        }
        return false;
    }

    private void hookSettingsProvider(XC_LoadPackage.LoadPackageParam lp) {
        Class<?> provider;
        try {
            provider = Class.forName("com.android.providers.settings.SettingsProvider",
                    false, lp.classLoader);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": SettingsProvider not found: " + t);
            return;
        }

        XposedBridge.hookAllMethods(provider, "call", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    if (param.args == null || param.args.length < 2) {
                        return;
                    }
                    Object keyArg = param.args[1];
                    if (!(keyArg instanceof String) || !isSpoofedKey((String) keyArg)) {
                        return;
                    }
                    // system / root / shell keep seeing the real value: adb
                    // debugging itself must not be broken by this module
                    if (Binder.getCallingUid() < 10000) {
                        return;
                    }
                    Object result = param.getResult();
                    Bundle bundle = (result instanceof Bundle) ? (Bundle) result : new Bundle();
                    bundle.putString("value", SPOOF_VALUE);
                    param.setResult(bundle);
                } catch (Throwable t) {
                    XposedBridge.log(TAG + ": call hook: " + t);
                }
            }
        });

        XposedBridge.log(TAG + ": SettingsProvider.call hooks installed");
    }

    private void hookSystemProperties(XC_LoadPackage.LoadPackageParam lp) {
        Class<?> props;
        try {
            props = Class.forName("android.os.SystemProperties", false, lp.classLoader);
        } catch (Throwable t) {
            return;
        }

        XposedBridge.hookAllMethods(props, "get", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                String key = firstString(param);
                if (key == null) {
                    return;
                }
                String fake = STRING_PROPS.get(key);
                if (fake != null) {
                    param.setResult(fake);
                }
            }
        });

        XC_MethodHook intHook = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                String key = firstString(param);
                if (key == null) {
                    return;
                }
                String fake = INT_PROPS.get(key);
                if (fake == null) {
                    return;
                }
                try {
                    param.setResult(Integer.parseInt(fake));
                } catch (Throwable ignored) {
                }
            }
        };
        XposedBridge.hookAllMethods(props, "getInt", intHook);
        XposedBridge.hookAllMethods(props, "getBoolean", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                String key = firstString(param);
                if (key == null) {
                    return;
                }
                String fake = INT_PROPS.get(key);
                if (fake != null) {
                    param.setResult("1".equals(fake));
                }
            }
        });

        appHooked = true;
        XposedBridge.log(TAG + ": SystemProperties hooks installed in " + lp.packageName);
    }

    private static String firstString(XC_MethodHook.MethodHookParam param) {
        if (param.args == null || param.args.length == 0) {
            return null;
        }
        Object a = param.args[0];
        return (a instanceof String) ? (String) a : null;
    }
}
