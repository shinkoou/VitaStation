package org.vita3k.emulator.ui.screens.emulation;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.BatteryManager;
import android.os.Debug;
import android.os.Process;
import android.view.View;

import org.vita3k.emulator.NativeLib;
import org.vita3k.emulator.data.PerformanceHudPrefs;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class PerformanceHudView extends View {
    private final Paint panelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint valuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shaderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shaderTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final float density;
    private int fps;
    private int shaderCount;
    private float ramMb;
    private float cpuPercent;
    private Float gpuPercent;
    private Float batteryTemp;
    private long previousCpuMs;
    private long previousWallMs;

    private final Runnable updater = new Runnable() {
        @Override public void run() {
            updateMetrics();
            invalidate();
            postDelayed(this, 500L);
        }
    };

    public PerformanceHudView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);

        panelPaint.setColor(Color.argb(205, 7, 12, 27));
        borderPaint.setColor(Color.argb(205, 89, 216, 255));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(dp(0.8f));

        labelPaint.setColor(Color.rgb(149, 163, 188));
        labelPaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif-medium", android.graphics.Typeface.NORMAL));
        valuePaint.setColor(Color.WHITE);
        valuePaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif", android.graphics.Typeface.BOLD));

        shaderPaint.setColor(Color.argb(210, 35, 23, 73));
        shaderTextPaint.setColor(Color.rgb(190, 166, 255));
        shaderTextPaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif-medium", android.graphics.Typeface.NORMAL));

        previousCpuMs = Process.getElapsedCpuTime();
        previousWallMs = android.os.SystemClock.elapsedRealtime();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        removeCallbacks(updater);
        post(updater);
    }

    @Override protected void onDetachedFromWindow() {
        removeCallbacks(updater);
        super.onDetachedFromWindow();
    }

    private void updateMetrics() {
        boolean enabled = PerformanceHudPrefs.isMasterEnabled(getContext());
        setVisibility(enabled ? VISIBLE : GONE);
        if (!enabled) return;

        try {
            fps = NativeLib.INSTANCE.getCurrentFps();
            shaderCount = NativeLib.INSTANCE.getRecentShaderCompileCount();
        } catch (Throwable ignored) {
            fps = 0;
            shaderCount = 0;
        }

        ramMb = Debug.getPss() / 1024f;

        long nowCpu = Process.getElapsedCpuTime();
        long nowWall = android.os.SystemClock.elapsedRealtime();
        long cpuDelta = Math.max(0L, nowCpu - previousCpuMs);
        long wallDelta = Math.max(1L, nowWall - previousWallMs);
        int cores = Math.max(1, Runtime.getRuntime().availableProcessors());
        cpuPercent = Math.min(100f, (cpuDelta * 100f) / (wallDelta * cores));
        previousCpuMs = nowCpu;
        previousWallMs = nowWall;

        gpuPercent = readGpuUsage();
        batteryTemp = readBatteryTemperature();
    }

    private Float readBatteryTemperature() {
        try {
            Intent battery = getContext().registerReceiver(
                    null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery == null) return null;
            int tenths = battery.getIntExtra(
                    BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            if (tenths == Integer.MIN_VALUE || tenths <= 0) return null;
            return tenths / 10f;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private Float readGpuUsage() {
        Float direct = readSinglePercent("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
        if (direct != null) return direct;

        try {
            String text = readText("/sys/class/kgsl/kgsl-3d0/gpubusy");
            if (text != null) {
                String[] parts = text.trim().split("\\s+");
                if (parts.length >= 2) {
                    float busy = Float.parseFloat(parts[0]);
                    float total = Float.parseFloat(parts[1]);
                    if (total > 0f) return clamp((busy / total) * 100f);
                }
            }
        } catch (Throwable ignored) {}

        try {
            File devfreq = new File("/sys/class/devfreq");
            File[] devices = devfreq.listFiles();
            if (devices != null) {
                for (File device : devices) {
                    String name = device.getName().toLowerCase(Locale.ROOT);
                    if (!name.contains("gpu") && !name.contains("mali")) continue;
                    for (String leaf : new String[]{"load", "utilization", "busy_percent"}) {
                        Float value = readSinglePercent(new File(device, leaf).getAbsolutePath());
                        if (value != null) return value;
                    }
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private Float readSinglePercent(String path) {
        try {
            String text = readText(path);
            if (text == null) return null;
            String token = text.trim().split("\\s+")[0].replace("%", "");
            float value = Float.parseFloat(token);
            if (value > 100f && value <= 1000f) value /= 10f;
            if (value < 0f) return null;
            return clamp(value);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private String readText(String path) {
        try {
            File file = new File(path);
            if (!file.canRead()) return null;
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private float clamp(float value) {
        return Math.max(0f, Math.min(100f, value));
    }

    private float dp(float value) {
        return value * density;
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (getVisibility() != VISIBLE) return;

        List<String[]> metrics = new ArrayList<>();
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_FPS))
            metrics.add(new String[]{"FPS", fps > 0 ? Integer.toString(fps) : "—"});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_RAM))
            metrics.add(new String[]{"RAM", ramMb >= 1024f
                    ? String.format(Locale.US, "%.1fG", ramMb / 1024f)
                    : String.format(Locale.US, "%.0fM", ramMb)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_CPU))
            metrics.add(new String[]{"CPU", String.format(Locale.US, "%.0f%%", cpuPercent)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_GPU))
            metrics.add(new String[]{"GPU", gpuPercent == null ? "—"
                    : String.format(Locale.US, "%.0f%%", gpuPercent)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_BATTERY))
            metrics.add(new String[]{"BATT", batteryTemp == null ? "—"
                    : String.format(Locale.US, "%.1f°C", batteryTemp)});

        if (metrics.isEmpty()) return;

        float available = Math.max(dp(180f), getWidth() - dp(20f));
        float panelWidth = Math.min(available, dp(520f));
        float left = (getWidth() - panelWidth) / 2f;
        float top = dp(5f);
        float height = dp(34f);
        float right = left + panelWidth;
        RectF panel = new RectF(left, top, right, top + height);

        canvas.drawRoundRect(panel, dp(12f), dp(12f), panelPaint);
        canvas.drawRoundRect(panel, dp(12f), dp(12f), borderPaint);

        labelPaint.setTextSize(dp(7.5f));
        valuePaint.setTextSize(dp(11.5f));

        float cellWidth = panelWidth / metrics.size();
        for (int i = 0; i < metrics.size(); i++) {
            float center = left + (cellWidth * i) + (cellWidth / 2f);
            String[] metric = metrics.get(i);

            float labelWidth = labelPaint.measureText(metric[0]);
            float valueWidth = valuePaint.measureText(metric[1]);
            float gap = dp(4f);
            float start = center - (labelWidth + gap + valueWidth) / 2f;
            float baseline = top + dp(22f);

            labelPaint.setTextAlign(Paint.Align.LEFT);
            valuePaint.setTextAlign(Paint.Align.LEFT);
            canvas.drawText(metric[0], start, baseline, labelPaint);
            canvas.drawText(metric[1], start + labelWidth + gap, baseline, valuePaint);
        }

        if (shaderCount > 0) {
            String shaderText = "SHADERS  •  " + shaderCount;
            shaderTextPaint.setTextSize(dp(8.5f));
            shaderTextPaint.setTextAlign(Paint.Align.CENTER);
            float textWidth = shaderTextPaint.measureText(shaderText);
            float pillWidth = textWidth + dp(22f);
            float pillTop = top + height + dp(3f);
            RectF pill = new RectF(
                    getWidth() / 2f - pillWidth / 2f,
                    pillTop,
                    getWidth() / 2f + pillWidth / 2f,
                    pillTop + dp(17f)
            );
            canvas.drawRoundRect(pill, dp(8.5f), dp(8.5f), shaderPaint);
            canvas.drawText(
                    shaderText,
                    getWidth() / 2f,
                    pillTop + dp(11.8f),
                    shaderTextPaint
            );
        }
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        setMeasuredDimension(width, (int) dp(62f));
    }
}
