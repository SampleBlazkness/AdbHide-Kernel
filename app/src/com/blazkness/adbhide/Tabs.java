package com.blazkness.adbhide;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

// the merged app is built with com.blazkness.adbng as its package, so R lives there
import com.blazkness.adbng.R;

/** Top tab bar shared by the two screens: the hide list (adbhide) and adbng. */
public final class Tabs {

    private static final String TAG = "adbngTabs";
    private static final int CHIP_STROKE = 0xFFD7DBE2;

    private Tabs() {
    }

    public static View bar(final Activity host, final boolean onHide) {
        LinearLayout row = new LinearLayout(host);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        row.addView(chip(host, host.getString(R.string.tab_hide), onHide, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!onHide)
                    swap(host, true);
            }
        }));

        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        gap.leftMargin = Ui.dp(host, 8);
        row.addView(chip(host, host.getString(R.string.tab_adbng), !onHide, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (onHide)
                    swap(host, false);
            }
        }), gap);

        return row;
    }

    /**
     * Jump to the other screen. Uses the class object rather than setClassName(String)
     * so a typo cannot fail silently, and only finishes the current screen once the
     * other one was actually started.
     */
    private static void swap(Activity host, boolean toHide) {
        Intent i = toHide
                ? new Intent(host, com.blazkness.adbhide.MainActivity.class)
                : new Intent(host, com.blazkness.adbng.MainActivity.class);
        // HyperOS silently cancels a startActivity that it treats as a background
        // launch, so never finish() this screen: an empty task stack turns the jump
        // into "back to the launcher". REORDER_TO_FRONT keeps one instance each.
        i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        try {
            host.startActivity(i);
        } catch (Throwable t) {
            Log.w(TAG, "cannot open the other screen", t);
        }
    }

    private static TextView chip(Activity host, String text, boolean active, View.OnClickListener l) {
        TextView tv = Ui.text(host, text, 13, active ? 0xFFFFFFFF : Ui.INK);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setGravity(Gravity.CENTER);
        tv.setIncludeFontPadding(false);
        int padH = Ui.dp(host, 16);
        int padV = Ui.dp(host, 8);
        tv.setPadding(padH, padV, padH, padV);
        // Both states carry a 1dp stroke on purpose: a filled chip without a stroke
        // measures 2dp narrower than an outlined one, which shifted the row on switch.
        tv.setBackground(active
                ? Ui.bg(Ui.ACCENT, 999, Ui.ACCENT, host)
                : Ui.bg(Ui.CARD, 999, CHIP_STROKE, host));
        tv.setClickable(true);
        tv.setOnClickListener(l);
        return tv;
    }
}