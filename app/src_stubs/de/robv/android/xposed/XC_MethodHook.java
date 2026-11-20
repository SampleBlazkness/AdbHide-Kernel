package de.robv.android.xposed;

/**
 * Compile-time stub only - signatures must match the real XC_MethodHook.
 */
public abstract class XC_MethodHook {

    protected XC_MethodHook() {
    }

    protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
    }

    protected void afterHookedMethod(MethodHookParam param) throws Throwable {
    }

    public static class MethodHookParam {
        public java.lang.reflect.Member method;
        public Object thisObject;
        public Object[] args;

        public Object getResult() {
            return null;
        }

        public void setResult(Object result) {
        }

        public Throwable getThrowable() {
            return null;
        }

        public void setThrowable(Throwable throwable) {
        }

        public Object getResultOrThrowable() throws Throwable {
            return null;
        }
    }

    public class Unhook {
        public void unhook() {
        }
    }
}
