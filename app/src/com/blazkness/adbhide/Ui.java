package com.blazkness.adbhide;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Small view helpers so the screens stay readable without any UI library. */
final class Ui {

    static final int BG = 0xFFF4F5F7;
    static final int CARD = 0xFFFFFFFF;
    static final int INK = 0xFF111827;
    static final int MUTED = 0xFF6B7280;
    static final int ACCENT = 0xFF2563EB;
    static final int OK = 0xFF16A34A;
    static final int BAD = 0xFFDC2626;
    static final int LINE = 0xFFE8EAED;

    private Ui() {
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                c.getResources().getDisplayMetrics());
    }

    static GradientDrawable bg(int color, float radius, Context c) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(c, radius));
        return d;
    }

    static GradientDrawable bg(int color, float radius, int stroke, Context c) {
        GradientDrawable d = bg(color, radius, c);
        d.setStroke(dp(c, 1), stroke);
        return d;
    }

    static LinearLayout card(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setBackground(bg(CARD, 18, c));
        l.setPadding(dp(c, 18), dp(c, 16), dp(c, 18), dp(c, 16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(c, 12);
        l.setLayoutParams(lp);
        return l;
    }

    static TextView text(Context c, String s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    static TextView title(Context c, String s) {
        TextView t = text(c, s, 16, INK);
        t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        return t;
    }

    static TextView sub(Context c, String s) {
        return text(c, s, 12, MUTED);
    }

    static TextView chip(Context c, String s, boolean ok) {
        TextView t = text(c, s, 11, ok ? OK : BAD);
        int padH = dp(c, 9);
        int padV = dp(c, 4);
        t.setPadding(padH, padV, padH, padV);
        t.setBackground(bg(ok ? 0xFFE9F7EF : 0xFFFDECEC, 999, c));
        return t;
    }

    static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(LINE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(c, 0.6f)));
        lp.topMargin = dp(c, 6);
        lp.bottomMargin = dp(c, 6);
        v.setLayoutParams(lp);
        return v;
    }

    static TextView action(Context c, String s, boolean primary) {
        TextView t = text(c, s, 13, primary ? 0xFFFFFFFF : ACCENT);
        t.setGravity(android.view.Gravity.CENTER);
        t.setPadding(dp(c, 14), dp(c, 10), dp(c, 14), dp(c, 10));
        t.setBackground(primary
                ? bg(ACCENT, 12, c)
                : bg(CARD, 12, 0xFFD7DBE2, c));
        return t;
    }
}
