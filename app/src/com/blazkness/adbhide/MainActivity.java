package com.blazkness.adbhide;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Build;
import android.content.Intent;
import android.os.Bundle;
import java.io.FileInputStream;
import android.os.Looper;
import android.os.Handler;
import android.util.Log;
import android.provider.Settings;
import org.json.JSONArray;
import org.json.JSONObject;
import android.content.res.ColorStateList;
import android.view.Gravity;
import android.view.View;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MainActivity extends Activity {

    private static final String SCRIPT = "echo SU=1\np=/proc/adbhide_kmod\n" +
            "echo KMOD=$([ -r $p ] && echo 1 || echo 0)\n" +
            "echo KMODINST=$([ -d /data/adb/modules/adbhide_kmod ] && echo 1 || echo 0)\n" +
            "echo KMODDIS=$([ -f /data/adb/modules/adbhide_kmod/disable ] && echo 1 || echo 0)\n" +
            "echo KMODVER=$(sed -n 's/^version=//p' /data/adb/modules/adbhide_kmod/module.prop 2>/dev/null)\n" +
            "if [ -r $p ]; then\n" +
            "  echo KVER=$(head -n1 $p | cut -d' ' -f2)\n" +
            "  echo KREDIR=$(sed -n 's/^redirect //p' $p)\n" +
            "  echo KNETSEQ=$(sed -n 's/^netseq //p' $p)\n" +
            "  echo KCOUNTERS=$(sed -n 's/^counters //p' $p)\n" +
            "  echo KTARGETS=$(sed -n 's/^targets [0-9]*://p' $p)\n" +
            "fi\n" +
            "echo NATIVEINST=$([ -d /data/adb/modules/adbhide_native ] && echo 1 || echo 0)\n" +
            "echo NATIVEDIS=$([ -f /data/adb/modules/adbhide_native/disable ] && echo 1 || echo 0)\n" +
            "echo NATIVE=$([ -f /data/adb/modules/adbhide_native/zygisk/arm64-v8a.so ] && echo 1 || echo 0)\n" +
            "echo NATIVEVER=$(sed -n 's/^version=//p' /data/adb/modules/adbhide_native/module.prop 2>/dev/null)\n" +
            "echo UIDS=$(tr '\\n' ' ' < /data/adb/adbhide/uids.txt 2>/dev/null)\n" +
            "echo CONF=$(paste -sd'|' /data/adb/adbhide/blacklist.conf 2>/dev/null)\n" +
            "pf=/data/system/adbhide.props\n" +
            "echo MASTER=$(sed -n 's/^master=//p' $pf 2>/dev/null)\n" +
            "echo DEV=$(sed -n 's/^dev=//p' $pf 2>/dev/null)\n" +
            "echo SWADB=$(sed -n 's/^adb=//p' $pf 2>/dev/null)\n" +
            "echo SWWIFI=$(sed -n 's/^wifi=//p' $pf 2>/dev/null)\n" +
            "echo PORTS=$(iptables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)\n" +
            "echo PORTS6=$(ip6tables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)\n" +
            "echo MDNS=$(iptables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)\n" +
            "echo MDNS6=$(ip6tables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)\n" +
            "";

    private static final String[] DEFAULT_PKGS = {
            "com.eltavine.duckdetector",
            "com.blazkness.adbhidedetector",
    };

    private static final String PROPS = "/data/system/adbhide.props";

    private LinearLayout body;
    private TextView hint;
    private TextView tvSysState;
    private TextView tvReads;
    private TextView tvUpdate;
    private String updateText = "";
    private String updateDetail = "";
    private LinearLayout bannerBox;

    private final java.util.Map<String, String> pending = new java.util.HashMap<String, String>();
    private long pendingUntil = 0L;
    private volatile boolean masterOn = true;

    private final Handler ticker = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            updateLive();
            ticker.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onStart() {
        super.onStart();
        ticker.removeCallbacks(tick);
        ticker.post(tick);
        if (updateText.isEmpty()) {
            checkUpdates();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        ticker.removeCallbacks(tick);
    }

    private String callGlobal(String key) {
        try {
            Bundle b = getContentResolver().call(Settings.Global.CONTENT_URI,
                    "GET_global", key, null);
            return b == null ? null : b.getString("value");
        } catch (Throwable t) {
            return null;
        }
    }

    private void addBanner(String text, int fg, int bg) {
        TextView t = Ui.text(this, text, 16, fg);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(dp(14), dp(12), dp(14), dp(12));
        t.setBackground(Ui.bg(bg, 12, this));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(8);
        bannerBox.addView(t, lp);
    }

    private void setSwitch(Switch sw, boolean on) {
        if (sw != null && sw.isChecked() != on) {
            sw.setChecked(on);
        }
    }

    private void updateLive() {
        String state = callGlobal("adbhide_props_state");
        if (state != null && state.indexOf(',') > 0) {
            String[] parts = state.split(",");
            if (parts.length >= 5) {
                props.put("master", parts[0]);
                props.put("dev", parts[1]);
                props.put("adb", parts[2]);
                props.put("wifi", parts[3]);
                props.put("expires", parts[4]);
            }
        } else {
            java.util.Map<String, String> fresh = readPropsFile();
            if (!fresh.isEmpty()) {
                props.clear();
                props.putAll(fresh);
            }
        }
        if (System.currentTimeMillis() < pendingUntil) {
            for (java.util.Map.Entry<String, String> entry : pending.entrySet()) {
                props.put(entry.getKey(), entry.getValue());
            }
        } else {
            pending.clear();
        }
        binding = true;
        setSwitch(swMaster, masterOn);
        setSwitch(swDev, !"0".equals(props.get("dev")));
        setSwitch(swAdb, !"0".equals(props.get("adb")));
        setSwitch(swWifi, !"0".equals(props.get("wifi")));
        binding = false;
        applyEnabledState(swMaster.isChecked());

        boolean alive = "1".equals(callGlobal("adbhide_hook_probe"));
        boolean master = this.masterOn;
        tvSysState.setText(!master ? getString(R.string.state_off)
                : alive ? getString(R.string.state_active) : getString(R.string.state_inactive));
        tvSysState.setTextColor(master && alive ? Ui.OK : Ui.BAD);
        tvSysState.setBackground(Ui.bg(master && alive ? 0xFFE9F7EF : 0xFFFDECEC, 999, this));
        if (tvUpdate != null) {
            tvUpdate.setText(updateText);
        }
        if (tvReads != null) {
            tvReads.setText(getString(R.string.reads_format, callGlobal("adb_enabled"), callGlobal("development_settings_enabled"), callGlobal("adb_wifi_enabled")));
        }
    }

    private java.util.Map<String, String> readPropsFile() {
        java.util.Map<String, String> out = new java.util.LinkedHashMap<String, String>();
        FileInputStream in = null;
        try {
            in = new FileInputStream("/data/system/adbhide.props");
            BufferedReader r = new BufferedReader(new InputStreamReader(in));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    out.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Throwable ignored) {
                }
            }
        }
        return out;
    }
    private TextView tvListSummary;
    private TextView tvMissingTitle;
    private LinearLayout listBox;
    private LinearLayout missingBox;
    private TextView tvKernel;
    private TextView tvNative;
    private TextView tvApp;

    private Switch swMaster;
    private Switch swDev;
    private Switch swAdb;
    private Switch swWifi;

    private boolean binding;
    private final Map<String, String> props = new LinkedHashMap<String, String>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        ensureConfig();
        refresh();
        Log.i("AdbHideDbg", "adb=" + Settings.Global.getInt(getContentResolver(), "adb_enabled", -1)
                + " dev=" + Settings.Global.getInt(getContentResolver(), "development_settings_enabled", -1)
                + " wifi=" + Settings.Global.getInt(getContentResolver(), "adb_wifi_enabled", -1));
    }

    // ---------------------------------------------------------------- layout

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setFitsSystemWindows(true);
        scroll.setBackgroundColor(Ui.BG);

        body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16), dp(22), dp(16), dp(30));
        scroll.addView(body, new ScrollView.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Runtime.getRuntime().exec(new String[]{"su", "-c",
                            "cmd appops set " + getPackageName() + " 10022 allow"}).waitFor();
                } catch (Throwable ignored) {
                }
            }
        }).start();


        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.VERTICAL);
        head.setPadding(0, 0, 0, dp(18));
        TextView title = Ui.text(this, "AdbHide", 24, Ui.INK);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setIncludeFontPadding(false);
        title.setPadding(0, 0, 0, dp(4));
        head.addView(title);
        TextView headSub = Ui.sub(this, "github.com/SampleBlazkness/AdbHide-Kernel");
        headSub.setIncludeFontPadding(false);
        headSub.setPadding(0, 0, 0, 0);
        headSub.setTextColor(Ui.ACCENT);
        headSub.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW,
                            android.net.Uri.parse("https://github.com/SampleBlazkness/AdbHide-Kernel")));
                } catch (Throwable ignored) {
                }
            }
        });
        head.addView(headSub);
        body.addView(head);

        LinearLayout updRow = new LinearLayout(this);
        updRow.setOrientation(LinearLayout.HORIZONTAL);
        updRow.setGravity(Gravity.CENTER_VERTICAL);
        updRow.setPadding(0, dp(10), 0, dp(2));
        tvUpdate = Ui.text(this, updateText, 11, Ui.MUTED);
        tvUpdate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (updateDetail == null || updateDetail.isEmpty()) {
                    return;
                }
                new android.app.AlertDialog.Builder(MainActivity.this)
                        .setTitle(getString(R.string.update_detail_title))
                        .setMessage(updateDetail)
                        .setPositiveButton(getString(R.string.ok), null)
                        .show();
            }
        });
        updRow.addView(tvUpdate, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView checkBtn = Ui.text(this, getString(R.string.check_update), 13, Ui.ACCENT);
        checkBtn.setPadding(dp(10), dp(6), dp(2), dp(6));
        checkBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                checkUpdates();
            }
        });
        updRow.addView(checkBtn);
        body.addView(updRow);

        bannerBox = new LinearLayout(this);
        bannerBox.setOrientation(LinearLayout.VERTICAL);
        bannerBox.setVisibility(View.GONE);
        LinearLayout.LayoutParams bannerLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        bannerLp.bottomMargin = dp(12);
        body.addView(bannerBox, bannerLp);

        body.addView(systemCard());
        body.addView(listCard());
        body.addView(wirelessCard());
        body.addView(moduleCard());

        hint = Ui.text(this, "", 11, Ui.MUTED);
        hint.setPadding(dp(2), dp(2), 0, 0);
        body.addView(hint);
    }

    private LinearLayout systemCard() {
        LinearLayout card = Ui.card(this);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.addView(Ui.title(this, getString(R.string.title_system)));
        titles.addView(Ui.sub(this, getString(R.string.sub_lsposed)));
        head.addView(titles, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        tvSysState = Ui.chip(this, getString(R.string.state_inactive), false);
        head.addView(tvSysState);
        card.addView(head);

        card.addView(Ui.divider(this));

        swMaster = newSwitch("master");
        card.addView(row(getString(R.string.opt_master), getString(R.string.opt_master_desc), swMaster));

        swDev = newSwitch("dev");
        card.addView(row(getString(R.string.opt_dev), "development_settings_enabled", swDev));

        swAdb = newSwitch("adb");
        card.addView(row(getString(R.string.opt_adb), "adb_enabled", swAdb));

        swWifi = newSwitch("wifi");
        card.addView(row(getString(R.string.opt_wifi), "adb_wifi_enabled", swWifi));

        tvReads = Ui.sub(this, "");
        tvReads.setPadding(0, dp(10), 0, 0);
        card.addView(tvReads);

        return card;
    }

    private LinearLayout listCard() {
        LinearLayout card = Ui.card(this);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.addView(Ui.title(this, getString(R.string.title_kernel)));
        tvListSummary = Ui.sub(this, getString(R.string.loading));
        titles.addView(tvListSummary);
        head.addView(titles, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(head);

        card.addView(Ui.divider(this));

        listBox = new LinearLayout(this);
        listBox.setOrientation(LinearLayout.VERTICAL);
        card.addView(listBox);

        tvMissingTitle = Ui.sub(this, getString(R.string.missing_title));
        tvMissingTitle.setTextColor(Ui.BAD);
        tvMissingTitle.setPadding(0, dp(12), 0, dp(2));
        tvMissingTitle.setVisibility(View.GONE);
        card.addView(tvMissingTitle);

        missingBox = new LinearLayout(this);
        missingBox.setOrientation(LinearLayout.VERTICAL);
        card.addView(missingBox);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setPadding(0, dp(14), 0, 0);

        TextView manage = Ui.action(this, getString(R.string.manage_apps), true);
        manage.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                AppPicker.show(MainActivity.this, new Runnable() {
                    @Override
                    public void run() {
                        refresh();
                    }
                });
            }
        });
        actions.addView(manage, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(actions);

        return card;
    }

    private LinearLayout wirelessCard() {
        LinearLayout card = Ui.card(this);
        card.addView(Ui.title(this, getString(R.string.title_wireless)));
        card.addView(Ui.sub(this, getString(R.string.wireless_desc)));
        TextView cmd = Ui.text(this,
                getString(R.string.wireless_cmd), 12, Ui.MUTED);
        cmd.setTypeface(Typeface.MONOSPACE);
        cmd.setPadding(0, dp(8), 0, 0);
        card.addView(cmd);
        return card;
    }

    private LinearLayout moduleCard() {
        LinearLayout card = Ui.card(this);
        card.addView(Ui.title(this, getString(R.string.title_modules)));
        card.addView(Ui.divider(this));
        tvKernel = addModuleRow(card, getString(R.string.mod_kernel));
        tvNative = addModuleRow(card, getString(R.string.mod_native));
        tvApp = addModuleRow(card, getString(R.string.mod_manager));

        return card;
    }

    private TextView addModuleRow(LinearLayout parent, String label) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(5), 0, dp(5));
        r.addView(Ui.text(this, label, 14, Ui.INK),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView value = Ui.text(this, "…", 13, Ui.MUTED);
        r.addView(value);
        parent.addView(r);
        return value;
    }

    /** One format for every row: version first, then the same status words. */
    private void setModuleState(TextView view, String version, boolean installed,
                                boolean disabled, boolean loaded) {
        String v = version == null || version.isEmpty() ? "" : "v" + version + " · ";
        if (!installed) {
            view.setText(v + getString(R.string.mod_missing));
            view.setTextColor(Ui.BAD);
        } else if (disabled) {
            view.setText(v + getString(R.string.mod_disabled));
            view.setTextColor(Ui.WARN);
        } else if (loaded) {
            view.setText(v + getString(R.string.mod_enabled));
            view.setTextColor(Ui.OK);
        } else {
            view.setText(v + getString(R.string.mod_enabled_not_loaded));
            view.setTextColor(Ui.BAD);
        }
    }

    private LinearLayout row(String label, String sub, View right) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(6), 0, dp(6));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(Ui.text(this, label, 15, Ui.INK));
        if (sub != null && !sub.isEmpty()) {
            texts.addView(Ui.text(this, sub, 11, Ui.MUTED));
        }
        r.addView(texts, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(right);
        return r;
    }

    private Switch newSwitch(final String key) {
        Switch sw = new Switch(this);
        if (Build.VERSION.SDK_INT >= 23) {
            sw.setThumbTintList(ColorStateList.valueOf(Ui.ACCENT));
        }
        sw.setTag(key);
        sw.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (binding) {
                    return;
                }
                props.put(key, isChecked ? "1" : "0");
                pending.put(key, isChecked ? "1" : "0");
                pendingUntil = System.currentTimeMillis() + 2500L;
                if ("master".equals(key)) {
                    masterOn = isChecked;
                    applyEnabledState(isChecked);
                }
                saveProps(key, isChecked ? "1" : "0");
            }
        });
        return sw;
    }

    private void applyEnabledState(boolean master) {
        swDev.setEnabled(master);
        swAdb.setEnabled(master);
        swWifi.setEnabled(master);
        float alpha = master ? 1f : 0.45f;
        swDev.setAlpha(alpha);
        swAdb.setAlpha(alpha);
        swWifi.setAlpha(alpha);
    }

    // -------------------------------------------------------------- data/io

    private void syncList() {
        hint.setText(getString(R.string.syncing));
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Runtime.getRuntime().exec(new String[]{"su", "-c",
                            "sh /data/adb/modules/adbhide_kmod/sync.sh"}).waitFor();
                } catch (Throwable ignored) {
                }
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        refresh();
                    }
                });
            }
        }).start();
    }

    private void refresh() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                if (!loadProps()) {
                    saveProps("master", "1");
                    saveProps("dev", "1");
                    saveProps("adb", "1");
                    saveProps("wifi", "1");
                    saveProps("expires", "0");
                }
                final State st = readState();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        render(st);
                    }
                });
            }
        }).start();
    }

    private boolean loadProps() {
        boolean found = false;
        Map<String, String> out = new LinkedHashMap<String, String>();
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c", "cat " + PROPS});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    out.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                    found = true;
                }
            }
            r.close();
            p.waitFor();
        } catch (Throwable ignored) {
        } finally {
            if (p != null) {
                p.destroy();
            }
        }
        if (out.isEmpty()) {
            out.put("master", "1");
            out.put("dev", "1");
            out.put("adb", "1");
            out.put("wifi", "1");
            out.put("expires", "0");
        }
        props.clear();
        props.putAll(out);
        return found;
    }

    private void saveProps(String key, String value) {
        String script = "f=/data/system/adbhide.props; touch $f; "
                + "grep -q '^" + key + "=' $f && "
                + "sed -i 's/^" + key + "=.*/" + key + "=" + value + "/' $f || "
                + "echo '" + key + "=" + value + "' >> $f; "
                + "chmod 644 $f; cp $f /data/adb/adbhide/props.conf";
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c", script}).waitFor();
        } catch (Throwable ignored) {
        }
    }

    private String on(String key) {
        String v = props.get(key);
        return "0".equals(v) ? "0" : "1";
    }

    private void render(State st) {
        binding = true;
        setSwitch(swMaster, !"0".equals(props.get("master")));
        setSwitch(swDev, !"0".equals(props.get("dev")));
        setSwitch(swAdb, !"0".equals(props.get("adb")));
        setSwitch(swWifi, !"0".equals(props.get("wifi")));
        binding = false;
        applyEnabledState(swMaster.isChecked());

        tvSysState.setText(!st.masterOn ? getString(R.string.state_off)
                : st.sysActive ? getString(R.string.state_active) : getString(R.string.state_inactive));
        if (tvReads != null) {
            try {
                int a = Settings.Global.getInt(getContentResolver(), "adb_enabled", -1);
                int d = Settings.Global.getInt(getContentResolver(),
                        "development_settings_enabled", -1);
                int w = Settings.Global.getInt(getContentResolver(), "adb_wifi_enabled", -1);
                tvReads.setText(getString(R.string.reads_format, String.valueOf(a), String.valueOf(d), String.valueOf(w)));
            } catch (Throwable t) {
                tvReads.setText(getString(R.string.read_failed));
            }
        }
        tvSysState.setTextColor(st.masterOn && st.sysActive ? Ui.OK : Ui.BAD);
        if (System.currentTimeMillis() >= pendingUntil) {
            this.masterOn = st.masterOn;
        }
        setSwitch(swMaster, this.masterOn);
        applyEnabledState(st.masterOn);
        tvSysState.setBackground(Ui.bg(st.masterOn && st.sysActive ? 0xFFE9F7EF : 0xFFFDECEC, 999, this));

        int unresolved = Math.max(0, st.confTotal - st.confResolved);
        String summary = getString(R.string.summary_format, st.confTotal, st.uids.size(), unresolved);
        if (!st.confPkgs.isEmpty() && st.uids.isEmpty()) {
            summary += "\n" + getString((st.kmodDis || !st.kmodOk)
                    ? R.string.pkg_inactive_kernel : R.string.pkg_inactive_lsposed);
        }
        tvListSummary.setText(summary);

        listBox.removeAllViews();
        for (int i = 0; i < st.uids.size(); i++) {
            listBox.addView(pkgRow(st.uids.get(i), st.pkgs.get(i), true));
        }
        if (st.uids.isEmpty()) {
            if (!st.confPkgs.isEmpty()) {
                for (String pkg : st.confPkgs) {
                    listBox.addView(plainRow(pkg));
                }
            } else {
                listBox.addView(Ui.text(this, getString(R.string.list_empty), 13, Ui.MUTED));
            }
        }

        missingBox.removeAllViews();
        tvMissingTitle.setVisibility(st.missing.isEmpty() ? View.GONE : View.VISIBLE);
        for (String pkg : st.missing) {
            missingBox.addView(pkgRow("-", pkg, false));
        }

        if (bannerBox != null) {
            bannerBox.removeAllViews();
            if (!st.suOk) {
                addBanner(getString(R.string.banner_su),
                        0xFFB91C1C, 0xFFFEE2E2);
            } else {
                if (!st.kmodInst) {
                    addBanner(getString(R.string.banner_kmod_missing),
                            0xFFB91C1C, 0xFFFEE2E2);
                } else if (st.kmodDis) {
                    addBanner(getString(R.string.banner_kmod_disabled),
                            0xFF92400E, 0xFFFEF3C7);
                } else if (!st.kmodOk) {
                    addBanner(getString(R.string.banner_kmod),
                            0xFFB91C1C, 0xFFFEE2E2);
                }
                if (!st.nativeInst) {
                    addBanner(getString(R.string.banner_zygisk_missing),
                            0xFFB91C1C, 0xFFFEE2E2);
                } else if (st.nativeDis) {
                    addBanner(getString(R.string.banner_zygisk_disabled),
                            0xFF92400E, 0xFFFEF3C7);
                } else if (!st.nativeOk) {
                    addBanner(getString(R.string.banner_zygisk),
                            0xFFB91C1C, 0xFFFEE2E2);
                }
            }
            bannerBox.setVisibility(bannerBox.getChildCount() == 0
                    ? View.GONE : View.VISIBLE);
        }

        setModuleState(tvKernel, st.kver, st.kmodInst, st.kmodDis, st.kmodOk);
        setModuleState(tvNative, st.nativeVer, st.nativeInst, st.nativeDis, st.nativeOk);
        tvApp.setText("v" + st.appVer + " · " + getString(R.string.mod_installed));
        tvApp.setTextColor(Ui.OK);

        StringBuilder hintText = new StringBuilder();
        if (!st.kmodInst) {
            hintText.append(getString(R.string.hint_no_kmod));
        } else if (st.kmodDis) {
            hintText.append(getString(R.string.hint_kmod_disabled));
        } else if (!st.kmodOk && st.error != null) {
            hintText.append(getString(R.string.hint_no_state, String.valueOf(st.error)));
        } else if (!st.kmodOk) {
            hintText.append(getString(R.string.hint_no_kmod));
        }
        if (st.nativeInst && st.nativeDis) {
              if (hintText.length() > 0) hintText.append("\n");
            hintText.append(getString(R.string.hint_native_disabled));
        }
        hint.setText(hintText.toString());
    }

    /** Configured entry that is not in the kernel list: no per-row status, no delete. */
    private LinearLayout plainRow(String pkg) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(7), 0, dp(7));

        ImageView icon = new ImageView(this);
        try {
            icon.setImageDrawable(getPackageManager().getApplicationIcon(pkg));
        } catch (Throwable ignored) {
            icon.setImageDrawable(null);
        }
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(dp(40), dp(40));
        ip.rightMargin = dp(12);
        r.addView(icon, ip);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        String label = pkg;
        try {
            label = String.valueOf(getPackageManager()
                    .getApplicationLabel(getPackageManager().getApplicationInfo(pkg, 0)));
        } catch (Throwable ignored) {
        }
        texts.addView(Ui.text(this, label, 15, Ui.INK));
        TextView pkgView = Ui.text(this, pkg, 11, Ui.MUTED);
        pkgView.setTypeface(Typeface.MONOSPACE);
        texts.addView(pkgView);
        r.addView(texts, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        return r;
    }

    private LinearLayout pkgRow(String uid, String pkg, boolean active) {
        return pkgRow(uid, pkg, active, R.string.pkg_missing);
    }

    private LinearLayout pkgRow(String uid, String pkg, boolean active, int inactiveRes) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(7), 0, dp(7));

        ImageView icon = new ImageView(this);
        try {
            icon.setImageDrawable(getPackageManager().getApplicationIcon(pkg));
        } catch (Throwable ignored) {
            icon.setImageDrawable(null);
        }
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(dp(40), dp(40));
        ip.rightMargin = dp(12);
        r.addView(icon, ip);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);

        String label = pkg;
        try {
            label = String.valueOf(getPackageManager()
                    .getApplicationLabel(getPackageManager().getApplicationInfo(pkg, 0)));
        } catch (Throwable ignored) {
        }

        if (active) {
            texts.addView(Ui.text(this, label, 15, Ui.INK));
            TextView pkgView = Ui.text(this, pkg, 11, Ui.MUTED);
            pkgView.setTypeface(Typeface.MONOSPACE);
            texts.addView(pkgView);
            texts.addView(Ui.text(this, "uid " + uid, 10, Ui.MUTED));
        } else {
            TextView pkgView = Ui.text(this, pkg, 14, Ui.INK);
            pkgView.setTypeface(Typeface.MONOSPACE);
            texts.addView(pkgView);
            texts.addView(Ui.text(this, getString(inactiveRes), 10, Ui.BAD));
        }
        r.addView(texts, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        if (!active) {
            TextView del = Ui.text(this, getString(R.string.remove), 13, Ui.BAD);
            del.setPadding(dp(10), dp(6), dp(2), dp(6));
            del.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    removePkg(pkg);
                }
            });
            r.addView(del);
        }
        return r;
    }

    private void ensureConfig() {
        StringBuilder sb = new StringBuilder();
        sb.append("f=/data/adb/adbhide/blacklist.conf\n");
        sb.append("if [ ! -f $f ]; then\n");
        sb.append("  mkdir -p /data/adb/adbhide\n");
        sb.append("  printf '%s\\n'");
        for (String p : DEFAULT_PKGS) {
            sb.append(" ").append(p);
        }
        sb.append(" > $f\n");
        sb.append("  chmod 600 $f\n");
        sb.append("fi\n");
        sb.append("sh /data/adb/modules/adbhide_kmod/sync.sh\n");
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c", sb.toString()}).waitFor();
        } catch (Throwable ignored) {
        }
    }

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Throwable t) {
            return "?";
        }
    }

    private String moduleVersion(String path) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c", "sed -n 's/^version=//p' " + path});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            r.close();
            p.waitFor();
            return line == null ? "" : line.trim().replace("v", "");
        } catch (Throwable t) {
            return "";
        } finally {
            if (p != null) {
                p.destroy();
            }
        }
    }

    private String httpGet(String url) throws Exception {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(8000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
        String line;
        while ((line = r.readLine()) != null) {
            sb.append(line);
        }
        r.close();
        c.disconnect();
        return sb.toString();
    }

    private String between(String s, String a, String b) {
        int i = s.indexOf(a);
        if (i < 0) {
            return "";
        }
        i += a.length();
        int j = s.indexOf(b, i);
        return j < 0 ? "" : s.substring(i, j);
    }

    private boolean isNewer(String latest, String current) {
        if (latest == null || latest.isEmpty() || current == null || current.isEmpty()) {
            return false;
        }
        String[] a = latest.split("[.]");
        String[] b = current.split("[.]");
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? num(a[i]) : 0;
            int y = i < b.length ? num(b[i]) : 0;
            if (x != y) {
                return x > y;
            }
        }
        return false;
    }

    private void checkUpdates() {
        if (tvUpdate == null) {
            return;
        }
        updateText = getString(R.string.update_checking);
        if (tvUpdate != null) {
            tvUpdate.setText(updateText);
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                String out;
                try {
                    String appVer = appVersion();
                    String kVer = moduleVersion("/data/adb/modules/adbhide_kmod/module.prop");
                    String nVer = moduleVersion("/data/adb/modules/adbhide_native/module.prop");
                    JSONObject o = new JSONObject(httpGet(
                            "https://api.github.com/repos/SampleBlazkness/AdbHide-Kernel/releases/latest"));
                    String tag = o.optString("tag_name", "").replace("v", "");
                    String kLatest = "";
                    String nLatest = "";
                    JSONArray assets = o.optJSONArray("assets");
                    if (assets != null) {
                        for (int i = 0; i < assets.length(); i++) {
                            String n = assets.getJSONObject(i).optString("name", "");
                            if (n.contains("kmod-v")) {
                                kLatest = between(n, "kmod-v", ".zip");
                            }
                            if (n.contains("native-v")) {
                                nLatest = between(n, "native-v", ".zip");
                            }
                        }
                    }
                    StringBuilder sb = new StringBuilder();
                    StringBuilder detail = new StringBuilder();
                    int hits = 0;
                    if (isNewer(tag, appVer)) {
                        hits++;
                        detail.append(getString(R.string.mod_app)).append(" ").append(appVer).append(" → ").append(tag).append("\n");
                    }
                    if (!kVer.isEmpty() && isNewer(kLatest, kVer)) {
                        hits++;
                        detail.append(getString(R.string.mod_kernel)).append(" ").append(kVer).append(" → ").append(kLatest).append("\n");
                    }
                    if (!nVer.isEmpty() && isNewer(nLatest, nVer)) {
                        hits++;
                        detail.append("Zygisk ").append(nVer).append(" → ").append(nLatest).append("\n");
                    }
                    updateDetail = detail.toString().trim();
                    out = hits == 0 ? getString(R.string.update_latest) : getString(R.string.update_available, hits);
                } catch (Throwable t) {
                    out = getString(R.string.update_failed);
                }
                final String text = out;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        updateText = text;
                        if (tvUpdate != null) {
                            tvUpdate.setText(updateText);
                        }
                    }
                });
            }
        }).start();
    }

    private State readState() {
        State st = new State();
        Map<String, String> v = new LinkedHashMap<String, String>();
        Process proc = null;
        try {
            proc = Runtime.getRuntime().exec(new String[]{"su", "-c", SCRIPT});
            BufferedReader r = new BufferedReader(new InputStreamReader(proc.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    v.put(line.substring(0, eq), line.substring(eq + 1).trim());
                }
            }
            r.close();
            proc.waitFor();
        } catch (Throwable t) {
            st.error = t.toString();
        } finally {
            if (proc != null) {
                proc.destroy();
            }
        }

        st.suOk = "1".equals(v.get("SU"));
        st.kmodOk = "1".equals(v.get("KMOD"));
        st.kmodInst = "1".equals(v.get("KMODINST"));
        st.kmodDis = "1".equals(v.get("KMODDIS"));
        st.nativeOk = "1".equals(v.get("NATIVE"));
        st.nativeInst = "1".equals(v.get("NATIVEINST"));
        st.nativeDis = "1".equals(v.get("NATIVEDIS"));
        st.kver = stripV(nz(v.get("KMODVER")));
        if (st.kver.isEmpty()) st.kver = stripV(nz(v.get("KVER")));
        st.nativeVer = stripV(nz(v.get("NATIVEVER")));

        String uidsRaw = nz(v.get("UIDS")).trim();
        if (!uidsRaw.isEmpty()) {
            for (String u : uidsRaw.split("\\s+")) {
                if (u.isEmpty()) {
                    continue;
                }
                String pkg = nz(v.get("PKG_" + u));
                if (pkg.isEmpty()) {
                    try {
                        String[] ps = getPackageManager().getPackagesForUid(Integer.parseInt(u));
                        if (ps != null && ps.length > 0) {
                            pkg = ps[0];
                        }
                    } catch (Throwable ignored) {
                    }
                }
                st.uids.add(u);
                st.pkgs.add(pkg);
            }
        }

        String confRaw = nz(v.get("CONF")).trim();
        if (!confRaw.isEmpty()) {
            for (String c : confRaw.split(java.util.regex.Pattern.quote("|"))) {
                if (!c.trim().isEmpty()) st.confPkgs.add(c.trim());
            }
        }

        st.missing = confStats(st);

        int ports = num(v.get("PORTS"));
        int ports6 = num(v.get("PORTS6"));
        int mdns = num(v.get("MDNS")) + num(v.get("MDNS6"));
        String masterRaw = nz(v.get("MASTER")).trim();
        if (masterRaw.isEmpty()) masterRaw = nz(props.get("master")).trim();
        st.masterOn = !"0".equals(masterRaw);
        st.sysActive = st.masterOn && hookAlive()
                && ports6 >= st.uids.size() * 2;

        try {
            st.appVer = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Throwable ignored) {
            st.appVer = "";
        }
        return st;
    }

    /** Fills total/resolved from the parsed blacklist; no root round-trip. */
    private List<String> confStats(State st) {
        List<String> missing = new ArrayList<String>();
        for (String pkg : st.confPkgs) {
            st.confTotal++;
            try {
                getPackageManager().getPackageUid(pkg, 0);
                st.confResolved++;
            } catch (Throwable t) {
                missing.add(pkg);
            }
        }
        return missing;
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    private static String stripV(String s) {
        return s.startsWith("v") || s.startsWith("V") ? s.substring(1) : s;
    }

    private static int num(String s) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Throwable t) {
            return 0;
        }
    }

    private int dp(int value) {
        return Ui.dp(this, value);
    }

    private static final class State {
        boolean suOk;
        boolean kmodOk;
        boolean kmodInst;
        boolean kmodDis;
        boolean nativeOk;
        boolean nativeInst;
        boolean nativeDis;
        boolean sysActive;
        boolean masterOn;
        String kver = "";
        String nativeVer = "";
        String appVer = "";
        String error;
        int confTotal;
        int confResolved;
        final List<String> uids = new ArrayList<String>();
        final List<String> confPkgs = new ArrayList<String>();
        final List<String> pkgs = new ArrayList<String>();
        List<String> missing = new ArrayList<String>();
    }

    private boolean hookAlive() {
        try {
            return Settings.Global.getInt(getContentResolver(), "adbhide_hook_probe", 0) == 1;
        } catch (Throwable t) {
            return false;
        }
    }

    private void removePkg(final String pkg) {
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c",
                    "sed -i '/^" + pkg + "$/d' /data/adb/adbhide/blacklist.conf; "
                            + "sh /data/adb/modules/adbhide_kmod/sync.sh"}).waitFor();
        } catch (Throwable ignored) {
        }
        refresh();
    }
}
