package com.blazkness.adbng;

// the merged app uses com.blazkness.adbhide as its package, so R lives there
import com.blazkness.adbhide.R;

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
        setContentView(R.layout.activity_main);
        ((LinearLayout) findViewById(R.id.tabHost))
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
                            msg(err == null ? "运行环境已安装到 /data/adb/adbng" : err,
                                    err != null);
                        }
                    });
                }
                if (Sh.run("id -u").text().equals("0")) {
                    return;
                }
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        msg("没有拿到 root：开关不可用", true);
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
            tvStatus.setText("读取状态失败\n" + err);
        } else {
            String adbd = "running".equals(st.get("adbd")) ? "运行中" : "已关闭";
            String adbng = "running".equals(st.get("state"))
                    ? "运行中（pid " + st.get("usbgwd_pid") + "）" : "已停止";
            String udc = st.get("udc_state");
            String link = "present".equals(st.get("adbng_link")) ? "已接入 USB" : "未接入";
            tvStatus.setText("系统 adb：" + adbd
                    + "\nadbng：" + adbng
                    + "\nUSB：" + link + "（" + (udc == null || udc.isEmpty() ? "未知" : udc) + "）"
                    + "\n已配对密钥：" + (st.get("keys") == null ? "-" : st.get("keys")) + " 条"
                    + "\n开关记忆：" + ("1".equals(st.get("enabled")) ? "开机自动启动" : "关闭"));
        }

        updatingSwitch = true;
        sw.setChecked("1".equals(st.get("enabled")) || "running".equals(st.get("state")));
        updatingSwitch = false;

        if (pend == null) {
            cardPending.setVisibility(View.GONE);
            shownFp = null;
        } else {
            cardPending.setVisibility(View.VISIBLE);
            tvPending.setText("来自 " + pend.host
                    + "\n指纹 " + pend.fp
                    + (pend.since.isEmpty() ? "" : "\n请求时间 " + pend.since));
            if (!pend.fp.equals(shownFp)) {
                shownFp = pend.fp;
            }
        }

        if (rows.isEmpty()) {
            tvDevices.setText("还没有记录");
        } else {
            StringBuilder sb = new StringBuilder();
            int n = Math.min(rows.size(), 8);
            for (int i = 0; i < n; i++) {
                Ctl.Row r = rows.get(i);
                sb.append(r.stamp).append("  ")
                        .append("allow".equals(r.verdict) ? "允许" : "拒绝").append("  ")
                        .append(r.fp.length() > 8 ? r.fp.substring(0, 8) : r.fp);
                if (!r.host.isEmpty()) {
                    sb.append("  ").append(r.host);
                }
                sb.append('\n');
            }
            tvDevices.setText(sb.toString().trim());
        }

        tvLog.setText(log.isEmpty() ? "（暂无日志）" : log);
    }

    private void toggle(final boolean on) {
        busy = true;
        msg(on ? "正在启动 adbng…" : "正在停止 adbng…", false);
        work.execute(new Runnable() {
            @Override
            public void run() {
                final Sh.R r = Sh.ctl(on ? "start" : "stop");
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        String out = r.text();
                        // the script already speaks Chinese for its refusals
                        if (!r.ok()) {
                            msg(out.isEmpty() ? "操作失败（rc=" + r.code + "）" : out, true);
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
                .setTitle("关闭 adbng")
                .setMessage("关闭后系统 adb 不会自动恢复，需要手动在开发者选项里重新打开。")
                .setPositiveButton("关闭", null)
                .show();
    }
}