package com.blazkness.adbng;

// the merged app uses com.blazkness.adbng as its package, so R lives there
import com.blazkness.adbng.R;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends Activity {

    private static final long POLL_MS = 2500;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService work = Executors.newSingleThreadExecutor();

    private Switch sw;
    private TextView tvSub;
    private TextView tvStatus;
    private TextView tvPending;
    private TextView tvDevices;
    private TextView tvLog;
    private TextView tvMsg;
    private View cardPending;
    private Button btnAllow;
    private Button btnDeny;

    private boolean updatingSwitch;
    private boolean busy;
    private boolean resumed;
    private String shownFp;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            refresh();
            if (resumed) {
                ui.postDelayed(this, POLL_MS);
            }
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Sh.init(this);
        setContentView(R.layout.activity_main);
        ((LinearLayout) findViewById(R.id.tabHost))
                // false = this is the Adbng screen, not the AdbHide one
                .addView(com.blazkness.adbhide.Tabs.bar(this, false));

        sw = findViewById(R.id.sw);
        tvSub = findViewById(R.id.tvSub);
        tvStatus = findViewById(R.id.tvStatus);
        tvPending = findViewById(R.id.tvPending);
        tvDevices = findViewById(R.id.tvDevices);
        tvLog = findViewById(R.id.tvLog);
        tvMsg = findViewById(R.id.tvMsg);
        cardPending = findViewById(R.id.cardPending);
        btnAllow = findViewById(R.id.btnAllow);
        btnDeny = findViewById(R.id.btnDeny);

        findViewById(R.id.btnRefresh).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                refresh();
            }
        });

        btnAllow.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                decide("allow");
            }
        });

        btnDeny.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                decide("deny");
            }
        });

        sw.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton view, boolean checked) {
                if (updatingSwitch) {
                    return;
                }
                toggle(checked);
            }
        });

        work.execute(new Runnable() {
            @Override
            public void run() {
                if (!Env.installed(MainActivity.this)) {
                    final String err = Env.install(MainActivity.this);
                    ui.post(new Runnable() {
                        @Override
                        public void run() {
                            msg(err == null
                                    ? getString(R.string.adbng_env_installed, Sh.DIR)
                                    : err, err != null);
                        }
                    });
                }
                if (Sh.run("id -u").text().equals("0")) {
                    return;
                }
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        msg(getString(R.string.adbng_no_root), true);
                        sw.setEnabled(false);
                    }
                });
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        ui.removeCallbacks(poll);
        ui.post(poll);
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
        ui.removeCallbacks(poll);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        work.shutdownNow();
    }

    private void refresh() {
        if (busy) {
            return;
        }
        busy = true;
        work.execute(new Runnable() {
            @Override
            public void run() {
                final Map<String, String> st = Ctl.status();
                final Ctl.Pending pend = Ctl.pending();
                final List<Ctl.Row> rows = Ctl.devices();
                final String log = Ctl.logTail(25);
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        render(st, pend, rows, log);
                    }
                });
            }
        });
    }

    private void render(Map<String, String> st, Ctl.Pending pend, List<Ctl.Row> rows, String log) {
        String err = st.get("error");
        if (err != null) {
            tvStatus.setText(getString(R.string.adbng_read_failed, err));
        } else {
            String adbd = getString("running".equals(st.get("adbd"))
                    ? R.string.adbng_adbd_on : R.string.adbng_adbd_off);
            String adbng = "running".equals(st.get("state"))
                    ? getString(R.string.adbng_running_pid,
                            String.valueOf(st.get("usbgwd_pid")))
                    : getString(R.string.adbng_stopped);
            String udc = st.get("udc_state");
            if (udc == null || udc.isEmpty()) {
                udc = getString(R.string.adbng_unknown);
            }
            String link = getString("present".equals(st.get("adbng_link"))
                    ? R.string.adbng_usb_present : R.string.adbng_usb_absent);
            String keys = st.get("keys") == null ? "-" : st.get("keys");
            String memory = getString("1".equals(st.get("enabled"))
                    ? R.string.adbng_boot_on : R.string.adbng_off);
            tvStatus.setText(getString(R.string.adbng_status_format,
                    adbd, adbng, link, udc, keys, memory));
        }

        updatingSwitch = true;
        sw.setChecked("1".equals(st.get("enabled")) || "running".equals(st.get("state")));
        updatingSwitch = false;

        if (pend == null) {
            cardPending.setVisibility(View.GONE);
            shownFp = null;
        } else {
            cardPending.setVisibility(View.VISIBLE);
            StringBuilder pb = new StringBuilder(getString(R.string.adbng_from, pend.host));
            pb.append('\n').append(getString(R.string.adbng_fp, pend.fp));
            if (!pend.since.isEmpty()) {
                pb.append('\n').append(getString(R.string.adbng_since, pend.since));
            }
            tvPending.setText(pb.toString());
            if (!pend.fp.equals(shownFp)) {
                shownFp = pend.fp;
            }
        }

        if (rows.isEmpty()) {
            tvDevices.setText(getString(R.string.adbng_no_devices));
        } else {
            StringBuilder sb = new StringBuilder();
            int n = Math.min(rows.size(), 8);
            for (int i = 0; i < n; i++) {
                Ctl.Row r = rows.get(i);
                sb.append(r.stamp).append("  ")
                        .append(getString("allow".equals(r.verdict)
                                ? R.string.adbng_case_allow : R.string.adbng_case_deny))
                        .append("  ")
                        .append(r.fp.length() > 8 ? r.fp.substring(0, 8) : r.fp);
                if (!r.host.isEmpty()) {
                    sb.append("  ").append(r.host);
                }
                sb.append('\n');
            }
            tvDevices.setText(sb.toString().trim());
        }

        tvLog.setText(log.isEmpty() ? getString(R.string.adbng_no_log) : log);
    }

    private void toggle(final boolean on) {
        busy = true;
        msg(getString(on ? R.string.adbng_starting : R.string.adbng_stopping), false);
        work.execute(new Runnable() {
            @Override
            public void run() {
                final Sh.R r = Sh.ctl(on ? "start" : "stop");
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        String out = r.text();
                        // adbngctl.sh still answers in Chinese on refusal; show it as is
                        if (!r.ok()) {
                            msg(out.isEmpty()
                                    ? getString(R.string.adbng_op_failed, r.code) : out, true);
                            updatingSwitch = true;
                            sw.setChecked(false);
                            updatingSwitch = false;
                        } else {
                            msg(out, false);
                        }
                        refresh();
                    }
                });
            }
        });
    }

    private void decide(final String verdict) {
        final String fp = shownFp;
        if (fp == null) {
            return;
        }
        busy = true;
        work.execute(new Runnable() {
            @Override
            public void run() {
                final Sh.R r = Sh.ctl(verdict + " " + fp);
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        msg(r.text(), !r.ok());
                        refresh();
                    }
                });
            }
        });
    }

    private void msg(String text, boolean bad) {
        tvMsg.setText(text);
        tvMsg.setTextColor(bad ? Color.parseColor("#FF7043") : Color.parseColor("#4CAF50"));
    }

    @SuppressWarnings("unused")
    private void confirmStop() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.adbng_stop_title)
                .setMessage(R.string.adbng_stop_msg)
                .setPositiveButton(R.string.adbng_stop_ok, null)
                .show();
    }
}