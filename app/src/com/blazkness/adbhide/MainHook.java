package com.blazkness.adbhide;

import android.os.Binder;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

/**
 * libxposed API 102 module.
 *
 * system_server: SettingsProvider runs in the system process on this ROM, so the
 * settings hook is installed from onSystemServerStarting(). Every caller with a
 * normal app uid gets "0" for the adb related global settings.
 *
 * app processes: SystemProperties and NsdManager are rewritten for the apps that
 * are selected in the LSPosed scope.
 */
public class MainHook extends XposedModule {

    private static final String TAG = "AdbHide";
    private static final String PROPS_FILE = "/data/system/adbhide.props";
    private static final long PROPS_TTL_MS = 2000L;
    private static final String SPOOF_VALUE = "0";

    private static final String[] SETTINGS_KEYS = {
            "adb_enabled",
            "development_settings_enabled",
            "adb_wifi_enabled",
    };

    private static final Map<String, String> STRING_PROPS = new HashMap<String, String>();
    private static final Map<String, String> INT_PROPS = new HashMap<String, String>();

    private static long propsAt = 0L;
    private static Map<String, String> propsCache = null;

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

    // ------------------------------------------------------------- lifecycle

    @Override
    public void onModuleLoaded(XposedModuleInterface.ModuleLoadedParam param) {
        log(Log.INFO, TAG, "loaded in " + param.getProcessName()
                + " system=" + param.isSystemServer());
    }

    @Override
    public void onSystemServerStarting(XposedModuleInterface.SystemServerStartingParam param) {
        log(Log.INFO, TAG, "system server starting, installing settings hook");
        hookSettingsProvider(param.getClassLoader());
    }

