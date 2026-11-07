package com.blazkness.adbhide;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
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
    static final String PROBE_KEY = "adbhide_hook_probe";
    static final String PROBE_STATE_KEY = "adbhide_props_state";
    private static final String PROPS_FILE = "/data/system/adbhide.props";
    private static final long PROPS_TTL_MS = 250L;
    private static long propsReadAt = 0L;
    private static boolean pMaster = true;
    private static boolean pDev = true;
    private static boolean pAdb = true;
    private static boolean pWifi = true;
    private static long pExpires = 0L;

    private static void loadProps() {
        long now = System.currentTimeMillis();
        if (now - propsReadAt < PROPS_TTL_MS) {
            return;
        }
        propsReadAt = now;
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.FileReader(PROPS_FILE));
            boolean master = true, dev = true, adb = true, wifi = true;
            long expires = 0L;
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq <= 0) {
                    continue;
                }
                String k = line.substring(0, eq).trim();
                String v = line.substring(eq + 1).trim();
                if ("master".equals(k)) {
                    master = "1".equals(v);
                } else if ("dev".equals(k)) {
                    dev = "1".equals(v);
                } else if ("adb".equals(k)) {
                    adb = "1".equals(v);
                } else if ("wifi".equals(k)) {
                    wifi = "1".equals(v);
                } else if ("expires".equals(k)) {
                    try {
                        expires = Long.parseLong(v);
                    } catch (Throwable ignored) {
                    }
                }
            }
            r.close();
            pMaster = master;
            pDev = dev;
            pAdb = adb;
            pWifi = wifi;
            pExpires = expires;
        } catch (Throwable ignored) {
        }
    }

    private static boolean masterOn() {
        loadProps();
        if (!pMaster) {
            return false;
        }
        return pExpires <= 0L || System.currentTimeMillis() < pExpires;
    }

    private static boolean spoofEnabled(String key) {
        if (!masterOn()) {
            return false;
        }
        if ("development_settings_enabled".equals(key)) {
            return pDev;
        }
        if ("adb_enabled".equals(key)) {
            return pAdb;
        }
        if ("adb_wifi_enabled".equals(key)) {
            return pWifi;
        }
        return true;
    }
    private static final String SYSTEM_PKG = "android";
    private static final String SETTINGS_PKG = "com.android.providers.settings";
    private static final String SPOOF_VALUE = "0";


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
            // System processes (uid < 10000) must never get the property layer:
            // it is meant for apps, and spoofing props inside telecom/settings
            // would change system behaviour. Only the settings provider matters.
            if (Process.myUid() < 10000) {
                if (SYSTEM_PKG.equals(lp.packageName) || SETTINGS_PKG.equals(lp.packageName)) {
                    hookSettingsProvider(lp);
                }
                if (SYSTEM_PKG.equals(lp.packageName)) {
                    hookRecents(lp);
                }
                return;
            }
            hookSystemProperties(lp);
            hookNsd(lp);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": load " + lp.packageName + " failed: " + t);
        }
    }

    /** Packages whose tasks are removed from the recents list. Empty file = feature off. */
    private static final String RECENTS_CONF = "/data/adb/adbhide/recents.conf";
    private static volatile Set<String> recentsPkgs = Collections.emptySet();
    private static volatile long recentsStamp = Long.MIN_VALUE;
    private static long recentsLogAt = 0L;

    private static Set<String> recentsPackages() {
        File f = new File(RECENTS_CONF);
        long stamp = (f.lastModified() << 20) ^ f.length();
        if (stamp == recentsStamp) {
            return recentsPkgs;
        }
        Set<String> out = new HashSet<>();
        BufferedReader r = null;
        try {
            r = new BufferedReader(new FileReader(f));
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                out.add(line);
            }
        } catch (Throwable ignored) {
            // missing file: feature off
        } finally {
            if (r != null) {
                try {
                    r.close();
                } catch (Throwable ignored) {
                }
            }
        }
        recentsPkgs = out;
        recentsStamp = stamp;
        return out;
    }

    private static String taskPackage(Object taskInfo) {
        try {
            Field f = taskInfo.getClass().getField("baseIntent");
            Object v = f.get(taskInfo);
            if (v instanceof Intent) {
                ComponentName cn = ((Intent) v).getComponent();
                if (cn != null) {
                    return cn.getPackageName();
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            Field f = taskInfo.getClass().getField("realActivity");
            Object v = f.get(taskInfo);
            if (v instanceof ComponentName) {
                return ((ComponentName) v).getPackageName();
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    /**
     * Drops selected packages from the list the recents UI reads. The task
     * itself keeps running; only its entry is filtered out.
     */
    private void hookRecents(XC_LoadPackage.LoadPackageParam lp) {
        Class<?> cls;
        try {
            cls = Class.forName("com.android.server.wm.RecentTasks", false, lp.classLoader);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": RecentTasks not found: " + t);
            return;
        }
        XposedBridge.hookAllMethods(cls, "getRecentTasks", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    Object res = param.getResult();
                    if (!(res instanceof List)) {
                        return;
                    }
                    Set<String> hide = recentsPackages();
                    if (hide.isEmpty()) {
                        return;
                    }
                    List<?> list = (List<?>) res;
                    if (!list.isEmpty() && System.currentTimeMillis() - recentsLogAt > 5000) {
                        recentsLogAt = System.currentTimeMillis();
                        XposedBridge.log(TAG + ": recents via " + param.method.getName() + " n=" + list.size());
                    }
                    List<Object> keep = new ArrayList<>(list.size());
                    for (Object o : list) {
                        if (o == null) {
                            continue;
                        }
                        String pkg = taskPackage(o);
                        if (pkg != null && hide.contains(pkg)) {
                            continue;
                        }
                        keep.add(o);
                    }
                    if (keep.size() != list.size()) {
                        param.setResult(keep);
                    }
                } catch (Throwable t) {
                    XposedBridge.log(TAG + ": recents filter failed: " + t);
                }
            }
        });
        XposedBridge.log(TAG + ": recents filter installed");

        Class<?> atms;
        try {
            atms = Class.forName("com.android.server.wm.ActivityTaskManagerService", false, lp.classLoader);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": ATMS not found: " + t);
            return;
        }
        XC_MethodHook listHook = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                try {
                    Object res = param.getResult();
                    if (!(res instanceof List)) {
                        return;
                    }
                    Set<String> hide = recentsPackages();
                    if (hide.isEmpty()) {
                        return;
                    }
                    List<?> list = (List<?>) res;
                    if (!list.isEmpty() && System.currentTimeMillis() - recentsLogAt > 5000) {
                        recentsLogAt = System.currentTimeMillis();
                        XposedBridge.log(TAG + ": recents via " + param.method.getName() + " n=" + list.size());
                    }
                    List<Object> keep = new ArrayList<>(list.size());
                    for (Object o : list) {
                        if (o == null) {
                            continue;
                        }
                        String pkg = taskPackage(o);
                        if (pkg != null && hide.contains(pkg)) {
                            continue;
                        }
                        keep.add(o);
                    }
                    if (keep.size() != list.size()) {
                        param.setResult(keep);
                    }
                } catch (Throwable t) {
                    XposedBridge.log(TAG + ": recents filter failed: " + t);
                }
            }
        };
        for (java.lang.reflect.Method m : atms.getDeclaredMethods()) {
            String n = m.getName();
            if ((n.equals("getTasks") || n.equals("getRecentTasks") || n.equals("getFilteredTasks"))
                    && List.class.isAssignableFrom(m.getReturnType())) {
                XposedBridge.hookMethod(m, listHook);
                XposedBridge.log(TAG + ": hooked ATMS." + n);
            }
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
                    if (!(keyArg instanceof String)) {
                        return;
                    }
                    if (PROBE_STATE_KEY.equals(keyArg)) {
                        if (Binder.getCallingUid() < 10000) {
                            return;
                        }
                        loadProps();
                        Bundle state = new Bundle();
                        state.putString("value", (pMaster ? "1" : "0") + ","
                                + (pDev ? "1" : "0") + ","
                                + (pAdb ? "1" : "0") + ","
                                + (pWifi ? "1" : "0") + ","
                                + pExpires);
                        param.setResult(state);
                        return;
                    }
                    if (PROBE_KEY.equals(keyArg)) {
                        if (Binder.getCallingUid() < 10000) {
                            return;
                        }
                        Bundle probe = new Bundle();
                        probe.putString("value", "1");
                        param.setResult(probe);
                        return;
                    }
                    if (!isSpoofedKey((String) keyArg)) {
                        return;
                    }
                    if (!spoofEnabled((String) keyArg)) {
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
        writeHookMark();
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
                if (!masterOn()) {
                    return;
                }
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
                if (!masterOn()) {
                    return;
                }
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
                if (!masterOn()) {
                    return;
                }
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

        HookState.appHooked = true;
        XposedBridge.log(TAG + ": SystemProperties hooks installed in " + lp.packageName);
    }

    private void hookNsd(XC_LoadPackage.LoadPackageParam lp) {
        try {
            Class<?> info = XposedHelpers.findClass("android.net.nsd.NsdServiceInfo", lp.classLoader);

            XC_MethodHook scrub = new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    Object r = param.getResult();
                    if (r instanceof String && ((String) r).contains("_adb")) {
                        param.setResult("_hidden._tcp");
                    }
                }
            };
            XposedHelpers.findAndHookMethod(info, "getServiceType", scrub);
            XposedHelpers.findAndHookMethod(info, "getServiceName", scrub);

            Class<?> listener = XposedHelpers.findClass(
                    "android.net.nsd.NsdManager$DiscoveryListener", lp.classLoader);
            Class<?> mgr = XposedHelpers.findClass("android.net.nsd.NsdManager", lp.classLoader);

            XposedHelpers.findAndHookMethod(mgr, "discoverServices", String.class, int.class,
                    listener, new XC_MethodHook() {
                        @Override
                        protected void beforeHookedMethod(MethodHookParam param) {
                            Object t = param.args[0];
                            if (t instanceof String
                                    && ((String) t).toLowerCase().contains("_adb")) {
                                param.setResult(null);
                                XposedBridge.log(TAG + ": blocked NsdManager discovery of " + t);
                            }
                        }
                    });

            XposedBridge.log(TAG + ": NsdManager hooks installed in " + lp.packageName);
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": NsdManager hook failed: " + t);
        }
    }

    private static String firstString(XC_MethodHook.MethodHookParam param) {
        if (param.args == null || param.args.length == 0) {
            return null;
        }
        Object a = param.args[0];
        return (a instanceof String) ? (String) a : null;
    }



    private void writeHookMark() {
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.FileReader("/proc/sys/kernel/random/boot_id"));
            String bootId = r.readLine();
            r.close();
            if (bootId == null) {
                return;
            }
            java.io.FileOutputStream out =
                    new java.io.FileOutputStream("/data/system/adbhide.hook", false);
            out.write(bootId.trim().getBytes("UTF-8"));
            out.close();
        } catch (Throwable ignored) {
        }
    }
}