    @Override
    public void onPackageReady(XposedModuleInterface.PackageReadyParam param) {
        try {
            String pkg = param.getPackageName();
            int uid = param.getApplicationInfo() != null
                    ? param.getApplicationInfo().uid : Process.myUid();
            if (uid < 10000) {
                if ("com.android.providers.settings".equals(pkg)) {
                    hookSettingsProvider(param.getClassLoader());
                }
                return;
            }
            hookSystemProperties(param.getClassLoader());
            hookNsd(param.getClassLoader());
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "package hooks failed: " + t);
        }
    }

    // -------------------------------------------------------------- settings

    private void hookSettingsProvider(ClassLoader cl) {
        Class<?> provider;
        try {
            provider = Class.forName("com.android.providers.settings.SettingsProvider", false, cl);
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "SettingsProvider class not found: " + t);
            return;
        }

        int count = 0;
        for (final Method m : provider.getDeclaredMethods()) {
            if (!"call".equals(m.getName())) {
                continue;
            }
            Class<?>[] types = m.getParameterTypes();
            if (types.length < 2 || types[0] != String.class || types[1] != String.class) {
                continue;
            }
            try {
                hook(m).intercept(new io.github.libxposed.api.XposedInterface.Hooker() {
                    @Override
                    public Object intercept(io.github.libxposed.api.XposedInterface.Chain chain)
                            throws Throwable {
                        Object result = chain.proceed();
                        try {
                            Object keyArg = chain.getArg(1);
                            if (!(keyArg instanceof String)) {
                                return result;
                            }
                            String key = (String) keyArg;
                            if (!isSpoofedKey(key) || !spoofEnabled(key)) {
                                return result;
                            }
                            if (Binder.getCallingUid() < 10000) {
                                return result;
                            }
                            Bundle bundle = (result instanceof Bundle)
                                    ? (Bundle) result : new Bundle();
                            bundle.putString("value", SPOOF_VALUE);
                            return bundle;
                        } catch (Throwable ignored) {
                            return result;
                        }
                    }
                });
                count++;
            } catch (Throwable t) {
                log(Log.ERROR, TAG, "hook call() failed: " + t);
            }
        }
        log(Log.INFO, TAG, "SettingsProvider.call hooks installed: " + count);
    }

    private boolean isSpoofedKey(String key) {
        for (String k : SETTINGS_KEYS) {
            if (k.equals(key)) {
                return true;
            }
        }
        return false;
    }

    private String propKey(String setting) {
        if ("development_settings_enabled".equals(setting)) {
            return "dev";
        }
        if ("adb_wifi_enabled".equals(setting)) {
            return "wifi";
        }
        return "adb";
    }

    private Map<String, String> props() {
        long now = SystemClock.elapsedRealtime();
        if (propsCache != null && now - propsAt < PROPS_TTL_MS) {
            return propsCache;
        }
        Map<String, String> out = new LinkedHashMap<String, String>();
        BufferedReader r = null;
        try {
            r = new BufferedReader(new FileReader(PROPS_FILE));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    out.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                }
            }
        } catch (Throwable ignored) {
        } finally {
            try {
                if (r != null) {
                    r.close();
                }
            } catch (Throwable ignored) {
            }
        }
        propsCache = out;
        propsAt = now;
        return out;
    }

    private boolean spoofEnabled(String setting) {
        Map<String, String> p = props();
        if (p.isEmpty()) {
            return true;
        }
        if ("0".equals(p.get("master"))) {
            return false;
        }
        String exp = p.get("expires");
        if (exp != null && !exp.isEmpty()) {
            try {
                long until = Long.parseLong(exp);
                if (until > 0L && System.currentTimeMillis() > until) {
                    return false;
                }
            } catch (Throwable ignored) {
            }
        }
        String v = p.get(propKey(setting));
        return v == null || "1".equals(v);
    }

    // -------------------------------------------------------- system props

    private void hookSystemProperties(ClassLoader cl) {
        Class<?> props;
        try {
            props = Class.forName("android.os.SystemProperties", false, cl);
        } catch (Throwable t) {
            return;
        }

        for (final Method m : props.getDeclaredMethods()) {
            final String name = m.getName();
            final Class<?> ret = m.getReturnType();
            boolean isString = "get".equals(name) && ret == String.class;
            boolean isInt = "getInt".equals(name) && ret == int.class;
            boolean isBool = "getBoolean".equals(name) && ret == boolean.class;
            if (!isString && !isInt && !isBool) {
                continue;
            }
            try {
                hook(m).intercept(new io.github.libxposed.api.XposedInterface.Hooker() {
                    @Override
                    public Object intercept(io.github.libxposed.api.XposedInterface.Chain chain)
                            throws Throwable {
                        Object keyArg = chain.getArg(0);
                        if (!(keyArg instanceof String)) {
                            return chain.proceed();
                        }
                        String key = (String) keyArg;
                        if (isString) {
                            String fake = STRING_PROPS.get(key);
                            return fake != null ? fake : chain.proceed();
                        }
                        String fake = INT_PROPS.get(key);
                        if (fake == null) {
                            return chain.proceed();
                        }
                        if (isBool) {
                            return "1".equals(fake);
                        }
                        try {
                            return Integer.parseInt(fake);
                        } catch (Throwable t) {
                            return chain.proceed();
                        }
                    }
                });
            } catch (Throwable ignored) {
            }
        }
        log(Log.INFO, TAG, "SystemProperties hooks installed");
    }

    // ---------------------------------------------------------------- nsd

    private void hookNsd(ClassLoader cl) {
        try {
            Class<?> info = Class.forName("android.net.nsd.NsdServiceInfo", false, cl);
            for (final Method m : info.getDeclaredMethods()) {
                if (!"getServiceType".equals(m.getName()) && !"getServiceName".equals(m.getName())) {
                    continue;
                }
                try {
                    hook(m).intercept(new io.github.libxposed.api.XposedInterface.Hooker() {
                        @Override
                        public Object intercept(io.github.libxposed.api.XposedInterface.Chain chain)
                                throws Throwable {
                            Object r = chain.proceed();
                            if (r instanceof String && ((String) r).contains("_adb")) {
                                return "_hidden._tcp";
                            }
                            return r;
                        }
                    });
                } catch (Throwable ignored) {
                }
            }

            Class<?> mgr = Class.forName("android.net.nsd.NsdManager", false, cl);
            for (final Method m : mgr.getDeclaredMethods()) {
                if (!"discoverServices".equals(m.getName())) {
                    continue;
                }
                try {
                    hook(m).intercept(new io.github.libxposed.api.XposedInterface.Hooker() {
                        @Override
                        public Object intercept(io.github.libxposed.api.XposedInterface.Chain chain)
                                throws Throwable {
                            Object t = chain.getArg(0);
                            if (t instanceof String
                                    && ((String) t).toLowerCase().contains("_adb")) {
                                log(Log.INFO, TAG, "blocked NsdManager discovery of " + t);
                                return null;
                            }
                            return chain.proceed();
                        }
                    });
                } catch (Throwable ignored) {
                }
            }
            log(Log.INFO, TAG, "NsdManager hooks installed");
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "NsdManager hook failed: " + t);
        }
    }
}
